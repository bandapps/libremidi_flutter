import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

/// Pins the bytes that `MidiOutput.send*` puts on the wire.
///
/// Every `MidiOutput.send*` method is a one-line delegation to
/// [MidiMessageBuilder], so asserting on the builder asserts on the production
/// encoder — the previous version of this file rebuilt the byte patterns inside
/// the test and therefore stayed green no matter what `MidiOutput` actually
/// sent (a swapped pitch-bend LSB/MSB or a reordered RPN sequence went
/// unnoticed).
///
/// `MidiOutput` itself cannot be constructed without an open native port, so
/// the delegation from `send*` to the builder is not covered here; the byte
/// layout below is.
void main() {
  group('channel voice messages', () {
    test('note on/off carry status nibble, note and velocity in order', () {
      // Regression: a swapped note/velocity or a wrong status nibble would
      // play the wrong pitch on every device.
      expect(MidiMessageBuilder.noteOn(channel: 0, note: 60, velocity: 100),
          [0x90, 60, 100]);
      expect(MidiMessageBuilder.noteOn(channel: 15, note: 127, velocity: 1),
          [0x9F, 127, 1]);
      expect(MidiMessageBuilder.noteOff(channel: 5, note: 72, velocity: 64),
          [0x85, 72, 64]);
    });

    test('note off defaults to velocity 0', () {
      expect(MidiMessageBuilder.noteOff(channel: 0, note: 60), [0x80, 60, 0]);
    });

    test('program change and channel aftertouch are two bytes', () {
      // Regression: emitting a third byte here desynchronises the receiver's
      // running-status parser, which then misreads the next message.
      expect(MidiMessageBuilder.programChange(channel: 9, program: 127),
          [0xC9, 127]);
      expect(MidiMessageBuilder.aftertouch(channel: 15, pressure: 100),
          [0xDF, 100]);
      expect(
          MidiMessageBuilder.polyAftertouch(
              channel: 0, note: 60, pressure: 80),
          [0xA0, 60, 80]);
    });

    test('control change puts controller before value', () {
      expect(
          MidiMessageBuilder.controlChange(
              channel: 3, controller: 64, value: 127),
          [0xB3, 64, 127]);
    });
  });

  group('pitch bend', () {
    test('sends LSB first, then MSB', () {
      // Regression: pitch bend is the one 14-bit message that goes LSB first,
      // the opposite of the RPN/NRPN parameter encoding. Swapping the two
      // bytes turns a centred bend (8192) into a near-maximum bend.
      expect(MidiMessageBuilder.pitchBend(channel: 0, value: 8192),
          [0xE0, 0x00, 0x40]);
      expect(MidiMessageBuilder.pitchBend(channel: 7, value: 1),
          [0xE7, 0x01, 0x00]);
      expect(MidiMessageBuilder.pitchBend(channel: 0, value: 16383),
          [0xE0, 0x7F, 0x7F]);
    });

    test('round-trips through MidiMessage parsing', () {
      for (final value in [0, 1, 64, 8191, 8192, 8193, 16382, 16383]) {
        final msg =
            MidiMessage(MidiMessageBuilder.pitchBend(channel: 0, value: value));
        expect(msg.isPitchBend, isTrue);
        expect(msg.data[1] | (msg.data[2] << 7), value,
            reason: 'pitch bend $value');
      }
    });
  });

  group('SysEx', () {
    test('adds F0/F7 framing by default and leaves framed data alone', () {
      final payload = Uint8List.fromList([0x7E, 0x7F, 0x09, 0x01]);
      expect(MidiMessageBuilder.sysEx(payload),
          [0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7]);

      // Regression: framing already-framed data would produce F0 F0 ... F7 F7,
      // which most devices drop silently.
      final framed = Uint8List.fromList([0xF0, 0x43, 0x12, 0xF7]);
      expect(MidiMessageBuilder.sysEx(framed, alreadyFramed: true), framed);
    });
  });

  group('bank select', () {
    test('sends CC 0 (MSB), CC 32 (LSB), then the program change', () {
      // Regression: the program change must come last — a device that receives
      // it before the bank CCs selects the program from the *old* bank.
      final messages =
          MidiMessageBuilder.bankSelect(channel: 2, bank: 129, program: 5);
      expect(messages, [
        [0xB2, 0, 1],
        [0xB2, 32, 1],
        [0xC2, 5],
      ]);
    });

    test('splits a 14-bit bank across MSB and LSB', () {
      final messages =
          MidiMessageBuilder.bankSelect(channel: 0, bank: 16383, program: 0);
      expect(messages.first, [0xB0, 0, 127]);
      expect(messages[1], [0xB0, 32, 127]);
    });
  });

  group('RPN / NRPN', () {
    test('RPN selects with CC 101/100, NRPN with CC 99/98', () {
      // Regression: swapping the two selector pairs addresses a completely
      // different parameter space — an RPN pitch-bend-range change would land
      // on an unrelated device-specific NRPN.
      final rpn = MidiMessageBuilder.parameterNumber(
          channel: 0, parameter: 0, value: 2, registered: true);
      final nrpn = MidiMessageBuilder.parameterNumber(
          channel: 0, parameter: 0, value: 2, registered: false);
      expect(rpn.first[1], 101);
      expect(rpn[1][1], 100);
      expect(nrpn.first[1], 99);
      expect(nrpn[1][1], 98);
    });

    test('14-bit value sends CC 6 (MSB) before CC 38 (LSB), then deselects', () {
      // Regression: CC 38 before CC 6 makes the receiver apply the LSB to the
      // previous parameter value; dropping the deselect leaves the parameter
      // selected so a later stray CC 6 retargets it.
      final messages = MidiMessageBuilder.parameterNumber(
        channel: 2,
        parameter: 129,
        value: 8193,
        registered: true,
      );
      expect(messages, [
        [0xB2, 101, 1], // parameter MSB
        [0xB2, 100, 1], // parameter LSB
        [0xB2, 6, 64], // value MSB
        [0xB2, 38, 1], // value LSB
        [0xB2, 101, 127], // deselect
        [0xB2, 100, 127],
      ]);
    });

    test('7-bit value sends CC 6 only', () {
      final messages = MidiMessageBuilder.parameterNumber(
        channel: 0,
        parameter: 0,
        value: 2,
        registered: true,
        fourteenBit: false,
      );
      expect(messages.map((m) => m[1]), [101, 100, 6, 101, 100]);
      expect(messages[2], [0xB0, 6, 2]);
    });

    test('deselect: false omits the trailing 127/127 pair', () {
      final messages = MidiMessageBuilder.parameterNumber(
        channel: 0,
        parameter: 0,
        value: 2,
        registered: false,
        fourteenBit: false,
        deselect: false,
      );
      expect(messages.map((m) => m[1]), [99, 98, 6]);
    });

    test('increment uses CC 96, decrement uses CC 97', () {
      // Regression: swapping these inverts every relative parameter change.
      final increment = MidiMessageBuilder.parameterNumberRelative(
        channel: 3,
        parameter: 129,
        registered: true,
        increment: true,
        amount: 2,
        deselect: false,
      );
      final decrement = MidiMessageBuilder.parameterNumberRelative(
        channel: 3,
        parameter: 129,
        registered: true,
        increment: false,
        amount: 2,
        deselect: false,
      );
      expect(increment.last, [0xB3, 96, 2]);
      expect(decrement.last, [0xB3, 97, 2]);
    });
  });

  group('range validation', () {
    test('rejects out-of-range arguments instead of truncating them', () {
      // Regression: masking instead of throwing silently retargets the message
      // — channel 16 would wrap to channel 0 and note 128 to note 0.
      expect(() => MidiMessageBuilder.noteOn(channel: 16, note: 0, velocity: 0),
          throwsRangeError);
      expect(() => MidiMessageBuilder.noteOn(channel: 0, note: 128, velocity: 0),
          throwsRangeError);
      expect(
          () => MidiMessageBuilder.controlChange(
              channel: 0, controller: 0, value: 128),
          throwsRangeError);
      expect(() => MidiMessageBuilder.pitchBend(channel: 0, value: 16384),
          throwsRangeError);
    });

    test('7-bit parameter values are capped at 127, 14-bit ones at 16383', () {
      expect(
          () => MidiMessageBuilder.parameterNumber(
              channel: 0,
              parameter: 0,
              value: 128,
              registered: true,
              fourteenBit: false),
          throwsRangeError);
      expect(
          MidiMessageBuilder.parameterNumber(
              channel: 0,
              parameter: 0,
              value: 128,
              registered: true,
              fourteenBit: true),
          isNotEmpty);
      expect(
          () => MidiMessageBuilder.parameterNumber(
              channel: 0,
              parameter: 0,
              value: 16384,
              registered: true,
              fourteenBit: true),
          throwsRangeError);
    });
  });
}
