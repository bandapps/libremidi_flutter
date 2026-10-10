import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

/// Locks the MidiPort fuzzy-match serialization used to persist a port
/// selection across app restarts. These are pure Dart (no native FFI):
/// fromMatchJson is the only public way to build a MidiPort without the
/// native enumeration, so the round-trip starts there.
void main() {
  group('MidiPort.fromMatchJson', () {
    test('returns null for null input', () {
      expect(MidiPort.fromMatchJson(null), isNull);
    });

    test('returns null when both port_name and display_name are empty', () {
      // Without any name there is nothing to match against later — restoring
      // such a port would always mis-match, so it must be rejected.
      expect(MidiPort.fromMatchJson({}), isNull);
      expect(
        MidiPort.fromMatchJson({'port_name': '', 'display_name': ''}),
        isNull,
      );
      expect(MidiPort.fromMatchJson({'serial': 'ABC'}), isNull);
    });

    test('accepts a port with only display_name', () {
      final port = MidiPort.fromMatchJson({'display_name': 'IAC Bus 1'});
      expect(port, isNotNull);
      expect(port!.displayName, 'IAC Bus 1');
    });

    test('missing matching fields default to empty string', () {
      final port = MidiPort.fromMatchJson({'port_name': 'Bus 1'})!;
      expect(port.portName, 'Bus 1');
      expect(port.deviceName, '');
      expect(port.manufacturer, '');
      expect(port.product, '');
      expect(port.serial, '');
    });

    test('non-matching fields are set to safe defaults', () {
      final port = MidiPort.fromMatchJson({'port_name': 'Bus 1'})!;
      expect(port.stableId, 0);
      expect(port.portId, 0);
      expect(port.index, -1);
      expect(port.transportType, MidiTransportType.unknown);
      expect(port.isVirtual, isFalse);
    });
  });

  group('toMatchJson / fromMatchJson round-trip', () {
    // Regression: a wrong/renamed key in either direction (e.g. 'device' vs
    // 'device_name') would silently break port restore after a restart. The
    // round-trip pins the exact key contract on both sides.
    test('preserves all six matching fields through a round-trip', () {
      const input = {
        'display_name': 'My Synth Port 1',
        'port_name': 'Port 1',
        'device_name': 'My Synth',
        'manufacturer': 'Acme',
        'product': 'SynthX',
        'serial': 'SN-12345',
      };

      final port = MidiPort.fromMatchJson(input)!;
      final out = port.toMatchJson();

      expect(out, input);
    });

    test('toMatchJson exposes exactly the six matching keys', () {
      final port = MidiPort.fromMatchJson({'port_name': 'Bus 1'})!;
      expect(
        port.toMatchJson().keys.toSet(),
        {
          'display_name',
          'port_name',
          'device_name',
          'manufacturer',
          'product',
          'serial',
        },
      );
    });
  });

  // Equality is the connection (portId); ports without one (fromMatchJson)
  // compare by their saved description. Before, == used stableId, which is 0
  // for every fromMatchJson port, so any two of them were "equal" and a Set
  // of saved selections collapsed to one entry.
  group('MidiPort equality without a portId', () {
    Map<String, dynamic> saved(String name) => {
          'port_name': name,
          'display_name': name,
          'manufacturer': 'TestCo',
        };

    test('different saved ports are not equal', () {
      final a = MidiPort.fromMatchJson(saved('Alpha'))!;
      final b = MidiPort.fromMatchJson(saved('Beta'))!;
      expect(a == b, isFalse);
      expect({a, b}, hasLength(2));
    });

    test('the same saved port is equal, with the same hash', () {
      final a = MidiPort.fromMatchJson(saved('Alpha'))!;
      final b = MidiPort.fromMatchJson(saved('Alpha'))!;
      expect(a, b);
      expect(a.hashCode, b.hashCode);
    });
  });
}
