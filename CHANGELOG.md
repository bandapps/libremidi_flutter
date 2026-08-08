# Changelog

## 0.8.6

### Fixed

- **iOS and macOS did not work in released apps.** Depending on the build system the plugin either shipped without its native code or had its symbols stripped, both failing at runtime with `Failed to lookup symbol 'lrm_...'`. Both build systems are fixed and verified.
- **Windows fell back to WinUWP** instead of Windows MIDI Services, because the bundled SDK headers were missing from the published package. Also affected 0.8.5.
- Reliability work in the native layer across all platforms: closing ports and observers, hotplug handling, Android's asynchronous device opening, and error reporting. Failed opens and sends previously reported success; they now surface as exceptions.

### Added

- `MidiMessageBuilder` — the byte builders behind every `MidiOutput.send*` method, usable without an open port.
- `MidiObserver.hotplugAvailable` / `LibremidiFlutter.hotplugAvailable` to detect the case where hotplug cannot be delivered, so an app can fall back to polling.

### Changed

- **Port ids changed on Windows MIDI Services and Android.** They were derived from values that shifted when devices were added or removed. Persist a selection with `MidiPort.toMatchJson` and restore it with `findClosestPort` rather than storing `portId`.
- `MidiMessage.timestamp` is documented as nanoseconds, which is what it has always been — the previous "microseconds" was a documentation error. Values are unchanged.
- Dart SDK constraint raised to `^3.7.0`, which `ffi ^2.2.0` already required.

## 0.8.5

### Added

- Add fuzzy port matching to re-resolve a saved port across reconnects, driver updates, or USB-port changes: `LibremidiFlutter.findClosestInputPort` / `findClosestOutputPort`, `MidiObserver.findClosestPort`, and `MidiPort.toMatchJson` / `MidiPort.fromMatchJson` for persisting and restoring a selection.

### Fixed

- Fix a use-after-free in the MIDI input callback that could drop or corrupt incoming messages — or crash — under dense input or large SysEx. Message bytes are now copied synchronously on the native side before being delivered to Dart.

## 0.8.4

### Added

- Add RPN and NRPN input/output support with 7-bit and 14-bit Data Entry, Data Increment/Decrement, optional output deselection, and decoded input parsing.

## 0.8.3

### Added

- Add Windows MIDI Services (WinMIDI) support with WinUWP fallback
- Bundle the minimal WinMIDI SDK headers/metadata needed for Windows builds
- Add input receive options for SysEx, MIDI timing/clock, and active sensing
- Add MIDI output helpers and example controls for Program Change with Bank Select, Pitch Bend, Channel Aftertouch, Polyphonic Aftertouch, SysEx, and Note On/Off
- Add input and output channel selectors to the example app
- Add widget tests for the example app covering startup, fake device listing, output sending, hotplug refresh, and incoming MIDI logs

### Changed

- Open ports by stable ID instead of volatile index
- Hide internal libremidi observer ports from public device lists
- Clean WinUWP MIDI port names
- Lower SDK constraints to broaden compatibility
- Lower Android minimum SDK from API 31 to API 29
- Improve example app lifecycle handling, hotplug behavior, disabled-state handling, and MIDI output labels
- Refactor the example app MIDI access layer for testability without changing runtime behavior
- Improve README quick start, input option placement, and bundled third-party license notes

### Fixed

- Thread-safe port enumeration (iOS/macOS)
- Fix use-after-free on dispose during hotplug
- Fix iOS SPM build
- Validate MIDI parameters in release builds

## 0.8.2

- Fix SPM header search paths to stay within package root
- Fix dynamic library loading to support both CocoaPods and SPM

## 0.8.1

- Added Swift Package Manager (SPM) support for iOS and macOS

## 0.8.0

- Initial release
