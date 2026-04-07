import 'dart:async';

import 'package:flutter/services.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

/// Groups input/output ports into a single device.
class MidiDevice {
  final String name;
  final List<MidiPortRef> inputPorts = [];
  final List<MidiPortRef> outputPorts = [];
  String? manufacturer;
  String? serial;
  MidiTransportType? transportType;

  MidiDevice(this.name);

  int get inputCount => inputPorts.length;
  int get outputCount => outputPorts.length;

  /// Stable ID based on device identity (not ports, which can change).
  int get stableId => '$name|${manufacturer ?? ''}|${serial ?? ''}'.hashCode;

  String get transportName => transportType?.name ?? '';
}

class MidiPortRef {
  const MidiPortRef({
    required this.displayName,
    this.deviceName = '',
    this.manufacturer = '',
    this.product = '',
    this.serial = '',
    this.transportType = MidiTransportType.unknown,
    this.nativePort,
  });

  final String displayName;
  final String deviceName;
  final String manufacturer;
  final String product;
  final String serial;
  final MidiTransportType transportType;
  final Object? nativePort;
}

abstract class MidiInputConnection {
  Stream<MidiMessage> get messages;
}

abstract class MidiOutputConnection {
  void sendNoteOn({
    required int channel,
    required int note,
    required int velocity,
  });

  void sendNoteOff({required int channel, required int note, int velocity = 0});

  void sendControlChange({
    required int channel,
    required int controller,
    required int value,
  });

  void sendProgramChange({required int channel, required int program});

  void sendBankSelect({
    required int channel,
    required int bank,
    required int program,
  });

  void sendSysEx(Uint8List data, {bool alreadyFramed = false});

  void sendPitchBend({required int channel, required int value});

  void sendAftertouch({required int channel, required int pressure});

  void sendPolyAftertouch({
    required int channel,
    required int note,
    required int pressure,
  });
}

abstract class MidiAccess {
  const MidiAccess();

  List<MidiDevice> getDevices();
  MidiInputConnection openInput(MidiPortRef port);
  MidiOutputConnection openOutput(MidiPortRef port);
  Stream<HotplugEventType> get onHotplug;
  void disconnectInput(MidiInputConnection input);
  void disconnectOutput(MidiOutputConnection output);
}

class LibremidiInputConnection implements MidiInputConnection {
  const LibremidiInputConnection(this.input);

  final MidiInput input;

  @override
  Stream<MidiMessage> get messages => input.messages;
}

class LibremidiOutputConnection implements MidiOutputConnection {
  const LibremidiOutputConnection(this.output);

  final MidiOutput output;

  @override
  void sendAftertouch({required int channel, required int pressure}) {
    output.sendAftertouch(channel: channel, pressure: pressure);
  }

  @override
  void sendBankSelect({
    required int channel,
    required int bank,
    required int program,
  }) {
    output.sendBankSelect(channel: channel, bank: bank, program: program);
  }

  @override
  void sendControlChange({
    required int channel,
    required int controller,
    required int value,
  }) {
    output.sendControlChange(
      channel: channel,
      controller: controller,
      value: value,
    );
  }

  @override
  void sendNoteOff({
    required int channel,
    required int note,
    int velocity = 0,
  }) {
    output.sendNoteOff(channel: channel, note: note, velocity: velocity);
  }

  @override
  void sendNoteOn({
    required int channel,
    required int note,
    required int velocity,
  }) {
    output.sendNoteOn(channel: channel, note: note, velocity: velocity);
  }

  @override
  void sendPitchBend({required int channel, required int value}) {
    output.sendPitchBend(channel: channel, value: value);
  }

  @override
  void sendPolyAftertouch({
    required int channel,
    required int note,
    required int pressure,
  }) {
    output.sendPolyAftertouch(channel: channel, note: note, pressure: pressure);
  }

  @override
  void sendProgramChange({required int channel, required int program}) {
    output.sendProgramChange(channel: channel, program: program);
  }

  @override
  void sendSysEx(Uint8List data, {bool alreadyFramed = false}) {
    output.sendSysEx(data, alreadyFramed: alreadyFramed);
  }
}

class LibremidiMidiAccess extends MidiAccess {
  const LibremidiMidiAccess();

  @override
  List<MidiDevice> getDevices() {
    final inputs = LibremidiFlutter.getInputPorts();
    final outputs = LibremidiFlutter.getOutputPorts();

    final deviceMap = <String, MidiDevice>{};

    for (final port in inputs) {
      final ref = _toPortRef(port);
      final name = ref.deviceName.isNotEmpty ? ref.deviceName : ref.displayName;
      deviceMap.putIfAbsent(name, () => MidiDevice(name));
      deviceMap[name]!.inputPorts.add(ref);
      deviceMap[name]!.manufacturer ??= ref.manufacturer;
      deviceMap[name]!.serial ??= ref.serial;
      deviceMap[name]!.transportType ??= ref.transportType;
    }

    for (final port in outputs) {
      final ref = _toPortRef(port);
      final name = ref.deviceName.isNotEmpty ? ref.deviceName : ref.displayName;
      deviceMap.putIfAbsent(name, () => MidiDevice(name));
      deviceMap[name]!.outputPorts.add(ref);
      deviceMap[name]!.manufacturer ??= ref.manufacturer;
      deviceMap[name]!.serial ??= ref.serial;
      deviceMap[name]!.transportType ??= ref.transportType;
    }

    return deviceMap.values.toList()..sort((a, b) => a.name.compareTo(b.name));
  }

  @override
  MidiInputConnection openInput(MidiPortRef port) {
    return LibremidiInputConnection(
      LibremidiFlutter.openInput(port.nativePort! as MidiPort),
    );
  }

  @override
  MidiOutputConnection openOutput(MidiPortRef port) {
    return LibremidiOutputConnection(
      LibremidiFlutter.openOutput(port.nativePort! as MidiPort),
    );
  }

  @override
  Stream<HotplugEventType> get onHotplug => LibremidiFlutter.onHotplug;

  @override
  void disconnectInput(MidiInputConnection input) {
    LibremidiFlutter.disconnectInput((input as LibremidiInputConnection).input);
  }

  @override
  void disconnectOutput(MidiOutputConnection output) {
    LibremidiFlutter.disconnectOutput(
      (output as LibremidiOutputConnection).output,
    );
  }

  MidiPortRef _toPortRef(MidiPort port) {
    return MidiPortRef(
      displayName: port.displayName,
      deviceName: port.deviceName,
      manufacturer: port.manufacturer,
      product: port.product,
      serial: port.serial,
      transportType: port.transportType,
      nativePort: port,
    );
  }
}
