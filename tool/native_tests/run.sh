#!/bin/zsh
# Builds and runs the host-side native tests. Needs a C++20 compiler.
set -euo pipefail
HERE=${0:A:h}
PKG=${HERE:h:h}
OUT=$(mktemp -d)
clang++ -std=c++20 -Wall -Wextra -I"$PKG/src" "$HERE/stream_parser_test.cpp" -o "$OUT/stream_parser_test"
"$OUT/stream_parser_test"
