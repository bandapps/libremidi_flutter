import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// Pins that every `MidiOutput.send*` method delegates to [MidiMessageBuilder]
/// instead of assembling MIDI bytes itself.
///
/// `midi_byte_construction_test.dart` covers the builders thoroughly, but it
/// cannot call `MidiOutput`: constructing one opens a native port, so there is
/// no way to drive `sendPitchBend()` in a unit test. That leaves a gap — someone
/// could reimplement `sendPitchBend` inline with the bytes swapped, or reorder
/// the Control Changes in `sendRpn`, and every builder test would stay green
/// while the wire output is wrong.
///
/// This closes the gap from the other side: it reads the source and fails if a
/// `send*` method stops going through the builder, or if MIDI byte literals
/// reappear inside `MidiOutput`. Only the raw `send(Uint8List)` entry point is
/// allowed to touch the FFI binding.
///
/// Pure Dart, no native FFI. Runs from the package root.
void main() {
  late String outputClassBody;

  setUpAll(() {
    final file = File('lib/libremidi_flutter.dart');
    if (!file.existsSync()) {
      fail('Expected file not found (run from package root): ${file.path}');
    }
    final source = file.readAsStringSync();

    // Match the declarations by name, not by their exact line: the classes
    // gained `implements Finalizable`, which broke a literal 'class X {'.
    final start = source.indexOf(RegExp(r'^class MidiOutput\b', multiLine: true));
    final end = source.indexOf(RegExp(r'^class MidiInput\b', multiLine: true));
    expect(start, greaterThan(-1), reason: 'class MidiOutput not found');
    expect(end, greaterThan(start),
        reason: 'class MidiInput not found after MidiOutput — the slice below '
            'would be wrong');
    outputClassBody = source.substring(start, end);
  });

  /// Matches the start of a public message-sending method: sendNoteOn,
  /// sendRpn, incrementNrpn, ... but deliberately not the raw `send(`.
  final methodStart =
      RegExp(r'\n  (?:void|Future<void>) ((?:send[A-Z]|increment|decrement)\w*)\(');

  test('every send* method delegates to MidiMessageBuilder', () {
    final matches = methodStart.allMatches(outputClassBody).toList();
    expect(matches.length, greaterThanOrEqualTo(14),
        reason: 'found only ${matches.length} send*/increment*/decrement* '
            'methods — the matcher is probably out of date with the API');

    final offenders = <String>[];
    for (var i = 0; i < matches.length; i++) {
      final name = matches[i].group(1)!;
      final bodyEnd = i + 1 < matches.length
          ? matches[i + 1].start
          : outputClassBody.length;
      final body = outputClassBody.substring(matches[i].start, bodyEnd);
      if (!body.contains('MidiMessageBuilder.')) {
        offenders.add(name);
      }
    }

    expect(offenders, isEmpty,
        reason: 'these MidiOutput methods no longer build their bytes through '
            'MidiMessageBuilder, so midi_byte_construction_test.dart no longer '
            'covers what they put on the wire: $offenders');
  });

  test('MidiOutput contains no inline MIDI byte construction', () {
    // Regression: reimplementing a message inline (e.g.
    // `send(Uint8List.fromList([0xE0 | channel, msb, lsb]))`) would bypass the
    // tested builders and could silently swap pitch bend's LSB/MSB.
    expect(outputClassBody.contains('Uint8List.fromList('), isFalse,
        reason: 'MidiOutput assembles MIDI bytes itself again — move the '
            'encoding into MidiMessageBuilder so it stays covered by tests');
  });

  test('only the raw send() talks to the native binding', () {
    // The FFI call belongs in exactly one place; a send* method calling
    // lrm_midi_out_send directly would skip the disposal check in send().
    final bindingCalls =
        RegExp(r'_bindings\.lrm_midi_out_send').allMatches(outputClassBody);
    expect(bindingCalls.length, 1,
        reason: 'expected exactly one lrm_midi_out_send call in MidiOutput, '
            'found ${bindingCalls.length}');
  });
}
