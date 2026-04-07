import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

void main() {
  group('HotplugEventType.fromValue', () {
    test('0 maps to inputAdded', () {
      expect(HotplugEventType.fromValue(0), HotplugEventType.inputAdded);
    });

    test('1 maps to inputRemoved', () {
      expect(HotplugEventType.fromValue(1), HotplugEventType.inputRemoved);
    });

    test('2 maps to outputAdded', () {
      expect(HotplugEventType.fromValue(2), HotplugEventType.outputAdded);
    });

    test('3 maps to outputRemoved', () {
      expect(HotplugEventType.fromValue(3), HotplugEventType.outputRemoved);
    });

    test('4 maps to setupChanged', () {
      expect(HotplugEventType.fromValue(4), HotplugEventType.setupChanged);
    });

    test('unknown value returns unknown', () {
      expect(HotplugEventType.fromValue(5), HotplugEventType.unknown);
      expect(HotplugEventType.fromValue(-1), HotplugEventType.unknown);
      expect(HotplugEventType.fromValue(99), HotplugEventType.unknown);
    });
  });
}
