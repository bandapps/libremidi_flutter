# libremidi_flutter

A Flutter wrapper around [libremidi](https://github.com/jcelerier/libremidi), providing cross-platform MIDI device access.

This is not a full-featured MIDI framework, but a stable and predictable wrapper for common control-oriented workflows.

**Supports iOS, Android, macOS, Windows, Linux.**

## Features

- MIDI Input / Output
- Hotplug detection (device connect/disconnect)
- Control Change, Program Change, Note On/Off, Pitch Bend, Aftertouch, SysEx
- Device identification across reconnects
- Device infos
- Simple API for sending and receiving messages
- Windows: prefers Windows MIDI Services (WinMIDI) when available, falls back to WinUWP

## Installation

```yaml
dependencies:
  libremidi_flutter: ^0.8.3
```

## Usage

```dart
import 'dart:typed_data';

import 'package:libremidi_flutter/libremidi_flutter.dart';
```

MIDI channels are zero-based in this package: channel `0` means MIDI channel 1, channel `15` means MIDI channel 16.

### Quick start

```dart
import 'dart:typed_data';

import 'package:libremidi_flutter/libremidi_flutter.dart';

final inputs = LibremidiFlutter.getInputPorts();
final outputs = LibremidiFlutter.getOutputPorts();

if (inputs.isNotEmpty) {
  final input = LibremidiFlutter.openInput(inputs.first);
  input.messages.listen((msg) {
    if (msg.isControlChange) {
      print('CC ${msg.controller}=${msg.value} ch:${msg.channel + 1}');
    }
  });
}

if (outputs.isNotEmpty) {
  final output = LibremidiFlutter.openOutput(outputs.first);
  output.sendControlChange(channel: 0, controller: 1, value: 64);
  output.sendSysEx(Uint8List.fromList([0x7E, 0x7F, 0x06, 0x01]));
}

LibremidiFlutter.onHotplug.listen((_) {
  // Device connected or disconnected - refresh your device list.
  final currentInputs = LibremidiFlutter.getInputPorts();
  final currentOutputs = LibremidiFlutter.getOutputPorts();
  print('MIDI ports: ${currentInputs.length} in, ${currentOutputs.length} out');
});
```

### Connecting and disconnecting

```dart
final input = LibremidiFlutter.openInput(inputs.first);
final output = LibremidiFlutter.openOutput(outputs.first);

// Disconnect specific ports.
LibremidiFlutter.disconnectInput(input);
LibremidiFlutter.disconnectOutput(output);

// Or disconnect all and cleanup.
LibremidiFlutter.dispose();
```

### Sending Control Change (CC)

```dart
output.sendControlChange(channel: 0, controller: 1, value: 64);
```

### Sending Program Change (PC)

```dart
output.sendProgramChange(channel: 0, program: 10);

// With bank select
output.sendBankSelect(channel: 0, bank: 1, program: 10);
```

### Sending Note On/Off

```dart
output.sendNoteOn(channel: 0, note: 60, velocity: 100);
output.sendNoteOff(channel: 0, note: 60);
```

### Sending SysEx

```dart
// With automatic F0/F7 framing
output.sendSysEx(Uint8List.fromList([0x7E, 0x7F, 0x06, 0x01]));

// Already framed (raw send)
output.send(Uint8List.fromList([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]));

// Or mark as already framed
output.sendSysEx(Uint8List.fromList([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]), alreadyFramed: true);
```

SysEx receiving is enabled by default:

```dart
// Disable SysEx reception if you do not need SysEx dumps.
final inputWithoutSysEx = LibremidiFlutter.openInput(port, receiveSysex: false);
```

### Sending Pitch Bend

```dart
// Value is 14-bit (0-16383), center is 8192
output.sendPitchBend(channel: 0, value: 8192);
```

### Sending Aftertouch

```dart
// Channel aftertouch (pressure)
output.sendAftertouch(channel: 0, pressure: 64);

// Polyphonic aftertouch (per-note)
output.sendPolyAftertouch(channel: 0, note: 60, pressure: 64);
```

### Receiving MIDI messages

By default, MIDI clock and active sensing messages are filtered out.

```dart
input.messages.listen((msg) {
  // isNoteOn/isNoteOff: msg.note, msg.velocity, msg.channel
  // isControlChange: msg.controller, msg.value, msg.channel
  // isProgramChange: msg.data[1], msg.channel
  // isPitchBend: msg.data[1], msg.data[2], msg.channel
  // isAftertouch: msg.data[1], msg.channel
  // isPolyAftertouch: msg.note, msg.data[2], msg.channel
  // isSysEx: msg.data
  if (msg.isControlChange) {
    print('CC ${msg.controller}=${msg.value} ch:${msg.channel + 1}');
  }
});

// Receive MIDI clock (timing) messages.
final timingInput = LibremidiFlutter.openInput(port, receiveTiming: true);

// Receive active sensing messages.
final sensingInput = LibremidiFlutter.openInput(port, receiveSensing: true);
```

## Port properties

```dart
port.displayName    // Full display name
port.portName       // Port name
port.deviceName     // Device name
port.manufacturer   // Manufacturer
port.transportType  // USB, Bluetooth, Software, etc.
port.stableId       // Stable ID for reconnection
```

## Additional features

### Library info

```dart
// Check if library is initialized
if (LibremidiFlutter.isInitialized) {
  print('Ready');
}

// Get libremidi version
print('Version: ${LibremidiFlutter.version}');

// Count open connections
print('Inputs: ${LibremidiFlutter.openInputCount}');
print('Outputs: ${LibremidiFlutter.openOutputCount}');
```

### Connection status

```dart
if (output.isConnected) {
  output.sendControlChange(channel: 0, controller: 1, value: 64);
}

if (input.isConnected) {
  print('Listening...');
}
```

### Filtered messages

```dart
// Only receive messages up to 256 bytes
input.messagesFiltered(maxBytes: 256).listen((msg) { ... });

// Exclude all SysEx
input.messagesFiltered(excludeSysEx: true).listen((msg) { ... });
```

### Message timestamp

```dart
input.messages.listen((msg) {
  print('Received at ${msg.timestamp}µs: ${msg.data}');
});
```

### Port details

```dart
// Additional port properties
print(port.product);      // Product name
print(port.serial);       // Serial number
print(port.isVirtual);    // true if software port
print(port.isHardware);   // true if USB or hardware
print(port.isOutput);     // opposite of isInput

// Export all port info as Map
print(port.toMap());
```

### Refresh port list

```dart
// High-level usage: re-read the current list when needed.
final inputs = LibremidiFlutter.getInputPorts();
final outputs = LibremidiFlutter.getOutputPorts();

// Advanced usage: if you manage your own observer, refresh its cache manually.
final observer = MidiObserver();
observer.refresh();
observer.dispose();
```

## Platform requirements

| Platform | Minimum Version |
|----------|-----------------|
| iOS | 13.0 |
| Android | API 29 (Android 10) |
| macOS | 10.15 |
| Windows | 10 |
| Linux | ALSA |

## Notes

- BLE MIDI requires native platform integration
- See the bundled `example/` app for device selection, port selection, hotplug handling, MIDI input logs, and common output message types.

## License

BSD-2-Clause. See [LICENSE](LICENSE) for details and bundled third-party notices.

This package bundles libremidi and a minimal Microsoft.Windows.Devices.Midi2 WinMIDI SDK subset for Windows builds. Their license notices are included in [LICENSE](LICENSE) and the corresponding `third_party/` directories.

Based on [libremidi](https://github.com/jcelerier/libremidi) by Jean-Michaël Celerier.
