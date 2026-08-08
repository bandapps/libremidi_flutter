#!/bin/bash
# Builds the stress harness against the real Apple implementation.
#
#   ./build.sh asan   -> AddressSanitizer  (use-after-free, double free)
#   ./build.sh tsan   -> ThreadSanitizer   (data races)
#   ./build.sh plain  -> no sanitizer      (throughput reference)
#
# macOS only: the harness drives apple/Classes/libremidi_flutter.cpp, which is
# the CoreMIDI implementation shared by iOS and macOS.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/../.." && pwd)"
FLAVOR="${1:-asan}"

case "$FLAVOR" in
  asan)  SAN=(-fsanitize=address -fno-omit-frame-pointer) ;;
  tsan)  SAN=(-fsanitize=thread) ;;
  plain) SAN=() ;;
  *) echo "usage: $0 <asan|tsan|plain>" >&2; exit 2 ;;
esac

clang++ -std=c++20 -g -O1 ${SAN[@]+"${SAN[@]}"} \
  -DLIBREMIDI_HEADER_ONLY=1 -DLIBREMIDI_COREMIDI=1 \
  -Wno-unguarded-availability-new -Wno-deprecated-declarations \
  -I"$PKG/src" -I"$PKG/third_party/libremidi/include" \
  "$HERE/stress_libremidi.cpp" \
  "$PKG/apple/Classes/libremidi_flutter.cpp" \
  -framework CoreMIDI -framework CoreFoundation -framework CoreAudio \
  -o "$HERE/stress_$FLAVOR"

echo "built $HERE/stress_$FLAVOR"
