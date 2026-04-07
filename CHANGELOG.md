# Changelog

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
