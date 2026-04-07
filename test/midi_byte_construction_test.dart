import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

/// These tests verify that MIDI byte patterns (as constructed by MidiOutput
/// send methods) are correctly parsed back by MidiMessage. This validates
/// the MIDI protocol implementation without requiring native FFI.

void main() {
  group('Note On byte construction', () {
    test('channel 0, note 60, velocity 100', () {
      final bytes = Uint8List.fromList([0x90 | 0, 60 & 0x7F, 100 & 0x7F]);
      final msg = MidiMessage(bytes);
      expect(msg.isNoteOn, isTrue);
      expect(msg.isNoteOff, isFalse);
      expect(msg.channel, 0);
      expect(msg.note, 60);
      expect(msg.velocity, 100);
    });

    test('channel 15, note 127, velocity 127', () {
      final bytes = Uint8List.fromList([0x90 | 15, 127 & 0x7F, 127 & 0x7F]);
      final msg = MidiMessage(bytes);
      expect(msg.isNoteOn, isTrue);
      expect(msg.channel, 15);
      expect(msg.note, 127);
      expect(msg.velocity, 127);
    });

    test('channel 0, note 0, velocity 1 (minimum non-zero)', () {
      final bytes = Uint8List.fromList([0x90, 0, 1]);
      final msg = MidiMessage(bytes);
      expect(msg.isNoteOn, isTrue);
      expect(msg.note, 0);
      expect(msg.velocity, 1);
    });
  });

  group('Note Off byte construction', () {
    test('explicit Note Off (0x80)', () {
      final bytes = Uint8List.fromList([0x80 | 0, 60 & 0x7F, 64 & 0x7F]);
      final msg = MidiMessage(bytes);
      expect(msg.isNoteOff, isTrue);
      expect(msg.isNoteOn, isFalse);
      expect(msg.channel, 0);
      expect(msg.note, 60);
      expect(msg.velocity, 64);
    });

    test('Note On with velocity 0 is Note Off', () {
      final bytes = Uint8List.fromList([0x90 | 5, 72, 0]);
      final msg = MidiMessage(bytes);
      expect(msg.isNoteOff, isTrue);
      expect(msg.isNoteOn, isFalse);
      expect(msg.channel, 5);
      expect(msg.note, 72);
    });
  });

  group('Control Change byte construction', () {
    test('volume (CC 7) on channel 0', () {
      final bytes = Uint8List.fromList([0xB0 | 0, 7 & 0x7F, 100 & 0x7F]);
      final msg = MidiMessage(bytes);
      expect(msg.isControlChange, isTrue);
      expect(msg.channel, 0);
      expect(msg.controller, 7);
      expect(msg.value, 100);
    });

    test('sustain pedal (CC 64) on, channel 3', () {
      final bytes = Uint8List.fromList([0xB0 | 3, 64, 127]);
      final msg = MidiMessage(bytes);
      expect(msg.isControlChange, isTrue);
      expect(msg.channel, 3);
      expect(msg.controller, 64);
      expect(msg.value, 127);
    });

    test('sustain pedal (CC 64) off', () {
      final bytes = Uint8List.fromList([0xB0, 64, 0]);
      final msg = MidiMessage(bytes);
      expect(msg.isControlChange, isTrue);
      expect(msg.controller, 64);
      expect(msg.value, 0);
    });

    test('modulation wheel (CC 1)', () {
      final bytes = Uint8List.fromList([0xB0, 1, 64]);
      final msg = MidiMessage(bytes);
      expect(msg.isControlChange, isTrue);
      expect(msg.controller, 1);
      expect(msg.value, 64);
    });

    test('all controllers 0-127 produce valid CC messages', () {
      for (var cc = 0; cc < 128; cc++) {
        final bytes = Uint8List.fromList([0xB0, cc, 0]);
        final msg = MidiMessage(bytes);
        expect(msg.isControlChange, isTrue, reason: 'CC $cc');
        expect(msg.controller, cc, reason: 'CC $cc');
      }
    });
  });

  group('Program Change byte construction', () {
    test('program 0 on channel 0', () {
      final bytes = Uint8List.fromList([0xC0 | 0, 0 & 0x7F]);
      final msg = MidiMessage(bytes);
      expect(msg.isProgramChange, isTrue);
      expect(msg.channel, 0);
      expect(msg.note, 0); // byte 1 accessor
    });

    test('program 127 on channel 9 (drums)', () {
      final bytes = Uint8List.fromList([0xC0 | 9, 127]);
      final msg = MidiMessage(bytes);
      expect(msg.isProgramChange, isTrue);
      expect(msg.channel, 9);
      expect(msg.note, 127); // program stored in byte 1
    });

    test('all programs 0-127', () {
      for (var prog = 0; prog < 128; prog++) {
        final bytes = Uint8List.fromList([0xC0, prog]);
        final msg = MidiMessage(bytes);
        expect(msg.isProgramChange, isTrue, reason: 'program $prog');
      }
    });
  });

  group('Pitch Bend byte construction', () {
    test('center value (8192)', () {
      const value = 8192;
      final lsb = value & 0x7F;
      final msb = (value >> 7) & 0x7F;
      final bytes = Uint8List.fromList([0xE0 | 0, lsb, msb]);
      final msg = MidiMessage(bytes);
      expect(msg.isPitchBend, isTrue);
      expect(msg.channel, 0);
      // Reconstruct 14-bit value from parsed bytes
      final reconstructed = msg.data[1] | (msg.data[2] << 7);
      expect(reconstructed, 8192);
    });

    test('minimum value (0)', () {
      final bytes = Uint8List.fromList([0xE0, 0, 0]);
      final msg = MidiMessage(bytes);
      expect(msg.isPitchBend, isTrue);
      final reconstructed = msg.data[1] | (msg.data[2] << 7);
      expect(reconstructed, 0);
    });

    test('maximum value (16383)', () {
      const value = 16383;
      final lsb = value & 0x7F;
      final msb = (value >> 7) & 0x7F;
      final bytes = Uint8List.fromList([0xE0, lsb, msb]);
      final msg = MidiMessage(bytes);
      expect(msg.isPitchBend, isTrue);
      final reconstructed = msg.data[1] | (msg.data[2] << 7);
      expect(reconstructed, 16383);
    });

    test('pitch bend on channel 7', () {
      final bytes = Uint8List.fromList([0xE0 | 7, 0, 64]);
      final msg = MidiMessage(bytes);
      expect(msg.isPitchBend, isTrue);
      expect(msg.channel, 7);
    });

    test('14-bit roundtrip for various values', () {
      for (final value in [0, 1, 64, 8191, 8192, 8193, 16382, 16383]) {
        final lsb = value & 0x7F;
        final msb = (value >> 7) & 0x7F;
        final bytes = Uint8List.fromList([0xE0, lsb, msb]);
        final msg = MidiMessage(bytes);
        final reconstructed = msg.data[1] | (msg.data[2] << 7);
        expect(reconstructed, value, reason: 'pitch bend $value');
      }
    });
  });

  group('SysEx byte construction', () {
    test('unframed data gets framed with F0/F7', () {
      final payload = Uint8List.fromList([0x7E, 0x7F, 0x09, 0x01]);
      final message = Uint8List(payload.length + 2);
      message[0] = 0xF0;
      message.setRange(1, payload.length + 1, payload);
      message[payload.length + 1] = 0xF7;

      final msg = MidiMessage(message);
      expect(msg.isSysEx, isTrue);
      expect(msg.data.first, 0xF0);
      expect(msg.data.last, 0xF7);
      expect(msg.data.length, 6);
    });

    test('already framed data passes through', () {
      final framed = Uint8List.fromList([0xF0, 0x43, 0x12, 0xF7]);
      final msg = MidiMessage(framed);
      expect(msg.isSysEx, isTrue);
      expect(msg.data, framed);
    });

    test('SysEx is not Note On/Off/CC/PC/PitchBend', () {
      final msg = MidiMessage(Uint8List.fromList([0xF0, 0x00, 0xF7]));
      expect(msg.isSysEx, isTrue);
      expect(msg.isNoteOn, isFalse);
      expect(msg.isNoteOff, isFalse);
      expect(msg.isControlChange, isFalse);
      expect(msg.isProgramChange, isFalse);
      expect(msg.isPitchBend, isFalse);
      expect(msg.isAftertouch, isFalse);
      expect(msg.isPolyAftertouch, isFalse);
    });
  });

  group('Channel Aftertouch byte construction', () {
    test('channel 0, pressure 100', () {
      final bytes = Uint8List.fromList([0xD0 | 0, 100 & 0x7F]);
      final msg = MidiMessage(bytes);
      expect(msg.isAftertouch, isTrue);
      expect(msg.isPolyAftertouch, isFalse);
      expect(msg.channel, 0);
      expect(msg.note, 100); // pressure is in byte 1
    });

    test('channel 15, pressure 0', () {
      final bytes = Uint8List.fromList([0xD0 | 15, 0]);
      final msg = MidiMessage(bytes);
      expect(msg.isAftertouch, isTrue);
      expect(msg.channel, 15);
    });

    test('channel 8, pressure 127', () {
      final bytes = Uint8List.fromList([0xD8, 127]);
      final msg = MidiMessage(bytes);
      expect(msg.isAftertouch, isTrue);
      expect(msg.channel, 8);
      expect(msg.note, 127);
    });
  });

  group('Polyphonic Aftertouch byte construction', () {
    test('channel 0, note 60, pressure 80', () {
      final bytes = Uint8List.fromList([0xA0 | 0, 60 & 0x7F, 80 & 0x7F]);
      final msg = MidiMessage(bytes);
      expect(msg.isPolyAftertouch, isTrue);
      expect(msg.isAftertouch, isFalse);
      expect(msg.channel, 0);
      expect(msg.note, 60);
      expect(msg.velocity, 80); // pressure is in byte 2
    });

    test('channel 15, note 127, pressure 127', () {
      final bytes = Uint8List.fromList([0xAF, 127, 127]);
      final msg = MidiMessage(bytes);
      expect(msg.isPolyAftertouch, isTrue);
      expect(msg.channel, 15);
      expect(msg.note, 127);
      expect(msg.velocity, 127);
    });
  });

  group('Bank Select byte construction', () {
    test('bank 0, program 0 produces correct CC sequence', () {
      const bank = 0;
      const program = 0;
      final msb = (bank >> 7) & 0x7F;
      final lsb = bank & 0x7F;

      // CC 0 (Bank MSB)
      final cc0 = MidiMessage(Uint8List.fromList([0xB0, 0, msb]));
      expect(cc0.isControlChange, isTrue);
      expect(cc0.controller, 0);
      expect(cc0.value, 0);

      // CC 32 (Bank LSB)
      final cc32 = MidiMessage(Uint8List.fromList([0xB0, 32, lsb]));
      expect(cc32.isControlChange, isTrue);
      expect(cc32.controller, 32);
      expect(cc32.value, 0);

      // Program Change
      final pc = MidiMessage(Uint8List.fromList([0xC0, program]));
      expect(pc.isProgramChange, isTrue);
    });

    test('bank 16383 (max) splits correctly into MSB/LSB', () {
      const bank = 16383;
      final msb = (bank >> 7) & 0x7F;
      final lsb = bank & 0x7F;
      expect(msb, 127);
      expect(lsb, 127);

      final cc0 = MidiMessage(Uint8List.fromList([0xB0, 0, msb]));
      expect(cc0.value, 127);

      final cc32 = MidiMessage(Uint8List.fromList([0xB0, 32, lsb]));
      expect(cc32.value, 127);
    });

    test('bank 128 has MSB=1, LSB=0', () {
      const bank = 128;
      final msb = (bank >> 7) & 0x7F;
      final lsb = bank & 0x7F;
      expect(msb, 1);
      expect(lsb, 0);
    });

    test('bank 129 has MSB=1, LSB=1', () {
      const bank = 129;
      final msb = (bank >> 7) & 0x7F;
      final lsb = bank & 0x7F;
      expect(msb, 1);
      expect(lsb, 1);
    });
  });

  group('Message type mutual exclusivity', () {
    test('each message type is exclusively identified', () {
      final messages = <String, MidiMessage>{
        'NoteOn': MidiMessage(Uint8List.fromList([0x90, 60, 100])),
        'NoteOff': MidiMessage(Uint8List.fromList([0x80, 60, 64])),
        'CC': MidiMessage(Uint8List.fromList([0xB0, 7, 100])),
        'PC': MidiMessage(Uint8List.fromList([0xC0, 5])),
        'PitchBend': MidiMessage(Uint8List.fromList([0xE0, 0, 64])),
        'SysEx': MidiMessage(Uint8List.fromList([0xF0, 0x7E, 0xF7])),
        'Aftertouch': MidiMessage(Uint8List.fromList([0xD0, 100])),
        'PolyAftertouch': MidiMessage(Uint8List.fromList([0xA0, 60, 80])),
      };

      for (final entry in messages.entries) {
        final name = entry.key;
        final msg = entry.value;

        expect(
          msg.isNoteOn,
          name == 'NoteOn',
          reason: '$name.isNoteOn',
        );
        // NoteOff is special: both 0x80 and 0x90-vel-0 are NoteOff
        expect(
          msg.isNoteOff,
          name == 'NoteOff',
          reason: '$name.isNoteOff',
        );
        expect(
          msg.isControlChange,
          name == 'CC',
          reason: '$name.isControlChange',
        );
        expect(
          msg.isProgramChange,
          name == 'PC',
          reason: '$name.isProgramChange',
        );
        expect(
          msg.isPitchBend,
          name == 'PitchBend',
          reason: '$name.isPitchBend',
        );
        expect(
          msg.isSysEx,
          name == 'SysEx',
          reason: '$name.isSysEx',
        );
        expect(
          msg.isAftertouch,
          name == 'Aftertouch',
          reason: '$name.isAftertouch',
        );
        expect(
          msg.isPolyAftertouch,
          name == 'PolyAftertouch',
          reason: '$name.isPolyAftertouch',
        );
      }
    });
  });
}
