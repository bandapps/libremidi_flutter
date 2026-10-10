# Host-side native tests

Pure C++ parts of the plugin that can be tested without a device or emulator.

```sh
tool/native_tests/run.sh
```

Exits non-zero if any case fails. Currently covers the Android input's MIDI
stream parser (`src/libremidi/backends/android/stream_parser.hpp`): messages
split across packets, running status, real-time bytes inside messages, SysEx
across packets.
