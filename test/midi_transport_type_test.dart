import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

void main() {
  group('MidiTransportType.fromValue', () {
    test('returns unknown for 0', () {
      expect(MidiTransportType.fromValue(0), MidiTransportType.unknown);
    });

    test('returns software for bit 1', () {
      expect(MidiTransportType.fromValue(2), MidiTransportType.software);
    });

    test('returns loopback for bit 2', () {
      expect(MidiTransportType.fromValue(4), MidiTransportType.loopback);
    });

    test('returns hardware for bit 3', () {
      expect(MidiTransportType.fromValue(8), MidiTransportType.hardware);
    });

    test('returns usb for bit 4', () {
      expect(MidiTransportType.fromValue(16), MidiTransportType.usb);
    });

    test('returns bluetooth for bit 5', () {
      expect(MidiTransportType.fromValue(32), MidiTransportType.bluetooth);
    });

    test('returns pci for bit 6', () {
      expect(MidiTransportType.fromValue(64), MidiTransportType.pci);
    });

    test('returns network for bit 7', () {
      expect(MidiTransportType.fromValue(128), MidiTransportType.network);
    });

    test('highest set bit wins when multiple bits set', () {
      // network (128) + usb (16) -> network wins (checked first)
      expect(MidiTransportType.fromValue(128 | 16), MidiTransportType.network);
      // bluetooth (32) + hardware (8) -> bluetooth wins
      expect(
        MidiTransportType.fromValue(32 | 8),
        MidiTransportType.bluetooth,
      );
      // usb (16) + software (2) -> usb wins
      expect(MidiTransportType.fromValue(16 | 2), MidiTransportType.usb);
    });

    test('returns unknown for value 1 (no matching bit)', () {
      expect(MidiTransportType.fromValue(1), MidiTransportType.unknown);
    });
  });

  group('MidiTransportType.displayName', () {
    test('all enum values have correct display names', () {
      expect(MidiTransportType.unknown.displayName, 'Unknown');
      expect(MidiTransportType.software.displayName, 'Software');
      expect(MidiTransportType.loopback.displayName, 'Loopback');
      expect(MidiTransportType.hardware.displayName, 'Hardware');
      expect(MidiTransportType.usb.displayName, 'USB');
      expect(MidiTransportType.bluetooth.displayName, 'Bluetooth');
      expect(MidiTransportType.pci.displayName, 'PCI');
      expect(MidiTransportType.network.displayName, 'Network');
    });
  });
}
