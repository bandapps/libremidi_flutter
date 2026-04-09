import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

MidiMessage cc(int channel, int controller, int value) {
  return MidiMessage(
    Uint8List.fromList([0xB0 | channel, controller & 0x7F, value & 0x7F]),
  );
}

void main() {
  group('RpnNrpnParser', () {
    test('decodes 7-bit RPN data entry', () {
      final parser = RpnNrpnParser();

      expect(parser.process(cc(0, 101, 0)), isNull);
      expect(parser.process(cc(0, 100, 0)), isNull);

      final event = parser.process(cc(0, 6, 2));

      expect(event, isNotNull);
      expect(event!.type, RpnNrpnType.rpn);
      expect(event.changeType, RpnNrpnChangeType.value);
      expect(event.channel, 0);
      expect(event.parameter, 0);
      expect(event.value, 2);
      expect(event.fourteenBit, isFalse);
    });

    test('decodes 14-bit RPN data entry refinement', () {
      final parser = RpnNrpnParser();

      parser.process(cc(2, 101, 0));
      parser.process(cc(2, 100, 0));

      final msbEvent = parser.process(cc(2, 6, 64));
      final lsbEvent = parser.process(cc(2, 38, 1));

      expect(msbEvent, isNotNull);
      expect(msbEvent!.value, 64);
      expect(msbEvent.fourteenBit, isFalse);

      expect(lsbEvent, isNotNull);
      expect(lsbEvent!.type, RpnNrpnType.rpn);
      expect(lsbEvent.changeType, RpnNrpnChangeType.value);
      expect(lsbEvent.channel, 2);
      expect(lsbEvent.parameter, 0);
      expect(lsbEvent.value, 8193);
      expect(lsbEvent.fourteenBit, isTrue);
    });

    test('decodes 14-bit NRPN data entry', () {
      final parser = RpnNrpnParser();

      parser.process(cc(15, 99, 9));
      parser.process(cc(15, 98, 82));
      final msbEvent = parser.process(cc(15, 6, 2));
      final lsbEvent = parser.process(cc(15, 38, 0));

      expect(msbEvent, isNotNull);
      expect(msbEvent!.type, RpnNrpnType.nrpn);
      expect(msbEvent.changeType, RpnNrpnChangeType.value);
      expect(msbEvent.parameter, 1234);
      expect(msbEvent.value, 2);
      expect(msbEvent.fourteenBit, isFalse);

      expect(lsbEvent, isNotNull);
      expect(lsbEvent!.type, RpnNrpnType.nrpn);
      expect(lsbEvent.changeType, RpnNrpnChangeType.value);
      expect(lsbEvent.channel, 15);
      expect(lsbEvent.parameter, 1234);
      expect(lsbEvent.value, 256);
      expect(lsbEvent.fourteenBit, isTrue);
    });

    test('RPN null function deselects the selected parameter', () {
      final parser = RpnNrpnParser();

      parser.process(cc(0, 101, 0));
      parser.process(cc(0, 100, 0));
      expect(parser.process(cc(0, 6, 2)), isNotNull);

      parser.process(cc(0, 101, 127));
      parser.process(cc(0, 100, 127));

      expect(parser.process(cc(0, 6, 3)), isNull);
    });

    test('NRPN null function deselects the selected parameter', () {
      final parser = RpnNrpnParser();

      parser.process(cc(0, 99, 0));
      parser.process(cc(0, 98, 1));
      expect(parser.process(cc(0, 6, 2)), isNotNull);

      parser.process(cc(0, 99, 127));
      parser.process(cc(0, 98, 127));

      expect(parser.process(cc(0, 6, 3)), isNull);
    });

    test('decodes RPN data increment and decrement', () {
      final parser = RpnNrpnParser();

      parser.process(cc(3, 101, 0));
      parser.process(cc(3, 100, 0));

      final increment = parser.process(cc(3, 96, 1));
      final decrement = parser.process(cc(3, 97, 2));

      expect(increment, isNotNull);
      expect(increment!.type, RpnNrpnType.rpn);
      expect(increment.changeType, RpnNrpnChangeType.increment);
      expect(increment.channel, 3);
      expect(increment.parameter, 0);
      expect(increment.value, 1);
      expect(increment.fourteenBit, isFalse);

      expect(decrement, isNotNull);
      expect(decrement!.type, RpnNrpnType.rpn);
      expect(decrement.changeType, RpnNrpnChangeType.decrement);
      expect(decrement.channel, 3);
      expect(decrement.parameter, 0);
      expect(decrement.value, 2);
      expect(decrement.fourteenBit, isFalse);
    });

    test('decodes NRPN data increment and decrement', () {
      final parser = RpnNrpnParser();

      parser.process(cc(4, 99, 9));
      parser.process(cc(4, 98, 82));

      final increment = parser.process(cc(4, 96, 1));
      final decrement = parser.process(cc(4, 97, 1));

      expect(increment, isNotNull);
      expect(increment!.type, RpnNrpnType.nrpn);
      expect(increment.changeType, RpnNrpnChangeType.increment);
      expect(increment.channel, 4);
      expect(increment.parameter, 1234);
      expect(increment.value, 1);

      expect(decrement, isNotNull);
      expect(decrement!.type, RpnNrpnType.nrpn);
      expect(decrement.changeType, RpnNrpnChangeType.decrement);
      expect(decrement.channel, 4);
      expect(decrement.parameter, 1234);
      expect(decrement.value, 1);
    });

    test('keeps RPN and NRPN selection state separate per channel', () {
      final parser = RpnNrpnParser();

      parser.process(cc(0, 101, 0));
      parser.process(cc(0, 100, 1));
      parser.process(cc(1, 99, 0));
      parser.process(cc(1, 98, 2));

      final rpnEvent = parser.process(cc(0, 6, 12));
      final nrpnEvent = parser.process(cc(1, 6, 34));

      expect(rpnEvent, isNotNull);
      expect(rpnEvent!.type, RpnNrpnType.rpn);
      expect(rpnEvent.changeType, RpnNrpnChangeType.value);
      expect(rpnEvent.channel, 0);
      expect(rpnEvent.parameter, 1);
      expect(rpnEvent.value, 12);

      expect(nrpnEvent, isNotNull);
      expect(nrpnEvent!.type, RpnNrpnType.nrpn);
      expect(nrpnEvent.changeType, RpnNrpnChangeType.value);
      expect(nrpnEvent.channel, 1);
      expect(nrpnEvent.parameter, 2);
      expect(nrpnEvent.value, 34);
    });

    test('ignores non-CC and incomplete CC messages', () {
      final parser = RpnNrpnParser();

      expect(
        parser.process(MidiMessage(Uint8List.fromList([0x90, 60, 100]))),
        isNull,
      );
      expect(
          parser.process(MidiMessage(Uint8List.fromList([0xB0, 6]))), isNull);
    });
  });
}
