#pragma once
#include <atomic>
#include <mutex>
#include <libremidi/backends/android/config.hpp>
#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/detail/midi_out.hpp>

NAMESPACE_LIBREMIDI
{
namespace android
{
class midi_out final
    : public midi1::out_api
    , public error_handler
{
public:
  explicit midi_out(
      libremidi::output_configuration&& conf, libremidi::android::output_configuration aconf)
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

  ~midi_out() override { close_port(); }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  stdx::error open_port(const output_port& port, std::string_view /*name*/) override
  {
    if (port_requested)
    {
      libremidi_handle_error(configuration, "midi_out::open_port: a port is already open");
      return std::errc::io_error;
    }

    const unsigned int port_number = port.port;

    auto env = context::get_thread_env();
    if (!env)
    {
      libremidi_handle_error(configuration, "midi_out::open_port: failed to get JNI environment");
      return std::errc::io_error;
    }

    auto ctx = context::get_context(env);
    if (!ctx)
    {
      libremidi_handle_error(configuration, "midi_out::open_port: failed to get Android context");
      return std::errc::io_error;
    }

    // The index below refers to the list this refresh produces, so both must
    // happen under one lock.
    std::lock_guard<std::recursive_mutex> lock(context::ports_mutex());
    context::refresh_midi_devices(env, ctx, true);

    if (port_number >= context::midi_ports.size())
    {
      libremidi_handle_error(configuration, "midi_out::open_port: invalid port number");
      return std::errc::io_error;
    }

    port_requested = true;
    // See midi_in::open_port: Java gets a token carrying both this object and
    // the port index, never a raw pointer or a shared index.
    const auto& entry = context::midi_ports[port_number];
    const int64_t token = register_pending_open(this, entry.port_index, true);
    if (!context::open_device(entry, token, true))
    {
      // Nothing reached Java, so no callback will arrive to clear this.
      port_requested = false;
      libremidi_handle_error(configuration, "midi_out::open_port: openDevice failed");
      return std::errc::io_error;
    }
    return stdx::error{};
  }

  stdx::error close_port() override
  {
    // Drop a not-yet-delivered open before this object can be destroyed.
    cancel_pending_open(this);

    if (!port_requested)
      return std::errc{};
    port_requested = false;

    // Take the handles under port_mutex so a send that already loaded the port
    // has finished before it is closed. The atomic load alone only rules out a
    // torn pointer; it does not stop AMidiInputPort_close() from running
    // between that load and AMidiInputPort_send().
    AMidiInputPort* port = nullptr;
    AMidiDevice* device = nullptr;
    jobject jdevice = nullptr;
    {
      std::lock_guard<std::mutex> lock(port_mutex);
      port = midi_input_port.exchange(nullptr, std::memory_order_acq_rel);
      device = send_device.exchange(nullptr, std::memory_order_acq_rel);
      jdevice = java_device.exchange(nullptr, std::memory_order_acq_rel);
    }

    if (port)
      AMidiInputPort_close(port);
    context::close_device(device, jdevice);

    return stdx::error{};
  }

  // Reports the actual state: opening is asynchronous on Android, so an
  // optimistic flag claimed "open" while a send still failed.
  bool is_port_open() const noexcept
  {
    return midi_input_port.load(std::memory_order_acquire) != nullptr;
  }

  // Clears the requested state when no callback will ever arrive.
  static void open_failed(midi_out* self) noexcept { self->port_requested = false; }

  stdx::error send_message(const unsigned char* message, size_t size) override
  {
    // Hold port_mutex across the load *and* the send: close_port() takes the
    // same lock before closing, so the handle cannot be closed underneath a
    // send that is already running.
    ssize_t result = 0;
    {
      std::lock_guard<std::mutex> lock(port_mutex);
      AMidiInputPort* const port = midi_input_port.load(std::memory_order_acquire);
      if (!port)
      {
        libremidi_handle_error(configuration, "midi_out::send_message: port is not open");
        return std::errc::io_error;
      }
      result = AMidiInputPort_send(port, message, size);
    }

    if (result < 0)
    {
      libremidi_handle_error(configuration, "midi_out::send_message: failed to send MIDI data");
      return std::errc::io_error;
    }
    return stdx::error{};
  }

  // Takes ownership of `device` and of the global reference `jdevice`.
  static void open_callback(midi_out* self, AMidiDevice* device, jobject jdevice, int port_index)
  {
    AMidiInputPort* port = nullptr;
    if (AMidiInputPort_open(device, port_index, &port) != AMEDIA_OK)
    {
      LOGE("AMidiInputPort_open failed for port %d", port_index);
      // See midi_in: close_port() bails out early, so release the device here
      // rather than leaking it.
      context::close_device(device, jdevice);
      open_failed(self);
      return;
    }
    self->java_device.store(jdevice, std::memory_order_release);
    self->send_device.store(device, std::memory_order_release);
    self->midi_input_port.store(port, std::memory_order_release);
  }

private:
  libremidi::output_configuration configuration;
  // See midi_in: written by the Java open callback, cleared by the caller in
  // close_port(), and read by send_message() and is_port_open() from whichever
  // thread the application uses.
  // Serialises sending against closing; see send_message() and close_port().
  mutable std::mutex port_mutex;
  std::atomic<AMidiDevice*> send_device{nullptr};
  std::atomic<jobject> java_device{nullptr};  // global ref to the MidiDevice
  std::atomic<AMidiInputPort*> midi_input_port{nullptr};
  std::atomic<bool> port_requested{false};
};
}
}
