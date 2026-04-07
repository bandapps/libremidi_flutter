import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

void main() {
  group('MidiException', () {
    test('stores message', () {
      const ex = MidiException('test error');
      expect(ex.message, 'test error');
      expect(ex.errorCode, isNull);
      expect(ex.nativeFunction, isNull);
    });

    test('stores errorCode', () {
      const ex = MidiException('fail', errorCode: -1);
      expect(ex.errorCode, -1);
    });

    test('stores nativeFunction', () {
      const ex = MidiException('fail', nativeFunction: 'lrm_midi_out_send');
      expect(ex.nativeFunction, 'lrm_midi_out_send');
    });

    test('implements Exception', () {
      const ex = MidiException('test');
      expect(ex, isA<Exception>());
    });
  });

  group('MidiException.toString', () {
    test('message only', () {
      const ex = MidiException('Something went wrong');
      expect(ex.toString(), 'MidiException: Something went wrong');
    });

    test('with nativeFunction', () {
      const ex = MidiException(
        'Send failed',
        nativeFunction: 'lrm_midi_out_send',
      );
      expect(
        ex.toString(),
        'MidiException: Send failed in lrm_midi_out_send',
      );
    });

    test('with errorCode', () {
      const ex = MidiException('Open failed', errorCode: -10);
      expect(ex.toString(), 'MidiException: Open failed (code: -10)');
    });

    test('with both nativeFunction and errorCode', () {
      const ex = MidiException(
        'Connection lost',
        nativeFunction: 'lrm_midi_in_open',
        errorCode: 42,
      );
      expect(
        ex.toString(),
        'MidiException: Connection lost in lrm_midi_in_open (code: 42)',
      );
    });

    test('can be used in try-catch', () {
      try {
        throw const MidiException('test', errorCode: 1);
      } on MidiException catch (e) {
        expect(e.message, 'test');
        expect(e.errorCode, 1);
      }
    });
  });
}
