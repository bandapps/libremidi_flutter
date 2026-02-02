// Patched version of libremidi/backends/android/helpers.cpp
// Uses cached ClassLoader for FindClass to work from any thread
// Adds extended port info functions (manufacturer, product, serial)

#if defined(__ANDROID__)

#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/backends/android/midi_in.hpp>
#include <libremidi/backends/android/midi_out.hpp>

// Declaration of our custom FindClass function from jni_shim.cpp
extern "C" jclass libremidi_find_class(JNIEnv* env, const char* name);

NAMESPACE_LIBREMIDI::android
{
// Main source of knowledge is RtMidi implementation
// which was done by Yellow Labrador

JNIEnv* context::get_thread_env()
{
  JNIEnv* env;
  JavaVM* jvm;
  jsize count;
  if (JNI_GetCreatedJavaVMs(&jvm, 1, &count) != JNI_OK || count < 1)
  {
    LOGE("No JVM found");
    return nullptr;
  }

  if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
  {
    if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    {
      LOGE("Failed to attach thread");
      return nullptr;
    }
  }

  return env;
}

jobject context::get_context(JNIEnv* env)
{
  auto activity_thread = env->FindClass("android/app/ActivityThread");
  auto current_activity_thread = env->GetStaticMethodID(
      activity_thread, "currentActivityThread", "()Landroid/app/ActivityThread;");
  auto at = env->CallStaticObjectMethod(activity_thread, current_activity_thread);

  if (!at)
  {
    LOGE("Failed to get ActivityThread");
    return nullptr;
  }

  auto get_application
      = env->GetMethodID(activity_thread, "getApplication", "()Landroid/app/Application;");
  auto app = env->CallObjectMethod(at, get_application);

  if (!app)
  {
    LOGE("Failed to get Application");
    return nullptr;
  }

  return app;
}

jobject context::get_midi_manager(JNIEnv* env, jobject ctx)
{
  auto context_class = env->FindClass("android/content/Context");
  auto get_system_service = env->GetMethodID(
      context_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
  auto midi_service_str = env->NewStringUTF("midi");
  auto service = env->CallObjectMethod(ctx, get_system_service, midi_service_str);
  env->DeleteLocalRef(midi_service_str);
  return service;
}

void context::refresh_midi_devices(JNIEnv* env, jobject ctx, bool is_output)
{
  cleanup_devices(env);

  auto midi_service = get_midi_manager(env, ctx);
  if (!midi_service)
    return;

  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto get_devices_method
      = env->GetMethodID(midi_mgr_class, "getDevices", "()[Landroid/media/midi/MidiDeviceInfo;");
  auto device_array = (jobjectArray)env->CallObjectMethod(midi_service, get_devices_method);

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_input_count = env->GetMethodID(device_info_class, "getInputPortCount", "()I");
  auto get_output_count = env->GetMethodID(device_info_class, "getOutputPortCount", "()I");

  jsize count = env->GetArrayLength(device_array);
  for (jsize i = 0; i < count; ++i)
  {
    auto device_info = env->GetObjectArrayElement(device_array, i);
    int port_count = is_output ? env->CallIntMethod(device_info, get_input_count)
                               : env->CallIntMethod(device_info, get_output_count);

    // Add an entry for each port on this device
    for (int port_idx = 0; port_idx < port_count; ++port_idx)
    {
      midi_port_entry entry;
      entry.device_info = env->NewGlobalRef(device_info);
      entry.port_index = port_idx;
      midi_ports.push_back(entry);
    }
    env->DeleteLocalRef(device_info);
  }

  env->DeleteLocalRef(device_array);
  env->DeleteLocalRef(midi_service);
}

// Helper to get a string from a Bundle
static std::string get_bundle_string(JNIEnv* env, jobject bundle, jclass bundle_class, jmethodID get_string_method, const char* key_name)
{
  std::string result;
  jstring key = env->NewStringUTF(key_name);
  jstring value = (jstring)env->CallObjectMethod(bundle, get_string_method, key);
  if (value)
  {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars) {
      result = chars;
      env->ReleaseStringUTFChars(value, chars);
    }
    env->DeleteLocalRef(value);
  }
  env->DeleteLocalRef(key);
  return result;
}

std::string context::port_name(JNIEnv* env, unsigned int port_number)
{
  if (port_number >= midi_ports.size())
  {
    LOGE("Invalid port number");
    return "";
  }

  const auto& entry = midi_ports[port_number];

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_props_method
      = env->GetMethodID(device_info_class, "getProperties", "()Landroid/os/Bundle;");
  auto bundle = env->CallObjectMethod(entry.device_info, get_props_method);

  auto bundle_class = env->FindClass("android/os/Bundle");
  auto get_string_method
      = env->GetMethodID(bundle_class, "getString", "(Ljava/lang/String;)Ljava/lang/String;");

  std::string result = get_bundle_string(env, bundle, bundle_class, get_string_method, "name");
  // Append port index to distinguish multiple ports on same device (1-based for display)
  result += " Port " + std::to_string(entry.port_index + 1);

  env->DeleteLocalRef(bundle);

  return result;
}

std::string context::port_manufacturer(JNIEnv* env, unsigned int port_number)
{
  if (port_number >= midi_ports.size())
    return "";

  const auto& entry = midi_ports[port_number];

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_props_method = env->GetMethodID(device_info_class, "getProperties", "()Landroid/os/Bundle;");
  auto bundle = env->CallObjectMethod(entry.device_info, get_props_method);

  auto bundle_class = env->FindClass("android/os/Bundle");
  auto get_string_method = env->GetMethodID(bundle_class, "getString", "(Ljava/lang/String;)Ljava/lang/String;");

  std::string result = get_bundle_string(env, bundle, bundle_class, get_string_method, "manufacturer");

  env->DeleteLocalRef(bundle);
  return result;
}

std::string context::port_product(JNIEnv* env, unsigned int port_number)
{
  if (port_number >= midi_ports.size())
    return "";

  const auto& entry = midi_ports[port_number];

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_props_method = env->GetMethodID(device_info_class, "getProperties", "()Landroid/os/Bundle;");
  auto bundle = env->CallObjectMethod(entry.device_info, get_props_method);

  auto bundle_class = env->FindClass("android/os/Bundle");
  auto get_string_method = env->GetMethodID(bundle_class, "getString", "(Ljava/lang/String;)Ljava/lang/String;");

  std::string result = get_bundle_string(env, bundle, bundle_class, get_string_method, "product");

  env->DeleteLocalRef(bundle);
  return result;
}

std::string context::port_serial(JNIEnv* env, unsigned int port_number)
{
  if (port_number >= midi_ports.size())
    return "";

  const auto& entry = midi_ports[port_number];

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_props_method = env->GetMethodID(device_info_class, "getProperties", "()Landroid/os/Bundle;");
  auto bundle = env->CallObjectMethod(entry.device_info, get_props_method);

  auto bundle_class = env->FindClass("android/os/Bundle");
  auto get_string_method = env->GetMethodID(bundle_class, "getString", "(Ljava/lang/String;)Ljava/lang/String;");

  std::string result = get_bundle_string(env, bundle, bundle_class, get_string_method, "serial_number");

  env->DeleteLocalRef(bundle);
  return result;
}

int context::port_type(JNIEnv* env, unsigned int port_number)
{
  if (port_number >= midi_ports.size())
    return 0;

  const auto& entry = midi_ports[port_number];

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_type_method = env->GetMethodID(device_info_class, "getType", "()I");

  return env->CallIntMethod(entry.device_info, get_type_method);
}

// Extended port info structure for Android
struct android_port_info {
  std::string name;
  std::string manufacturer;
  std::string product;
  std::string serial_number;
  std::string version;
  int input_port_count;
  int output_port_count;
};

// Get detailed port information from Android MidiDeviceInfo
android_port_info context_get_port_info(JNIEnv* env, unsigned int port_number)
{
  android_port_info info = {};

  if (port_number >= context::midi_ports.size())
  {
    LOGE("Invalid port number for get_port_info");
    return info;
  }

  const auto& entry = context::midi_ports[port_number];
  jobject device_info = entry.device_info;

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_props_method = env->GetMethodID(device_info_class, "getProperties", "()Landroid/os/Bundle;");
  auto get_input_count = env->GetMethodID(device_info_class, "getInputPortCount", "()I");
  auto get_output_count = env->GetMethodID(device_info_class, "getOutputPortCount", "()I");

  info.input_port_count = env->CallIntMethod(device_info, get_input_count);
  info.output_port_count = env->CallIntMethod(device_info, get_output_count);

  jobject bundle = env->CallObjectMethod(device_info, get_props_method);
  if (bundle)
  {
    auto bundle_class = env->FindClass("android/os/Bundle");
    auto get_string_method = env->GetMethodID(bundle_class, "getString", "(Ljava/lang/String;)Ljava/lang/String;");

    info.name = get_bundle_string(env, bundle, bundle_class, get_string_method, "name");
    info.manufacturer = get_bundle_string(env, bundle, bundle_class, get_string_method, "manufacturer");
    info.product = get_bundle_string(env, bundle, bundle_class, get_string_method, "product");
    info.serial_number = get_bundle_string(env, bundle, bundle_class, get_string_method, "serial_number");
    info.version = get_bundle_string(env, bundle, bundle_class, get_string_method, "version");

    env->DeleteLocalRef(bundle);
  }

  return info;
}

void context::cleanup_devices(JNIEnv* env)
{
  for (const auto& entry : midi_ports)
  {
    env->DeleteGlobalRef(entry.device_info);
  }
  midi_ports.clear();
}

void context::open_device(const midi_port_entry& port_entry, void* target, bool is_output)
{
  LOGI("open_device called, is_output=%d, target=%p, port_index=%d", is_output, target, port_entry.port_index);

  auto env = get_thread_env();
  if (!env) {
    LOGE("open_device: failed to get JNI env");
    return;
  }

  auto ctx = get_context(env);
  if (!ctx)
    return;

  auto midi_mgr = get_midi_manager(env, ctx);
  if (!midi_mgr)
    return;

  // Store port index for use in the callback
  pending_port_index = port_entry.port_index;

  // Check if we're on the main thread
  auto looper_class = env->FindClass("android/os/Looper");
  auto get_my_looper_method
      = env->GetStaticMethodID(looper_class, "myLooper", "()Landroid/os/Looper;");
  auto my_looper = env->CallStaticObjectMethod(looper_class, get_my_looper_method);

  jobject handler = nullptr;
  if (!my_looper)
  {
    LOGI("Not on a Looper thread, using main looper");
    auto get_main_looper_method
        = env->GetStaticMethodID(looper_class, "getMainLooper", "()Landroid/os/Looper;");
    auto main_looper = env->CallStaticObjectMethod(looper_class, get_main_looper_method);

    auto handler_class = env->FindClass("android/os/Handler");
    auto handler_ctor = env->GetMethodID(handler_class, "<init>", "(Landroid/os/Looper;)V");
    handler = env->NewObject(handler_class, handler_ctor, main_looper);

    env->DeleteLocalRef(main_looper);
  }

  // PATCHED: Use libremidi_find_class instead of env->FindClass for app classes
  LOGI("open_device: looking for MidiDeviceCallback class");
  auto callback_class = libremidi_find_class(env, "dev/celtera/libremidi/MidiDeviceCallback");
  if (!callback_class)
  {
    LOGE("MidiDeviceCallback class not found - ensure the Java class is loaded");
    if (handler)
      env->DeleteLocalRef(handler);
    return;
  }
  LOGI("open_device: MidiDeviceCallback class found");

  auto callback_ctor = env->GetMethodID(callback_class, "<init>", "(JZ)V");
  if (!callback_ctor) {
    LOGE("open_device: MidiDeviceCallback constructor not found");
    if (handler)
      env->DeleteLocalRef(handler);
    return;
  }

  auto callback
      = env->NewObject(callback_class, callback_ctor, (jlong)target, (jboolean)is_output);
  LOGI("open_device: MidiDeviceCallback object created");

  // Open the device
  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto open_device_method = env->GetMethodID(
      midi_mgr_class, "openDevice",
      "(Landroid/media/midi/MidiDeviceInfo;Landroid/media/midi/"
      "MidiManager$OnDeviceOpenedListener;Landroid/os/Handler;)V");

  env->CallVoidMethod(midi_mgr, open_device_method, port_entry.device_info, callback, handler);

  env->DeleteLocalRef(callback);
  if (handler)
    env->DeleteLocalRef(handler);
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpened(
    JNIEnv* env, jobject /*thiz*/, jobject midi_device, jlong target_ptr, jboolean is_output)
{
  LOGI("onDeviceOpened callback received! target_ptr=%lld, is_output=%d", (long long)target_ptr, is_output);

  if (!midi_device || target_ptr == 0)
  {
    LOGE("Invalid device or target pointer in callback");
    return;
  }

  AMidiDevice* amidi_device = nullptr;
  AMidiDevice_fromJava(env, midi_device, &amidi_device);

  if (!amidi_device)
  {
    LOGE("Failed to convert Java MIDI device to AMidiDevice");
    return;
  }

  if (is_output)
  {
    midi_out::open_callback(reinterpret_cast<midi_out*>(target_ptr), amidi_device);
  }
  else
  {
    midi_in::open_callback(reinterpret_cast<midi_in*>(target_ptr), amidi_device);
  }
}

// =============================================================================
// Hotplug support
// =============================================================================

jobject context::register_device_callback(JNIEnv* env, jobject midi_manager, void* observer_ptr)
{
  LOGI("Registering device callback for observer %p", observer_ptr);

  // Find our callback class using the cached ClassLoader
  auto callback_class = libremidi_find_class(env, "dev/celtera/libremidi/MidiDeviceStatusCallback");
  if (!callback_class)
  {
    LOGE("MidiDeviceStatusCallback class not found");
    return nullptr;
  }

  // Create the callback object
  auto callback_ctor = env->GetMethodID(callback_class, "<init>", "(J)V");
  if (!callback_ctor)
  {
    LOGE("MidiDeviceStatusCallback constructor not found");
    return nullptr;
  }

  jobject callback = env->NewObject(callback_class, callback_ctor, (jlong)observer_ptr);
  if (!callback)
  {
    LOGE("Failed to create MidiDeviceStatusCallback");
    return nullptr;
  }

  // Register the callback with MidiManager
  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto register_method = env->GetMethodID(
      midi_mgr_class, "registerDeviceCallback",
      "(Landroid/media/midi/MidiManager$DeviceCallback;Landroid/os/Handler;)V");

  if (!register_method)
  {
    LOGE("registerDeviceCallback method not found");
    env->DeleteLocalRef(callback);
    return nullptr;
  }

  // Use main looper for the handler
  auto looper_class = env->FindClass("android/os/Looper");
  auto get_main_looper = env->GetStaticMethodID(looper_class, "getMainLooper", "()Landroid/os/Looper;");
  auto main_looper = env->CallStaticObjectMethod(looper_class, get_main_looper);

  auto handler_class = env->FindClass("android/os/Handler");
  auto handler_ctor = env->GetMethodID(handler_class, "<init>", "(Landroid/os/Looper;)V");
  jobject handler = env->NewObject(handler_class, handler_ctor, main_looper);

  env->CallVoidMethod(midi_manager, register_method, callback, handler);

  env->DeleteLocalRef(handler);
  env->DeleteLocalRef(main_looper);

  // Return a global ref to keep the callback alive
  jobject global_callback = env->NewGlobalRef(callback);
  env->DeleteLocalRef(callback);

  LOGI("Device callback registered successfully");
  return global_callback;
}

void context::unregister_device_callback(JNIEnv* env, jobject midi_manager, jobject callback)
{
  if (!callback)
    return;

  LOGI("Unregistering device callback");

  // Invalidate the callback first
  auto callback_class = env->GetObjectClass(callback);
  auto invalidate_method = env->GetMethodID(callback_class, "invalidate", "()V");
  if (invalidate_method)
  {
    env->CallVoidMethod(callback, invalidate_method);
  }

  // Unregister from MidiManager
  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto unregister_method = env->GetMethodID(
      midi_mgr_class, "unregisterDeviceCallback",
      "(Landroid/media/midi/MidiManager$DeviceCallback;)V");

  if (unregister_method)
  {
    env->CallVoidMethod(midi_manager, unregister_method, callback);
  }

  // Delete the global ref
  env->DeleteGlobalRef(callback);
}

// Storage for active observer that needs hotplug notifications
// Note: Only one observer can receive hotplug notifications at a time
static void* g_active_hotplug_observer = nullptr;
static hotplug_callback_t g_on_device_added = nullptr;
static hotplug_callback_t g_on_device_removed = nullptr;

void set_hotplug_observer(void* observer, hotplug_callback_t on_added, hotplug_callback_t on_removed)
{
  g_active_hotplug_observer = observer;
  g_on_device_added = on_added;
  g_on_device_removed = on_removed;
}

void clear_hotplug_observer()
{
  g_active_hotplug_observer = nullptr;
  g_on_device_added = nullptr;
  g_on_device_removed = nullptr;
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceStatusCallback_onDeviceAddedNative(
    JNIEnv* env, jobject /*thiz*/, jlong observer_ptr, jobject device_info)
{
  LOGI("Device added callback, observer_ptr=%lld", (long long)observer_ptr);

  if (observer_ptr != 0 && g_on_device_added && (void*)observer_ptr == g_active_hotplug_observer)
  {
    g_on_device_added((void*)observer_ptr);
  }
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceStatusCallback_onDeviceRemovedNative(
    JNIEnv* env, jobject /*thiz*/, jlong observer_ptr, jobject device_info)
{
  LOGI("Device removed callback, observer_ptr=%lld", (long long)observer_ptr);

  if (observer_ptr != 0 && g_on_device_removed && (void*)observer_ptr == g_active_hotplug_observer)
  {
    g_on_device_removed((void*)observer_ptr);
  }
}

}

#endif // __ANDROID__
