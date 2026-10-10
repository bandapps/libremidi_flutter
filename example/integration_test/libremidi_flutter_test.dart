import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';


/// Returns an input port that is safe to open in an automated test.
///
/// Never returns hardware. These tests used to take `inputs.first`, which on a
/// developer machine is whatever USB device happens to be plugged in — opening
/// and closing real hardware from a test run is how a connected MIDI device
/// ends up needing to be physically re-plugged. Only virtual/software ports
/// (an IAC bus on macOS, a loopback port elsewhere) are acceptable, and when
/// there is none the test skips rather than reaching for hardware.
MidiPort? safeTestInput(MidiObserver observer) {
  for (final port in observer.getInputPorts()) {
    if (port.isVirtual) return port;
  }
  return null;
}

/// An input and an output that echo into each other, or null.
///
/// Two shapes exist, and picking the wrong one yields a pair that never
/// delivers — the test then fails instead of skipping, which is worse than
/// having no loopback at all.
///
/// Windows MIDI Services loopbacks are *pairs*: what is sent to endpoint A is
/// received by B and vice versa, never by the sender itself. Both ends carry
/// the same association id in [MidiPort.serial] and differ by display name, so
/// they must be paired across the two names. Transport `loopback` identifies
/// them.
///
/// Everywhere else the same device echoes onto itself (the macOS IAC bus, a
/// loopback MidiDeviceService app on Android), so equal display names are the
/// criterion. On Android several identical virtual devices can be installed,
/// so the two ports must also belong to the same device: the port id carries
/// the device id in its upper 32 bits there.
(MidiPort, MidiPort)? virtualLoopbackPair(
  List<MidiPort> inputs,
  List<MidiPort> outputs,
) {
  bool isPairedLoopback(MidiPort p) =>
      p.isVirtual && p.transportType == MidiTransportType.loopback;

  for (final i in inputs.where(isPairedLoopback)) {
    for (final o in outputs.where(isPairedLoopback)) {
      if (i.serial.isNotEmpty &&
          o.serial == i.serial &&
          o.displayName != i.displayName) {
        return (i, o);
      }
    }
  }

  for (final i in inputs.where((p) => p.isVirtual)) {
    for (final o in outputs.where((p) => p.isVirtual)) {
      final sameDevice = !Platform.isAndroid || (i.portId >> 32) == (o.portId >> 32);
      if (o.displayName == i.displayName && sameDevice) return (i, o);
    }
  }
  return null;
}

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  // Several of the tests below need a virtual loopback pair and skip without
  // one. A skip is easy to overlook — the run still ends in "All tests
  // passed!" — and that is how the SysEx conversion bugs survived: on Windows
  // the three loopback tests had been skipping unnoticed. This one states
  // plainly whether that coverage is in effect.
  testWidgets('loopback-backed coverage is enabled', (tester) async {
    final observer = MidiObserver();
    try {
      final pair = virtualLoopbackPair(
          observer.getInputPorts(), observer.getOutputPorts());
      if (pair != null) {
        return; // coverage is live
      }
      // ignore: avoid_print
      print('''

  ============================================================
   NO VIRTUAL LOOPBACK PAIR - message delivery is NOT covered
  ============================================================
   Everything that sends a message and checks what arrives is
   being skipped, including the SysEx conversion regressions.
   A green run here says nothing about those paths.

   macOS   enable the IAC Driver in Audio MIDI Setup
   Windows see tool/midi_loopback/README.md
  ============================================================
''');
      markTestSkipped(
          'no virtual loopback pair - see tool/midi_loopback/README.md');
    } finally {
      observer.dispose();
    }
  });

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

  // =========================================================================
  // Fuzzy port matching (native lrm_find_closest_port)
  // =========================================================================

  group('Fuzzy port matching', () {
    tearDown(() {
      LibremidiFlutter.dispose();
    });

    testWidgets('findClosest returns -1 for an unknown port without throwing', (
      tester,
    ) async {
      // Exercises the native lrm_find_closest_port symbol end to end (this is
      // exactly the kind of call that would fail if the export were missing on
      // a platform). A port that matches nothing must return -1, not crash.
      final phantom = MidiPort.fromMatchJson({
        'port_name': '__no_such_port__',
        'display_name': '__no_such_port__',
      })!;
      expect(LibremidiFlutter.findClosestInputPort(phantom), -1);
      expect(LibremidiFlutter.findClosestOutputPort(phantom), -1);
    });

    testWidgets('a real port round-trips through toMatchJson and is re-found', (
      tester,
    ) async {
      // Persisting a real port via toMatchJson and restoring it via
      // fromMatchJson must resolve back to a valid index. Skips when the host
      // has no MIDI ports (e.g. headless CI).
      final outputs = LibremidiFlutter.getOutputPorts();
      if (outputs.isEmpty) return;
      final restored = MidiPort.fromMatchJson(outputs.first.toMatchJson())!;
      final index = LibremidiFlutter.findClosestOutputPort(restored);
      expect(index, greaterThanOrEqualTo(0));
      expect(index, lessThan(outputs.length));
    });

    testWidgets('observer.findClosestPort resolves a restored input port', (
      tester,
    ) async {
      final observer = MidiObserver();
      try {
        final inputs = observer.getInputPorts();
        if (inputs.isEmpty) return;
        final restored = MidiPort.fromMatchJson(inputs.first.toMatchJson())!;
        final index = observer.findClosestPort(restored, isInput: true);
        expect(index, greaterThanOrEqualTo(0));
        expect(index, lessThan(inputs.length));
      } finally {
        observer.dispose();
      }
    });
  });

  // =========================================================================
  // Input connection lifecycle (NativeCallable / message callback path)
  // =========================================================================
  //
  // Note: the actual SysEx-large-buffer / lrm_free path only fires when a MIDI
  // source sends data, which needs a connected (or virtual) device — not
  // available headlessly. These smoke tests exercise the open -> subscribe ->
  // dispose lifecycle of the callback machinery, which is where the
  // use-after-free fix lives.

  group('Input connection', () {
    testWidgets('open, subscribe and dispose an input does not crash', (
      tester,
    ) async {
      final observer = MidiObserver();
      try {
        final port = safeTestInput(observer);
        if (port == null) return;
        final input = observer.openInput(port);
        final sub = input.messages.listen((_) {});
        await tester.pump(const Duration(milliseconds: 50));
        await sub.cancel();
        input.dispose();
      } finally {
        observer.dispose();
      }
    });

    // Locks the delivery path end to end: bytes and timestamp sent to a
    // loopback reach `messages` intact. Without it, a break in how native code
    // posts to Dart (port, message layout, buffer ownership) only shows up on
    // a device. Needs a virtual port pair that echoes — the macOS IAC bus, or
    // a loopback MidiDeviceService app on Android — and is skipped otherwise.
    testWidgets('a message sent to a loopback arrives intact', (tester) async {
      final observer = MidiObserver();
      try {
        final pair = virtualLoopbackPair(
            observer.getInputPorts(), observer.getOutputPorts());
        final inPort = pair?.$1;
        final outPort = pair?.$2;
        if (inPort == null || outPort == null) {
          markTestSkipped('no virtual loopback port pair available');
          return;
        }

        final input = observer.openInput(inPort);
        final output = observer.openOutput(outPort);
        final received = <MidiMessage>[];
        final sub = input.messages.listen(received.add);
        try {
          final sysex = MidiMessageBuilder.sysEx(
            Uint8List.fromList([0x7D, for (var i = 0; i < 300; i++) i & 0x7F]),
          );
          output.send(MidiMessageBuilder.noteOn(channel: 2, note: 61, velocity: 99));
          output.send(sysex);
          for (var i = 0; i < 40 && received.length < 2; i++) {
            await tester.runAsync(
              () => Future<void>.delayed(const Duration(milliseconds: 50)),
            );
          }
          final note = received.where((m) => m.isNoteOn).toList();
          expect(note, hasLength(1));
          expect([note.single.channel, note.single.note, note.single.velocity],
              [2, 61, 99]);
          expect(note.single.timestamp, isNonZero);
          final dump = received.where((m) => m.isSysEx).toList();
          expect(dump, hasLength(1));
          expect(dump.single.data, sysex);
        } finally {
          await sub.cancel();
          input.dispose();
          output.dispose();
        }
      } finally {
        observer.dispose();
      }
    });

    // A 256-byte SysEx used to hang the sending thread outright. The MIDI 1 to
    // UMP converter tracked both stream offsets through uint8_t pointers onto
    // size_t fields, so every offset wrapped at 256: at exactly 256 bytes the
    // source index returned to 0 and the conversion loop never terminated,
    // above that it wrapped into the SysEx payload and the send failed. Only
    // MIDI 2.0 backends run that converter, so a UMP transport (Windows MIDI
    // Services) is what regresses; running it elsewhere costs nothing.
    //
    // The receive side had its own defect, exposed by the same transport: the
    // SysEx7 reassembly buffer lived on the stack of the UMP-to-MIDI-1
    // converter, so with one UMP delivered per callback every packet but the
    // last was dropped and the receiver got 0xF0 + <last packet> + 0xF7 — a
    // short message that is indistinguishable from a complete one. Sizes here
    // straddle 256 and run well past it, so a regression in either direction
    // fails: a hang times the test out, a lost packet fails the comparison.
    testWidgets('a SysEx across the 256-byte conversion boundary round-trips',
        (tester) async {
      final observer = MidiObserver();
      try {
        final pair = virtualLoopbackPair(
            observer.getInputPorts(), observer.getOutputPorts());
        final inPort = pair?.$1;
        final outPort = pair?.$2;
        if (inPort == null || outPort == null) {
          markTestSkipped('no virtual loopback port pair available');
          return;
        }

        final input = observer.openInput(inPort);
        final output = observer.openOutput(outPort);
        final received = <MidiMessage>[];
        final sub = input.messages.listen(received.add);
        try {
          for (final total in [255, 256, 257, 600, 1200]) {
            received.clear();
            final message = MidiMessageBuilder.sysEx(
              Uint8List.fromList([for (var i = 0; i < total - 2; i++) i & 0x7F]),
            );
            expect(message, hasLength(total));
            expect(() => output.send(message), returnsNormally,
                reason: 'sending a $total-byte SysEx must not throw');

            for (var i = 0; i < 50 && !received.any((m) => m.isSysEx); i++) {
              await tester.runAsync(
                () => Future<void>.delayed(const Duration(milliseconds: 40)),
              );
            }
            final dump = received.where((m) => m.isSysEx).toList();
            expect(dump, hasLength(1),
                reason: 'a $total-byte SysEx must arrive as one message');
            expect(dump.single.data, message,
                reason: 'a $total-byte SysEx must arrive byte for byte');
          }
        } finally {
          await sub.cancel();
          input.dispose();
          output.dispose();
        }
      } finally {
        observer.dispose();
      }
    });

    // send() takes a byte range, not a message, so a caller may put several
    // messages in one call. The MIDI 1 to UMP converter seeded its destination
    // index — which counts uint32_t — from a byte counter, so anything after
    // the first message landed four times too far into the output buffer. A
    // lone message starting at offset zero hides it, which is every message
    // this example sends.
    testWidgets('two messages in one send buffer both arrive', (tester) async {
      final observer = MidiObserver();
      try {
        final pair = virtualLoopbackPair(
            observer.getInputPorts(), observer.getOutputPorts());
        final inPort = pair?.$1;
        final outPort = pair?.$2;
        if (inPort == null || outPort == null) {
          markTestSkipped('no virtual loopback port pair available');
          return;
        }

        final input = observer.openInput(inPort);
        final output = observer.openOutput(outPort);
        final received = <MidiMessage>[];
        final sub = input.messages.listen(received.add);
        try {
          final note =
              MidiMessageBuilder.noteOn(channel: 3, note: 70, velocity: 88);
          final sysex = MidiMessageBuilder.sysEx(
            Uint8List.fromList([0x7D, for (var i = 0; i < 40; i++) i & 0x7F]),
          );
          output.send(Uint8List.fromList([...note, ...sysex]));

          for (var i = 0; i < 40 && received.length < 2; i++) {
            await tester.runAsync(
              () => Future<void>.delayed(const Duration(milliseconds: 40)),
            );
          }
          final notes = received.where((m) => m.isNoteOn).toList();
          expect(notes, hasLength(1), reason: 'the leading note must arrive');
          expect([notes.single.channel, notes.single.note,
              notes.single.velocity], [3, 70, 88]);
          final dumps = received.where((m) => m.isSysEx).toList();
          expect(dumps, hasLength(1),
              reason: 'the SysEx following it must arrive too');
          expect(dumps.single.data, sysex);
        } finally {
          await sub.cancel();
          input.dispose();
          output.dispose();
        }
      } finally {
        observer.dispose();
      }
    });

    // The converter writes SysEx packets into a fixed buffer and checked the
    // remaining room in bytes against a count of packets, which are eight bytes
    // each — so a payload past roughly 48 KiB wrote beyond the end of it. A
    // message too large to convert has to be refused, not half-written.
    testWidgets('a SysEx too large to convert is refused, not written past the '
        'buffer', (tester) async {
      final observer = MidiObserver();
      try {
        final pair = virtualLoopbackPair(
            observer.getInputPorts(), observer.getOutputPorts());
        final outPort = pair?.$2;
        if (outPort == null) {
          markTestSkipped('no virtual loopback port pair available');
          return;
        }

        final output = observer.openOutput(outPort);
        try {
          final huge = MidiMessageBuilder.sysEx(
            Uint8List.fromList([for (var i = 0; i < 60000; i++) i & 0x7F]),
          );
          expect(
            () => output.send(huge),
            throwsA(isA<MidiException>()),
            reason: 'a SysEx beyond the converter buffer must fail cleanly',
          );
          // Still usable afterwards: the refusal must not leave the port or the
          // converter in a broken state.
          expect(
            () => output.send(
                MidiMessageBuilder.noteOn(channel: 0, note: 60, velocity: 1)),
            returnsNormally,
          );
        } finally {
          output.dispose();
        }
      } finally {
        observer.dispose();
      }
    });

    // The MIDI 1 to UMP converter writes into a fixed buffer and checked the
    // room for SysEx only. Channel voice messages went in unchecked, and each
    // three-byte message becomes an eight-byte UMP, so a run of them past
    // roughly 24 KB in a single send() wrote beyond the end of it. send() takes
    // a byte range, so the length is the caller's to choose.
    testWidgets('a long run of messages in one send is refused, not written '
        'past the buffer', (tester) async {
      final observer = MidiObserver();
      try {
        final pair = virtualLoopbackPair(
            observer.getInputPorts(), observer.getOutputPorts());
        final outPort = pair?.$2;
        if (outPort == null) {
          markTestSkipped('no virtual loopback port pair available');
          return;
        }

        final output = observer.openOutput(outPort);
        try {
          // 12000 note-ons: 36000 bytes in, 96000 bytes of UMP out, well past
          // the 64 KiB conversion buffer.
          final many = <int>[];
          for (var i = 0; i < 12000; i++) {
            many.addAll(
                MidiMessageBuilder.noteOn(channel: 0, note: 60, velocity: 64));
          }
          expect(
            () => output.send(Uint8List.fromList(many)),
            throwsA(isA<MidiException>()),
            reason: 'more UMP than the conversion buffer holds must be refused',
          );
          // A refusal must leave the port usable.
          expect(
            () => output.send(
                MidiMessageBuilder.noteOn(channel: 0, note: 60, velocity: 1)),
            returnsNormally,
          );
        } finally {
          output.dispose();
        }
      } finally {
        observer.dispose();
      }
    });

    // openInputAsync/openOutputAsync open on a native thread and hand the
    // ports back to this isolate; without this test a break in that hand-over
    // (ticket not taken, handle not adopted, messages posted to the wrong
    // port) would only show up in an app. Same loopback requirement as above.
    testWidgets('ports opened asynchronously carry messages', (tester) async {
      final observer = MidiObserver();
      try {
        final pair = virtualLoopbackPair(
            observer.getInputPorts(), observer.getOutputPorts());
        final inPort = pair?.$1;
        final outPort = pair?.$2;
        if (inPort == null || outPort == null) {
          markTestSkipped('no virtual loopback port pair available');
          return;
        }

        late final MidiInput input;
        late final MidiOutput output;
        await tester.runAsync(() async {
          input = await observer.openInputAsync(inPort);
          output = await observer.openOutputAsync(outPort);
        });
        final received = <MidiMessage>[];
        final sub = input.messages.listen(received.add);
        try {
          expect(input.isConnected, isTrue);
          expect(output.isConnected, isTrue);
          output.send(MidiMessageBuilder.noteOn(channel: 4, note: 72, velocity: 5));
          for (var i = 0; i < 40 && received.isEmpty; i++) {
            await tester.runAsync(
              () => Future<void>.delayed(const Duration(milliseconds: 50)),
            );
          }
          expect(received.where((m) => m.isNoteOn && m.note == 72 && m.channel == 4),
              hasLength(1));
        } finally {
          await sub.cancel();
          input.dispose();
          output.dispose();
        }
      } finally {
        observer.dispose();
      }
    });

    // LibremidiFlutter.dispose() promises to release everything. Without the
    // generation check, an async open that completes after it registered its
    // port anyway, leaving a connection nobody can reach to close.
    testWidgets('dispose during an async open releases the port', (
      tester,
    ) async {
      MidiPort? inPort;
      MidiPort? outPort;
      // LibremidiFlutter's observer is a hotplug one, which on iOS/macOS fills
      // its port list in the background; give it a moment.
      for (var attempt = 0; attempt < 40 && inPort == null; attempt++) {
        final pair = virtualLoopbackPair(
            LibremidiFlutter.getInputPorts(), LibremidiFlutter.getOutputPorts());
        inPort = pair?.$1;
        outPort = pair?.$2;
        if (inPort == null) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 50)),
          );
        }
      }
      if (inPort == null || outPort == null) {
        LibremidiFlutter.dispose();
        markTestSkipped('no virtual loopback port pair available');
        return;
      }

      await tester.runAsync(() async {
        final inputF = LibremidiFlutter.openInputAsync(inPort!);
        final outputF = LibremidiFlutter.openOutputAsync(outPort!);
        LibremidiFlutter.dispose();
        await expectLater(inputF, throwsStateError);
        await expectLater(outputF, throwsStateError);
      });
      expect(LibremidiFlutter.openInputCount, 0);
      expect(LibremidiFlutter.openOutputCount, 0);

      // Nothing stale blocks the ports: they open again normally. The fresh
      // observer behind LibremidiFlutter lists them asynchronously on
      // iOS/macOS, so wait for the input to reappear first.
      for (var attempt = 0;
          attempt < 40 &&
              !LibremidiFlutter.getInputPorts()
                  .any((p) => p.portId == inPort!.portId);
          attempt++) {
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 50)),
        );
      }
      final input = LibremidiFlutter.openInput(inPort);
      final output = LibremidiFlutter.openOutput(outPort);
      expect(LibremidiFlutter.openInputCount, 1);
      expect(LibremidiFlutter.openOutputCount, 1);
      input.dispose();
      output.dispose();
      LibremidiFlutter.dispose();
    });

    testWidgets('open and dispose without listening does not crash', (
      tester,
    ) async {
      final observer = MidiObserver();
      try {
        final port = safeTestInput(observer);
        if (port == null) return;
        final input = observer.openInput(port);
        input.dispose();
      } finally {
        observer.dispose();
      }
    });
  });
}
