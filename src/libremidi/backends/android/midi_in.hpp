// Replaces upstream libremidi's backends/android/midi_in.hpp. See helpers.hpp
// for how devices are opened and owned.

#pragma once
#include <libremidi/backends/android/config.hpp>
#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/detail/midi_in.hpp>
#include <libremidi/detail/midi_stream_decoder.hpp>

#include <atomic>
#include <chrono>
#include <thread>

NAMESPACE_LIBREMIDI
{
namespace android
{
class midi_in final
    : public midi1::in_api
    , public error_handler
{
public:
  explicit midi_in(libremidi::input_configuration&& conf, input_configuration)
      : configuration{std::move(conf)}
  {
    client_open_ = stdx::error{};
  }

  ~midi_in() override { close_port(); }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  // Synchronous: returns once the port is open and being read, or fails with
  // the reason in last_error().
  stdx::error open_port(const input_port& port, std::string_view) override
  {
    std::string error;
    auto lease = open_device(handle_device_id(port.port), error);
    if (!lease)
      return fail(error);

    AMidiOutputPort* p = nullptr;
    const auto status = AMidiOutputPort_open(lease->device(), handle_port_index(port.port), &p);
    if (status != AMEDIA_OK || !p)
      return fail("the port could not be opened, it may be in use by another app ("
                  + std::to_string(status) + ")");

    lease_ = std::move(lease);
    port_ = p;
    running_ = true;
    reader_ = std::thread(&midi_in::read_loop, this, p);
    return stdx::error{};
  }

  // Runs on the owning thread. The reader thread only ever uses the port it
  // was started with, and is joined before that port is closed.
  stdx::error close_port() override
  {
    running_ = false;
    if (reader_.joinable())
      reader_.join();
    if (port_)
      AMidiOutputPort_close(port_);
    port_ = nullptr;
    lease_.reset();
    return stdx::error{};
  }

  timestamp absolute_timestamp() const noexcept override { return system_ns(); }

private:
  stdx::error fail(const std::string& reason)
  {
    set_last_error(reason);
    libremidi_handle_error(configuration, "midi_in::open_port: " + reason);
    return std::errc::io_error;
  }

  void read_loop(AMidiOutputPort* port)
  {
    static constexpr timestamp_backend_info timestamp_info{
        .has_absolute_timestamps = false,
        .absolute_is_monotonic = false,
        .has_samples = false,
    };

    // Larger than any single packet MidiService delivers.
    uint8_t buffer[1024];
    bool reported_error = false;

    while (running_)
    {
      int32_t op_code = 0;
      int64_t timestamp = 0;
      size_t num_bytes = 0;
      // Non-blocking, one packet per call. Sleep only when nothing is queued:
      // sleeping before every call capped delivery at about one packet per
      // millisecond.
      const ssize_t received = AMidiOutputPort_receive(
          port, &op_code, buffer, sizeof(buffer), &num_bytes, &timestamp);

      if (received < 0)
      {
        // A removed device keeps failing here until the port is closed; say so
        // once instead of every few milliseconds.
        if (!reported_error)
          LOGW("MIDI input stopped receiving (%d), device removed?", (int)received);
        reported_error = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (received == 0)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      // FLUSH packets carry no data.
      if (op_code != AMIDI_OPCODE_DATA || num_bytes == 0)
        continue;

      const auto to_ns = [=] { return timestamp; };
      m_processing.on_bytes_multi(
          {buffer, buffer + num_bytes}, m_processing.timestamp<timestamp_info>(to_ns, timestamp));
    }
  }

  libremidi::input_configuration configuration;
  std::shared_ptr<device_lease> lease_;
  AMidiOutputPort* port_ = nullptr;
  std::thread reader_;
  std::atomic<bool> running_{false};
  midi1::input_state_machine m_processing{this->configuration};
};
}
}
