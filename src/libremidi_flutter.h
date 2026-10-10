#ifndef LIBREMIDI_FLUTTER_H
#define LIBREMIDI_FLUTTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if _WIN32
#define FFI_PLUGIN_EXPORT __declspec(dllexport)
#else
// `used` is required in addition to default visibility on the Apple platforms.
// SwiftPM links the plugin's objects statically into the runner binary, and
// nothing references these FFI entry points at link time — Dart looks them up
// at runtime via DynamicLibrary.process(). Without `used` (which emits the
// Mach-O .no_dead_strip directive) Xcode's Release dead-code stripping removes
// them and every lookup fails with "Failed to lookup symbol". Debug builds do
// not strip, which is why this only ever showed up in Release.
#define FFI_PLUGIN_EXPORT __attribute__((visibility("default"), used))
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

// Called when MIDI device configuration changes
// event_type: 0 = input_added, 1 = input_removed, 2 = output_added,
//             3 = output_removed, 4 = setup_changed (generic, re-enumerate)
typedef void (*LrmHotplugCallback)(
    void* context,
    int32_t event_type
);

// Dart's Dart_PostCObject, as handed out by NativeApi.postCObject. `message`
// is a Dart_CObject*. Used by the *_with_port functions below.
typedef bool (*LrmPostCObject)(int64_t port, void* message);

// =============================================================================
// Library info
// =============================================================================

// Get library version string
FFI_PLUGIN_EXPORT const char* lrm_get_version(void);

// Why the last lrm_midi_in_open*/lrm_midi_out_open* call on this thread
// returned NULL, or "" if there is no reason to give. Currently only Android
// reports reasons. The string stays valid until the next open on this thread.
FFI_PLUGIN_EXPORT const char* lrm_get_last_error(void);

// Free a heap buffer that the library handed to the caller (e.g. the MIDI
// message data delivered to LrmMidiCallback). Safe to call with NULL.
FFI_PLUGIN_EXPORT void lrm_free(void* ptr);

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

// Create a new observer that posts hotplug events to a Dart native port
// (a ReceivePort's sendPort.nativePort) as an int (the event_type above).
//
// Prefer this over lrm_observer_new_with_callbacks from Dart: a NativeCallable
// that has been closed — because its isolate ended or the app was hot
// restarted without disposing the observer — aborts the process when invoked,
// while posting to a closed port is simply dropped.
FFI_PLUGIN_EXPORT LrmObserver* lrm_observer_new_with_port(
    LrmPostCObject post,
    int64_t port
);

// Free the observer
FFI_PLUGIN_EXPORT void lrm_observer_free(LrmObserver* observer);

// Refresh port list
FFI_PLUGIN_EXPORT void lrm_observer_refresh(LrmObserver* observer);

// Whether this observer can actually deliver hotplug events *right now*.
//
// Returns false for an observer created without a hotplug callback, and — on
// iOS/macOS — also when the CoreMIDI notification client could not be created
// (e.g. kMIDINoConnection when the MIDI server is unreachable). Port
// enumeration still works in that case, so the failure is otherwise invisible:
// the port list is correct and the initial setup_changed event still arrives,
// but no further hotplug notification ever will.
//
// On iOS/macOS this is false until the observer's background initialisation
// has finished, so a caller asking immediately after creating the observer
// gets false. Check it again once the first setup_changed event has arrived:
// that event is emitted right after the notification client is set up, so by
// then the answer is final.
FFI_PLUGIN_EXPORT bool lrm_observer_hotplug_available(LrmObserver* observer);

// Get count of available input ports
FFI_PLUGIN_EXPORT int32_t lrm_observer_get_input_count(LrmObserver* observer);

// Get count of available output ports
FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output_count(LrmObserver* observer);

// Get input port info by index (returns 0 on success, fills info struct)
FFI_PLUGIN_EXPORT int32_t lrm_observer_get_input(LrmObserver* observer, int32_t index, LrmPortInfo* info);

// Get output port info by index (returns 0 on success, fills info struct)
FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output(LrmObserver* observer, int32_t index, LrmPortInfo* info);

// Find the closest matching port using heuristic fuzzy matching.
// Takes a saved LrmPortInfo and searches current ports for the best match.
// Returns the port index (>= 0) on success, or -1 if no match found.
FFI_PLUGIN_EXPORT int32_t lrm_find_closest_port(
    LrmObserver* observer, const LrmPortInfo* saved, bool is_input);

// =============================================================================
// MIDI Output API
// =============================================================================

// Open a MIDI output port by index
FFI_PLUGIN_EXPORT LrmMidiOut* lrm_midi_out_open(LrmObserver* observer, int32_t port_index);

// Open a MIDI output port by port_id (stable across hotplug)
FFI_PLUGIN_EXPORT LrmMidiOut* lrm_midi_out_open_by_id(LrmObserver* observer, uint64_t port_id);

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

// Open a MIDI input port by port_id (stable across hotplug)
FFI_PLUGIN_EXPORT LrmMidiIn* lrm_midi_in_open_by_id(
    LrmObserver* observer,
    uint64_t port_id,
    LrmMidiCallback callback,
    void* context,
    bool receive_sysex,
    bool receive_timing,
    bool receive_sensing
);

// Open a MIDI input port by port_id, posting each message to a Dart native
// port as a two-element array: [Uint8List bytes, int timestamp]. The bytes are
// copied into the Dart heap by the post, so nothing has to be freed. See
// lrm_observer_new_with_port for why this is preferable to a callback.
FFI_PLUGIN_EXPORT LrmMidiIn* lrm_midi_in_open_by_id_with_port(
    LrmObserver* observer,
    uint64_t port_id,
    LrmPostCObject post,
    int64_t port,
    bool receive_sysex,
    bool receive_timing,
    bool receive_sensing
);

// Close and free a MIDI input
FFI_PLUGIN_EXPORT void lrm_midi_in_close(LrmMidiIn* midi_in);

// -----------------------------------------------------------------------------
// Asynchronous open
// -----------------------------------------------------------------------------
//
// The *_async functions resolve the port immediately (NULL if it is unknown,
// see lrm_get_last_error) and open it on a native thread, which can take a
// while on Android. When the open has finished, a null message is posted to
// done_port. The result is held by the returned ticket: take it with
// lrm_open_ticket_take, read a failure reason with lrm_open_ticket_error, then
// release the ticket.
//
// The ticket has two owners, the opening thread and the caller, and whoever
// releases it last closes a handle nobody took. A caller whose isolate ends
// mid-open therefore only has to release the ticket (e.g. from a
// NativeFinalizer) — the port is closed when the open finishes. The observer
// is not used after the *_async call returns.
typedef struct LrmOpenTicket LrmOpenTicket;

FFI_PLUGIN_EXPORT LrmOpenTicket* lrm_midi_in_open_by_id_async(
    LrmObserver* observer,
    uint64_t port_id,
    LrmPostCObject post,
    int64_t message_port,
    int64_t done_port,
    bool receive_sysex,
    bool receive_timing,
    bool receive_sensing
);

FFI_PLUGIN_EXPORT LrmOpenTicket* lrm_midi_out_open_by_id_async(
    LrmObserver* observer,
    uint64_t port_id,
    LrmPostCObject post,
    int64_t done_port
);

// After the done message: the opened LrmMidiIn* / LrmMidiOut* (ownership
// passes to the caller), or NULL if the open failed or it was already taken.
FFI_PLUGIN_EXPORT void* lrm_open_ticket_take(LrmOpenTicket* ticket);

// After the done message: why the open failed, or "". Valid until release.
FFI_PLUGIN_EXPORT const char* lrm_open_ticket_error(LrmOpenTicket* ticket);

// Gives up the caller's share of the ticket.
FFI_PLUGIN_EXPORT void lrm_open_ticket_release(LrmOpenTicket* ticket);

// -----------------------------------------------------------------------------
// Deferred release, for garbage-collection finalizers
// -----------------------------------------------------------------------------
//
// Same as lrm_midi_in_close / lrm_midi_out_close / lrm_observer_free /
// lrm_open_ticket_release, but they only queue the object and return at once;
// a native cleanup thread does the actual work. Closing can block (joining a
// reader thread, a system call into the MIDI service), which a Dart
// NativeFinalizer callback must not do. Use the immediate functions
// everywhere else.
FFI_PLUGIN_EXPORT void lrm_midi_in_close_deferred(LrmMidiIn* midi_in);
FFI_PLUGIN_EXPORT void lrm_midi_out_close_deferred(LrmMidiOut* midi_out);
FFI_PLUGIN_EXPORT void lrm_observer_free_deferred(LrmObserver* observer);
FFI_PLUGIN_EXPORT void lrm_open_ticket_release_deferred(LrmOpenTicket* ticket);

// Check if input is connected
FFI_PLUGIN_EXPORT bool lrm_midi_in_is_connected(LrmMidiIn* midi_in);

#ifdef __cplusplus
}
#endif

#endif // LIBREMIDI_FLUTTER_H
