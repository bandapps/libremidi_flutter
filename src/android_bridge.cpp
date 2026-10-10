// JNI bridge between the Android MIDI backend and dev.celtera.libremidi.MidiBridge.
// See src/libremidi/backends/android/helpers.hpp for the contract.
//
// Everything JNI lives in this file: the JavaVM, thread attachment, the
// MidiBridge class and method ids, and all Java references. Nothing here looks
// classes up by name after JNI_OnLoad, so it works from any thread, and the one
// native method is bound with RegisterNatives rather than by symbol name.

#if defined(__ANDROID__)

#include <libremidi/backends/android/helpers.hpp>

#include <amidi/AMidi.h>
#include <jni.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <utility>

NAMESPACE_LIBREMIDI::android
{
namespace
{
constexpr jlong kOpenTimeoutMs = 5000;

// ---------------------------------------------------------------------------
// JavaVM, class and method ids — set once in JNI_OnLoad.

std::atomic<JavaVM*> g_vm{nullptr};
jclass g_bridge = nullptr;  // global ref
jmethodID g_list_ports = nullptr;
jmethodID g_open_device = nullptr;
jmethodID g_close_device = nullptr;
jmethodID g_is_available = nullptr;

// ---------------------------------------------------------------------------
// Thread attachment.
//
// Threads that call in from native code (Dart isolates, the input reader) are
// attached on first use and detached when they exit; ART aborts the process if
// an attached thread exits without detaching. These are threads we do not own,
// so a pthread key destructor — which runs on the exiting thread — is the only
// place to do it.

pthread_key_t g_detach_key;

void detach_on_exit(void*)
{
  if (auto vm = g_vm.load(std::memory_order_acquire))
    vm->DetachCurrentThread();
}

bool init_detach_key()
{
  static const bool ok = pthread_key_create(&g_detach_key, detach_on_exit) == 0;
  return ok;
}

JNIEnv* env()
{
  JavaVM* vm = g_vm.load(std::memory_order_acquire);
  if (!vm)
    return nullptr;

  JNIEnv* e = nullptr;
  const jint status = vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
  if (status == JNI_OK)
    return e;
  if (status != JNI_EDETACHED || !init_detach_key())
    return nullptr;
  if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK)
    return nullptr;
  // The key's value only has to be non-null for the destructor to run.
  if (pthread_setspecific(g_detach_key, vm) != 0)
  {
    // Without a way to detach later, do not stay attached.
    vm->DetachCurrentThread();
    return nullptr;
  }
  return e;
}

// A JNIEnv for the current thread for the lifetime of this object. Uses env()
// where it can; if the thread cannot be registered for detach-on-exit, it is
// attached only for this scope. For teardown paths that must not give up.
class scoped_env
{
public:
  scoped_env()
  {
    if ((e_ = env()))
      return;
    if (JavaVM* vm = g_vm.load(std::memory_order_acquire))
    {
      if (vm->AttachCurrentThread(&e_, nullptr) == JNI_OK)
        detach_ = vm;
      else
        e_ = nullptr;
    }
  }
  ~scoped_env()
  {
    if (detach_)
      detach_->DetachCurrentThread();
  }
  scoped_env(const scoped_env&) = delete;
  scoped_env& operator=(const scoped_env&) = delete;
  JNIEnv* get() const noexcept { return e_; }

private:
  JNIEnv* e_ = nullptr;
  JavaVM* detach_ = nullptr;
};

// Clears a pending Java exception; returns whether there was one.
bool clear_exception(JNIEnv* e)
{
  if (!e->ExceptionCheck())
    return false;
  e->ExceptionDescribe();
  e->ExceptionClear();
  return true;
}

// Owns one local reference. Threads attached from native code never return to
// Java, so their local references are only ever freed explicitly.
class local_ref
{
public:
  local_ref(JNIEnv* e, jobject o) : e_{e}, o_{o} {}
  ~local_ref()
  {
    if (o_)
      e_->DeleteLocalRef(o_);
  }
  local_ref(const local_ref&) = delete;
  local_ref& operator=(const local_ref&) = delete;
  jobject get() const noexcept { return o_; }
  explicit operator bool() const noexcept { return o_ != nullptr; }

private:
  JNIEnv* e_;
  jobject o_;
};

std::string to_string(JNIEnv* e, jstring s)
{
  if (!s)
    return {};
  const char* chars = e->GetStringUTFChars(s, nullptr);
  if (!chars)
  {
    clear_exception(e);
    return {};
  }
  std::string result{chars};
  e->ReleaseStringUTFChars(s, chars);
  return result;
}

int32_t to_int(const std::string& s)
{
  return static_cast<int32_t>(std::strtol(s.c_str(), nullptr, 10));
}

// ---------------------------------------------------------------------------
// Devices.

class lease;

// Device bookkeeping. A device id is "busy" while it is being opened, while it
// is open (an entry in g_devices), and while its last lease is closing it —
// the entry stays until the close has finished. open_device() waits out any
// busy state it cannot share: a new open overlapping the last close would race
// MidiService and AMidi on the same device.
std::mutex g_devices_mutex;
std::condition_variable g_devices_cv;
std::map<int32_t, std::weak_ptr<lease>> g_devices;
std::set<int32_t> g_opening;

class lease final : public device_lease
{
public:
  lease(int32_t id, AMidiDevice* device, jobject java_device)
      : id_{id}, device_{device}, java_device_{java_device}
  {
  }

  ~lease() override
  {
    // The last lease can be dropped on any thread — one that shared an already
    // open device and never needed JNI itself, or the cleanup thread. Attach it
    // first: AMidiDevice_release() aborts the process on a thread that is not
    // attached to the JVM.
    scoped_env attached;
    if (JNIEnv* e = attached.get())
    {
      AMidiDevice_release(device_);
      e->CallStaticVoidMethod(g_bridge, g_close_device, java_device_);
      clear_exception(e);
      e->DeleteGlobalRef(java_device_);
    }
    else
    {
      // Only when the JVM itself is gone, i.e. the process is ending.
      LOGE("closing MIDI device: the JVM is unreachable, device left open");
    }

    {
      std::lock_guard<std::mutex> lock(g_devices_mutex);
      g_devices.erase(id_);
    }
    g_devices_cv.notify_all();
  }

  AMidiDevice* device() const noexcept override { return device_; }

private:
  int32_t id_;
  AMidiDevice* device_;
  jobject java_device_;  // global ref
};

// Asks Java to open the device. Called without any lock held: it blocks until
// MidiManager has answered.
std::shared_ptr<lease> open_new(int32_t device_id, std::string& error)
{
  JNIEnv* e = env();
  if (!e || !g_bridge)
  {
    error = "MIDI is not initialised (the plugin's Java side did not load)";
    return nullptr;
  }

  local_ref result{e, e->CallStaticObjectMethod(g_bridge, g_open_device, jint(device_id), kOpenTimeoutMs)};
  if (clear_exception(e) || !result)
  {
    error = "opening the device failed in Java";
    return nullptr;
  }

  local_ref string_class{e, e->FindClass("java/lang/String")};
  if (string_class && e->IsInstanceOf(result.get(), static_cast<jclass>(string_class.get())))
  {
    error = to_string(e, static_cast<jstring>(result.get()));
    return nullptr;
  }
  clear_exception(e);

  AMidiDevice* device = nullptr;
  const auto status = AMidiDevice_fromJava(e, result.get(), &device);
  jobject java_device = (status == AMEDIA_OK && device) ? e->NewGlobalRef(result.get()) : nullptr;
  if (!java_device)
  {
    if (device)
      AMidiDevice_release(device);
    e->CallStaticVoidMethod(g_bridge, g_close_device, result.get());
    clear_exception(e);
    error = "the device could not be used from native code (" + std::to_string(status) + ")";
    return nullptr;
  }
  return std::make_shared<lease>(device_id, device, java_device);
}

class shared_send_port;
using send_port_key = std::pair<int32_t, int32_t>;  // device id, port index

// An entry stays here until its port is actually closed, so an open racing
// with the last close waits for it instead of hitting a still-open port.
std::mutex g_send_ports_mutex;
std::condition_variable g_send_ports_cv;
std::map<send_port_key, std::weak_ptr<shared_send_port>> g_send_ports;

class shared_send_port final : public send_port
{
public:
  shared_send_port(send_port_key key, std::shared_ptr<device_lease> device, AMidiInputPort* port)
      : key_{key}, device_{std::move(device)}, port_{port}
  {
  }

  ~shared_send_port() override
  {
    {
      std::lock_guard<std::mutex> lock(g_send_ports_mutex);
      AMidiInputPort_close(port_);
      g_send_ports.erase(key_);
    }
    g_send_ports_cv.notify_all();
  }

  // AMidiInputPort_send() may take fewer bytes than offered when the port's
  // buffer is full; success is only when all of them went out. A port that
  // stays full for about 100 ms counts as failed.
  bool send(const unsigned char* data, size_t size) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t sent = 0;
    int stalled = 0;
    while (sent < size)
    {
      const ssize_t n = AMidiInputPort_send(port_, data + sent, size - sent);
      if (n < 0)
        return false;
      if (n == 0)
      {
        if (++stalled > 100)
          return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      sent += static_cast<size_t>(n);
      stalled = 0;
    }
    return true;
  }

private:
  send_port_key key_;
  std::shared_ptr<device_lease> device_;  // keeps the device open
  AMidiInputPort* port_;
  std::mutex mutex_;  // serialises sends from the midi_outs sharing this port
};

// ---------------------------------------------------------------------------
// Hotplug.

std::mutex g_hotplug_mutex;  // held while callbacks run, see unregister_hotplug()
std::map<int, std::function<void()>> g_hotplug;
int g_hotplug_next = 0;

void JNICALL native_devices_changed(JNIEnv*, jclass)
{
  std::lock_guard<std::mutex> lock(g_hotplug_mutex);
  for (auto& [id, callback] : g_hotplug)
    callback();
}

thread_local std::string t_last_error;
}

// ---------------------------------------------------------------------------
// Public interface (helpers.hpp).

std::vector<port_desc> list_ports(bool inputs)
{
  std::vector<port_desc> ports;
  JNIEnv* e = env();
  if (!e || !g_bridge)
    return ports;

  local_ref array{e, e->CallStaticObjectMethod(g_bridge, g_list_ports, jboolean(inputs))};
  if (clear_exception(e) || !array)
    return ports;

  constexpr jsize kFields = 7;  // MidiBridge.FIELDS_PER_PORT
  const auto arr = static_cast<jobjectArray>(array.get());
  const jsize count = e->GetArrayLength(arr) / kFields;
  ports.reserve(count);
  for (jsize i = 0; i < count; ++i)
  {
    std::string f[kFields];
    for (jsize k = 0; k < kFields; ++k)
    {
      local_ref s{e, e->GetObjectArrayElement(arr, i * kFields + k)};
      f[k] = to_string(e, static_cast<jstring>(s.get()));
    }
    ports.push_back(port_desc{
        to_int(f[0]), to_int(f[1]), to_int(f[2]), std::move(f[3]), std::move(f[4]),
        std::move(f[5]), std::move(f[6])});
  }
  return ports;
}

std::shared_ptr<device_lease> open_device(int32_t device_id, std::string& error)
{
  std::unique_lock<std::mutex> lock(g_devices_mutex);
  for (;;)
  {
    if (auto it = g_devices.find(device_id); it != g_devices.end())
    {
      if (auto existing = it->second.lock())
        return existing;
      // The last lease is closing the device right now; its destructor
      // removes the entry and wakes us when done.
    }
    // Another thread is opening this device: wait and share its result. Two
    // overlapping opens would make MidiService reject the second one while it
    // is still binding a virtual or Bluetooth device.
    else if (!g_opening.count(device_id))
    {
      break;
    }
    g_devices_cv.wait(lock);
  }
  g_opening.insert(device_id);
  lock.unlock();

  auto opened = open_new(device_id, error);

  lock.lock();
  g_opening.erase(device_id);
  if (opened)
    g_devices[device_id] = opened;
  lock.unlock();
  g_devices_cv.notify_all();
  return opened;
}

std::shared_ptr<send_port>
open_send_port(int32_t device_id, int32_t port_index, std::string& error)
{
  // Opening the device can block for seconds, so it happens before taking the
  // port lock; opening the port itself is quick.
  auto device = open_device(device_id, error);
  if (!device)
    return nullptr;

  std::unique_lock<std::mutex> lock(g_send_ports_mutex);
  const send_port_key key{device_id, port_index};
  for (auto it = g_send_ports.find(key); it != g_send_ports.end(); it = g_send_ports.find(key))
  {
    if (auto existing = it->second.lock())
      return existing;
    // The last user just let go and the port is being closed; wait for it.
    g_send_ports_cv.wait(lock);
  }

  AMidiInputPort* port = nullptr;
  const auto status = AMidiInputPort_open(device->device(), port_index, &port);
  if (status != AMEDIA_OK || !port)
  {
    error = "the port could not be opened, it may be in use by another app ("
            + std::to_string(status) + ")";
    return nullptr;
  }
  auto shared = std::make_shared<shared_send_port>(key, std::move(device), port);
  g_send_ports[key] = shared;
  return shared;
}

bool midi_available()
{
  JNIEnv* e = env();
  if (!e || !g_bridge)
    return false;
  const jboolean available = e->CallStaticBooleanMethod(g_bridge, g_is_available);
  return !clear_exception(e) && available;
}

int register_hotplug(std::function<void()> callback)
{
  std::lock_guard<std::mutex> lock(g_hotplug_mutex);
  const int id = g_hotplug_next++;
  g_hotplug.emplace(id, std::move(callback));
  return id;
}

void unregister_hotplug(int id)
{
  std::lock_guard<std::mutex> lock(g_hotplug_mutex);
  g_hotplug.erase(id);
}

void set_last_error(std::string message)
{
  if (!message.empty())
    LOGE("%s", message.c_str());
  t_last_error = std::move(message);
}

const char* last_error() noexcept
{
  return t_last_error.c_str();
}
}

// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*)
{
  using namespace libremidi::android;
  JNIEnv* e = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK)
    return JNI_VERSION_1_6;

  // Runs on the thread that called System.loadLibrary, with the app's class
  // loader current — the only point where FindClass can see app classes.
  jclass cls = e->FindClass("dev/celtera/libremidi/MidiBridge");
  if (!cls)
  {
    clear_exception(e);
    LOGE("MidiBridge class not found; Android MIDI is unavailable");
    g_vm.store(vm, std::memory_order_release);
    return JNI_VERSION_1_6;
  }

  const JNINativeMethod natives[] = {
      {"nativeDevicesChanged", "()V", reinterpret_cast<void*>(native_devices_changed)},
  };
  g_list_ports = e->GetStaticMethodID(cls, "listPorts", "(Z)[Ljava/lang/String;");
  g_open_device = e->GetStaticMethodID(cls, "openDevice", "(IJ)Ljava/lang/Object;");
  g_close_device = e->GetStaticMethodID(cls, "closeDevice", "(Landroid/media/midi/MidiDevice;)V");
  g_is_available = e->GetStaticMethodID(cls, "isAvailable", "()Z");
  const bool bound = g_list_ports && g_open_device && g_close_device && g_is_available
                     && e->RegisterNatives(cls, natives, 1) == JNI_OK;
  if (!bound)
  {
    clear_exception(e);
    LOGE("MidiBridge does not match this native library; Android MIDI is unavailable");
  }
  else
  {
    g_bridge = static_cast<jclass>(e->NewGlobalRef(cls));
  }
  e->DeleteLocalRef(cls);

  // Published last: anything that sees the VM also sees the ids above.
  g_vm.store(vm, std::memory_order_release);
  return JNI_VERSION_1_6;
}

#endif // __ANDROID__
