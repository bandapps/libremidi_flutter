import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

import 'package:libremidi_flutter_example/main.dart';
import 'package:libremidi_flutter_example/midi_access.dart';

class FakeMidiAccess extends MidiAccess {
  FakeMidiAccess({List<MidiDevice>? devices}) : devices = devices ?? [];

  List<MidiDevice> devices;
  final hotplugController = StreamController<HotplugEventType>.broadcast(
    sync: true,
  );
  final output = FakeMidiOutputConnection();
  final input = FakeMidiInputConnection();
  int openInputCount = 0;
  int openOutputCount = 0;
  int disconnectInputCount = 0;
  int disconnectOutputCount = 0;
  final disconnectedOutputs = <MidiOutputConnection>[];

  /// When set, every openOutput waits for the test to complete it.
  bool holdOutputOpens = false;
  final pendingOutputOpens = <Completer<MidiOutputConnection>>[];

  @override
  List<MidiDevice> getDevices() => devices;

  @override
  Stream<HotplugEventType> get onHotplug => hotplugController.stream;

  @override
  Future<MidiInputConnection> openInput(MidiPortRef port) async {
    openInputCount++;
    return input;
  }

  /// Output ports opening or open, by display name. Like the plugin, a port
  /// counts as open from the start of its open until it is disconnected, and
  /// a second open meanwhile throws.
  final _heldOutputs = <String>{};
  final _outputPorts = <MidiOutputConnection, String>{};

  @override
  Future<MidiOutputConnection> openOutput(MidiPortRef port) {
    openOutputCount++;
    final name = port.displayName;
    if (!_heldOutputs.add(name)) {
      return Future.error(StateError('Output port $name is already open'));
    }
    final Future<MidiOutputConnection> opened;
    if (holdOutputOpens) {
      final pending = Completer<MidiOutputConnection>();
      pendingOutputOpens.add(pending);
      opened = pending.future;
    } else {
      opened = Future.value(output);
    }
    return opened.then((connection) {
      _outputPorts[connection] = name;
      return connection;
    }, onError: (Object e) {
      _heldOutputs.remove(name);
      throw e;
    });
  }

  @override
  void disconnectInput(MidiInputConnection input) {
    disconnectInputCount++;
  }

  @override
  void disconnectOutput(MidiOutputConnection output) {
    disconnectOutputCount++;
    disconnectedOutputs.add(output);
    final name = _outputPorts.remove(output);
    if (name != null) _heldOutputs.remove(name);
  }
}

class FakeMidiInputConnection implements MidiInputConnection {
  final controller = StreamController<MidiMessage>.broadcast();

  @override
  Stream<MidiMessage> get messages => controller.stream;
}

class FakeMidiOutputConnection implements MidiOutputConnection {
  final controlChanges = <({int channel, int controller, int value})>[];
  final notesOn = <({int channel, int note, int velocity})>[];
  final sysExMessages = <Uint8List>[];

  @override
  void sendAftertouch({required int channel, required int pressure}) {}

  @override
  void sendBankSelect({
    required int channel,
    required int bank,
    required int program,
  }) {}

  @override
  void sendControlChange({
    required int channel,
    required int controller,
    required int value,
  }) {
    controlChanges.add((
      channel: channel,
      controller: controller,
      value: value,
    ));
  }

  @override
  void sendNoteOff({
    required int channel,
    required int note,
    int velocity = 0,
  }) {}

  @override
  void sendNoteOn({
    required int channel,
    required int note,
    required int velocity,
  }) {
    notesOn.add((channel: channel, note: note, velocity: velocity));
  }

  @override
  void sendPitchBend({required int channel, required int value}) {}

  @override
  void sendPolyAftertouch({
    required int channel,
    required int note,
    required int pressure,
  }) {}

  @override
  void sendProgramChange({required int channel, required int program}) {}

  @override
  void sendSysEx(Uint8List data, {bool alreadyFramed = false}) {
    sysExMessages.add(data);
  }
}

MidiDevice fakeDevice({
  String name = 'Test Keyboard',
  bool input = false,
  bool output = false,
}) {
  final device = MidiDevice(name)
    ..manufacturer = 'TestCo'
    ..serial = '123'
    ..transportType = MidiTransportType.usb;

  if (input) {
    device.inputPorts.add(
      MidiPortRef(
        displayName: '$name In',
        deviceName: name,
        manufacturer: 'TestCo',
        serial: '123',
        transportType: MidiTransportType.usb,
      ),
    );
  }

  if (output) {
    device.outputPorts.add(
      MidiPortRef(
        displayName: '$name Out',
        deviceName: name,
        manufacturer: 'TestCo',
        serial: '123',
        transportType: MidiTransportType.usb,
      ),
    );
  }

  return device;
}

Future<void> openDeviceDropdown(WidgetTester tester) async {
  await tester.tap(
    find.byWidgetPredicate(
      (widget) => widget is DropdownButtonFormField<MidiDevice>,
    ),
  );
  await tester.pumpAndSettle();
}

void main() {
  testWidgets('example app smoke test', (WidgetTester tester) async {
    final midiAccess = FakeMidiAccess();

    await tester.pumpWidget(
      MaterialApp(
        debugShowCheckedModeBanner: false,
        home: MidiDemoPage(midiAccess: midiAccess),
      ),
    );
    await tester.pump();

    expect(find.byType(MidiDemoPage), findsOneWidget);
    expect(find.text('Select device'), findsOneWidget);
    expect(midiAccess.openOutputCount, 0);
  });

  testWidgets('shows fake MIDI device in the device menu', (tester) async {
    final midiAccess = FakeMidiAccess(
      devices: [fakeDevice(input: true, output: true)],
    );

    await tester.pumpWidget(
      MaterialApp(home: MidiDemoPage(midiAccess: midiAccess)),
    );
    await tester.pump();

    await openDeviceDropdown(tester);

    expect(find.text('Test Keyboard'), findsWidgets);
    expect(find.textContaining('In: 1  Out: 1'), findsOneWidget);
  });

  testWidgets('selecting an output device enables sending CC messages', (
    tester,
  ) async {
    final midiAccess = FakeMidiAccess(devices: [fakeDevice(output: true)]);

    await tester.pumpWidget(
      MaterialApp(home: MidiDemoPage(midiAccess: midiAccess)),
    );
    await tester.pump();

    await openDeviceDropdown(tester);
    await tester.tap(find.text('Test Keyboard').last);
    await tester.pumpAndSettle();

    expect(midiAccess.openOutputCount, 1);

    await tester.tap(find.widgetWithText(ElevatedButton, 'Send'));
    await tester.pump();

    expect(midiAccess.output.controlChanges, [
      (channel: 0, controller: 1, value: 64),
    ]);
    expect(find.text('CC 1=64 ch:1'), findsOneWidget);
  });

  testWidgets('hotplug refresh adds a newly available device', (tester) async {
    final midiAccess = FakeMidiAccess();

    await tester.pumpWidget(
      MaterialApp(home: MidiDemoPage(midiAccess: midiAccess)),
    );
    await tester.pump();

    midiAccess.devices = [fakeDevice(output: true)];
    midiAccess.hotplugController.add(HotplugEventType.outputAdded);
    await tester.pump();

    await openDeviceDropdown(tester);

    expect(find.text('Test Keyboard'), findsWidgets);
  });

  testWidgets('incoming MIDI messages are logged', (tester) async {
    final midiAccess = FakeMidiAccess(devices: [fakeDevice(input: true)]);

    await tester.pumpWidget(
      MaterialApp(home: MidiDemoPage(midiAccess: midiAccess)),
    );
    await tester.pump();

    await openDeviceDropdown(tester);
    await tester.tap(find.text('Test Keyboard').last);
    await tester.pumpAndSettle();

    midiAccess.input.controller.add(
      MidiMessage(Uint8List.fromList([0xB0, 7, 100])),
    );
    await tester.pump(const Duration(milliseconds: 20));

    expect(midiAccess.openInputCount, 1);
    expect(find.text('CC 7=100 ch:1'), findsOneWidget);
  });

  // Opening is asynchronous: a device picked while the previous one is still
  // opening supersedes it. Without the generation check, the late port of the
  // first device was installed over the second one, so sends went to the
  // device the user had just deselected and its connection was never closed.
  testWidgets('a port that opens after the device was switched is closed', (
    tester,
  ) async {
    final midiAccess = FakeMidiAccess(devices: [
      fakeDevice(name: 'Alpha', output: true),
      fakeDevice(name: 'Beta', output: true),
    ])
      ..holdOutputOpens = true;
    final alphaOut = FakeMidiOutputConnection();
    final betaOut = FakeMidiOutputConnection();

    await tester.pumpWidget(
      MaterialApp(home: MidiDemoPage(midiAccess: midiAccess)),
    );
    await tester.pump();

    await openDeviceDropdown(tester);
    await tester.tap(find.text('Alpha').last);
    await tester.pumpAndSettle();
    await openDeviceDropdown(tester);
    await tester.tap(find.text('Beta').last);
    await tester.pumpAndSettle();
    // Beta's attempt waits for Alpha's to return before opening anything.
    expect(midiAccess.pendingOutputOpens, hasLength(1));

    // Alpha's open completes after the switch to Beta.
    midiAccess.pendingOutputOpens[0].complete(alphaOut);
    await tester.pumpAndSettle();
    expect(midiAccess.disconnectedOutputs, [alphaOut]);
    expect(midiAccess.pendingOutputOpens, hasLength(2));
    midiAccess.pendingOutputOpens[1].complete(betaOut);
    await tester.pumpAndSettle();

    expect(midiAccess.disconnectedOutputs, [alphaOut]);
    await tester.tap(find.widgetWithText(ElevatedButton, 'Send'));
    await tester.pump();
    expect(betaOut.controlChanges, hasLength(1));
    expect(alphaOut.controlChanges, isEmpty);
  });

  // Without waiting for the superseded attempt, the reconnect opened the port
  // while the old open still held it, got "already open", and the device
  // stayed without an output until the next hotplug.
  testWidgets('a reconnect while the old open is running still connects', (
    tester,
  ) async {
    final midiAccess = FakeMidiAccess(devices: [
      fakeDevice(name: 'Alpha', output: true),
    ])
      ..holdOutputOpens = true;
    final firstOut = FakeMidiOutputConnection();
    final secondOut = FakeMidiOutputConnection();

    await tester.pumpWidget(
      MaterialApp(home: MidiDemoPage(midiAccess: midiAccess)),
    );
    await tester.pump();

    await openDeviceDropdown(tester);
    await tester.tap(find.text('Alpha').last);
    await tester.pumpAndSettle();
    expect(midiAccess.pendingOutputOpens, hasLength(1));

    // The device goes away and comes back while its first open is running.
    midiAccess.devices = [];
    midiAccess.hotplugController.add(HotplugEventType.outputRemoved);
    await tester.pumpAndSettle();
    midiAccess.devices = [fakeDevice(name: 'Alpha', output: true)];
    midiAccess.hotplugController.add(HotplugEventType.outputAdded);
    await tester.pumpAndSettle();

    midiAccess.pendingOutputOpens[0].complete(firstOut);
    await tester.pumpAndSettle();
    expect(midiAccess.disconnectedOutputs, [firstOut]);
    expect(midiAccess.pendingOutputOpens, hasLength(2),
        reason: 'the reconnect must open the port once it is free');
    midiAccess.pendingOutputOpens[1].complete(secondOut);
    await tester.pumpAndSettle();

    await tester.tap(find.widgetWithText(ElevatedButton, 'Send'));
    await tester.pump();
    expect(secondOut.controlChanges, hasLength(1));
  });
}
