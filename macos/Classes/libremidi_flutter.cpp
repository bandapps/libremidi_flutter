#include "libremidi_flutter.h"

// Enable header-only mode and CoreMIDI backend
#define LIBREMIDI_HEADER_ONLY 1

#if defined(__APPLE__)
  #define LIBREMIDI_COREMIDI 1
#elif defined(_WIN32)
  #define LIBREMIDI_WINMM 1
#elif defined(__linux__)
  #define LIBREMIDI_ALSA 1
#endif

#include <libremidi/libremidi.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dispatch/dispatch.h>
#endif

// =============================================================================
// Internal structures using Generic API
// =============================================================================

// Event types for hotplug callback
#define LRM_EVENT_INPUT_ADDED    0
#define LRM_EVENT_INPUT_REMOVED  1
#define LRM_EVENT_OUTPUT_ADDED   2
#define LRM_EVENT_OUTPUT_REMOVED 3

// Forward declaration
struct LrmObserver;

#if defined(__APPLE__)
// We use MIDIClientCreateWithBlock because it delivers notifications via dispatch queue
// instead of CFRunLoop. This is the same approach used by FlutterMidiCommand and works
// properly with Flutter's event loop.
static void handleMIDINotification(LrmObserver* obs, const MIDINotification* notification);
#endif

struct LrmObserver {
    std::unique_ptr<libremidi::observer> observer;
    std::vector<libremidi::input_port> input_ports;
    std::vector<libremidi::output_port> output_ports;
    LrmHotplugCallback hotplug_callback;
    void* hotplug_context;

#if defined(__APPLE__)
    MIDIClientRef midiClient;
#endif

    LrmObserver(LrmHotplugCallback callback = nullptr, void* context = nullptr)
        : hotplug_callback(callback), hotplug_context(context)
#if defined(__APPLE__)
        , midiClient(0)
#endif
    {
        printf("[libremidi] Creating observer, callback=%p\n", (void*)callback);

#if defined(__APPLE__)
        if (hotplug_callback) {
            // Create our own MIDI client using MIDIClientCreateWithBlock
            // This delivers notifications via dispatch queue (works with Flutter)
            // instead of CFRunLoop (which doesn't work with Flutter)
            OSStatus status = MIDIClientCreateWithBlock(
                CFSTR("libremidi_flutter"),
                &midiClient,
                ^(const MIDINotification* notification) {
                    handleMIDINotification(this, notification);
                }
            );

            if (status != noErr) {
                printf("[libremidi] MIDIClientCreateWithBlock failed: %d\n", (int)status);
            } else {
                printf("[libremidi] MIDIClientCreateWithBlock succeeded, client=%u\n", (unsigned)midiClient);
            }
        }
#endif

        // Create libremidi observer WITHOUT callbacks (we handle hotplug ourselves on macOS)
        libremidi::observer_configuration config;
        config.track_hardware = true;
        config.track_virtual = true;
        config.notify_in_constructor = false;

        // Don't set callbacks - we use our own MIDIClient for notifications on macOS
        observer = std::make_unique<libremidi::observer>(std::move(config));
        printf("[libremidi] Observer created successfully\n");
        refreshInternal();
        printf("[libremidi] Found %zu inputs, %zu outputs\n", input_ports.size(), output_ports.size());
    }

    ~LrmObserver() {
        // Prevent late callbacks during/after dispose
        hotplug_callback = nullptr;
#if defined(__APPLE__)
        if (midiClient) {
            MIDIClientDispose(midiClient);
            midiClient = 0;
        }
#endif
    }

    void refreshInternal() {
        if (observer) {
            input_ports = observer->get_input_ports();
            output_ports = observer->get_output_ports();
        }
    }

    void refresh() {
        refreshInternal();
    }

    void notifyHotplug(int eventType) {
        if (hotplug_callback) {
            hotplug_callback(hotplug_context, eventType);
        }
    }
};

#if defined(__APPLE__)
static void handleMIDINotification(LrmObserver* obs, const MIDINotification* notification) {
    printf("[libremidi] MIDI notification: messageID=%d\n", (int)notification->messageID);

    switch (notification->messageID) {
        case kMIDIMsgObjectAdded: {
            const MIDIObjectAddRemoveNotification* addRemove =
                (const MIDIObjectAddRemoveNotification*)notification;

            obs->refreshInternal();

            if (addRemove->childType == kMIDIObjectType_Source) {
                printf("[libremidi] Input added\n");
                obs->notifyHotplug(LRM_EVENT_INPUT_ADDED);
            } else if (addRemove->childType == kMIDIObjectType_Destination) {
                printf("[libremidi] Output added\n");
                obs->notifyHotplug(LRM_EVENT_OUTPUT_ADDED);
            }
            break;
        }
        case kMIDIMsgObjectRemoved: {
            const MIDIObjectAddRemoveNotification* addRemove =
                (const MIDIObjectAddRemoveNotification*)notification;

            obs->refreshInternal();

            if (addRemove->childType == kMIDIObjectType_Source) {
                printf("[libremidi] Input removed\n");
                obs->notifyHotplug(LRM_EVENT_INPUT_REMOVED);
            } else if (addRemove->childType == kMIDIObjectType_Destination) {
                printf("[libremidi] Output removed\n");
                obs->notifyHotplug(LRM_EVENT_OUTPUT_REMOVED);
            }
            break;
        }
        case kMIDIMsgSetupChanged:
            // macOS/iOS may send SetupChanged instead of ObjectAdded/Removed.
            // Notify both so Dart UI refreshes the device list.
            printf("[libremidi] MIDI setup changed\n");
            obs->refreshInternal();
            obs->notifyHotplug(LRM_EVENT_INPUT_ADDED);
            obs->notifyHotplug(LRM_EVENT_OUTPUT_ADDED);
            break;
        default:
            break;
    }
}
#endif

struct LrmMidiIn {
    std::unique_ptr<libremidi::midi_in> midi_in;
    LrmMidiCallback callback;
    void* context;

    LrmMidiIn(libremidi::input_port& port, LrmMidiCallback cb, void* ctx,
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

        midi_in = std::make_unique<libremidi::midi_in>(std::move(config));
        midi_in->open_port(port);
    }
};

struct LrmMidiOut {
    std::unique_ptr<libremidi::midi_out> midi_out;

    LrmMidiOut(libremidi::output_port& port) {
        midi_out = std::make_unique<libremidi::midi_out>();
        midi_out->open_port(port);
    }
};

// =============================================================================
// Library info
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT const char* lrm_get_version(void) {
    return "0.0.1";
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
    return static_cast<int32_t>(observer->input_ports.size());
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output_count(LrmObserver* observer) {
    if (!observer) return 0;
    return static_cast<int32_t>(observer->output_ports.size());
}

// Helper to copy string safely
static void safe_strcpy(char* dest, size_t dest_size, const std::string& src) {
    std::strncpy(dest, src.c_str(), dest_size - 1);
    dest[dest_size - 1] = '\0';
}

// FNV-1a 64-bit hash for stable_id generation
static uint64_t fnv1a_hash(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
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
    info->port_id = static_cast<uint64_t>(port.port);
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
    if (index < 0 || index >= static_cast<int32_t>(observer->input_ports.size())) {
        return LRM_ERR_NOT_FOUND;
    }

    fill_port_info(observer->input_ports[index], index, true, info);
    return LRM_OK;
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output(LrmObserver* observer, int32_t index, LrmPortInfo* info) {
    if (!observer || !info) return LRM_ERR_INVALID;
    if (index < 0 || index >= static_cast<int32_t>(observer->output_ports.size())) {
        return LRM_ERR_NOT_FOUND;
    }

    fill_port_info(observer->output_ports[index], index, false, info);
    return LRM_OK;
}

// =============================================================================
// MIDI Output API
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT LrmMidiOut* lrm_midi_out_open(LrmObserver* observer, int32_t port_index) {
    if (!observer) return nullptr;
    if (port_index < 0 || port_index >= static_cast<int32_t>(observer->output_ports.size())) {
        return nullptr;
    }

    try {
        return new LrmMidiOut(observer->output_ports[port_index]);
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
    if (port_index < 0 || port_index >= static_cast<int32_t>(observer->input_ports.size())) {
        return nullptr;
    }

    try {
        return new LrmMidiIn(observer->input_ports[port_index], callback, context,
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
