#ifndef LIBREMIDI_FLUTTER_H
#define LIBREMIDI_FLUTTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if _WIN32
#define FFI_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FFI_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Error codes
// =============================================================================

#define LRM_OK              0
#define LRM_ERR_INVALID     -1
#define LRM_ERR_NOT_FOUND   -2
#define LRM_ERR_OPEN_FAILED -3
#define LRM_ERR_SEND_FAILED -4
#define LRM_ERR_INIT_FAILED -5

// =============================================================================
// Opaque handle types
// =============================================================================

typedef struct LrmObserver LrmObserver;
typedef struct LrmMidiIn LrmMidiIn;
typedef struct LrmMidiOut LrmMidiOut;

// =============================================================================
// Port information
// =============================================================================

// Transport types (matching libremidi::transport_type)
#define LRM_TRANSPORT_UNKNOWN   0
#define LRM_TRANSPORT_SOFTWARE  2
#define LRM_TRANSPORT_LOOPBACK  4
#define LRM_TRANSPORT_HARDWARE  8
#define LRM_TRANSPORT_USB       16
#define LRM_TRANSPORT_BLUETOOTH 32
#define LRM_TRANSPORT_PCI       64
#define LRM_TRANSPORT_NETWORK   128

typedef struct LrmPortInfo {
    // Identifiers
    uint64_t stable_id;         // Cross-platform stable ID (survives hotplug/reorder)
    uint64_t port_id;           // Unique port ID (CoreMIDI: kMIDIPropertyUniqueID)
    uint64_t client_handle;     // API client handle
    int32_t index;              // Index in enumeration (may change on hotplug)

    // Names
    char display_name[256];     // Full display name (e.g. "IAC Driver Bus 1")
    char port_name[256];        // Port name (e.g. "Bus 1")
    char device_name[256];      // Device/model name (e.g. "IAC Driver")
    char manufacturer[256];     // Manufacturer name
    char product[256];          // Product name
    char serial[128];           // Serial number (often empty)

    // Type info
    uint8_t transport_type;     // Transport type (software, usb, bluetooth, etc.)
    bool is_input;              // true = input, false = output
    bool is_virtual;            // true if virtual/software port
} LrmPortInfo;

// =============================================================================
// Callback types
// =============================================================================

// Called when MIDI message is received
typedef void (*LrmMidiCallback)(
    void* context,
    const uint8_t* data,
    size_t length,
    int64_t timestamp
);

// Called when MIDI device is added or removed
// event_type: 0 = input_added, 1 = input_removed, 2 = output_added, 3 = output_removed
typedef void (*LrmHotplugCallback)(
    void* context,
    int32_t event_type
);

// =============================================================================
// Library info
// =============================================================================

// Get library version string
FFI_PLUGIN_EXPORT const char* lrm_get_version(void);

// =============================================================================
// Observer API - Enumerate MIDI ports
// =============================================================================

// Create a new observer for enumerating MIDI ports
FFI_PLUGIN_EXPORT LrmObserver* lrm_observer_new(void);

// Create a new observer with hotplug callback
FFI_PLUGIN_EXPORT LrmObserver* lrm_observer_new_with_callbacks(
    LrmHotplugCallback callback,
    void* context
);

// Free the observer
FFI_PLUGIN_EXPORT void lrm_observer_free(LrmObserver* observer);

// Refresh port list
FFI_PLUGIN_EXPORT void lrm_observer_refresh(LrmObserver* observer);

// Get count of available input ports
FFI_PLUGIN_EXPORT int32_t lrm_observer_get_input_count(LrmObserver* observer);

// Get count of available output ports
FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output_count(LrmObserver* observer);

// Get input port info by index (returns 0 on success, fills info struct)
FFI_PLUGIN_EXPORT int32_t lrm_observer_get_input(LrmObserver* observer, int32_t index, LrmPortInfo* info);

// Get output port info by index (returns 0 on success, fills info struct)
FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output(LrmObserver* observer, int32_t index, LrmPortInfo* info);

// =============================================================================
// MIDI Output API
// =============================================================================

// Open a MIDI output port by index
FFI_PLUGIN_EXPORT LrmMidiOut* lrm_midi_out_open(LrmObserver* observer, int32_t port_index);

// Close and free a MIDI output
FFI_PLUGIN_EXPORT void lrm_midi_out_close(LrmMidiOut* midi_out);

// Check if output is connected
FFI_PLUGIN_EXPORT bool lrm_midi_out_is_connected(LrmMidiOut* midi_out);

// Send a MIDI message
FFI_PLUGIN_EXPORT int32_t lrm_midi_out_send(LrmMidiOut* midi_out, const uint8_t* data, size_t length);

// =============================================================================
// MIDI Input API
// =============================================================================

// Open a MIDI input port by index
// The callback will be called on a background thread when messages arrive
// receive_sysex: if true, SysEx messages (F0..F7) are passed to callback
// receive_timing: if true, MIDI clock messages (F8) are passed to callback
// receive_sensing: if true, active sensing messages (FE) are passed to callback
FFI_PLUGIN_EXPORT LrmMidiIn* lrm_midi_in_open(
    LrmObserver* observer,
    int32_t port_index,
    LrmMidiCallback callback,
    void* context,
    bool receive_sysex,
    bool receive_timing,
    bool receive_sensing
);

// Close and free a MIDI input
FFI_PLUGIN_EXPORT void lrm_midi_in_close(LrmMidiIn* midi_in);

// Check if input is connected
FFI_PLUGIN_EXPORT bool lrm_midi_in_is_connected(LrmMidiIn* midi_in);

#ifdef __cplusplus
}
#endif

#endif // LIBREMIDI_FLUTTER_H