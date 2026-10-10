#include "libremidi_flutter.h"

// Library version
#define LRM_VERSION "0.9.0"

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
#include <libremidi/port_comparison.hpp>

// Guard: the patched cmidi2 copy must be the one that gets included.
//
// src/libremidi/cmidi2.hpp fixes two defects that only MIDI 2.0 backends hit —
// a conversion loop that never terminates on a SysEx of exactly 256 bytes, and
// a SysEx7 reassembly buffer that cannot span calls. It takes effect purely
// through header search order: `src` must come before `third_party`. If that
// order is ever reversed, or libremidi is updated without carrying the patch
// forward, the unpatched upstream header is used and nothing looks wrong — the
// failure first shows up as a frozen app in someone's hands. So fail here,
// loudly, at build time. CMIDI2_SYSEX7_BUFFER_SIZE exists only in the copy.
#include <libremidi/cmidi2.hpp>
#if !defined(CMIDI2_SYSEX7_BUFFER_SIZE)
  #error "Unpatched cmidi2 header in use: <libremidi/cmidi2.hpp> resolved to third_party instead of src/libremidi/cmidi2.hpp. Put src ahead of third_party/libremidi/include in the include path (CMake: target_include_directories in src/CMakeLists.txt; Apple: both podspecs plus the patched_libremidi symlink in each Package.swift). Building against the unpatched header means a 256-byte SysEx hangs the sending thread and received SysEx messages are truncated to their final packet."
#endif

#include <dart_api/dart_native_api.h>
#include <lrm_transport.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <functional>
#include <deque>
#include <condition_variable>
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Where a *_with_port export delivers to; see "Delivery to a Dart native port".
struct DartPortSink {
    LrmPostCObject post;
    int64_t port;
};
static void post_midi_to_port(void* context, const uint8_t* data, size_t length,
                              int64_t timestamp);

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

// FNV-1a 64-bit hash for stable_id / public port id generation.
// Iterates over unsigned char: plain `char` is signed on x86/arm64 but unsigned
// on some ABIs, so a byte >= 0x80 (any non-ASCII port name) would sign-extend
// to a different 64-bit value per platform and break the "cross-platform stable
// id" promise. Must stay identical to the copy in apple/Classes/libremidi_flutter.cpp.
static uint64_t fnv1a_hash(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (unsigned char c : str) {
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

template<typename PortType>
static uint64_t public_port_id(const PortType& port) {
    // Windows MIDI Services: port.port is only the group terminal block number
    // (1-based, repeats across endpoints) and port_name is that block's display
    // name — two identical devices therefore collide, and open_by_id would
    // always hit the first one. The unique part is the endpoint device id
    // (port_information.hpp: "WinMIDI: EndpointDeviceId (std::string)"), and
    // libremidi's own open_port() identifies a port by exactly the pair
    // (device id, port.port) — so hash that pair.
    if (port.api == libremidi::API::WINDOWS_MIDI_SERVICES) {
        const auto suffix = "|" + std::to_string(static_cast<uint64_t>(port.port));
        if (const auto* device_id
                = libremidi_variant_alias::get_if<std::string>(&port.device)) {
            return fnv1a_hash(*device_id + suffix);
        }
        // No endpoint id available — still better than a bare name collision.
        return fnv1a_hash(port.port_name + suffix);
    }

    // WinUWP: port.port is always 0, but port_name carries the unique device id
    // (winuwp/observer.hpp sets .port_name = portId), so hashing it is correct.
    if (port.api == libremidi::API::WINDOWS_UWP) {
        return fnv1a_hash(port.port_name);
    }

    // Android: port.port packs MidiDeviceInfo.getId() with the port index.
    // Android assigns that id per connection and never reuses it, so it tells
    // two identical devices apart — hashing the descriptive strings did not,
    // and opening the second of two identical controllers opened the first.
    // A reconnected device gets a new id; recognising it again is what
    // stableId / toMatchJson / findClosestPort are for. So Android, like the
    // remaining backends, uses port.port as it is.
    return static_cast<uint64_t>(port.port);
}

// Reason recorded by the last failed open on this thread, or "". Only the
// Android backend reports reasons. Valid until the next open on this thread.
static const char* current_open_error() {
#if defined(__ANDROID__)
    return libremidi::android::last_error();
#else
    return "";
#endif
}

// Clears the reason at the start of every open, so a stale reason from an
// earlier failure is never reported for a later one, and records a new one.
struct OpenErrorScope {
    OpenErrorScope() {
#if defined(__ANDROID__)
        libremidi::android::set_last_error({});
#endif
    }
    void set([[maybe_unused]] const char* reason) {
#if defined(__ANDROID__)
        libremidi::android::set_last_error(reason);
#endif
    }
};

struct LrmObserver {
    std::unique_ptr<libremidi::observer> observer;
    std::vector<libremidi::input_port> input_ports;
    std::vector<libremidi::output_port> output_ports;
    // Written by the destructor on the caller's thread, read by the backend's
    // hotplug thread in notifyHotplug — atomic so teardown is not a data race.
    std::atomic<LrmHotplugCallback> hotplug_callback;
    void* hotplug_context;
    // Set by lrm_observer_new_with_port: the context hotplug_callback posts to.
    std::unique_ptr<DartPortSink> port_sink;
    mutable std::mutex ports_mutex;  // Thread safety for port vectors
    // Set by the backend's hotplug callbacks; the lists are rebuilt on the next
    // read, on the reader's thread. The callbacks must not call into
    // `observer` themselves: while ~LrmObserver tears it down, the backend
    // waits for a running callback, and a callback that re-entered the
    // half-destroyed observer crashed (reproduced on Android).
    std::atomic<bool> ports_stale{false};
    // The backend could not set up hotplug (e.g. WinMIDI without its endpoint
    // watcher) and the observer was rebuilt without it. Written in the
    // constructor only.
    bool hotplug_failed = false;

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
                ports_stale.store(true, std::memory_order_release);
                notifyHotplug(LRM_EVENT_INPUT_ADDED);
            };
            config.input_removed = [this](const libremidi::input_port&) {
                ports_stale.store(true, std::memory_order_release);
                notifyHotplug(LRM_EVENT_INPUT_REMOVED);
            };
            config.output_added = [this](const libremidi::output_port&) {
                ports_stale.store(true, std::memory_order_release);
                notifyHotplug(LRM_EVENT_OUTPUT_ADDED);
            };
            config.output_removed = [this](const libremidi::output_port&) {
                ports_stale.store(true, std::memory_order_release);
                notifyHotplug(LRM_EVENT_OUTPUT_REMOVED);
            };
        }

        // WinMIDI if available, otherwise platform default (WinUWP, CoreMIDI, ALSA...)
        auto api = get_preferred_api();
        auto make = [api](libremidi::observer_configuration conf) {
            auto api_conf = libremidi::observer_configuration_for(api);
            libremidi::set_client_name(api_conf, kInternalClientName);
            return std::make_unique<libremidi::observer>(std::move(conf), std::move(api_conf));
        };
        try {
            observer = make(config);
        } catch (...) {
            if (!hotplug_callback) throw;
            // The backend refused hotplug; keep enumeration working without it
            // and report it through lrm_observer_hotplug_available.
            libremidi::observer_configuration plain;
            plain.track_hardware = true;
            plain.track_virtual = true;
            plain.notify_in_constructor = true;
            observer = make(std::move(plain));
            hotplug_failed = true;
        }
        refreshInternal();
    }

    ~LrmObserver() {
        // Destroy the observer first: the backend waits for a hotplug callback
        // that is still running, and those callbacks touch ports_stale and the
        // Dart delivery (hotplug_callback / port_sink) — so those must outlive
        // it. With the defaulted destructor, members went in reverse
        // declaration order, i.e. after everything the callbacks use. The
        // callbacks never call back into `observer`, which unique_ptr::reset()
        // already nulls before the backend's destructor runs.
        hotplug_callback.store(nullptr, std::memory_order_release);
        observer.reset();
    }

    template <typename PortType>
    static bool isInternalObserverPort(const PortType& port) {
        if (!lrm_is_virtual_transport(port.type)) {
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
        ports_stale.store(false, std::memory_order_release);
        refreshInternal();
    }

    void refreshIfStale() {
        if (ports_stale.exchange(false, std::memory_order_acq_rel))
            refreshInternal();
    }

    size_t getInputCount() {
        refreshIfStale();
        std::lock_guard<std::mutex> lock(ports_mutex);
        return input_ports.size();
    }

    size_t getOutputCount() {
        refreshIfStale();
        std::lock_guard<std::mutex> lock(ports_mutex);
        return output_ports.size();
    }

    bool getInputPort(size_t index, libremidi::input_port& port) {
        refreshIfStale();
        std::lock_guard<std::mutex> lock(ports_mutex);
        if (index >= input_ports.size()) return false;
        port = input_ports[index];
        return true;
    }

    bool getOutputPort(size_t index, libremidi::output_port& port) {
        refreshIfStale();
        std::lock_guard<std::mutex> lock(ports_mutex);
        if (index >= output_ports.size()) return false;
        port = output_ports[index];
        return true;
    }

    bool getInputPortById(uint64_t port_id, libremidi::input_port& port) {
        refreshIfStale();
        std::lock_guard<std::mutex> lock(ports_mutex);
        for (const auto& p : input_ports) {
            if (public_port_id(p) == port_id) {
                port = p;
                return true;
            }
        }
        return false;
    }

    bool getOutputPortById(uint64_t port_id, libremidi::output_port& port) {
        refreshIfStale();
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
        if (auto cb = hotplug_callback.load(std::memory_order_acquire)) {
            cb(hotplug_context, eventType);
        }
    }
};


// Ownership of the message buffers handed to Dart.
//
// Each incoming message is copied to the heap and released again by the Dart
// listener through lrm_free. Invocations still queued when
// NativeCallable.close() runs are discarded by the Dart runtime, so their
// copies never reached lrm_free and leaked — bounded by what was in flight at
// dispose, which for large SysEx is not nothing.
//
// Tracking the buffers here fixes that: the input reclaims whatever is still
// outstanding when it closes, and lrm_free ignores a pointer that has already
// been reclaimed, so a late Dart call cannot double free.
namespace {
// Both intentionally never freed, like the deferred releaser: its thread may
// still release a port while the process exits, after function-local statics
// have been destroyed.
std::mutex& message_buffers_mutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::unordered_map<void*, const void*>& message_buffers() {
    static auto* m = new std::unordered_map<void*, const void*>();
    return *m;
}
}

// Allocates a message buffer owned by `owner` until Dart frees it or the owner
// goes away.
static uint8_t* claim_message_buffer(size_t size, const void* owner) {
    auto* buffer = static_cast<uint8_t*>(std::malloc(size));
    if (!buffer)
        return nullptr;
    std::lock_guard<std::mutex> lock(message_buffers_mutex());
    message_buffers()[buffer] = owner;
    return buffer;
}

// Frees every buffer still outstanding for `owner`.
static void release_message_buffers_of(const void* owner) {
    std::vector<void*> orphans;
    {
        std::lock_guard<std::mutex> lock(message_buffers_mutex());
        auto& map = message_buffers();
        for (auto it = map.begin(); it != map.end();) {
            if (it->second == owner) {
                orphans.push_back(it->first);
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto* buffer : orphans)
        std::free(buffer);
}

struct LrmMidiIn {
    std::unique_ptr<libremidi::midi_in> midi_in;
    LrmMidiCallback callback;
    void* context;
    // Set by lrm_midi_in_open_by_id_with_port: the context callback posts to.
    std::unique_ptr<DartPortSink> port_sink;

    // Drains the message callback before libremidi is torn down. The backends
    // differ in what they guarantee at destruction: the ALSA sequencer backend
    // stops its own polling thread in close_port(), but CoreMIDI's teardown is
    // ~midi_in_core -> close_port() -> MIDIPortDispose(), and MIDIPortDispose
    // does not promise that a MIDIReadProc already running on CoreMIDI's thread
    // has returned. Rather than depend per backend on an undocumented promise,
    // guard our own callback: without it, closing an input under dense traffic
    // can run the lambda below against members that are already being
    // destroyed. Must stay in sync with apple/Classes/libremidi_flutter.cpp.
    std::mutex callback_mutex;
    bool closing = false;

    LrmMidiIn(libremidi::input_port port, LrmMidiCallback cb, void* ctx,
              bool receive_sysex, bool receive_timing, bool receive_sensing)
        : callback(cb), context(ctx) {

        libremidi::input_configuration config;
        config.ignore_sysex = !receive_sysex;
        config.ignore_timing = !receive_timing;
        config.ignore_sensing = !receive_sensing;
        config.on_message = [this](const libremidi::message& msg) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (closing || !callback)
                return;
            const size_t size = msg.bytes.size();
            if (size == 0)
                return;
            // libremidi reuses a single message buffer: it is cleared as soon as
            // this synchronous callback returns and overwritten for every further
            // message in the same packet. Port delivery copies the bytes into
            // the Dart heap inside the post, i.e. before that, so it reads the
            // buffer directly.
            if (callback == post_midi_to_port) {
                post_midi_to_port(context, msg.bytes.data(), size, msg.timestamp);
                return;
            }
            // A plain LrmMidiCallback may read the bytes later (a Dart
            // NativeCallable.listener does), so it gets an owning heap copy,
            // released via lrm_free once read.
            auto* copy = claim_message_buffer(size, this);
            if (!copy)
                return;
            std::memcpy(copy, msg.bytes.data(), size);
            callback(context, copy, size, msg.timestamp);
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
        // Propagate a failed open. libremidi reports it as a return value, not
        // an exception; ignoring it handed Dart a live LrmMidiIn* for a port
        // that was never opened, so every later call silently did nothing.
        // The lrm_midi_in_open* wrappers turn this into a nullptr result.
        if (auto err = midi_in->open_port(port); err != stdx::error{}) {
            throw std::runtime_error("open_port failed");
        }
    }

    ~LrmMidiIn() {
        // Reject further deliveries, wait for one that is already inside the
        // callback, and only then let libremidi close the port. A delivery that
        // *starts* during libremidi's own teardown still dereferences
        // libremidi's object before reaching our lambda — that part cannot be
        // fixed from here.
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            closing = true;
        }
        midi_in.reset();
        // Nothing can deliver any more, so anything still outstanding will
        // never come back through lrm_free. Reclaim it.
        release_message_buffers_of(this);
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
        // See LrmMidiIn: a failed open must not look like success.
        if (auto err = midi_out->open_port(port); err != stdx::error{}) {
            throw std::runtime_error("open_port failed");
        }
    }
};

// =============================================================================
// Library info
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT const char* lrm_get_version(void) {
    return LRM_VERSION;
}

extern "C" FFI_PLUGIN_EXPORT const char* lrm_get_last_error(void) {
    return current_open_error();
}

// Free a heap buffer handed to Dart by the input callback. Ignores a pointer
// that is not outstanding: the owning input may have reclaimed it already when
// it closed, and the Dart listener can still be holding a stale copy of it.
extern "C" FFI_PLUGIN_EXPORT void lrm_free(void* ptr) {
    if (!ptr)
        return;
    {
        std::lock_guard<std::mutex> lock(message_buffers_mutex());
        auto& map = message_buffers();
        auto it = map.find(ptr);
        if (it == map.end())
            return;
        map.erase(it);
    }
    std::free(ptr);
}

// =============================================================================
// Delivery to a Dart native port
// =============================================================================
//
// The *_with_port exports reuse the callback paths above with these adapters
// as the callback and a DartPortSink as its context. The sink is owned by the
// LrmObserver / LrmMidiIn it serves, so it lives exactly as long as the
// callback can run — the existing teardown (which waits out a running
// delivery) covers it too.
//
// Posting instead of calling a NativeCallable is what makes an ended isolate
// harmless: Dart_PostCObject on a closed port returns false and drops the
// message, whereas invoking a deleted NativeCallable aborts the process. That
// happened whenever an isolate ended — or the app was hot restarted — with an
// input or hotplug observer still open. Must stay in sync with the other
// native copy. DartPortSink itself is defined near the top of the file.

static void post_hotplug_to_port(void* context, int32_t event_type) {
    auto* sink = static_cast<DartPortSink*>(context);
    Dart_CObject event;
    event.type = Dart_CObject_kInt64;
    event.value.as_int64 = event_type;
    sink->post(sink->port, &event);
}

static void post_midi_to_port(void* context, const uint8_t* data, size_t length,
                              int64_t timestamp) {
    auto* sink = static_cast<DartPortSink*>(context);
    Dart_CObject bytes;
    bytes.type = Dart_CObject_kTypedData;
    bytes.value.as_typed_data.type = Dart_TypedData_kUint8;
    bytes.value.as_typed_data.length = static_cast<intptr_t>(length);
    bytes.value.as_typed_data.values = data;
    Dart_CObject stamp;
    stamp.type = Dart_CObject_kInt64;
    stamp.value.as_int64 = timestamp;
    Dart_CObject* items[] = {&bytes, &stamp};
    Dart_CObject message;
    message.type = Dart_CObject_kArray;
    message.value.as_array.length = 2;
    message.value.as_array.values = items;
    // The post copies the bytes into the Dart heap; `data` is libremidi's own
    // buffer and stays with it (see LrmMidiIn's on_message).
    sink->post(sink->port, &message);
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

extern "C" FFI_PLUGIN_EXPORT LrmObserver* lrm_observer_new_with_port(
    LrmPostCObject post,
    int64_t port
) {
    if (!post) return nullptr;
    try {
        auto sink = std::make_unique<DartPortSink>(DartPortSink{post, port});
        auto* observer = new LrmObserver(post_hotplug_to_port, sink.get());
        observer->port_sink = std::move(sink);
        return observer;
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT void lrm_observer_free(LrmObserver* observer) {
    delete observer;
}

extern "C" FFI_PLUGIN_EXPORT bool lrm_observer_hotplug_available(LrmObserver* observer) {
    if (!observer) return false;
    if (observer->hotplug_callback.load(std::memory_order_acquire) == nullptr)
        return false;
#if defined(__ANDROID__)
    // Android delivers hotplug through MidiBridge's listener, which exists only
    // if the plugin initialised MIDI; the libremidi observer itself cannot fail
    // there, so ask the bridge.
    return libremidi::android::midi_available();
#else
    // These backends register their hotplug callbacks with the libremidi
    // observer itself, which throws on failure (WinMIDI through our patched
    // backend); the constructor then rebuilt it without hotplug.
    return !observer->hotplug_failed;
#endif
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
    info->is_virtual = lrm_is_virtual_transport(port.type);
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
// Fuzzy Port Matching (libremidi heuristic matcher)
// =============================================================================

template<typename PortType>
static int32_t find_closest_port_impl(
    const std::vector<PortType>& ports, const LrmPortInfo* saved) {
    if (!saved || ports.empty()) return -1;

    PortType target{};
    target.port_name = saved->port_name;
    target.device_name = saved->device_name;
    target.display_name = saved->display_name;
    target.manufacturer = saved->manufacturer;
    target.product = saved->product;
    target.serial = saved->serial;

    auto result = libremidi::find_closest_port(
        target, std::span<const PortType>{ports.data(), ports.size()});

    if (!result.found) return -1;
    return static_cast<int32_t>(result.port - ports.data());
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_find_closest_port(
    LrmObserver* observer, const LrmPortInfo* saved, bool is_input) {
    if (!observer || !saved) return -1;
    // Rebuild a stale list first: refreshInternal() takes ports_mutex itself.
    observer->refreshIfStale();
    // Hold ports_mutex for the whole match: a refresh replaces these vectors
    // under the same lock, so reading them unlocked would race with it.
    std::lock_guard<std::mutex> lock(observer->ports_mutex);
    if (is_input) {
        return find_closest_port_impl(observer->input_ports, saved);
    } else {
        return find_closest_port_impl(observer->output_ports, saved);
    }
}

// =============================================================================
// MIDI Output API
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT LrmMidiOut* lrm_midi_out_open(LrmObserver* observer, int32_t port_index) {
    if (!observer) return nullptr;
    if (port_index < 0) return nullptr;

    OpenErrorScope open_error;

    libremidi::output_port port;
    if (!observer->getOutputPort(static_cast<size_t>(port_index), port)) {
        open_error.set("port not found");
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

    OpenErrorScope open_error;

    libremidi::output_port port;
    if (!observer->getOutputPortById(port_id, port)) {
        open_error.set("port not found, the device may have been disconnected");
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
        // send_message reports failure by return value. Only reporting thrown
        // exceptions made this return LRM_OK for sends the backend rejected.
        if (auto err = midi_out->midi_out->send_message(data, length);
            err != stdx::error{}) {
            return LRM_ERR_SEND_FAILED;
        }
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

    OpenErrorScope open_error;

    libremidi::input_port port;
    if (!observer->getInputPort(static_cast<size_t>(port_index), port)) {
        open_error.set("port not found");
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

    OpenErrorScope open_error;

    libremidi::input_port port;
    if (!observer->getInputPortById(port_id, port)) {
        open_error.set("port not found, the device may have been disconnected");
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

extern "C" FFI_PLUGIN_EXPORT LrmMidiIn* lrm_midi_in_open_by_id_with_port(
    LrmObserver* observer,
    uint64_t port_id,
    LrmPostCObject post,
    int64_t port,
    bool receive_sysex,
    bool receive_timing,
    bool receive_sensing
) {
    if (!post) return nullptr;
    std::unique_ptr<DartPortSink> sink;
    try {
        sink = std::make_unique<DartPortSink>(DartPortSink{post, port});
    } catch (...) {
        return nullptr;
    }
    // Deliveries may start before this returns; they only use the sink's
    // address, which moving the unique_ptr below does not change.
    LrmMidiIn* midi_in = lrm_midi_in_open_by_id(
        observer, port_id, post_midi_to_port, sink.get(),
        receive_sysex, receive_timing, receive_sensing);
    if (midi_in) midi_in->port_sink = std::move(sink);
    return midi_in;
}

// Resolve a port id against the observer for the asynchronous open.
static bool resolve_input_port(LrmObserver* observer, uint64_t port_id,
                               libremidi::input_port& port) {
    return observer->getInputPortById(port_id, port);
}

static bool resolve_output_port(LrmObserver* observer, uint64_t port_id,
                                libremidi::output_port& port) {
    return observer->getOutputPortById(port_id, port);
}

// =============================================================================
// Asynchronous open
// =============================================================================
//
// See the header. The ticket outlives whichever side finishes first: the
// opening thread posts "done" and drops its share; the caller takes the handle
// and drops its share — or, if its isolate ended, its NativeFinalizer drops it.
// The last share closes a handle nobody took. Must stay in sync with the other
// native copy; the copy-specific parts are resolve_*_port and the open-error
// helpers above.
struct LrmOpenTicket {
    explicit LrmOpenTicket(bool input) : is_input(input) {}
    const bool is_input;
    std::atomic<int> owners{2};  // the opening thread and the caller
    std::mutex mutex;            // guards handle and error
    void* handle = nullptr;      // LrmMidiIn* / LrmMidiOut* until taken
    std::string error;
};

static void close_open_handle(bool is_input, void* handle) {
    if (!handle) return;
    if (is_input) delete static_cast<LrmMidiIn*>(handle);
    else delete static_cast<LrmMidiOut*>(handle);
}

static void release_open_ticket(LrmOpenTicket* ticket) {
    if (ticket->owners.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    close_open_handle(ticket->is_input, ticket->handle);
    delete ticket;
}

// Runs on the opening thread once the open has finished either way.
static void finish_open(LrmOpenTicket* ticket, void* handle, LrmPostCObject post,
                        int64_t done_port) {
    {
        std::lock_guard<std::mutex> lock(ticket->mutex);
        ticket->handle = handle;
        if (!handle) {
            ticket->error = current_open_error();
            if (ticket->error.empty()) ticket->error = "open failed";
        }
    }
    Dart_CObject done;
    done.type = Dart_CObject_kNull;
    post(done_port, &done);  // false if the caller's isolate is gone; its
                             // finalizer then releases the other share
    release_open_ticket(ticket);
}

extern "C" FFI_PLUGIN_EXPORT LrmOpenTicket* lrm_midi_in_open_by_id_async(
    LrmObserver* observer,
    uint64_t port_id,
    LrmPostCObject post,
    int64_t message_port,
    int64_t done_port,
    bool receive_sysex,
    bool receive_timing,
    bool receive_sensing
) {
    if (!observer || !post) return nullptr;
    OpenErrorScope open_error;
    libremidi::input_port port;
    if (!resolve_input_port(observer, port_id, port)) {
        open_error.set("port not found, the device may have been disconnected");
        return nullptr;
    }
    LrmOpenTicket* ticket = nullptr;
    try {
        auto sink = std::make_unique<DartPortSink>(DartPortSink{post, message_port});
        ticket = new LrmOpenTicket(true);
        std::thread([ticket, port, sink = std::move(sink), post, done_port,
                     receive_sysex, receive_timing, receive_sensing]() mutable {
            OpenErrorScope thread_error;
            LrmMidiIn* midi_in = nullptr;
            try {
                // As in lrm_midi_in_open_by_id_with_port: deliveries may start
                // before the sink is handed over; they only use its address.
                midi_in = new LrmMidiIn(port, post_midi_to_port, sink.get(),
                                        receive_sysex, receive_timing, receive_sensing);
                midi_in->port_sink = std::move(sink);
            } catch (...) {
                midi_in = nullptr;
            }
            finish_open(ticket, midi_in, post, done_port);
        }).detach();
        return ticket;
    } catch (...) {
        delete ticket;
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT LrmOpenTicket* lrm_midi_out_open_by_id_async(
    LrmObserver* observer,
    uint64_t port_id,
    LrmPostCObject post,
    int64_t done_port
) {
    if (!observer || !post) return nullptr;
    OpenErrorScope open_error;
    libremidi::output_port port;
    if (!resolve_output_port(observer, port_id, port)) {
        open_error.set("port not found, the device may have been disconnected");
        return nullptr;
    }
    LrmOpenTicket* ticket = nullptr;
    try {
        ticket = new LrmOpenTicket(false);
        std::thread([ticket, port, post, done_port]() mutable {
            OpenErrorScope thread_error;
            LrmMidiOut* midi_out = nullptr;
            try {
                midi_out = new LrmMidiOut(port);
            } catch (...) {
                midi_out = nullptr;
            }
            finish_open(ticket, midi_out, post, done_port);
        }).detach();
        return ticket;
    } catch (...) {
        delete ticket;
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT void* lrm_open_ticket_take(LrmOpenTicket* ticket) {
    if (!ticket) return nullptr;
    std::lock_guard<std::mutex> lock(ticket->mutex);
    void* handle = ticket->handle;
    ticket->handle = nullptr;
    return handle;
}

extern "C" FFI_PLUGIN_EXPORT const char* lrm_open_ticket_error(LrmOpenTicket* ticket) {
    if (!ticket) return "";
    std::lock_guard<std::mutex> lock(ticket->mutex);
    return ticket->error.c_str();
}

extern "C" FFI_PLUGIN_EXPORT void lrm_open_ticket_release(LrmOpenTicket* ticket) {
    if (ticket) release_open_ticket(ticket);
}

// =============================================================================
// Deferred release
// =============================================================================
//
// See the header. One cleanup thread, started on first use, works through the
// queue for the rest of the process. The queue itself is never destroyed, so
// a finalizer that runs while the process is exiting cannot touch a dead
// object. Must stay in sync with the other native copy.
namespace {
class DeferredReleaser {
public:
    // Throws if the thread cannot be started or the queue cannot grow; nothing
    // is queued then, so the caller releases inline. started_ is set only once
    // the thread runs: setting it first left a failed start permanent, with
    // every later release queued for a thread that never existed.
    void post(std::function<void()> release) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!started_) {
                std::thread([this] { run(); }).detach();
                started_ = true;
            }
            queue_.push_back(std::move(release));
        }
        cv_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> release;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !queue_.empty(); });
                release = std::move(queue_.front());
                queue_.pop_front();
            }
            release();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool started_ = false;
};

DeferredReleaser& deferred_releaser() {
    static auto* releaser = new DeferredReleaser();  // intentionally never freed
    return *releaser;
}

template <typename T>
void release_later(T* object, void (*release)(T*)) {
    if (!object) return;
    try {
        deferred_releaser().post([object, release] { release(object); });
    } catch (...) {
        release(object);  // out of memory: better to block than to leak
    }
}
}  // namespace

extern "C" FFI_PLUGIN_EXPORT void lrm_midi_in_close_deferred(LrmMidiIn* midi_in) {
    release_later(midi_in, lrm_midi_in_close);
}

extern "C" FFI_PLUGIN_EXPORT void lrm_midi_out_close_deferred(LrmMidiOut* midi_out) {
    release_later(midi_out, lrm_midi_out_close);
}

extern "C" FFI_PLUGIN_EXPORT void lrm_observer_free_deferred(LrmObserver* observer) {
    release_later(observer, lrm_observer_free);
}

extern "C" FFI_PLUGIN_EXPORT void lrm_open_ticket_release_deferred(LrmOpenTicket* ticket) {
    release_later(ticket, lrm_open_ticket_release);
}
