#!/bin/zsh
# Runs the Android release harness on one device. Build it first with setup.sh.
#
#   run.sh <adb-serial> [harness.apk]
#
# Installs both apps, starts the harness with the loopback service stopped (so
# the first open has to bind it, as with a Bluetooth device), removes and
# re-adds the loopback device five times while the harness has an input open,
# then prints the results. Exits 0 only if every check passed ("VERDICT: PASS").
# Takes about four minutes.
set -u

D=${1:?usage: run.sh <adb-serial> [harness.apk]}
HERE=${0:A:h}
LOOP=$HERE/build/midi_loopback/build/app/outputs/flutter-apk/app-debug.apk
APK=${2:-$HERE/build/lrm_harness/build/app/outputs/flutter-apk/app-release.apk}

adb -s $D install -r $LOOP >/dev/null
adb -s $D install -r $APK >/dev/null
adb -s $D shell am force-stop dev.test.lrm_harness
adb -s $D shell am force-stop dev.test.midi_loopback
adb -s $D logcat -c
adb -s $D shell am start -n dev.test.lrm_harness/.MainActivity >/dev/null

# Hotplug: wait for the harness to open its window, then cycle the device.
for i in $(seq 120); do
  adb -s $D logcat -d | grep -q "HARNESS HOTPLUG-WINDOW\|Fatal signal" && break
  sleep 2
done
sleep 2
for c in 1 2 3 4 5; do
  adb -s $D uninstall dev.test.midi_loopback >/dev/null
  sleep 3
  adb -s $D install $LOOP >/dev/null
  sleep 6
done

for i in $(seq 180); do
  adb -s $D logcat -d | grep -q "HARNESS DONE\|Fatal signal" && break
  sleep 1
done
# Read right at DONE: the probe that follows leaves an input open on purpose.
conn_count() {
  adb -s $D shell dumpsys midi | grep -E "Loopback.*DeviceConnection count" \
    | sed -E 's/.*DeviceConnection count: ([0-9]+).*/\1/' | head -1
}
CONN_DONE=$(conn_count)
for i in $(seq 60); do
  adb -s $D logcat -d | grep -q "HARNESS ORPHAN-GC\|Fatal signal" && break
  sleep 1
done
CONN_GC=$(conn_count)
ALIVE=$(adb -s $D shell pidof dev.test.lrm_harness)
LOG=$(adb -s $D logcat -d)

echo "$LOG" | grep -E "HARNESS|Fatal signal|Abort message|HWAddressSanitizer"
echo "--- connections at DONE: ${CONN_DONE:-?}, after the orphan was collected: ${CONN_GC:-?}"
echo "--- process after the probe: ${ALIVE:-DEAD}"

# Verdict, as exit status: 0 only if every check holds.
fail=0
echo "$LOG" | grep -q "HARNESS DONE failures=0" || { echo "VERDICT: harness checks failed"; fail=1; }
echo "$LOG" | grep -qE "Fatal signal|Abort message|HWAddressSanitizer" && { echo "VERDICT: crash or memory error"; fail=1; }
[ "${CONN_DONE:-x}" = 0 ] || { echo "VERDICT: devices left open after close"; fail=1; }
[ "${CONN_GC:-x}" = 0 ] || { echo "VERDICT: orphaned input not cleaned up"; fail=1; }
[ -n "$ALIVE" ] || { echo "VERDICT: process died"; fail=1; }
echo "$LOG" | grep -q "HARNESS ORPHAN-GC" || { echo "VERDICT: harness did not finish"; fail=1; }
[ $fail = 0 ] && echo "VERDICT: PASS"
exit $fail
