#!/bin/zsh
# Generates and builds the two apps of the Android release harness under
# build/ (git- and pub-ignored):
#
#   build/midi_loopback  a MidiDeviceService with one port that echoes what is
#                        sent to it — a virtual MIDI device, no hardware needed
#   build/midi_loopback2 the same device again, indistinguishable by its data
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
make_loopback() {
  local name=$1
  flutter create --platforms android --org dev.test --project-name $name \
    "$OUT/$name" >/dev/null
  # flutter create's sample widget test targets the template app, which is gone.
  rm -rf "$OUT/$name/test"
  local main=$OUT/$name/android/app/src/main
  mkdir -p "$main/java/dev/test/loopback" "$main/res/xml"
  cp "$HERE/loopback/LoopbackService.java" "$main/java/dev/test/loopback/"
  cp "$HERE/loopback/loopback_device_info.xml" "$main/res/xml/"
  python3 - "$main/AndroidManifest.xml" <<'EOF2'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace(
    '<manifest xmlns:android="http://schemas.android.com/apk/res/android">',
    '<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n'
    '    <uses-feature android:name="android.software.midi" android:required="true" />', 1)
s = s.replace('    </application>', '''        <service android:name="dev.test.loopback.LoopbackService"
            android:exported="true"
            android:permission="android.permission.BIND_MIDI_DEVICE_SERVICE">
            <intent-filter>
                <action android:name="android.media.midi.MidiDeviceService" />
            </intent-filter>
            <meta-data android:name="android.media.midi.MidiDeviceService"
                android:resource="@xml/loopback_device_info" />
        </service>
    </application>''', 1)
open(p, 'w').write(s)
EOF2
  (cd "$OUT/$name" && flutter build apk --debug)
}
make_loopback midi_loopback
make_loopback midi_loopback2

# --- harness -----------------------------------------------------------------
flutter create --platforms android --org dev.test --project-name lrm_harness \
  "$OUT/lrm_harness" >/dev/null
rm -rf "$OUT/lrm_harness/test"
sed -i '' 's/minSdk = flutter.minSdkVersion/minSdk = 29/' \
  "$OUT/lrm_harness/android/app/build.gradle.kts"
cp "$HERE/harness/main.dart" "$OUT/lrm_harness/lib/main.dart"
(cd "$OUT/lrm_harness" \
  && flutter pub add "libremidi_flutter:{\"path\":\"$PKG\"}" \
  && flutter build apk --release)

echo "Built. Run: $HERE/run.sh <adb-serial>"
