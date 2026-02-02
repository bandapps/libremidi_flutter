// Patched version of libremidi/backends/android/helpers.hpp
// Adds additional port info functions and hotplug support

#pragma once
#include <libremidi/error.hpp>

#include <amidi/AMidi.h>
#include <android/log.h>

#include <jni.h>
#include <pthread.h>

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

  static JNIEnv* get_thread_env();
  static jobject get_context(JNIEnv* env);
  static jobject get_midi_manager(JNIEnv* env, jobject context);
  static void refresh_midi_devices(JNIEnv* env, jobject context, bool is_output);
  static void open_device(const midi_port_entry& port_entry, void* target, bool is_output);
  static std::string port_name(JNIEnv* env, unsigned int port_number);
  static void cleanup_devices(JNIEnv* env);
  static inline int pending_port_index = 0;  // Port index to open in callback

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

// Hotplug observer registration (defined in android_helpers.cpp)
using hotplug_callback_t = void (*)(void* observer);
void set_hotplug_observer(void* observer, hotplug_callback_t on_added, hotplug_callback_t on_removed);
void clear_hotplug_observer();
}
}
