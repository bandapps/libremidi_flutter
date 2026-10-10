// Shared Apple (iOS + macOS) CoreMIDI implementation.
// Both platforms compile this single file via their SwiftPM targets
// (ios/ and macos/ Sources symlink here). Hotplug notifications use
// MIDIClientCreate on a dedicated thread with its own persistent CFRunLoop,
// keeping the slow first CoreMIDI contact off Flutter's UI thread while
// keeping notification delivery reliable.
#include "libremidi_flutter.h"

// Enable header-only mode and CoreMIDI backend
#define LIBREMIDI_HEADER_ONLY 1
#define LIBREMIDI_COREMIDI 1

#include <libremidi/libremidi.hpp>
#include <libremidi/port_comparison.hpp>
#include <dart_api/dart_native_api.h>
#include <lrm_transport.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <functional>
#include <deque>
#include <unordered_map>
#include <mutex>
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

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dispatch/dispatch.h>

// =============================================================================
// Internal structures using Generic API
// =============================================================================

// Event types for hotplug callback
#define LRM_EVENT_INPUT_ADDED    0
#define LRM_EVENT_INPUT_REMOVED  1
#define LRM_EVENT_OUTPUT_ADDED   2
#define LRM_EVENT_OUTPUT_REMOVED 3
#define LRM_EVENT_SETUP_CHANGED  4

// Forward declaration
struct ObserverState;

// Hotplug notifications use the classic run-loop based MIDIClientCreate, served
// by a dedicated thread that owns a persistent CFRunLoop (see LrmObserver).
static void handleMIDINotification(ObserverState* obs, const MIDINotification* notification);

struct ObserverState {
    std::unique_ptr<libremidi::observer> observer;
    std::vector<libremidi::input_port> input_ports;
    std::vector<libremidi::output_port> output_ports;
    // Guarded by callback_mutex: requestStop() clears it and, by taking the
    // same lock, waits out a delivery that is already running. That is what
    // lets the caller close its NativeCallable and walk away while this thread
    // is still stuck inside CoreMIDI.
    LrmHotplugCallback hotplug_callback;
    mutable std::mutex callback_mutex;
    // Whether this observer was created for hotplug. Fixed at construction:
    // requestStop() clears hotplug_callback, and reading that pointer to decide
    // *what kind of observer this is* would both race with it and change the
    // answer halfway through initialisation.
    const bool wants_hotplug;
    void* hotplug_context;
    // Set by lrm_observer_new_with_port: the context hotplug_callback posts to.
    // Owned here, not by the handle, because this state can outlive the handle
    // (see LrmObserver); requestStop() ends all use of it.
    std::unique_ptr<DartPortSink> port_sink;
    mutable std::mutex ports_mutex;

    MIDIClientRef midiClient;

    // Dedicated CoreMIDI thread with its own CFRunLoop (hotplug path only).
    // CoreMIDI notifications are delivered to the run loop of the thread that
    // created the client, so that thread must stay alive and run its loop for
    // the observer's lifetime. A transient GCD-queue worker thread does NOT
    // satisfy this — using one silently killed hotplug delivery, so a device
    // plugged in after launch never appeared.
    std::thread midiThread;
    CFRunLoopRef midiRunLoop;
    std::mutex runloop_mutex;
    std::condition_variable runloop_cv;
    bool runloop_ready;
    // Signalled by the destructor to break the run loop. Guarded by
    // runloop_mutex; the thread clears and releases it there too, so the
    // destructor can never signal a source that is being released.
    CFRunLoopSourceRef stopSource;
    // Read by the MIDI thread outside the lock, hence atomic. This flag — not
    // CFRunLoopStop — is what actually ends the loop.
    std::atomic<bool> stop_requested;
    // Result of MIDIClientCreate, or kPending until initInternal has run.
    //
    // It must NOT start at noErr: initialisation happens on the MIDI thread, so
    // a caller asking right after construction would otherwise be told hotplug
    // works before we have even tried. Pending reads as "not available yet",
    // which is the safe answer — the caller is told to check again after the
    // first setup_changed event, which is emitted immediately after this is
    // set.
    static constexpr OSStatus kPending = 1;  // no CoreMIDI status uses this
    std::atomic<OSStatus> client_status{kPending};

    ObserverState(LrmHotplugCallback callback = nullptr, void* context = nullptr)
        : hotplug_callback(callback), hotplug_context(context)
        , wants_hotplug(callback != nullptr)
        , midiClient(0)
        , midiRunLoop(nullptr)
        , runloop_ready(false)
        , stopSource(nullptr)
        , stop_requested(false)
    {
        if (wants_hotplug) {
            // Hotplug (long-lived UI) path. On a real iOS device the first
            // CoreMIDI/midiserverd contact can block for minutes, and Dart
            // calls this constructor synchronously over FFI on the UI isolate
            // — doing the work here froze the whole UI (white screen until it
            // returned). Run all CoreMIDI work on a dedicated thread that owns
            // a persistent CFRunLoop: the slow first contact no longer blocks
            // the UI, and (unlike a GCD queue) the run loop stays alive so
            // hotplug notifications keep being delivered. SETUP_CHANGED is
            // emitted once the initial port list is ready.
            // Started by create(), which owns a shared_ptr it can hand to the
            // thread. Starting it here would give the thread a bare `this` and
            // reintroduce the lifetime problem this design removes.
        } else {
            // No-hotplug (one-shot query) path: kept synchronous so a
            // construct-then-read stays valid. The integration tests and any
            // poll-based caller rely on ports being available right away. This
            // path creates no MIDI client and starts no thread.
            initInternal();
        }
    }

    // Body of the dedicated CoreMIDI thread: stand up a persistent run loop,
    // perform the (possibly slow) initialisation on it, then service hotplug
    // notifications until the destructor stops the loop.
    void midiThreadMain() {
        midiRunLoop = CFRunLoopGetCurrent();

        // Keep the run loop from returning immediately when it has no other
        // input source yet. This source does nothing; it just holds the loop.
        CFRunLoopSourceContext keepAliveCtx = {};
        keepAliveCtx.perform = [](void*) {};
        CFRunLoopSourceRef keepAlive = CFRunLoopSourceCreate(nullptr, 0, &keepAliveCtx);
        CFRunLoopAddSource(midiRunLoop, keepAlive, kCFRunLoopCommonModes);

        // Source the destructor signals to break the loop. Signalling a source
        // is order-independent: signalled before the loop runs, the signal
        // stays pending and fires on the first pass.
        CFRunLoopSourceContext stopCtx = {};
        stopCtx.info = this;
        stopCtx.perform = [](void* info) {
            auto* self = static_cast<ObserverState*>(info);
            if (self->stop_requested.load(std::memory_order_acquire)) {
                CFRunLoopStop(CFRunLoopGetCurrent());
            }
        };
        CFRunLoopSourceRef stop = CFRunLoopSourceCreate(nullptr, 0, &stopCtx);
        CFRunLoopAddSource(midiRunLoop, stop, kCFRunLoopCommonModes);

        // Publish the run loop and the stop source for the destructor.
        {
            std::lock_guard<std::mutex> lock(runloop_mutex);
            stopSource = stop;
            runloop_ready = true;
        }
        runloop_cv.notify_all();

        initInternal();
        notifyHotplug(LRM_EVENT_SETUP_CHANGED);

        // Service CoreMIDI notifications until a stop is requested.
        //
        // Two earlier attempts lost a stop that arrived before this loop was
        // entered — first a bare CFRunLoopStop, then a CFRunLoopPerformBlock —
        // after which the loop ran forever and the destructor's join() blocked
        // the calling isolate permanently. Both relied on CoreFoundation
        // holding on to a stop for a run loop that is not running yet, which it
        // does not do reliably.
        //
        // What makes this correct is that a *signalled source* is state on the
        // source itself and survives until the source is serviced, so the stop
        // cannot be dropped no matter when it arrives:
        //
        //   - stop before the check below   -> the loop is never entered
        //   - stop while the loop runs      -> the signalled source stops it
        //   - stop between check and entry  -> the signal is already pending
        //                                      and fires on the first pass
        //
        // No timeout is needed for this, and none is used: tool/stress verifies
        // it. Under a hotplug storm with disposal jitter aimed at the window
        // between initialisation and loop entry, this survives thousands of
        // create/free cycles, while the CFRunLoopPerformBlock version hangs
        // within seconds on the same run.
        while (!stop_requested.load(std::memory_order_acquire)) {
            CFRunLoopRun();
        }

        // Tear down the CoreMIDI objects on the same thread that created them.
        if (midiClient) {
            MIDIClientDispose(midiClient);
            midiClient = 0;
        }
        {
            std::lock_guard<std::mutex> lock(ports_mutex);
            observer.reset();
        }

        // Take the stop source back under the lock before releasing it, so a
        // destructor running right now cannot signal a released source.
        CFRunLoopSourceRef stopToRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(runloop_mutex);
            stopToRelease = stopSource;
            stopSource = nullptr;
        }
        if (stopToRelease) {
            CFRunLoopRemoveSource(midiRunLoop, stopToRelease, kCFRunLoopCommonModes);
            CFRelease(stopToRelease);
        }
        CFRunLoopRemoveSource(midiRunLoop, keepAlive, kCFRunLoopCommonModes);
        CFRelease(keepAlive);
    }

    // Performs the actual (potentially slow) CoreMIDI initialisation: creates
    // our notification client, the libremidi observer and fills the port cache.
    // Runs on the dedicated MIDI thread for the hotplug path, or synchronously
    // on the caller for the no-hotplug path.
    void initInternal() {
        if (wants_hotplug) {
            // Classic, run-loop based MIDIClientCreate: notifications are
            // delivered on THIS thread's CFRunLoop, which midiThreadMain keeps
            // running for the observer's lifetime, so hotplug stays reliable.
            // On failure midiClient is left 0, which every other path tolerates.
            const OSStatus status = MIDIClientCreate(
                CFSTR("libremidi_flutter"),
                [](const MIDINotification* notification, void* refCon) {
                    handleMIDINotification(static_cast<ObserverState*>(refCon), notification);
                },
                this,
                &midiClient
            );
            client_status.store(status, std::memory_order_release);
            if (status != noErr) {
                // No notification client: port enumeration below still works,
                // so the port list and SETUP_CHANGED stay correct, but no
                // hotplug notification will ever arrive for this observer.
                // This happens for real — a CoreMIDI server that is not
                // reachable returns kMIDINoConnection (-10833) here.
                //
                // Dart cannot currently tell this apart from a healthy
                // observer: the C ABI has no channel to report it, and
                // inventing one is an API change. lrm_observer_hotplug_status()
                // is the obvious addition for the next minor version.
                midiClient = 0;
            }
        }

        // Create libremidi observer WITHOUT callbacks (we handle hotplug ourselves).
        libremidi::observer_configuration config;
        config.track_hardware = true;
        config.track_virtual = true;
        config.notify_in_constructor = false;

        auto obs = std::make_unique<libremidi::observer>(std::move(config));
        {
            std::lock_guard<std::mutex> lock(ports_mutex);
            observer = std::move(obs);
        }
        refreshInternal();
    }

    ~ObserverState() {
        // Runs on whichever thread drops the last reference — usually the MIDI
        // thread itself, once CoreMIDI finally lets go. Nothing to join here:
        // requestStop() already released the caller.
        if (midiThread.joinable()) {
            midiThread.detach();
        } else if (midiClient) {
            // No-thread path: normally no client is created here, dispose
            // defensively just in case.
            MIDIClientDispose(midiClient);
            midiClient = 0;
        }
    }

    // Creates the state together with the thread that co-owns it.
    static std::shared_ptr<ObserverState> create(LrmHotplugCallback cb, void* ctx) {
        auto self = std::make_shared<ObserverState>(cb, ctx);
        if (cb) {
            // The thread holds its own strong reference, so the state outlives
            // the handle Dart gave back. That is the point: disposing must not
            // wait for a CoreMIDI call that can take minutes to return.
            self->midiThread = std::thread([self] { self->midiThreadMain(); });
        }
        return self;
    }

    // Ends the MIDI thread and stops delivering to Dart. Returns immediately;
    // it never waits for CoreMIDI.
    void requestStop() {
        // Drain first: once this returns, no hotplug callback is running and
        // none can start, so the caller may close its NativeCallable. Without
        // it the detached thread could call into a closed callable.
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            hotplug_callback = nullptr;
        }

        if (!midiThread.joinable())
            return;

        std::lock_guard<std::mutex> lock(runloop_mutex);
        stop_requested.store(true, std::memory_order_release);
        if (stopSource) {
            CFRunLoopSourceSignal(stopSource);
            CFRunLoopWakeUp(midiRunLoop);
        }
        // If the run loop is not published yet the thread has not reached
        // CFRunLoopRun either, and the flag alone ends it there.
    }

    // Refresh the port cache and notify. Called from the CoreMIDI notification
    // handler, which already runs on the dedicated MIDI thread, so the work is
    // done synchronously (no cross-thread dispatch needed).
    void refreshAndNotify(int eventType) {
        refreshInternal();
        notifyHotplug(eventType);
    }

    void refreshInternal() {
        // The observer pointer is assigned on the dedicated MIDI thread for the
        // hotplug path, so snapshot it under the lock before using it.
        libremidi::observer* obs;
        {
            std::lock_guard<std::mutex> lock(ports_mutex);
            obs = observer.get();
        }
        if (!obs) return;
        // Enumerate without holding the lock to avoid deadlock with
        // CoreMIDI's internal locks during notification handling.
        auto new_inputs = obs->get_input_ports();
        auto new_outputs = obs->get_output_ports();
        std::lock_guard<std::mutex> lock(ports_mutex);
        input_ports = std::move(new_inputs);
        output_ports = std::move(new_outputs);
    }

    void refresh() {
        refreshInternal();
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
            if (static_cast<uint64_t>(p.port) == port_id) {
                port = p;
                return true;
            }
        }
        return false;
    }

    bool getOutputPortById(uint64_t port_id, libremidi::output_port& port) const {
        std::lock_guard<std::mutex> lock(ports_mutex);
        for (const auto& p : output_ports) {
            if (static_cast<uint64_t>(p.port) == port_id) {
                port = p;
                return true;
            }
        }
        return false;
    }

    void notifyHotplug(int eventType) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        if (hotplug_callback) {
            hotplug_callback(hotplug_context, eventType);
        }
    }
};

// The handle Dart holds. It owns one reference to the state; the MIDI thread
// owns another. Freeing the handle therefore never has to wait for the thread:
// whoever drops the last reference destroys the state, and if CoreMIDI is still
// holding the thread hostage that simply happens later, on that thread.
struct LrmObserver {
    std::shared_ptr<ObserverState> state;
};

static void handleMIDINotification(ObserverState* obs, const MIDINotification* notification) {
    // This runs on the dedicated MIDI thread's run loop (see LrmObserver), not
    // the UI/main thread, so it is safe to refresh and notify synchronously.

    switch (notification->messageID) {
        case kMIDIMsgObjectAdded: {
            const MIDIObjectAddRemoveNotification* addRemove =
                (const MIDIObjectAddRemoveNotification*)notification;

            if (addRemove->childType == kMIDIObjectType_Source) {
                obs->refreshAndNotify(LRM_EVENT_INPUT_ADDED);
            } else if (addRemove->childType == kMIDIObjectType_Destination) {
                obs->refreshAndNotify(LRM_EVENT_OUTPUT_ADDED);
            }
            break;
        }
        case kMIDIMsgObjectRemoved: {
            const MIDIObjectAddRemoveNotification* addRemove =
                (const MIDIObjectAddRemoveNotification*)notification;

            if (addRemove->childType == kMIDIObjectType_Source) {
                obs->refreshAndNotify(LRM_EVENT_INPUT_REMOVED);
            } else if (addRemove->childType == kMIDIObjectType_Destination) {
                obs->refreshAndNotify(LRM_EVENT_OUTPUT_REMOVED);
            }
            break;
        }
        case kMIDIMsgSetupChanged:
            obs->refreshAndNotify(LRM_EVENT_SETUP_CHANGED);
            break;
        default:
            break;
    }
}

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
std::mutex& message_buffers_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<void*, const void*>& message_buffers() {
    static std::unordered_map<void*, const void*> m;
    return m;
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

    // Drains the message callback before libremidi is torn down. libremidi's
    // midi_in destructor does not wait for an in-flight on_message: on CoreMIDI
    // the path is ~midi_in_core -> close_port() -> MIDIPortDispose(), and
    // MIDIPortDispose gives no guarantee that a MIDIReadProc already running on
    // CoreMIDI's thread has returned. Without this drain, closing an input
    // under dense traffic can run the lambda below against members that are
    // already being destroyed, and hand Dart a buffer nobody owns any more.
    // (Locking in a MIDI callback is not free, but this path already calls
    // malloc per message, so it is not a new class of cost.)
    std::mutex callback_mutex;
    bool closing = false;

    LrmMidiIn(libremidi::input_port& port, LrmMidiCallback cb, void* ctx,
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

        midi_in = std::make_unique<libremidi::midi_in>(std::move(config));
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
        // callback, and only then let libremidi close the port. This closes the
        // window we own. A delivery that *starts* during libremidi's own
        // teardown still dereferences libremidi's object before ever reaching
        // our lambda — that part cannot be fixed from here.
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

    LrmMidiOut(libremidi::output_port& port) {
        midi_out = std::make_unique<libremidi::midi_out>();
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
    return "0.8.7";
}

// Open failures carry no reason on Apple platforms (see the header).
extern "C" FFI_PLUGIN_EXPORT const char* lrm_get_last_error(void) {
    return "";
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
        return new LrmObserver{ObserverState::create(nullptr, nullptr)};
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT LrmObserver* lrm_observer_new_with_callbacks(
    LrmHotplugCallback callback,
    void* context
) {
    try {
        return new LrmObserver{ObserverState::create(callback, context)};
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
        auto state = ObserverState::create(post_hotplug_to_port, sink.get());
        // The MIDI thread may already deliver; it only uses the sink's address.
        state->port_sink = std::move(sink);
        return new LrmObserver{std::move(state)};
    } catch (...) {
        return nullptr;
    }
}

extern "C" FFI_PLUGIN_EXPORT void lrm_observer_free(LrmObserver* observer) {
    if (!observer) return;
    // Ask the thread to finish and stop calling into Dart, then drop this
    // handle's reference. Returns straight away even when the thread is still
    // inside a CoreMIDI call that has not come back yet — which used to block
    // the calling isolate for as long as CoreMIDI took.
    observer->state->requestStop();
    delete observer;
}

extern "C" FFI_PLUGIN_EXPORT bool lrm_observer_hotplug_available(LrmObserver* observer) {
    if (!observer) return false;
    const auto& state = *observer->state;
    return state.wants_hotplug
        && state.client_status.load(std::memory_order_acquire) == noErr;
}

extern "C" FFI_PLUGIN_EXPORT void lrm_observer_refresh(LrmObserver* observer) {
    if (!observer) return;
    observer->state->refresh();
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_input_count(LrmObserver* observer) {
    if (!observer) return 0;
    std::lock_guard<std::mutex> lock(observer->state->ports_mutex);
    return static_cast<int32_t>(observer->state->input_ports.size());
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output_count(LrmObserver* observer) {
    if (!observer) return 0;
    std::lock_guard<std::mutex> lock(observer->state->ports_mutex);
    return static_cast<int32_t>(observer->state->output_ports.size());
}

// Helper to copy string safely
static void safe_strcpy(char* dest, size_t dest_size, const std::string& src) {
    std::strncpy(dest, src.c_str(), dest_size - 1);
    dest[dest_size - 1] = '\0';
}

// FNV-1a 64-bit hash for stable_id generation.
// Iterates over unsigned char: plain `char` is signed on x86/arm64 but unsigned
// on some ABIs, so a byte >= 0x80 (any non-ASCII port name) would sign-extend
// to a different 64-bit value per platform and break the "cross-platform stable
// id" promise. Must stay identical to the copy in src/libremidi_flutter.cpp.
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
    info->is_virtual = lrm_is_virtual_transport(port.type);
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_input(LrmObserver* observer, int32_t index, LrmPortInfo* info) {
    if (!observer || !info) return LRM_ERR_INVALID;
    if (index < 0) return LRM_ERR_NOT_FOUND;

    libremidi::input_port port;
    if (!observer->state->getInputPort(static_cast<size_t>(index), port)) {
        return LRM_ERR_NOT_FOUND;
    }

    fill_port_info(port, index, true, info);
    return LRM_OK;
}

extern "C" FFI_PLUGIN_EXPORT int32_t lrm_observer_get_output(LrmObserver* observer, int32_t index, LrmPortInfo* info) {
    if (!observer || !info) return LRM_ERR_INVALID;
    if (index < 0) return LRM_ERR_NOT_FOUND;

    libremidi::output_port port;
    if (!observer->state->getOutputPort(static_cast<size_t>(index), port)) {
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
    // Hold ports_mutex for the whole match: hotplug refresh replaces these
    // vectors under the same lock, so reading them unlocked would race with it.
    std::lock_guard<std::mutex> lock(observer->state->ports_mutex);
    if (is_input) {
        return find_closest_port_impl(observer->state->input_ports, saved);
    } else {
        return find_closest_port_impl(observer->state->output_ports, saved);
    }
}

// =============================================================================
// MIDI Output API
// =============================================================================

extern "C" FFI_PLUGIN_EXPORT LrmMidiOut* lrm_midi_out_open(LrmObserver* observer, int32_t port_index) {
    if (!observer) return nullptr;
    if (port_index < 0) return nullptr;

    libremidi::output_port port;
    if (!observer->state->getOutputPort(static_cast<size_t>(port_index), port)) {
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
    if (!observer->state->getOutputPortById(port_id, port)) {
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

    libremidi::input_port port;
    if (!observer->state->getInputPort(static_cast<size_t>(port_index), port)) {
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
    if (!observer->state->getInputPortById(port_id, port)) {
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

// Apple platforms report no reason for a failed open (see lrm_get_last_error);
// these keep the shared asynchronous-open code identical to the other copy.
struct OpenErrorScope {
    OpenErrorScope() {}  // user-provided, like the other copy's: no unused-variable noise
    void set(const char*) {}
};

static const char* current_open_error() {
    return "";
}

// Resolve a port id against the observer for the asynchronous open.
static bool resolve_input_port(LrmObserver* observer, uint64_t port_id,
                               libremidi::input_port& port) {
    return observer->state->getInputPortById(port_id, port);
}

static bool resolve_output_port(LrmObserver* observer, uint64_t port_id,
                                libremidi::output_port& port) {
    return observer->state->getOutputPortById(port_id, port);
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
    void post(std::function<void()> release) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(release));
            if (!started_) {
                started_ = true;
                std::thread([this] { run(); }).detach();
            }
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
