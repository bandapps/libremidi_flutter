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

## Installation

```yaml
dependencies:
  libremidi_flutter: ^0.8.1
```

## Usage

```dart
import 'package:libremidi_flutter/libremidi_flutter.dart';
```

### Listing devices

```dart
final inputs = LibremidiFlutter.getInputPorts();
final outputs = LibremidiFlutter.getOutputPorts();

for (final port in inputs) {
  print('${port.displayName} - ${port.manufacturer}');
}
```

### Connecting to a device

```dart
final input = LibremidiFlutter.openInput(inputs[0]);
final output = LibremidiFlutter.openOutput(outputs[0]);
```

### Input options

By default, SysEx messages are received while MIDI clock and active sensing are filtered out.

```dart
// Receive MIDI clock (timing) messages
final input = LibremidiFlutter.openInput(port, receiveTiming: true);

// Disable SysEx reception
final input = LibremidiFlutter.openInput(port, receiveSysex: false);

// Receive active sensing messages
final input = LibremidiFlutter.openInput(port, receiveSensing: true);
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
    print('CC ${msg.controller}=${msg.value} ch:${msg.channel}');
  }
});
```

### Hotplug detection

```dart
LibremidiFlutter.onHotplug.listen((_) {
  // Device connected or disconnected - refresh your device list
  final inputs = LibremidiFlutter.getInputPorts();
  final outputs = LibremidiFlutter.getOutputPorts();
});
```

### Disconnecting

```dart
// Disconnect specific ports
LibremidiFlutter.disconnectInput(input);
LibremidiFlutter.disconnectOutput(output);

// Or disconnect all and cleanup
LibremidiFlutter.dispose();
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
// Manually refresh port cache (usually not needed)
observer.refresh();
```

## Platform requirements

| Platform | Minimum Version |
|----------|-----------------|
| iOS | 13.0 |
| Android | API 31 (Android 12) |
| macOS | 10.15 |
| Windows | 10 |
| Linux | ALSA |

## Notes

- Focused on control-oriented workflows
- BLE MIDI requires native platform integration

## License

BSD-2-Clause. See [LICENSE](LICENSE) for details.

Based on [libremidi](https://github.com/jcelerier/libremidi) by Jean-Michaël Celerier.
