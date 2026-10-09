// Patched version of libremidi/backends/android/helpers.cpp
// Uses cached ClassLoader for FindClass to work from any thread
// Adds extended port info functions (manufacturer, product, serial)

#if defined(__ANDROID__)

#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/backends/android/midi_in.hpp>
#include <libremidi/backends/android/midi_out.hpp>

#include <deque>
#include <iterator>
#include <condition_variable>
#include <map>
#include <mutex>

#include <pthread.h>

// Declaration of our custom FindClass function from jni_shim.cpp
extern "C" jclass libremidi_find_class(JNIEnv* env, const char* name);

namespace
{
// Scoped JNI local reference frame.
//
// Every FindClass / CallObjectMethod / GetObjectClass result is a local
// reference. Local references are normally reclaimed when the Java frame that
// created them returns — but these helpers run on native FFI threads that
// get_thread_env() attaches to the JVM and that then stay attached, so there is
// no returning Java frame and nothing is ever reclaimed. The class references
// below (several per call) therefore accumulate: a poll loop calling refresh()
// eventually hits ART's local reference table limit and aborts the process with
// "local reference table overflow (max=512)".
//
// PushLocalFrame bounds each helper: everything created inside is released when
// the frame pops. Global references (NewGlobalRef) are unaffected by frames.
class local_frame
{
public:
  explicit local_frame(JNIEnv* env, jint capacity = 16)
      : env_{env}
  {
    pushed_ = env_->PushLocalFrame(capacity) == JNI_OK;
  }

  ~local_frame()
  {
    if (pushed_)
      env_->PopLocalFrame(nullptr);
  }

  // Pops the frame but keeps `result` alive as a local reference in the
  // caller's frame. Use for helpers that hand a jobject back out.
  jobject pop_keeping(jobject result)
  {
    if (!pushed_)
      return result;
    pushed_ = false;
    return env_->PopLocalFrame(result);
  }

  local_frame(const local_frame&) = delete;
  local_frame& operator=(const local_frame&) = delete;

private:
  JNIEnv* env_;
  bool pushed_;
};
}

NAMESPACE_LIBREMIDI::android
{
// Main source of knowledge is RtMidi implementation
// which was done by Yellow Labrador

namespace
{
// Threads that get_thread_env() attached to the JVM must be detached again
// before they exit; ART requires it. These are native threads we do not own
// (Dart worker threads, which the VM retires when idle), so there is no
// shutdown hook to call DetachCurrentThread from. A pthread key destructor
// runs on the exiting thread itself, which is the documented way to do it.
pthread_key_t& detach_key()
{
  static pthread_key_t key;
  return key;
}

void detach_on_thread_exit(void* vm)
{
  static_cast<JavaVM*>(vm)->DetachCurrentThread();
}

bool init_detach_key()
{
  static const bool ok = pthread_key_create(&detach_key(), detach_on_thread_exit) == 0;
  return ok;
}
}

JNIEnv* context::get_thread_env()
{
  JNIEnv* env = nullptr;
  JavaVM* jvm = nullptr;
  jsize count = 0;
  if (JNI_GetCreatedJavaVMs(&jvm, 1, &count) != JNI_OK || count < 1)
  {
    LOGE("No JVM found");
    return nullptr;
  }

  const jint status = jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
  if (status == JNI_OK)
    return env;

  if (status != JNI_EDETACHED)
  {
    LOGE("GetEnv failed (%d)", (int)status);
    return nullptr;
  }

  if (!init_detach_key())
  {
    // Attaching without a way to detach would abort the process when this
    // thread exits, so refuse instead.
    LOGE("Cannot register thread-exit detach");
    return nullptr;
  }

  if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK)
  {
    LOGE("Failed to attach thread");
    return nullptr;
  }

  if (pthread_setspecific(detach_key(), jvm) != 0)
  {
    LOGE("Cannot register thread-exit detach");
    jvm->DetachCurrentThread();
    return nullptr;
  }

  return env;
}

// Returns the Application as a global reference that lives for the rest of
// the process; callers must not delete it. Handing out a fresh local reference
// per call leaked one reference per enumeration on threads that never return
// to Java, until ART's local reference table overflowed.
jobject context::get_context(JNIEnv* env)
{
  static std::mutex mutex;
  static jobject cached = nullptr;

  std::lock_guard<std::mutex> lock(mutex);
  if (cached)
    return cached;

  local_frame frame{env};

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

  cached = env->NewGlobalRef(app);
  return cached;
}

jobject context::get_midi_manager(JNIEnv* env, jobject ctx)
{
  local_frame frame{env};

  auto context_class = env->FindClass("android/content/Context");
  auto get_system_service = env->GetMethodID(
      context_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
  auto midi_service_str = env->NewStringUTF("midi");
  auto service = env->CallObjectMethod(ctx, get_system_service, midi_service_str);
  return frame.pop_keeping(service);
}

std::recursive_mutex& context::ports_mutex()
{
  static std::recursive_mutex m;
  return m;
}

void context::refresh_midi_devices(JNIEnv* env, jobject ctx, bool is_output)
{
  local_frame frame{env};
  std::lock_guard<std::recursive_mutex> lock(ports_mutex());

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
  std::lock_guard<std::recursive_mutex> lock(context::ports_mutex());
  if (port_number >= midi_ports.size())
  {
    LOGE("Invalid port number");
    return "";
  }

  local_frame frame{env};
  const auto& entry = midi_ports[port_number];

  auto device_info_class = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_props_method
      = env->GetMethodID(device_info_class, "getProperties", "()Landroid/os/Bundle;");
  auto bundle = env->CallObjectMethod(entry.device_info, get_props_method);

  auto bundle_class = env->FindClass("android/os/Bundle");
  auto get_string_method
      = env->GetMethodID(bundle_class, "getString", "(Ljava/lang/String;)Ljava/lang/String;");

  std::string result = get_bundle_string(env, bundle, bundle_class, get_string_method, "name");
  // MidiDeviceService apps often declare only manufacturer/product (Google's
  // own sample does), which left the port named just " Port 1".
  if (result.empty())
    result = get_bundle_string(env, bundle, bundle_class, get_string_method, "product");
  if (result.empty())
    result = get_bundle_string(env, bundle, bundle_class, get_string_method, "manufacturer");
  if (result.empty())
    result = "MIDI Device";
  // Append port index to distinguish multiple ports on same device (1-based for display)
  result += " Port " + std::to_string(entry.port_index + 1);

  env->DeleteLocalRef(bundle);

  return result;
}

std::string context::port_manufacturer(JNIEnv* env, unsigned int port_number)
{
  std::lock_guard<std::recursive_mutex> lock(context::ports_mutex());
  if (port_number >= midi_ports.size())
    return "";

  local_frame frame{env};
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
  std::lock_guard<std::recursive_mutex> lock(context::ports_mutex());
  if (port_number >= midi_ports.size())
    return "";

  local_frame frame{env};
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
  std::lock_guard<std::recursive_mutex> lock(context::ports_mutex());
  if (port_number >= midi_ports.size())
    return "";

  local_frame frame{env};
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
  std::lock_guard<std::recursive_mutex> lock(context::ports_mutex());
  if (port_number >= midi_ports.size())
    return 0;

  local_frame frame{env};
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
  std::lock_guard<std::recursive_mutex> lock(context::ports_mutex());
  android_port_info info = {};

  if (port_number >= context::midi_ports.size())
  {
    LOGE("Invalid port number for get_port_info");
    return info;
  }

  local_frame frame{env};
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
  std::lock_guard<std::recursive_mutex> lock(ports_mutex());
  for (const auto& entry : midi_ports)
  {
    env->DeleteGlobalRef(entry.device_info);
  }
  midi_ports.clear();
}

// Pending asynchronous opens, keyed by an opaque token handed to Java.
// See the comment on pending_open in helpers.hpp for why Java must not get a
// raw pointer.
namespace
{
std::mutex& pending_opens_mutex()
{
  static std::mutex m;
  return m;
}

struct pending_record
{
  pending_open open;
  bool in_flight;  // a callback is currently using open.target
};

std::map<int64_t, pending_record>& pending_opens()
{
  static std::map<int64_t, pending_record> m;
  return m;
}
}

std::condition_variable& pending_opens_cv()
{
  static std::condition_variable cv;
  return cv;
}

int64_t register_pending_open(void* target, int port_index, bool is_output)
{
  static int64_t next_token = 1;
  std::lock_guard<std::mutex> lock(pending_opens_mutex());
  // Reuse of a token would let a stale callback resolve to a new requester, so
  // never wrap: int64 at one open per microsecond lasts longer than any device.
  const int64_t token = next_token++;
  pending_opens()[token] = pending_record{{target, port_index, is_output}, false};
  return token;
}

void cancel_pending_open(void* target)
{
  std::unique_lock<std::mutex> lock(pending_opens_mutex());
  auto& map = pending_opens();
  for (;;)
  {
    bool waiting = false;
    for (auto it = map.begin(); it != map.end();)
    {
      if (it->second.open.target != target)
      {
        ++it;
      }
      else if (it->second.in_flight)
      {
        // The callback is already inside open_callback() with this target.
        // Erasing here would let the caller destroy the object while the
        // callback still writes to it, which is the use-after-free this whole
        // token mechanism exists to prevent. Wait it out instead.
        waiting = true;
        ++it;
      }
      else
      {
        it = map.erase(it);
      }
    }
    if (!waiting)
      return;
    pending_opens_cv().wait(lock);
  }
}

bool take_pending_open(int64_t token, pending_open& out)
{
  std::lock_guard<std::mutex> lock(pending_opens_mutex());
  auto& map = pending_opens();
  auto it = map.find(token);
  if (it == map.end())
    return false;
  // Pin the entry for the duration of the callback rather than erasing it now:
  // between taking it and using target, a concurrent close_port() would
  // otherwise find nothing to cancel and destroy the object underneath us.
  it->second.in_flight = true;
  out = it->second.open;
  return true;
}

void finish_pending_open(int64_t token)
{
  {
    std::lock_guard<std::mutex> lock(pending_opens_mutex());
    pending_opens().erase(token);
  }
  pending_opens_cv().notify_all();
}

// Hands one open request to MidiManager.openDevice(). Returns false when
// nothing reached Java, i.e. no callback will arrive for this token; the token
// is left registered so the caller can resolve it.
static bool issue_open_device(JNIEnv* env, jobject device_info, int64_t token, bool is_output)
{
  LOGI("open_device: issuing, is_output=%d, token=%lld", is_output, (long long)token);

  local_frame frame{env};

  auto ctx = context::get_context(env);
  if (!ctx)
  {
    return false;
  }

  auto midi_mgr = context::get_midi_manager(env, ctx);
  if (!midi_mgr)
  {
    return false;
  }

  // Check if we're on the main thread.
  //
  // These are Android system classes, so a lookup failing here means the JVM is
  // in a state where nothing works anyway. Check anyway: a failed lookup leaves
  // a pending exception, and calling further JNI functions with one pending is
  // undefined. The callback is delivered on this handler, so continuing with a
  // half-built one would produce an open that never completes.
  auto looper_class = env->FindClass("android/os/Looper");
  auto get_my_looper_method
      = looper_class
            ? env->GetStaticMethodID(looper_class, "myLooper", "()Landroid/os/Looper;")
            : nullptr;
  if (!get_my_looper_method)
  {
    LOGE("open_device: android.os.Looper.myLooper not found");
    env->ExceptionClear();
    return false;
  }
  auto my_looper = env->CallStaticObjectMethod(looper_class, get_my_looper_method);

  jobject handler = nullptr;
  if (!my_looper)
  {
    LOGI("Not on a Looper thread, using main looper");
    auto get_main_looper_method
        = env->GetStaticMethodID(looper_class, "getMainLooper", "()Landroid/os/Looper;");
    auto handler_class = env->FindClass("android/os/Handler");
    auto handler_ctor
        = handler_class ? env->GetMethodID(handler_class, "<init>", "(Landroid/os/Looper;)V")
                        : nullptr;
    if (!get_main_looper_method || !handler_ctor)
    {
      LOGE("open_device: could not reach Handler(Looper)");
      env->ExceptionClear();
        return false;
    }

    auto main_looper = env->CallStaticObjectMethod(looper_class, get_main_looper_method);
    handler = env->NewObject(handler_class, handler_ctor, main_looper);
    if (main_looper)
      env->DeleteLocalRef(main_looper);

    if (!handler)
    {
      LOGE("open_device: could not construct the main-looper Handler");
      env->ExceptionClear();
        return false;
    }
  }

  // PATCHED: Use libremidi_find_class instead of env->FindClass for app classes
  LOGI("open_device: looking for MidiDeviceCallback class");
  auto callback_class = libremidi_find_class(env, "dev/celtera/libremidi/MidiDeviceCallback");
  if (!callback_class)
  {
    LOGE("MidiDeviceCallback class not found - ensure the Java class is loaded");
    if (handler)
      env->DeleteLocalRef(handler);
    return false;
  }
  LOGI("open_device: MidiDeviceCallback class found");

  auto callback_ctor = env->GetMethodID(callback_class, "<init>", "(JZ)V");
  if (!callback_ctor) {
    LOGE("open_device: MidiDeviceCallback constructor not found");
    // GetMethodID leaves a NoSuchMethodError pending on failure.
    env->ExceptionClear();
    if (handler)
      env->DeleteLocalRef(handler);
    return false;
  }

  auto callback
      = env->NewObject(callback_class, callback_ctor, (jlong)token, (jboolean)is_output);
  if (!callback)
  {
    LOGE("open_device: could not construct MidiDeviceCallback");
    env->ExceptionClear();
    if (handler)
      env->DeleteLocalRef(handler);
    return false;
  }
  LOGI("open_device: MidiDeviceCallback object created");

  // Open the device
  auto midi_mgr_class = env->FindClass("android/media/midi/MidiManager");
  auto open_device_method
      = midi_mgr_class ? env->GetMethodID(
            midi_mgr_class, "openDevice",
            "(Landroid/media/midi/MidiDeviceInfo;Landroid/media/midi/"
            "MidiManager$OnDeviceOpenedListener;Landroid/os/Handler;)V")
                       : nullptr;
  if (!open_device_method)
  {
    LOGE("open_device: MidiManager.openDevice not found");
    env->ExceptionClear();
    env->DeleteLocalRef(callback);
    if (handler)
      env->DeleteLocalRef(handler);
    return false;
  }

  env->CallVoidMethod(midi_mgr, open_device_method, device_info, callback, handler);

  // A throwing openDevice() means no callback will ever arrive, so this has to
  // be reported as a failed open rather than left pending.
  const bool threw = env->ExceptionCheck();
  if (threw)
  {
    LOGE("open_device: MidiManager.openDevice threw");
    env->ExceptionDescribe();
    env->ExceptionClear();
  }

  env->DeleteLocalRef(callback);
  if (handler)
    env->DeleteLocalRef(handler);

  if (threw)
  {
    return false;
  }
  return true;
}

// Opens are serialised per device.
//
// For a device whose MidiDeviceService is not bound yet — virtual devices and
// Bluetooth MIDI — MidiService binds the service on the first connection and
// rejects every further one until the bind completes ("No way to connect to
// device in addDeviceConnection"), handing back a null device. Opening the
// input and output of a Bluetooth controller together therefore lost one of
// them. A request for a device that already has an open in flight now waits
// until that open has called back; by then the service is bound and the next
// connection is accepted. USB devices are always bound, so for them this only
// orders the requests.
namespace
{
struct queued_open
{
  int64_t token;
  jobject device_info;  // own global ref, the port cache may be rebuilt meanwhile
  bool is_output;
};

struct device_queue
{
  std::deque<queued_open> waiting;
};

std::mutex& open_queue_mutex()
{
  static std::mutex m;
  return m;
}

// Keyed by MidiDeviceInfo.getId(). An entry exists while an open for that
// device is in flight.
std::map<jint, device_queue>& open_queues()
{
  static std::map<jint, device_queue> m;
  return m;
}

// The device each in-flight token belongs to.
std::map<int64_t, jint>& in_flight_device()
{
  static std::map<int64_t, jint> m;
  return m;
}

jint device_id(JNIEnv* env, jobject device_info)
{
  local_frame frame{env};
  auto cls = env->FindClass("android/media/midi/MidiDeviceInfo");
  auto get_id = cls ? env->GetMethodID(cls, "getId", "()I") : nullptr;
  if (!get_id)
  {
    env->ExceptionClear();
    return -1;
  }
  return env->CallIntMethod(device_info, get_id);
}

bool is_pending(int64_t token)
{
  std::lock_guard<std::mutex> lock(pending_opens_mutex());
  return pending_opens().count(token) != 0;
}

void close_java_device(JNIEnv* env, jobject java_device)
{
  if (!java_device)
    return;
  local_frame frame{env};
  auto cls = env->GetObjectClass(java_device);
  auto close = cls ? env->GetMethodID(cls, "close", "()V") : nullptr;
  if (close)
    env->CallVoidMethod(java_device, close);
  // close() declares IOException; nothing useful to do with it here.
  if (env->ExceptionCheck())
  {
    LOGW("MidiDevice.close() threw");
    env->ExceptionClear();
  }
}

// Resolves a token with the result of its open: hands the device to the
// requester, or tells it the open failed, or — if the requester is gone —
// closes the device again. `midi_device` may be null for a failed open.
void deliver_open_result(JNIEnv* env, int64_t token, jobject midi_device)
{
  pending_open pending{};
  if (!take_pending_open(token, pending))
  {
    // The midi_in/midi_out that asked for this device has been closed in the
    // meantime, so there is nothing to call back into. Close the connection,
    // otherwise MidiService keeps it until the process dies.
    LOGI("onDeviceOpened: request was cancelled, closing the device");
    close_java_device(env, midi_device);
    return;
  }

  auto fail = [&] {
    if (pending.is_output)
      midi_out::open_failed(reinterpret_cast<midi_out*>(pending.target));
    else
      midi_in::open_failed(reinterpret_cast<midi_in*>(pending.target));
    finish_pending_open(token);
  };

  // MidiManager reports a failed open by passing null. The requester is pinned
  // for the duration of this call, so clearing its state here is safe.
  if (!midi_device)
  {
    LOGE("onDeviceOpened: MidiManager returned no device");
    fail();
    return;
  }

  AMidiDevice* amidi_device = nullptr;
  AMidiDevice_fromJava(env, midi_device, &amidi_device);
  jobject java_device = amidi_device ? env->NewGlobalRef(midi_device) : nullptr;
  if (!amidi_device || !java_device)
  {
    LOGE("Failed to convert Java MIDI device to AMidiDevice");
    if (amidi_device)
      AMidiDevice_release(amidi_device);
    close_java_device(env, midi_device);
    fail();
    return;
  }

  // The port index comes from the token, so overlapping opens cannot steal each
  // other's target port.
  if (pending.is_output)
    midi_out::open_callback(
        reinterpret_cast<midi_out*>(pending.target), amidi_device, java_device,
        pending.port_index);
  else
    midi_in::open_callback(
        reinterpret_cast<midi_in*>(pending.target), amidi_device, java_device,
        pending.port_index);

  // Unpin: from here on close_port() may destroy the object.
  finish_pending_open(token);
}

// Called once the open for `token` has concluded, however it ended. Issues the
// next waiting request for the same device, if any.
void advance_open_queue(JNIEnv* env, int64_t token)
{
  for (;;)
  {
    queued_open next{};
    {
      std::lock_guard<std::mutex> lock(open_queue_mutex());
      auto dev = in_flight_device().find(token);
      if (dev == in_flight_device().end())
        return;
      const jint id = dev->second;
      in_flight_device().erase(dev);

      auto q = open_queues().find(id);
      if (q == open_queues().end())
        return;
      if (q->second.waiting.empty())
      {
        open_queues().erase(q);
        return;
      }
      next = q->second.waiting.front();
      q->second.waiting.pop_front();
      in_flight_device()[next.token] = id;
    }

    bool issued = false;
    if (is_pending(next.token))
      issued = issue_open_device(env, next.device_info, next.token, next.is_output);
    else
      LOGI("open_device: queued request %lld was cancelled", (long long)next.token);
    env->DeleteGlobalRef(next.device_info);

    if (issued)
      return;

    // Nothing reached Java for this one, so no callback will advance the
    // queue: resolve it as failed here and move on to the next request.
    deliver_open_result(env, next.token, nullptr);
    token = next.token;
  }
}
}

bool context::open_device(const midi_port_entry& port_entry, int64_t token, bool is_output)
{
  LOGI("open_device called, is_output=%d, token=%lld, port_index=%d", is_output,
       (long long)token, port_entry.port_index);

  auto env = get_thread_env();
  if (!env)
  {
    LOGE("open_device: failed to get JNI env");
    finish_pending_open(token);
    return false;
  }

  const jint id = device_id(env, port_entry.device_info);
  {
    std::lock_guard<std::mutex> lock(open_queue_mutex());
    auto q = open_queues().find(id);
    if (q != open_queues().end())
    {
      jobject info = env->NewGlobalRef(port_entry.device_info);
      if (!info)
      {
        env->ExceptionClear();
        finish_pending_open(token);
        return false;
      }
      LOGI("open_device: device %d busy, queueing token %lld", (int)id, (long long)token);
      q->second.waiting.push_back({token, info, is_output});
      return true;
    }
    open_queues()[id];
    in_flight_device()[token] = id;
  }

  if (issue_open_device(env, port_entry.device_info, token, is_output))
    return true;

  // Nothing was handed to Java. The caller resets its own state when this
  // returns false; release the token and let anything queued meanwhile go.
  finish_pending_open(token);
  advance_open_queue(env, token);
  return false;
}

void context::close_device(AMidiDevice* device, jobject java_device)
{
  if (device)
    AMidiDevice_release(device);
  if (!java_device)
    return;

  if (auto env = get_thread_env())
  {
    close_java_device(env, java_device);
    env->DeleteGlobalRef(java_device);
  }
  else
  {
    LOGE("close_device: no JNI env, MidiDevice left open");
  }
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceCallback_onDeviceOpened(
    JNIEnv* env, jobject /*thiz*/, jobject midi_device, jlong token, jboolean /*is_output*/)
{
  LOGI("onDeviceOpened callback received! token=%lld", (long long)token);

  if (token == 0)
  {
    LOGE("Invalid token in callback");
    return;
  }

  deliver_open_result(env, token, midi_device);
  advance_open_queue(env, token);
}

// =============================================================================
// Hotplug support
// =============================================================================

jobject context::register_device_callback(JNIEnv* env, jobject midi_manager, void* observer_ptr)
{
  LOGI("Registering device callback for observer %p", observer_ptr);

  local_frame frame{env};

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
    // GetMethodID leaves a NoSuchMethodError pending on failure; returning
    // with it pending aborts the process on the next JNI call.
    env->ExceptionClear();
    return nullptr;
  }

  jobject callback = env->NewObject(callback_class, callback_ctor, (jlong)observer_ptr);
  if (!callback)
  {
    LOGE("Failed to create MidiDeviceStatusCallback");
    env->ExceptionClear();
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
    env->ExceptionClear();
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

  if (env->ExceptionCheck())
  {
    LOGE("registerDeviceCallback threw");
    env->ExceptionClear();
    env->DeleteLocalRef(callback);
    return nullptr;
  }

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

  local_frame frame{env};

  // Invalidate the callback first
  auto callback_class = env->GetObjectClass(callback);
  auto invalidate_method = env->GetMethodID(callback_class, "invalidate", "()V");
  if (invalidate_method)
  {
    env->CallVoidMethod(callback, invalidate_method);
  }
  else
  {
    LOGE("MidiDeviceStatusCallback.invalidate not found");
    env->ExceptionClear();
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

// Registered hotplug observers.
//
// This used to be three process-wide variables holding *one* observer and its
// callbacks. The public API lets an application create any number of
// MidiObserver.withHotplug() instances, so the last one created silently took
// over every event, and destroying any observer cleared the state belonging to
// the others. The values were also read from the Java callback thread while
// being written from FFI threads, with no synchronisation at all.
//
// A map keyed by the observer pointer fixes both: every registered observer
// gets its own entry, and a callback is dispatched only to the observer it was
// registered for. Entries are copied out under the lock before being invoked so
// a callback cannot run while the map is being modified.
namespace
{
struct hotplug_entry
{
  hotplug_callback_t on_added;
  hotplug_callback_t on_removed;
};

std::mutex& hotplug_mutex()
{
  static std::mutex m;
  return m;
}

std::map<void*, hotplug_entry>& hotplug_observers()
{
  static std::map<void*, hotplug_entry> m;
  return m;
}
}

void set_hotplug_observer(void* observer, hotplug_callback_t on_added, hotplug_callback_t on_removed)
{
  if (!observer)
    return;
  std::lock_guard<std::mutex> lock(hotplug_mutex());
  hotplug_observers()[observer] = hotplug_entry{on_added, on_removed};
}

void clear_hotplug_observer(void* observer)
{
  std::lock_guard<std::mutex> lock(hotplug_mutex());
  hotplug_observers().erase(observer);
}

namespace
{
// Returns the callback registered for `observer`, or nullptr. Looking the
// observer up by the pointer Java hands back means a stale callback for an
// already-destroyed observer finds nothing instead of dispatching into it.
hotplug_callback_t lookup_hotplug(void* observer, bool added)
{
  std::lock_guard<std::mutex> lock(hotplug_mutex());
  auto& map = hotplug_observers();
  auto it = map.find(observer);
  if (it == map.end())
    return nullptr;
  return added ? it->second.on_added : it->second.on_removed;
}
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceStatusCallback_onDeviceAddedNative(
    JNIEnv* env, jobject /*thiz*/, jlong observer_ptr, jobject device_info)
{
  LOGI("Device added callback, observer_ptr=%lld", (long long)observer_ptr);

  if (observer_ptr == 0)
    return;

  if (auto cb = lookup_hotplug((void*)observer_ptr, true))
  {
    cb((void*)observer_ptr);
  }
}

extern "C" JNIEXPORT void JNICALL Java_dev_celtera_libremidi_MidiDeviceStatusCallback_onDeviceRemovedNative(
    JNIEnv* env, jobject /*thiz*/, jlong observer_ptr, jobject device_info)
{
  LOGI("Device removed callback, observer_ptr=%lld", (long long)observer_ptr);

  if (observer_ptr == 0)
    return;

  if (auto cb = lookup_hotplug((void*)observer_ptr, false))
  {
    cb((void*)observer_ptr);
  }
}

}

#endif // __ANDROID__
