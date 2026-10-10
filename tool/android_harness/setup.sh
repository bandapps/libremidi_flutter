#!/bin/zsh
# Generates and builds the two apps of the Android release harness under
# build/ (git- and pub-ignored):
#
#   build/midi_loopback  a MidiDeviceService with one port that echoes what is
#                        sent to it — a virtual MIDI device, no hardware needed
#   build/midi_loopback2 the same device again, indistinguishable by its data
#   build/midi_slowloop  a loopback whose service takes 7 s to start
#   build/lrm_harness    a release (R8) build that drives libremidi_flutter
#                        against that device and logs results to logcat
#
# Only the sources in loopback/ and harness/ are ours; everything else comes
# from `flutter create`, so the projects always match the installed Flutter.
set -euo pipefail

HERE=${0:A:h}
PKG=${HERE:h:h}
OUT=$HERE/build

rm -rf "$OUT"
mkdir -p "$OUT"

# --- loopback devices ----------------------------------------------------------
# Built twice under different package names but with identical device data
# (same name, manufacturer and product, no serial): two indistinguishable
# devices, as with two identical controllers.
# Adds a loopback MidiDeviceService to the Android app in $1: service class $2
# (LoopbackService or SlowLoopbackService), device description $3.
add_loopback_service() {
  local main=$1/android/app/src/main service=$2 info=$3
  mkdir -p "$main/java/dev/test/loopback" "$main/res/xml"
  cp "$HERE/loopback/LoopbackService.java" "$HERE/loopback/SlowLoopbackService.java" \
    "$main/java/dev/test/loopback/"
  cp "$HERE/loopback/$info.xml" "$main/res/xml/"
  python3 - "$main/AndroidManifest.xml" "$service" "$info" <<'EOF2'
import sys
p, service, info = sys.argv[1:4]
s = open(p).read()
if 'android.software.midi' not in s:
    s = s.replace(
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android">',
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <uses-feature android:name="android.software.midi" android:required="true" />', 1)
s = s.replace('    </application>', f'''        <service android:name="dev.test.loopback.{service}"
            android:exported="true"
            android:permission="android.permission.BIND_MIDI_DEVICE_SERVICE">
            <intent-filter>
                <action android:name="android.media.midi.MidiDeviceService" />
            </intent-filter>
            <meta-data android:name="android.media.midi.MidiDeviceService"
                android:resource="@xml/{info}" />
        </service>
    </application>''', 1)
open(p, 'w').write(s)
EOF2
}

make_loopback() {
  local name=$1 service=$2 info=$3
  flutter create --platforms android --org dev.test --project-name $name \
    "$OUT/$name" >/dev/null
  # flutter create's sample widget test targets the template app, which is gone.
  rm -rf "$OUT/$name/test"
  add_loopback_service "$OUT/$name" $service $info
  (cd "$OUT/$name" && flutter build apk --debug)
}
make_loopback midi_loopback LoopbackService loopback_device_info
make_loopback midi_loopback2 LoopbackService loopback_device_info
make_loopback midi_slowloop SlowLoopbackService slow_device_info

# --- harness -----------------------------------------------------------------
flutter create --platforms android --org dev.test --project-name lrm_harness \
  "$OUT/lrm_harness" >/dev/null
rm -rf "$OUT/lrm_harness/test"
sed -i '' 's/minSdk = flutter.minSdkVersion/minSdk = 29/' \
  "$OUT/lrm_harness/android/app/build.gradle.kts"
cp "$HERE/harness/main.dart" "$OUT/lrm_harness/lib/main.dart"
# The harness app provides a device of its own, for the main-thread check.
add_loopback_service "$OUT/lrm_harness" LoopbackService own_device_info
(cd "$OUT/lrm_harness" \
  && flutter pub add "libremidi_flutter:{\"path\":\"$PKG\"}" ffi \
  && flutter build apk --release)

echo "Built. Run: $HERE/run.sh <adb-serial>"
