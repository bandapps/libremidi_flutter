// Replaces upstream libremidi's backends/android/midi_out.hpp. See helpers.hpp
// for how devices and ports are opened and shared.

#pragma once
#include <libremidi/backends/android/config.hpp>
#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/detail/midi_out.hpp>

#include <mutex>

NAMESPACE_LIBREMIDI
{
namespace android
{
class midi_out final
    : public midi1::out_api
    , public error_handler
{
public:
  explicit midi_out(libremidi::output_configuration&& conf, output_configuration)
      : configuration{std::move(conf)}
  {
    client_open_ = stdx::error{};
  }

  ~midi_out() override { close_port(); }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  // Synchronous: returns once the port is open, or fails with the reason in
  // last_error().
  stdx::error open_port(const output_port& port, std::string_view) override
  {
    std::string error;
    auto p = open_send_port(handle_device_id(port.port), handle_port_index(port.port), error);
    if (!p)
    {
      set_last_error(error);
      libremidi_handle_error(configuration, "midi_out::open_port: " + error);
      return std::errc::io_error;
    }
    std::lock_guard<std::mutex> lock(mutex);
    port_ = std::move(p);
    return stdx::error{};
  }

  stdx::error close_port() override
  {
    // Under the lock, so a send that already holds the port finishes first.
    std::lock_guard<std::mutex> lock(mutex);
    port_.reset();
    return stdx::error{};
  }

  stdx::error send_message(const unsigned char* message, size_t size) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!port_)
    {
      libremidi_handle_error(configuration, "midi_out::send_message: port is not open");
      return std::errc::not_connected;
    }
    if (!port_->send(message, size))
    {
      libremidi_handle_error(configuration, "midi_out::send_message: failed to send MIDI data");
      return std::errc::io_error;
    }
    return stdx::error{};
  }

private:
  libremidi::output_configuration configuration;
  std::mutex mutex;  // guards port_
  std::shared_ptr<send_port> port_;
};
}
}
