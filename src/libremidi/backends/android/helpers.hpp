// Patched version of libremidi/backends/android/helpers.hpp
// Adds additional port info functions and hotplug support

#pragma once
#include <libremidi/error.hpp>

#include <amidi/AMidi.h>
#include <android/log.h>

#include <jni.h>
#include <pthread.h>

#include <mutex>
#include <string>
#include <vector>
#include <functional>

#define LOG_TAG "libremidi"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

NAMESPACE_LIBREMIDI
{
namespace android
{

// Forward declaration for hotplug callback
class observer;

// Represents a single MIDI port (device + port index within that device)
struct midi_port_entry
{
  jobject device_info;  // Global ref to MidiDeviceInfo
  int port_index;       // Port index within the device
};

struct context
{
  static inline std::string client_name;
  static inline std::vector<midi_port_entry> midi_ports;

  // Guards midi_ports.
  //
  // The cache is rebuilt for one direction at a time and then read positionally
  // ("refresh, then ask for entry i"), and inputs and outputs share it — so an
  // output enumeration running between another caller's refresh and its reads
  // silently replaced the list under it, and cleanup_devices() deleted the
  // global references that caller was still indexing. Every refresh-and-read
  // sequence must therefore hold this lock for its whole duration, not just
  // around single operations.
  //
  // Recursive so the individual accessors below can lock defensively while
  // still being callable from inside a sequence that already holds it.
  static std::recursive_mutex& ports_mutex();

  static JNIEnv* get_thread_env();
  static jobject get_context(JNIEnv* env);
  static jobject get_midi_manager(JNIEnv* env, jobject context);
  static void refresh_midi_devices(JNIEnv* env, jobject context, bool is_output);
  // Returns false when nothing was handed to Java, i.e. no callback will ever
  // arrive for this token. The token is released on those paths.
  static bool open_device(const midi_port_entry& port_entry, int64_t token, bool is_output);
  // Ends a device connection opened through open_device(): releases the native
  // handle and closes the Java MidiDevice, then drops the global reference.
  // AMidiDevice_release() alone only detaches the native object; the
  // connection in MidiService (and for Bluetooth the radio link) stays up
  // until the Java side is closed. Either argument may be null.
  static void close_device(AMidiDevice* device, jobject java_device);
  static std::string port_name(JNIEnv* env, unsigned int port_number);
  static void cleanup_devices(JNIEnv* env);

  // Extended port info functions
  static std::string port_manufacturer(JNIEnv* env, unsigned int port_number);
  static std::string port_product(JNIEnv* env, unsigned int port_number);
  static std::string port_serial(JNIEnv* env, unsigned int port_number);
  static int port_type(JNIEnv* env, unsigned int port_number);

  // Hotplug support
  static jobject register_device_callback(JNIEnv* env, jobject midi_manager, void* observer_ptr);
  static void unregister_device_callback(JNIEnv* env, jobject midi_manager, jobject callback);
};

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpened(
    JNIEnv* env, jobject /*thiz*/, jobject midi_device, jlong target_ptr, jboolean is_output);

// Hotplug callbacks from Java
extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceStatusCallback_onDeviceAddedNative(
    JNIEnv* env, jobject /*thiz*/, jlong observer_ptr, jobject device_info);
extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceStatusCallback_onDeviceRemovedNative(
    JNIEnv* env, jobject /*thiz*/, jlong observer_ptr, jobject device_info);

// Bookkeeping for the asynchronous device open.
//
// MidiManager.openDevice() is asynchronous: it returns immediately and calls
// back later. Previously the midi_in/midi_out pointer was handed to Java
// directly and the port index was parked in one process-wide variable. Both
// were broken:
//
//   - closing the port before the callback arrived left Java holding a pointer
//     to a destroyed object, which it then wrote through;
//   - two overlapping opens overwrote each other's port index, so a callback
//     could open a different port than the one that was requested.
//
// Java now receives an opaque token instead. The token resolves — under a lock,
// and only once — to the requesting object *and* its port index. A token whose
// requester has been closed in the meantime resolves to nothing, so the late
// callback has nothing to write to.
struct pending_open
{
  void* target;
  int port_index;
  bool is_output;
};

int64_t register_pending_open(void* target, int port_index, bool is_output);
void cancel_pending_open(void* target);
// Marks the entry as in flight and hands out its contents. The entry stays in
// the registry until finish_pending_open(), so cancel_pending_open() blocks
// instead of letting the target be destroyed mid-callback.
bool take_pending_open(int64_t token, pending_open& out);
void finish_pending_open(int64_t token);

// Hotplug observer registration (defined in android_helpers.cpp)
using hotplug_callback_t = void (*)(void* observer);
void set_hotplug_observer(void* observer, hotplug_callback_t on_added, hotplug_callback_t on_removed);
void clear_hotplug_observer(void* observer);
}
}
