#pragma once
#include <libremidi/backends/coremidi/config.hpp>
#include <libremidi/backends/coremidi/helpers.hpp>
#include <libremidi/detail/midi_in.hpp>
#include <libremidi/detail/midi_stream_decoder.hpp>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>

NAMESPACE_LIBREMIDI
{
namespace coremidi_callback_registry
{
// Patched copy of libremidi/backends/coremidi/midi_in.hpp.
//
// Upstream hands `this` to MIDIInputPortCreate/MIDIDestinationCreate and
// dereferences that raw pointer at the top of midiInputCallback. CoreMIDI does
// not promise that a read proc already running has returned when
// MIDIPortDispose() comes back, so destroying a midi_in_core while a packet is
// being delivered dereferences a dead object — before any guard the wrapper
// above could install, because the wrapper's callback is the *last* step of
// that chain, not the first.
//
// ThreadSanitizer reproduces this reliably: closing an input under dense
// traffic reported races in ~midi_in_core against the read proc.
//
// The fix is the same shape as the Android asynchronous-open fix: CoreMIDI is
// handed an opaque token instead of a pointer. The token resolves under a lock
// and stays pinned for the duration of the delivery, and unregistering waits
// for a delivery that is already running. A callback whose object has gone
// resolves to nothing and returns.
struct entry
{
  void* target;
  int in_flight;
};

inline std::mutex& mutex()
{
  static std::mutex m;
  return m;
}

inline std::condition_variable& cv()
{
  static std::condition_variable c;
  return c;
}

inline std::map<std::uintptr_t, entry>& entries()
{
  static std::map<std::uintptr_t, entry> m;
  return m;
}

inline std::uintptr_t register_target(void* target)
{
  static std::uintptr_t next = 1;
  std::lock_guard<std::mutex> lock(mutex());
  const std::uintptr_t token = next++;
  entries()[token] = entry{target, 0};
  return token;
}

// Removes every token for `target` and waits until no delivery is still using
// it. After this returns the object can be destroyed safely.
inline void unregister_target(void* target)
{
  std::unique_lock<std::mutex> lock(mutex());
  for (;;)
  {
    bool busy = false;
    auto& map = entries();
    for (auto it = map.begin(); it != map.end();)
    {
      if (it->second.target != target)
        ++it;
      else if (it->second.in_flight > 0)
      {
        busy = true;
        ++it;
      }
      else
        it = map.erase(it);
    }
    if (!busy)
      return;
    cv().wait(lock);
  }
}

// Pins the token and returns its target, or nullptr if it is already gone.
inline void* acquire(std::uintptr_t token)
{
  std::lock_guard<std::mutex> lock(mutex());
  auto& map = entries();
  auto it = map.find(token);
  if (it == map.end())
    return nullptr;
  ++it->second.in_flight;
  return it->second.target;
}

inline void release(std::uintptr_t token)
{
  {
    std::lock_guard<std::mutex> lock(mutex());
    auto& map = entries();
    auto it = map.find(token);
    if (it != map.end() && it->second.in_flight > 0)
      --it->second.in_flight;
  }
  cv().notify_all();
}
}

class midi_in_core final
    : public midi1::in_api
    , public coremidi_data
    , public error_handler
{
public:
  using midi_api::client_open_;
  struct
      : input_configuration
      , coremidi_input_configuration
  {
  } configuration;

  std::uintptr_t m_callback_token = 0;

  midi_in_core(input_configuration&& conf, coremidi_input_configuration&& apiconf)
      : configuration{std::move(conf), std::move(apiconf)}
  {
    if (auto result = init_client(configuration); result != noErr)
    {
      libremidi_handle_error(
          this->configuration, "error creating MIDI client object: " + std::to_string(result));
      client_open_ = from_osstatus(result);
      return;
    }

    client_open_ = stdx::error{};
  }

  ~midi_in_core() override
  {
    // Before anything is torn down: stop CoreMIDI from reaching this object and
    // wait for a delivery that is already inside the callback.
    coremidi_callback_registry::unregister_target(this);

    // Close a connection if it exists.
    midi_in_core::close_port();

    if (this->endpoint)
      MIDIEndpointDispose(this->endpoint);

    close_client(*this);
  }

  libremidi::API get_current_api() const noexcept override { return libremidi::API::COREMIDI; }

  // One token per object, created on first use and valid until the destructor
  // unregisters it.
  std::uintptr_t callback_token()
  {
    if (m_callback_token == 0)
      m_callback_token = coremidi_callback_registry::register_target(this);
    return m_callback_token;
  }

  stdx::error open_port(const input_port& info, std::string_view portName) override
  {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);

    auto source = locate_object(*this, info, kMIDIObjectType_Source);
    if (source == 0)
      return std::errc::invalid_argument;

    // Create our local sink
    MIDIPortRef port;
    OSStatus result = MIDIInputPortCreate(
        this->client, toCFString(portName).get(), midiInputCallback,
        (void*)callback_token(), &port);

    if (result != noErr)
    {
      close_client(*this);
      libremidi_handle_error(
          this->configuration, "error creating macOS MIDI input port: " + std::to_string(result));
      return from_osstatus(result);
    }

    // Make the connection.
    if (result = MIDIPortConnectSource(port, source, nullptr); result != noErr)
    {
      MIDIPortDispose(port);
      close_client(*this);
      libremidi_handle_error(this->configuration, "error connecting macOS MIDI input port.");
      return from_osstatus(result);
    }

    // Save our api-specific port information.
    this->port = port;
    return stdx::error{};
  }

  stdx::error open_virtual_port(std::string_view portName) override
  {
    // Create a virtual MIDI input destination.
    MIDIEndpointRef endpoint;
    OSStatus result = MIDIDestinationCreate(
        this->client, toCFString(portName).get(), midiInputCallback,
        (void*)callback_token(), &endpoint);

    if (result != noErr)
    {
      libremidi_handle_error(
          this->configuration,
          "error creating virtual macOS MIDI "
          "destination.");
      return from_osstatus(result);
    }

    // Save our api-specific connection information.
    this->endpoint = endpoint;
    return stdx::error{};
  }

  stdx::error close_port() override { return coremidi_data::close_port(); }

  timestamp absolute_timestamp() const noexcept override
  {
    return coremidi_data::time_in_nanos(LIBREMIDI_AUDIO_GET_CURRENT_HOST_TIME());
  }

  static void midiInputCallback(const MIDIPacketList* list, void* procRef, void* /*srcRef*/)
  {
    static constexpr timestamp_backend_info timestamp_info{
        .has_absolute_timestamps = true,
        .absolute_is_monotonic = false,
        .has_samples = false,
    };

    const auto token = reinterpret_cast<std::uintptr_t>(procRef);
    auto* const target
        = static_cast<midi_in_core*>(coremidi_callback_registry::acquire(token));
    if (!target)
      return;  // closed while this packet was on its way
    struct release_guard
    {
      std::uintptr_t token;
      ~release_guard() { coremidi_callback_registry::release(token); }
    } guard{token};

    auto& self = *target;

    const MIDIPacket* packet = &list->packet[0];
    for (unsigned int i = 0; i < list->numPackets; ++i)
    {
      // My interpretation of the CoreMIDI documentation: all message
      // types, except sysex, are complete within a packet and there may
      // be several of them in a single packet.  Sysex messages can be
      // broken across multiple packets and PacketLists but are bundled
      // alone within each packet (these packets do not contain other
      // message types).  If sysex messages are split across multiple
      // MIDIPacketLists, they must be handled by multiple calls to this
      // function.

      if (packet->length > 0)
      {
        auto to_ns = [packet] { return time_in_nanos(packet->timeStamp); };
        self.m_processing.on_bytes_multi(
            {packet->data, packet->data + packet->length},
            self.m_processing.timestamp<timestamp_info>(to_ns, 0));
      }

      packet = MIDIPacketNext(packet);
    }
  }

  midi1::input_state_machine m_processing{this->configuration};
};
}
