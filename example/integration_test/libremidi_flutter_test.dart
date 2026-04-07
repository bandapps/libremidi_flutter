import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  // =========================================================================
  // Native library loading
  // =========================================================================

  group('Native library', () {
    testWidgets('version string is not empty', (tester) async {
      final version = LibremidiFlutter.version;
      expect(version, isNotEmpty);
    });

    testWidgets('version string looks like a version', (tester) async {
      final version = LibremidiFlutter.version;
      // libremidi versions are typically "x.y.z" or similar
      expect(version, matches(RegExp(r'^\d+\.\d+')));
    });
  });

  // =========================================================================
  // Observer lifecycle
  // =========================================================================

  group('MidiObserver lifecycle', () {
    testWidgets('can create and dispose observer', (tester) async {
      final observer = MidiObserver();
      // If we get here without exception, creation succeeded
      observer.dispose();
    });

    testWidgets('can create and dispose observer with hotplug', (tester) async {
      final observer = MidiObserver.withHotplug();
      observer.dispose();
    });

    testWidgets('double dispose is safe', (tester) async {
      final observer = MidiObserver();
      observer.dispose();
      observer.dispose(); // should not throw
    });

    testWidgets('disposed observer throws on getInputPorts', (tester) async {
      final observer = MidiObserver();
      observer.dispose();
      expect(() => observer.getInputPorts(), throwsStateError);
    });

    testWidgets('disposed observer throws on getOutputPorts', (tester) async {
      final observer = MidiObserver();
      observer.dispose();
      expect(() => observer.getOutputPorts(), throwsStateError);
    });

    testWidgets('disposed observer throws on refresh', (tester) async {
      final observer = MidiObserver();
      observer.dispose();
      expect(() => observer.refresh(), throwsStateError);
    });
  });

  // =========================================================================
  // Port enumeration
  // =========================================================================

  group('Port enumeration', () {
    late MidiObserver observer;

    setUp(() {
      observer = MidiObserver();
    });

    tearDown(() {
      observer.dispose();
    });

    testWidgets('getInputPorts returns a list', (tester) async {
      final ports = observer.getInputPorts();
      expect(ports, isA<List<MidiPort>>());
    });

    testWidgets('getOutputPorts returns a list', (tester) async {
      final ports = observer.getOutputPorts();
      expect(ports, isA<List<MidiPort>>());
    });

    testWidgets('refresh does not throw', (tester) async {
      observer.refresh();
      // If we get here without exception, refresh succeeded
    });

    testWidgets('port enumeration after refresh returns a list', (
      tester,
    ) async {
      observer.refresh();
      final inputs = observer.getInputPorts();
      final outputs = observer.getOutputPorts();
      expect(inputs, isA<List<MidiPort>>());
      expect(outputs, isA<List<MidiPort>>());
    });

    testWidgets('input ports are marked as input', (tester) async {
      final ports = observer.getInputPorts();
      for (final port in ports) {
        expect(port.isInput, isTrue, reason: port.displayName);
        expect(port.isOutput, isFalse, reason: port.displayName);
      }
    });

    testWidgets('output ports are marked as output', (tester) async {
      final ports = observer.getOutputPorts();
      for (final port in ports) {
        expect(port.isOutput, isTrue, reason: port.displayName);
        expect(port.isInput, isFalse, reason: port.displayName);
      }
    });

    testWidgets('ports have non-empty displayName', (tester) async {
      final allPorts = [
        ...observer.getInputPorts(),
        ...observer.getOutputPorts(),
      ];
      for (final port in allPorts) {
        expect(
          port.displayName,
          isNotEmpty,
          reason: 'port index ${port.index}',
        );
        expect(port.name, port.displayName);
      }
    });

    testWidgets('ports have valid transport type', (tester) async {
      final allPorts = [
        ...observer.getInputPorts(),
        ...observer.getOutputPorts(),
      ];
      for (final port in allPorts) {
        expect(
          MidiTransportType.values.contains(port.transportType),
          isTrue,
          reason: port.displayName,
        );
      }
    });

    testWidgets('port toMap contains expected keys', (tester) async {
      final allPorts = [
        ...observer.getInputPorts(),
        ...observer.getOutputPorts(),
      ];
      for (final port in allPorts) {
        final map = port.toMap();
        expect(map, containsPair('stable_id', isA<int>()));
        expect(map, containsPair('display_name', isA<String>()));
        expect(map, containsPair('port_name', isA<String>()));
        expect(map, containsPair('device_name', isA<String>()));
        expect(map, containsPair('manufacturer', isA<String>()));
        expect(map, containsPair('is_input', isA<bool>()));
        expect(map, containsPair('is_virtual', isA<bool>()));
      }
    });

    testWidgets('port toString is readable', (tester) async {
      final allPorts = [
        ...observer.getInputPorts(),
        ...observer.getOutputPorts(),
      ];
      for (final port in allPorts) {
        final str = port.toString();
        expect(str, startsWith('MidiPort('));
        expect(str, contains(port.displayName));
      }
    });
  });

  // =========================================================================
  // Observer port validation
  // =========================================================================

  group('Observer port validation', () {
    late MidiObserver observer;

    setUp(() {
      observer = MidiObserver();
    });

    tearDown(() {
      observer.dispose();
    });

    testWidgets('openInput rejects output port', (tester) async {
      final outputs = observer.getOutputPorts();
      if (outputs.isNotEmpty) {
        expect(() => observer.openInput(outputs.first), throwsArgumentError);
      }
    });

    testWidgets('openOutput rejects input port', (tester) async {
      final inputs = observer.getInputPorts();
      if (inputs.isNotEmpty) {
        expect(() => observer.openOutput(inputs.first), throwsArgumentError);
      }
    });
  });

  // =========================================================================
  // Hotplug stream
  // =========================================================================

  group('Hotplug observer', () {
    testWidgets('onHotplug stream is available', (tester) async {
      final observer = MidiObserver.withHotplug();
      final stream = observer.onHotplug;
      expect(stream, isA<Stream<HotplugEventType>>());
      observer.dispose();
    });
  });

  // =========================================================================
  // High-level API (LibremidiFlutter)
  // =========================================================================

  group('LibremidiFlutter high-level API', () {
    tearDown(() {
      LibremidiFlutter.dispose();
    });

    testWidgets('isInitialized is false before first use', (tester) async {
      // dispose first to ensure clean state
      LibremidiFlutter.dispose();
      expect(LibremidiFlutter.isInitialized, isFalse);
    });

    testWidgets('getInputPorts initializes the library', (tester) async {
      LibremidiFlutter.getInputPorts();
      expect(LibremidiFlutter.isInitialized, isTrue);
    });

    testWidgets('getOutputPorts initializes the library', (tester) async {
      LibremidiFlutter.getOutputPorts();
      expect(LibremidiFlutter.isInitialized, isTrue);
    });

    testWidgets('dispose resets initialization state', (tester) async {
      LibremidiFlutter.getInputPorts();
      expect(LibremidiFlutter.isInitialized, isTrue);
      LibremidiFlutter.dispose();
      expect(LibremidiFlutter.isInitialized, isFalse);
    });

    testWidgets('dispose is idempotent', (tester) async {
      LibremidiFlutter.dispose();
      LibremidiFlutter.dispose();
      LibremidiFlutter.dispose();
      // should not throw
    });

    testWidgets('open counts start at zero', (tester) async {
      LibremidiFlutter.dispose();
      expect(LibremidiFlutter.openInputCount, 0);
      expect(LibremidiFlutter.openOutputCount, 0);
    });

    testWidgets('disconnectAll on empty state is safe', (tester) async {
      LibremidiFlutter.disconnectAll();
      expect(LibremidiFlutter.openInputCount, 0);
      expect(LibremidiFlutter.openOutputCount, 0);
    });

    testWidgets('can re-initialize after dispose', (tester) async {
      LibremidiFlutter.getInputPorts();
      LibremidiFlutter.dispose();
      // Should auto-create new observer
      final ports = LibremidiFlutter.getInputPorts();
      expect(ports, isA<List<MidiPort>>());
      expect(LibremidiFlutter.isInitialized, isTrue);
    });

    testWidgets('version is accessible via high-level API', (tester) async {
      final version = LibremidiFlutter.version;
      expect(version, isNotEmpty);
    });

    testWidgets('onHotplug stream is accessible', (tester) async {
      final stream = LibremidiFlutter.onHotplug;
      expect(stream, isA<Stream<HotplugEventType>>());
    });
  });
}
