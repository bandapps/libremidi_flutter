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
class midi_in
    : public midi1::in_api
    , public error_handler
{
public:
  explicit midi_in(
      libremidi::input_configuration&& conf, libremidi::android::input_configuration aconf)
      : configuration{std::move(conf)}
  {
    if (!context::client_name.empty() && context::client_name != aconf.client_name)
    {
      libremidi_handle_error(
          configuration, "Android backend only supports one client name per process");
      return;
    }
    context::client_name = std::string(aconf.client_name);
    client_open_ = stdx::error{};
  }

  ~midi_in() override { close_port(); }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  stdx::error open_port(const input_port& port, std::string_view /*name*/) override
  {
    if (port_requested)
    {
      libremidi_handle_error(configuration, "midi_in::open_port: a port is already open");
      return std::errc::io_error;
    }

    const unsigned int port_number = port.port;
    auto env = context::get_thread_env();
    if (!env)
    {
      libremidi_handle_error(configuration, "midi_in::open_port: failed to get JNI environment");
      return std::errc::io_error;
    }

    auto ctx = context::get_context(env);
    if (!ctx)
    {
      libremidi_handle_error(configuration, "midi_in::open_port: failed to get Android context");
      return std::errc::io_error;
    }

    // The index below refers to the list this refresh produces, so both must
    // happen under one lock.
    std::lock_guard<std::recursive_mutex> lock(context::ports_mutex());
    context::refresh_midi_devices(env, ctx, false);

    if (port_number >= context::midi_ports.size())
    {
      libremidi_handle_error(configuration, "midi_in::open_port: invalid port number");
      return std::errc::io_error;
    }

    port_requested = true;
    running = true;
    // Hand Java a token, not `this`: the open completes asynchronously and this
    // object may be gone by then. The token also carries the port index, so a
    // second open cannot redirect this one to a different port.
    const auto& entry = context::midi_ports[port_number];
    const int64_t token = register_pending_open(this, entry.port_index, false);
    if (!context::open_device(entry, token, false))
    {
      // Nothing was handed to Java, so no callback will arrive to clear this.
      running = false;
      port_requested = false;
      libremidi_handle_error(configuration, "midi_in::open_port: openDevice failed");
      return std::errc::io_error;
    }
    return stdx::error{};
  }

  stdx::error close_port() override
  {
    // Drop any open that has not called back yet, so a late callback resolves
    // to nothing instead of writing into this object after it is destroyed.
    cancel_pending_open(this);

    if (!port_requested)
      return std::errc{};
    port_requested = false;

    running = false;

    if (poll_thread.joinable())
    {
      poll_thread.join();
    }

    if (auto* port = midi_output_port.exchange(nullptr, std::memory_order_acq_rel))
    {
      AMidiOutputPort_close(port);
    }

    if (auto* device = receive_device.exchange(nullptr, std::memory_order_acq_rel))
    {
      AMidiDevice_release(device);
    }

    return stdx::error{};
  }

  // Reports the *actual* state, not the request. Opening is asynchronous on
  // Android: open_port() returns before MidiManager has handed the device
  // back, so a caller that trusted an optimistic flag saw "open" while a send
  // still failed with "port is not open".
  bool is_port_open() const noexcept
  {
    return midi_output_port.load(std::memory_order_acquire) != nullptr;
  }

  // Clears the requested state when no callback will ever arrive.
  static void open_failed(midi_in* self) noexcept
  {
    self->running = false;
    self->port_requested = false;
  }

  void start_midi_thread() { poll_thread = std::thread(&midi_in::poll_midi, this); }

  timestamp absolute_timestamp() const noexcept override { return system_ns(); }
  void poll_midi()
  {
    static constexpr timestamp_backend_info timestamp_info{
        .has_absolute_timestamps = false,
        .absolute_is_monotonic = false,
        .has_samples = false,
    };

    // The poll thread starts only after the port was stored, and close_port()
    // joins this thread before clearing it, so one load is enough.
    AMidiOutputPort* const port = midi_output_port.load(std::memory_order_acquire);
    if (!port)
      return;

    uint8_t buffer[1024];
    int32_t op_code;
    int64_t timestamp;
    size_t num_bytes;

    while (running)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

      ssize_t num_messages = AMidiOutputPort_receive(
          port, &op_code, buffer, sizeof(buffer), &num_bytes, &timestamp);

      if (num_messages < 0)
      {
        LOGE("Error receiving MIDI data: %d", (int)num_messages);
        continue;
      }

      if (num_messages > 0 && num_bytes > 0)
      {
        const auto to_ns = [=] { return timestamp; };

        m_processing.on_bytes_multi(
            {buffer, buffer + num_bytes},
            m_processing.timestamp<timestamp_info>(to_ns, timestamp));
      }
    }
  }

  static void open_callback(midi_in* self, AMidiDevice* device, int port_index)
  {
    // Check the result: on failure starting the poll thread would spin
    // AMidiOutputPort_receive() on a null port forever, while the API reported
    // the input as open.
    AMidiOutputPort* port = nullptr;
    if (AMidiOutputPort_open(device, port_index, &port) != AMEDIA_OK)
    {
      LOGE("AMidiOutputPort_open failed for port %d", port_index);
      // Release the device here: close_port() bails out early once the request
      // is cleared, so it would never get to AMidiDevice_release().
      AMidiDevice_release(device);
      open_failed(self);
      return;
    }
    // Publish both before the poll thread can observe them.
    self->receive_device.store(device, std::memory_order_release);
    self->midi_output_port.store(port, std::memory_order_release);
    self->start_midi_thread();
  }

private:
  libremidi::input_configuration configuration;
  // Touched by three threads: the Java open callback stores them, the poll
  // thread reads the port, and the caller clears both in close_port(). Plain
  // pointers here were a data race, and is_port_open()/send paths made it a
  // read from yet another thread.
  std::atomic<AMidiDevice*> receive_device{nullptr};
  std::atomic<AMidiOutputPort*> midi_output_port{nullptr};
  std::thread poll_thread;
  std::atomic<bool> port_requested{false};
  std::atomic<bool> running{false};
  midi1::input_state_machine m_processing{this->configuration};
};
}
}
