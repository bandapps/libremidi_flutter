#include "libremidi_flutter.h"

// Library version
#define LRM_VERSION "0.8.4"

// Enable header-only mode
#define LIBREMIDI_HEADER_ONLY 1

// Platform-specific backend selection
#if defined(__APPLE__)
  #define LIBREMIDI_COREMIDI 1
#elif defined(_WIN32)
  #if defined(LIBREMIDI_WINMIDI)
    // LIBREMIDI_WINMIDI already defined by CMake — WinMIDI + WinUWP fallback
  #else
    #define LIBREMIDI_WINUWP 1
  #endif
#elif defined(__ANDROID__)
  #define LIBREMIDI_ANDROID 1
#elif defined(__linux__)
  #define LIBREMIDI_ALSA 1
#endif

#include <libremidi/libremidi.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <algorithm>
#include <string>
#include <vector>

// =============================================================================
// API selection — prefer WinMIDI (MIDI 2.0), fall back to platform default
// =============================================================================

static libremidi::API get_preferred_api() {
#if defined(LIBREMIDI_WINMIDI)
    // WinMIDI checks at construction time whether the Windows MIDI Service
    // is installed, running, and reachable via COM. If all checks pass,
    // winmidi::backend::available() returns true.
    if (libremidi::winmidi::backend::available()) {
        return libremidi::API::WINDOWS_MIDI_SERVICES;
    }
    // Service not present — fall back to WinUWP.
    return libremidi::API::WINDOWS_UWP;
#else
    return libremidi::midi1::default_api();
#endif
}

// =============================================================================
// Internal structures using Generic API
// =============================================================================

static constexpr const char* kInternalClientName = "libremidi_flutter_internal";

// Event types for hotplug callback
#define LRM_EVENT_INPUT_ADDED    0
#define LRM_EVENT_INPUT_REMOVED  1
#define LRM_EVENT_OUTPUT_ADDED   2
#define LRM_EVENT_OUTPUT_REMOVED 3

// FNV-1a 64-bit hash for stable_id / public port id generation
static uint64_t fnv1a_hash(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

template<typename PortType>
static uint64_t public_port_id(const PortType& port) {
    // WinRT and WinMIDI ports use string-based device IDs, not numeric.
    // Hash port_name for a stable uint64_t identifier across the FFI boundary.
    if (port.api == libremidi::API::WINDOWS_UWP
        || port.api == libremidi::API::WINDOWS_MIDI_SERVICES) {
        return fnv1a_hash(port.port_name);
    }

    return static_cast<uint64_t>(port.port);
}

struct LrmObserver {
    std::unique_ptr<libremidi::observer> observer;
    std::vector<libremidi::input_port> input_ports;
    std::vector<libremidi::output_port> output_ports;
    LrmHotplugCallback hotplug_callback;
    void* hotplug_context;
    mutable std::mutex ports_mutex;  // Thread safety for port vectors

    LrmObserver(LrmHotplugCallback callback = nullptr, void* context = nullptr)
        : hotplug_callback(callback), hotplug_context(context)
    {
        libremidi::observer_configuration config;
        config.track_hardware = true;
        config.track_virtual = true;
        // Must be true so m_knownClients is populated for unregister_port() to work
        config.notify_in_constructor = true;

        if (hotplug_callback) {
            config.input_added = [this](const libremidi::input_port&) {
                refreshInternal();
                notifyHotplug(LRM_EVENT_INPUT_ADDED);
            };
            config.input_removed = [this](const libremidi::input_port&) {
                refreshInternal();
                notifyHotplug(LRM_EVENT_INPUT_REMOVED);
            };
            config.output_added = [this](const libremidi::output_port&) {
                refreshInternal();
                notifyHotplug(LRM_EVENT_OUTPUT_ADDED);
            };
            config.output_removed = [this](const libremidi::output_port&) {
                refreshInternal();
                notifyHotplug(LRM_EVENT_OUTPUT_REMOVED);
            };
        }

        // WinMIDI if available, otherwise platform default (WinUWP, CoreMIDI, ALSA...)
        auto api = get_preferred_api();
        auto api_conf = libremidi::observer_configuration_for(api);
        libremidi::set_client_name(api_conf, kInternalClientName);
        observer = std::make_unique<libremidi::observer>(
            std::move(config),
            std::move(api_conf)
        );
        refreshInternal();
    }

    ~LrmObserver() = default;

    template <typename PortType>
    static bool isInternalObserverPort(const PortType& port) {
        if (port.type != libremidi::transport_type::software &&
            port.type != libremidi::transport_type::loopback) {
            return false;
        }

        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        };

        const auto device = lower(port.device_name);
        const auto display = lower(port.display_name);
        const auto port_name = lower(port.port_name);

        // Hide only our own libremidi infrastructure from the public device
        // list. The ALSA observer port can surface separately under its port
        // name, while backends with client_name support use our stable name.
        return device == kInternalClientName ||
               display == "libremidi-observe" ||
               port_name == "libremidi-observe";
    }

    void refreshInternal() {
        if (observer) {
            auto new_inputs = observer->get_input_ports();
            auto new_outputs = observer->get_output_ports();

            new_inputs.erase(
                std::remove_if(
                    new_inputs.begin(),
                    new_inputs.end(),
                    [](const auto& port) { return isInternalObserverPort(port); }),
                new_inputs.end());
            new_outputs.erase(
                std::remove_if(
                    new_outputs.begin(),
                    new_outputs.end(),
                    [](const auto& port) { return isInternalObserverPort(port); }),
                new_outputs.end());

            std::lock_guard<std::mutex> lock(ports_mutex);
            input_ports = std::move(new_inputs);
            output_ports = std::move(new_outputs);
        }
    }

    void refresh() {
        refreshInternal();
    }

    size_t getInputCount() const {
        std::lock_guard<std::mutex> lock(ports_mutex);
        return input_ports.size();
    }

    size_t getOutputCount() const {
        std::lock_guard<std::mutex> lock(ports_mutex);
        return output_ports.size();
    }

    bool getInputPort(size_t index, libremidi::input_port& port) const {
        std::lock_guard<std::mutex> lock(ports_mutex);
        if (index >= input_ports.size()) return false;
        port = input_ports[index];
        return true;
    }

    bool getOutputPort(size_t index, libremidi::output_port& port) const {
        std::lock_guard<std::mutex> lock(ports_mutex);
        if (index >= output_ports.size()) return false;
        port = output_ports[index];
        return true;
    }

    bool getInputPortById(uint64_t port_id, libremidi::input_port& port) const {
        std::lock_guard<std::mutex> lock(ports_mutex);
        for (const auto& p : input_ports) {
            if (public_port_id(p) == port_id) {
                port = p;
                return true;
            }
        }
        return false;
    }

    bool getOutputPortById(uint64_t port_id, libremidi::output_port& port) const {
        std::lock_guard<std::mutex> lock(ports_mutex);
        for (const auto& p : output_ports) {
            if (public_port_id(p) == port_id) {
                port = p;
                return true;
            }
        }
        return false;
    }

    void notifyHotplug(int eventType) {
        if (hotplug_callback) {
            hotplug_callback(hotplug_context, eventType);
        }
    }
};


struct LrmMidiIn {
    std::unique_ptr<libremidi::midi_in> midi_in;
    LrmMidiCallback callback;
    void* context;

    LrmMidiIn(libremidi::input_port port, LrmMidiCallback cb, void* ctx,
              bool receive_sysex, bool receive_timing, bool receive_sensing)
        : callback(cb), context(ctx) {

        libremidi::input_configuration config;
        config.ignore_sysex = !receive_sysex;
        config.ignore_timing = !receive_timing;
        config.ignore_sensing = !receive_sensing;
        config.on_message = [this](const libremidi::message& msg) {
            if (callback) {
                callback(context, msg.bytes.data(), msg.bytes.size(), msg.timestamp);
            }
        };

        // Use the API that enumerated this port. For MIDI 2 backends
        // (WinMIDI), libremidi wraps the MIDI 1 config with UMP conversion.
        auto api = port.api;
        auto api_conf = libremidi::midi_in_configuration_for(api);
        libremidi::set_client_name(api_conf, kInternalClientName);
        midi_in = std::make_unique<libremidi::midi_in>(
            std::move(config),
            std::move(api_conf)
        );
        midi_in->open_port(port);
    }
};

struct LrmMidiOut {
    std::unique_ptr<libremidi::midi_out> midi_out;

    LrmMidiOut(libremidi::output_port port) {
        // Use the API that enumerated this port. For MIDI 2 backends
        // (WinMIDI), libremidi converts MIDI 1 send_message() to UMP.
        auto api = port.api;
        auto api_conf = libremidi::midi_out_configuration_for(api);
        libremidi::set_client_name(api_conf, kInternalClientName);
        midi_out = std::make_unique<libremidi::midi_out>(
            libremidi::output_configuration{},
            std::move(api_conf)
        );
        midi_out->open_port(port);
    }
};

// =============================================================================
// Library info
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT const char* lrm_get_version(void) {
    return LRM_VERSION;
}

// =============================================================================
// Observer API
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT LrmObserver* lrm_observer_new(void) {
    try {
        return new LrmObserver();
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT LrmObserver* lrm_observer_new_with_callbacks(
    LrmHotplugCallback callback,
    void* context
) {
    try {
        return new LrmObserver(callback, context);
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT void lrm_observer_free(LrmObserver* observer) {
    delete observer;
}

extern "C" FFI_PLUGIN_EXPORT void lrm_observer_refresh(LrmObserver* observer) {
    if (!observer) return;
    observer->refresh();
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_input_count(LrmObserver* observer) {
    if (!observer) return 0;
    return static_cast<int32_t>(observer->getInputCount());
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output_count(LrmObserver* observer) {
    if (!observer) return 0;
    return static_cast<int32_t>(observer->getOutputCount());
}

// Helper to copy string safely
static void safe_strcpy(char* dest, size_t dest_size, const std::string& src) {
    std::strncpy(dest, src.c_str(), dest_size - 1);
    dest[dest_size - 1] = '\0';
}

// Generate stable port key for cross-platform identification
template<typename PortType>
static std::string port_key(const PortType& port) {
    return port.port_name + "|" + port.manufacturer + "|" + port.product + "|" + port.serial;
}

// Helper to fill port info from libremidi port
template<typename PortType>
static void fill_port_info(const PortType& port, int32_t index, bool is_input, LrmPortInfo* info) {
    // Clear struct
    std::memset(info, 0, sizeof(LrmPortInfo));

    // Identifiers
    info->port_id = public_port_id(port);
    info->client_handle = static_cast<uint64_t>(port.client);
    info->index = index;

    // Generate stable_id from port key hash
    // On macOS/iOS port.port (MIDIEndpointRef) is already stable, but we use hash for consistency
    info->stable_id = fnv1a_hash(port_key(port));

    // Names
    safe_strcpy(info->display_name, sizeof(info->display_name), port.display_name);
    safe_strcpy(info->port_name, sizeof(info->port_name), port.port_name);
    safe_strcpy(info->device_name, sizeof(info->device_name), port.device_name);
    safe_strcpy(info->manufacturer, sizeof(info->manufacturer), port.manufacturer);
    safe_strcpy(info->product, sizeof(info->product), port.product);
    safe_strcpy(info->serial, sizeof(info->serial), port.serial);

    // Type info
    info->transport_type = static_cast<uint8_t>(port.type);
    info->is_input = is_input;
    info->is_virtual = (port.type == libremidi::transport_type::software ||
                        port.type == libremidi::transport_type::loopback);
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_input(LrmObserver* observer, int32_t index, LrmPortInfo* info) {
    if (!observer || !info) return LRM_ERR_INVALID;
    if (index < 0) return LRM_ERR_NOT_FOUND;

    libremidi::input_port port;
    if (!observer->getInputPort(static_cast<size_t>(index), port)) {
        return LRM_ERR_NOT_FOUND;
    }

    fill_port_info(port, index, true, info);
    return LRM_OK;
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output(LrmObserver* observer, int32_t index, LrmPortInfo* info) {
    if (!observer || !info) return LRM_ERR_INVALID;
    if (index < 0) return LRM_ERR_NOT_FOUND;

    libremidi::output_port port;
    if (!observer->getOutputPort(static_cast<size_t>(index), port)) {
        return LRM_ERR_NOT_FOUND;
    }

    fill_port_info(port, index, false, info);
    return LRM_OK;
}

// =============================================================================
// MIDI Output API
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT LrmMidiOut* lrm_midi_out_open(LrmObserver* observer, int32_t port_index) {
    if (!observer) return nullptr;
    if (port_index < 0) return nullptr;

    libremidi::output_port port;
    if (!observer->getOutputPort(static_cast<size_t>(port_index), port)) {
        return nullptr;
    }

    try {
        return new LrmMidiOut(port);
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT LrmMidiOut* lrm_midi_out_open_by_id(LrmObserver* observer, uint64_t port_id) {
    if (!observer) return nullptr;

    libremidi::output_port port;
    if (!observer->getOutputPortById(port_id, port)) {
        return nullptr;
    }

    try {
        return new LrmMidiOut(port);
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT void lrm_midi_out_close(LrmMidiOut* midi_out) {
    delete midi_out;
}

extern "C" FFI_PLUGIN_EXPORT bool lrm_midi_out_is_connected(LrmMidiOut* midi_out) {
    if (!midi_out || !midi_out->midi_out) return false;
    return midi_out->midi_out->is_port_connected();
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_midi_out_send(LrmMidiOut* midi_out, const uint8_t* data, size_t length) {
    if (!midi_out || !midi_out->midi_out || !data) return LRM_ERR_INVALID;

    try {
        midi_out->midi_out->send_message(data, length);
        return LRM_OK;
    } catch (...) {
        return LRM_ERR_SEND_FAILED;
    }
}

// =============================================================================
// MIDI Input API
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT LrmMidiIn* lrm_midi_in_open(
    LrmObserver* observer,
    int32_t port_index,
    LrmMidiCallback callback,
    void* context,
    bool receive_sysex,
    bool receive_timing,
    bool receive_sensing
) {
    if (!observer) return nullptr;
    if (port_index < 0) return nullptr;

    libremidi::input_port port;
    if (!observer->getInputPort(static_cast<size_t>(port_index), port)) {
        return nullptr;
    }

    try {
        return new LrmMidiIn(port, callback, context,
                            receive_sysex, receive_timing, receive_sensing);
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT LrmMidiIn* lrm_midi_in_open_by_id(
    LrmObserver* observer,
    uint64_t port_id,
    LrmMidiCallback callback,
    void* context,
    bool receive_sysex,
    bool receive_timing,
    bool receive_sensing
) {
    if (!observer) return nullptr;

    libremidi::input_port port;
    if (!observer->getInputPortById(port_id, port)) {
        return nullptr;
    }

    try {
        return new LrmMidiIn(port, callback, context,
                            receive_sysex, receive_timing, receive_sensing);
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT void lrm_midi_in_close(LrmMidiIn* midi_in) {
    delete midi_in;
}

extern "C" FFI_PLUGIN_EXPORT bool lrm_midi_in_is_connected(LrmMidiIn* midi_in) {
    if (!midi_in || !midi_in->midi_in) return false;
    return midi_in->midi_in->is_port_connected();
}
