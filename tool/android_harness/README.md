# Android release harness

Drives the real plugin on an Android device or emulator in a **release build**
(R8 on) against a virtual MIDI device, and reports to logcat. Not part of the
published package (`/tool/` is in `.pubignore`).

Run it before every release on API 29, API 30 and a current device. Every Android
bug fixed after 0.8.6 was invisible to debug builds, to the Dart unit tests and
to "the app starts and lists devices".

## Use

```sh
tool/android_harness/setup.sh            # once per change of the plugin
tool/android_harness/run.sh <adb-serial> # per device, about 4 minutes
```

`adb devices` lists the serials. Emulators need a `google_apis` arm64 image;
`sdkmanager "system-images;android-29;google_apis;arm64-v8a"` and the same for
30 get the two oldest supported levels.

`run.sh` exits 0 and prints `VERDICT: PASS` only when every check held: no
failed check, no crash or HWASan report, no device left open, the orphaned
input cleaned up, the process alive. Any other exit status blocks a release.

## What it covers

| Check | Catches |
|---|---|
| note and 200-byte SysEx round trip, six times | broken open, send or receive path |
| first open of a stopped device, input and output together | MidiService rejecting the second connection while it binds the service (Bluetooth MIDI behaves the same) |
| first open of a device that takes 7 s to start, then an immediate retry | a retry racing the open MidiService is still binding |
| synchronous open of a device provided by the app itself, from the main thread | an open that can never finish instead of a clear error |
| a note sent in pieces with a clock byte between and running status after | input decoded per packet instead of as a byte stream |
| `handleAddress` used after its `MidiOutput` was garbage-collected | the native output freed under another plugin |
| `refresh()` ×2000 on the main thread and on a worker isolate | JNI local reference leaks (the table holds 512 on API 29) |
| round trip from a worker isolate, then 70 s idle | threads exiting while attached to the JVM |
| main isolate and worker opening/closing the same device 50× in parallel | shared device and send-port bookkeeping |
| device removed/added 5× with an input open | hotplug dispatch, reopening after reconnect |
| `DeviceConnection count` at the end | devices left open in MidiService after close |
| `openInputAsync`/`openOutputAsync` on a closed device, with a timer running | async open blocking the calling isolate, or not working |
| `LibremidiFlutter.dispose()` while its async opens are running | ports registered or left open after dispose |
| an isolate ends while its async opens are still running | ports opened for nobody staying open, use of a freed observer |
| an isolate ends with an input still open, then data arrives | delivering to a dead isolate (used to abort the process) |
| the collector runs after that | the orphaned input never being closed natively |

Results are only meaningful if the harness can fail: run it once against the
previous release (check it out, `setup.sh`, `run.sh`) and see it go red.

## Memory errors (HWASan)

On an arm64 device with Android 14 or later:

1. In `src/CMakeLists.txt`, inside `if(ANDROID)`, temporarily add
   ```cmake
   if(ANDROID_ABI STREQUAL "arm64-v8a")
     target_compile_options(libremidi_flutter PRIVATE -fsanitize=hwaddress -fno-omit-frame-pointer)
     target_link_options(libremidi_flutter PRIVATE -fsanitize=hwaddress)
   endif()
   ```
2. In `build/lrm_harness/android/app/build.gradle.kts`, inside `android { }`:
   `packaging { jniLibs { useLegacyPackaging = true } }`
3. Create `build/lrm_harness/android/app/src/main/resources/lib/arm64-v8a/wrap.sh`
   (executable) containing `#!/system/bin/sh` and `LD_HWASAN=1 exec "$@"`.
4. `flutter build apk --debug` in `build/lrm_harness`, then
   `run.sh <serial> build/lrm_harness/build/app/outputs/flutter-apk/app-debug.apk`.
5. Any `HWAddressSanitizer` line in the output is a finding. Revert step 1.
