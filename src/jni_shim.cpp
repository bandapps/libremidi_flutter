// JNI shim for Android NDK
// Provides JNI functions and cached ClassLoader for libremidi

#if defined(__ANDROID__)

#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <pthread.h>
#include <string>

#define LOG_TAG "libremidi_jni"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Cached JavaVM and ClassLoader
static JavaVM* g_jvm = nullptr;
static jobject g_classLoader = nullptr;
static jmethodID g_findClassMethod = nullptr;

// Function pointer type for JNI_GetCreatedJavaVMs
typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
static JNI_GetCreatedJavaVMs_t s_JNI_GetCreatedJavaVMs = nullptr;

static void init_jni_functions() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    // Try to load from libnativehelper.so (system library)
    void* handle = dlopen("libnativehelper.so", RTLD_LAZY);
    if (handle) {
        s_JNI_GetCreatedJavaVMs = (JNI_GetCreatedJavaVMs_t)dlsym(handle, "JNI_GetCreatedJavaVMs");
        if (s_JNI_GetCreatedJavaVMs) {
            LOGI("JNI_GetCreatedJavaVMs loaded from libnativehelper.so");
            return;
        }
    }

    // Try libart.so as fallback
    handle = dlopen("libart.so", RTLD_LAZY);
    if (handle) {
        s_JNI_GetCreatedJavaVMs = (JNI_GetCreatedJavaVMs_t)dlsym(handle, "JNI_GetCreatedJavaVMs");
        if (s_JNI_GetCreatedJavaVMs) {
            LOGI("JNI_GetCreatedJavaVMs loaded from libart.so");
            return;
        }
    }

    LOGE("Could not find JNI_GetCreatedJavaVMs in system libraries");
}

// Provide the JNI_GetCreatedJavaVMs symbol
extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vmBuf, jsize bufLen, jsize* nVMs) {
    init_jni_functions();

    if (s_JNI_GetCreatedJavaVMs) {
        return s_JNI_GetCreatedJavaVMs(vmBuf, bufLen, nVMs);
    }

    LOGE("JNI_GetCreatedJavaVMs not available");
    if (nVMs) *nVMs = 0;
    return JNI_ERR;
}

// Cache the ClassLoader when library is loaded from Java
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    LOGI("JNI_OnLoad called");
    g_jvm = vm;

    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("Failed to get JNIEnv in JNI_OnLoad");
        return JNI_VERSION_1_6;
    }

    // Get the current thread's context ClassLoader
    jclass threadClass = env->FindClass("java/lang/Thread");
    if (!threadClass) {
        LOGE("Failed to find Thread class");
        return JNI_VERSION_1_6;
    }

    jmethodID currentThreadMethod = env->GetStaticMethodID(threadClass, "currentThread", "()Ljava/lang/Thread;");
    jobject currentThread = env->CallStaticObjectMethod(threadClass, currentThreadMethod);

    jmethodID getContextClassLoaderMethod = env->GetMethodID(threadClass, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoader = env->CallObjectMethod(currentThread, getContextClassLoaderMethod);

    if (classLoader) {
        g_classLoader = env->NewGlobalRef(classLoader);

        jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
        g_findClassMethod = env->GetMethodID(classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");

        LOGI("ClassLoader cached successfully");
    } else {
        LOGE("Failed to get ClassLoader");
    }

    env->DeleteLocalRef(currentThread);
    env->DeleteLocalRef(threadClass);

    return JNI_VERSION_1_6;
}

// Find a class using the cached ClassLoader (works from any thread)
extern "C" jclass libremidi_find_class(JNIEnv* env, const char* name) {
    if (g_classLoader && g_findClassMethod) {
        // Convert class name from JNI format (com/example/Class) to Java format (com.example.Class)
        std::string className(name);
        for (char& c : className) {
            if (c == '/') c = '.';
        }

        jstring jClassName = env->NewStringUTF(className.c_str());
        jclass clazz = static_cast<jclass>(env->CallObjectMethod(g_classLoader, g_findClassMethod, jClassName));
        env->DeleteLocalRef(jClassName);

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("Exception while loading class: %s", name);
            return nullptr;
        }

        return clazz;
    }

    // Fallback to default FindClass
    return env->FindClass(name);
}

#endif // __ANDROID__
