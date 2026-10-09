// Android backend support: the bridge to dev.celtera.libremidi.MidiBridge.
//
// Replaces upstream libremidi's backends/android/helpers.hpp (this directory
// comes first on the include path). Java owns device management — listing,
// opening, closing, hotplug — and this layer only exposes it to the backend
// classes in C++ terms. Implemented in src/android_bridge.cpp.
//
// Ownership rules:
//   - A device is open while at least one device_lease for it is alive. Inputs
//     and outputs of the same device share one lease, i.e. one MidiDevice:
//     MidiService accepts only one connection while it is still binding a
//     virtual or Bluetooth device, and an AMidiDevice can be created from a
//     MidiDevice only once.
//   - The last lease to go closes the device on the Java side as well.
//     AMidiDevice_release() alone leaves the connection in MidiService (and a
//     Bluetooth link) up until the process exits.
//   - A device's input port (what the app sends to) can only be opened once, so
//     it is shared the same way, see send_port.
//   - No JNI references or JNIEnv pointers leave android_bridge.cpp.

#pragma once
#include <libremidi/config.hpp>

#include <amidi/AMidi.h>
#include <android/log.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define LOG_TAG "libremidi"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

NAMESPACE_LIBREMIDI
{
namespace android
{
struct port_desc
{
  int32_t device_id;  // MidiDeviceInfo.getId(): stable while the device stays connected
  int32_t port_index;
  int32_t type;       // MidiDeviceInfo.TYPE_USB / TYPE_VIRTUAL / TYPE_BLUETOOTH
  std::string name;
  std::string manufacturer;
  std::string product;
  std::string serial;
};

// Lists the ports the app can receive from (`inputs`) or send to. Empty when
// MIDI is unavailable.
std::vector<port_desc> list_ports(bool inputs);

// Packs device id and port index into libremidi's port handle and back.
inline uint64_t make_port_handle(int32_t device_id, int32_t port_index)
{
  return (uint64_t(uint32_t(device_id)) << 32) | uint32_t(port_index);
}
inline int32_t handle_device_id(uint64_t handle) { return int32_t(handle >> 32); }
inline int32_t handle_port_index(uint64_t handle) { return int32_t(handle & 0xffffffffu); }

// An open device. Obtained from open_device(); the device closes when the last
// shared_ptr to it goes away.
class device_lease
{
public:
  virtual ~device_lease() = default;
  virtual AMidiDevice* device() const noexcept = 0;
};

// Opens the device, or shares the lease of an already open one. Blocks until
// the system has opened it. On failure returns null and sets `error`.
std::shared_ptr<device_lease> open_device(int32_t device_id, std::string& error);

// A port the app sends to (an input port of the device). Android lets a
// device's input port be opened only once, so every midi_out for the same
// device and port shares one, and sends through it are serialised. Keeps the
// device open while alive.
class send_port
{
public:
  virtual ~send_port() = default;
  // Returns false if the data could not be sent.
  virtual bool send(const unsigned char* data, size_t size) = 0;
};

// Opens (or shares) the send port. Blocks like open_device(). On failure
// returns null and sets `error`.
std::shared_ptr<send_port>
open_send_port(int32_t device_id, int32_t port_index, std::string& error);

// Whether Android MIDI is usable at all: the native library is bound to
// MidiBridge, the device supports MIDI, and the plugin has initialised it —
// which includes registering the hotplug listener.
bool midi_available();

// Hotplug. The callback runs on the MIDI thread whenever a device was added or
// removed. unregister_hotplug() does not return while that callback is running,
// so the owner may be destroyed right after it.
int register_hotplug(std::function<void()> callback);
void unregister_hotplug(int id);

// Reason for the last failed open on this thread, for lrm_get_last_error().
void set_last_error(std::string message);
const char* last_error() noexcept;
}
}
