import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

void main() {
  group('MidiMessage constructor', () {
    test('stores data and default timestamp', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 100]));
      expect(msg.data, Uint8List.fromList([0x90, 60, 100]));
      expect(msg.timestamp, 0);
    });

    test('stores custom timestamp', () {
      final msg = MidiMessage(
        Uint8List.fromList([0x90, 60, 100]),
        timestamp: 123456,
      );
      expect(msg.timestamp, 123456);
    });
  });

  group('MidiMessage status byte parsing', () {
    test('status returns first byte', () {
      final msg = MidiMessage(Uint8List.fromList([0xB0, 7, 100]));
      expect(msg.status, 0xB0);
    });

    test('status returns 0 for empty data', () {
      final msg = MidiMessage(Uint8List(0));
      expect(msg.status, 0);
    });

    test('type returns high nibble', () {
      final msg = MidiMessage(Uint8List.fromList([0x95, 60, 100]));
      expect(msg.type, 0x90);
    });

    test('channel returns low nibble', () {
      final msg = MidiMessage(Uint8List.fromList([0x95, 60, 100]));
      expect(msg.channel, 5);
    });

    test('channel 0 for channel 0', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 100]));
      expect(msg.channel, 0);
    });

    test('channel 15 for 0x9F', () {
      final msg = MidiMessage(Uint8List.fromList([0x9F, 60, 100]));
      expect(msg.channel, 15);
    });
  });

  group('MidiMessage.isNoteOn', () {
    test('true for 0x90 with velocity > 0', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 100]));
      expect(msg.isNoteOn, isTrue);
    });

    test('false for 0x90 with velocity 0 (is Note Off)', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 0]));
      expect(msg.isNoteOn, isFalse);
    });

    test('true on any channel', () {
      for (var ch = 0; ch < 16; ch++) {
        final msg = MidiMessage(Uint8List.fromList([0x90 | ch, 60, 100]));
        expect(msg.isNoteOn, isTrue, reason: 'channel $ch');
      }
    });

    test('false for Note Off status 0x80', () {
      final msg = MidiMessage(Uint8List.fromList([0x80, 60, 64]));
      expect(msg.isNoteOn, isFalse);
    });

    test('false for data with fewer than 3 bytes', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60]));
      expect(msg.isNoteOn, isFalse);
    });

    test('false for CC message', () {
      final msg = MidiMessage(Uint8List.fromList([0xB0, 7, 100]));
      expect(msg.isNoteOn, isFalse);
    });
  });

  group('MidiMessage.isNoteOff', () {
    test('true for 0x80 status', () {
      final msg = MidiMessage(Uint8List.fromList([0x80, 60, 64]));
      expect(msg.isNoteOff, isTrue);
    });

    test('true for 0x90 with velocity 0', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 0]));
      expect(msg.isNoteOff, isTrue);
    });

    test('false for 0x90 with velocity > 0', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 100]));
      expect(msg.isNoteOff, isFalse);
    });

    test('true on any channel with 0x80', () {
      for (var ch = 0; ch < 16; ch++) {
        final msg = MidiMessage(Uint8List.fromList([0x80 | ch, 60, 64]));
        expect(msg.isNoteOff, isTrue, reason: 'channel $ch');
      }
    });
  });

  group('MidiMessage.isControlChange', () {
    test('true for 0xB0', () {
      final msg = MidiMessage(Uint8List.fromList([0xB0, 7, 100]));
      expect(msg.isControlChange, isTrue);
    });

    test('true on channel 15', () {
      final msg = MidiMessage(Uint8List.fromList([0xBF, 1, 64]));
      expect(msg.isControlChange, isTrue);
    });

    test('false for Note On', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 100]));
      expect(msg.isControlChange, isFalse);
    });
  });

  group('MidiMessage.isProgramChange', () {
    test('true for 0xC0', () {
      final msg = MidiMessage(Uint8List.fromList([0xC0, 5]));
      expect(msg.isProgramChange, isTrue);
    });

    test('true on channel 9', () {
      final msg = MidiMessage(Uint8List.fromList([0xC9, 0]));
      expect(msg.isProgramChange, isTrue);
    });

    test('false for CC', () {
      final msg = MidiMessage(Uint8List.fromList([0xB0, 7, 100]));
      expect(msg.isProgramChange, isFalse);
    });
  });

  group('MidiMessage.isPitchBend', () {
    test('true for 0xE0', () {
      final msg = MidiMessage(Uint8List.fromList([0xE0, 0, 64]));
      expect(msg.isPitchBend, isTrue);
    });

    test('true on channel 7', () {
      final msg = MidiMessage(Uint8List.fromList([0xE7, 0, 64]));
      expect(msg.isPitchBend, isTrue);
    });

    test('false for Note On', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 100]));
      expect(msg.isPitchBend, isFalse);
    });
  });

  group('MidiMessage.isSysEx', () {
    test('true for 0xF0 start byte', () {
      final msg = MidiMessage(
        Uint8List.fromList([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7]),
      );
      expect(msg.isSysEx, isTrue);
    });

    test('false for channel messages', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 100]));
      expect(msg.isSysEx, isFalse);
    });

    test('false for empty data', () {
      final msg = MidiMessage(Uint8List(0));
      expect(msg.isSysEx, isFalse);
    });
  });

  group('MidiMessage.isAftertouch', () {
    test('true for 0xD0 (Channel Aftertouch)', () {
      final msg = MidiMessage(Uint8List.fromList([0xD0, 100]));
      expect(msg.isAftertouch, isTrue);
    });

    test('true on channel 3', () {
      final msg = MidiMessage(Uint8List.fromList([0xD3, 50]));
      expect(msg.isAftertouch, isTrue);
    });

    test('false for Poly Aftertouch', () {
      final msg = MidiMessage(Uint8List.fromList([0xA0, 60, 80]));
      expect(msg.isAftertouch, isFalse);
    });
  });

  group('MidiMessage.isPolyAftertouch', () {
    test('true for 0xA0', () {
      final msg = MidiMessage(Uint8List.fromList([0xA0, 60, 80]));
      expect(msg.isPolyAftertouch, isTrue);
    });

    test('true on channel 10', () {
      final msg = MidiMessage(Uint8List.fromList([0xAA, 48, 90]));
      expect(msg.isPolyAftertouch, isTrue);
    });

    test('false for Channel Aftertouch', () {
      final msg = MidiMessage(Uint8List.fromList([0xD0, 100]));
      expect(msg.isPolyAftertouch, isFalse);
    });
  });

  group('MidiMessage data accessors', () {
    test('note returns byte 1', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 72, 100]));
      expect(msg.note, 72);
    });

    test('note returns 0 for single-byte data', () {
      final msg = MidiMessage(Uint8List.fromList([0x90]));
      expect(msg.note, 0);
    });

    test('velocity returns byte 2', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 127]));
      expect(msg.velocity, 127);
    });

    test('velocity returns 0 for two-byte data', () {
      final msg = MidiMessage(Uint8List.fromList([0xC0, 5]));
      expect(msg.velocity, 0);
    });

    test('controller returns byte 1 for CC', () {
      final msg = MidiMessage(Uint8List.fromList([0xB0, 64, 127]));
      expect(msg.controller, 64);
    });

    test('value returns byte 2 for CC', () {
      final msg = MidiMessage(Uint8List.fromList([0xB0, 7, 100]));
      expect(msg.value, 100);
    });

    test('value returns 0 for empty data', () {
      final msg = MidiMessage(Uint8List(0));
      expect(msg.value, 0);
    });
  });

  group('MidiMessage.toString', () {
    test('formats bytes as hex', () {
      final msg = MidiMessage(Uint8List.fromList([0x90, 60, 100]));
      expect(msg.toString(), 'MidiMessage(90 3c 64)');
    });

    test('pads single-digit hex values', () {
      final msg = MidiMessage(Uint8List.fromList([0x80, 0, 0]));
      expect(msg.toString(), 'MidiMessage(80 00 00)');
    });

    test('empty data', () {
      final msg = MidiMessage(Uint8List(0));
      expect(msg.toString(), 'MidiMessage()');
    });

    test('SysEx message', () {
      final msg = MidiMessage(Uint8List.fromList([0xF0, 0x7E, 0xF7]));
      expect(msg.toString(), 'MidiMessage(f0 7e f7)');
    });
  });

  group('MidiMessage edge cases', () {
    test('empty message has safe defaults', () {
      final msg = MidiMessage(Uint8List(0));
      expect(msg.status, 0);
      expect(msg.type, 0);
      expect(msg.channel, 0);
      expect(msg.note, 0);
      expect(msg.velocity, 0);
      expect(msg.controller, 0);
      expect(msg.value, 0);
      expect(msg.isNoteOn, isFalse);
      expect(msg.isNoteOff, isFalse);
      expect(msg.isControlChange, isFalse);
      expect(msg.isProgramChange, isFalse);
      expect(msg.isPitchBend, isFalse);
      expect(msg.isSysEx, isFalse);
      expect(msg.isAftertouch, isFalse);
      expect(msg.isPolyAftertouch, isFalse);
    });

    test('single byte message has safe defaults for data accessors', () {
      final msg = MidiMessage(Uint8List.fromList([0x90]));
      expect(msg.status, 0x90);
      expect(msg.note, 0);
      expect(msg.velocity, 0);
      expect(msg.isNoteOn, isFalse); // needs 3 bytes
    });

    test('all MIDI note range 0-127', () {
      for (var note = 0; note < 128; note++) {
        final msg = MidiMessage(Uint8List.fromList([0x90, note, 100]));
        expect(msg.note, note);
        expect(msg.isNoteOn, isTrue);
      }
    });

    test('all MIDI channels 0-15', () {
      for (var ch = 0; ch < 16; ch++) {
        final msg = MidiMessage(Uint8List.fromList([0x90 | ch, 60, 100]));
        expect(msg.channel, ch);
        expect(msg.type, 0x90);
      }
    });
  });
}
