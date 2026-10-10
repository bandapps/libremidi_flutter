import 'dart:async';
import 'dart:convert' show utf8;

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
  ///
  /// Uses FNV-1a rather than String.hashCode: Dart makes no promise that
  /// hashCode is the same value in another process or another SDK version, so
  /// persisting it would hand back a different id after a restart or an
  /// upgrade.
  int get stableId {
    var hash = 0xcbf29ce484222325;
    for (final byte in utf8.encode(_identityKey)) {
      hash = ((hash ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF;
    }
    return hash;
  }

  String get _identityKey => '$name|${manufacturer ?? ''}|${serial ?? ''}';

  String get transportName => transportType?.name ?? '';

  /// Two instances describing the same device are equal.
  ///
  /// Every device-list rebuild — and hotplug triggers one — creates fresh
  /// [MidiDevice] objects. Without this, a dropdown holding the previously
  /// selected instance finds no match among the new items and falls back to
  /// showing the first entry, so after re-plugging a device the UI names a
  /// different one while the ports stay correctly connected. The rest of this
  /// example already compares devices by [stableId]; this makes `==` agree.
  @override
  bool operator ==(Object other) =>
      other is MidiDevice && other.stableId == stableId;

  @override
  int get hashCode => stableId.hashCode;
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
  // Asynchronous: on Android, opening a Bluetooth or virtual device can take
  // a while, and a synchronous open would freeze the UI meanwhile.
  Future<MidiInputConnection> openInput(MidiPortRef port);
  Future<MidiOutputConnection> openOutput(MidiPortRef port);
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

    // Group by device identity: the name, plus the serial when the platform
    // supplies one.
    // Grouping on the name merged two identical devices into one entry and
    // pooled their ports — the very case the plugin now keeps apart with
    // distinct port ids. Where the platform supplies a serial this separates
    // them correctly; where it does not (some Android and WinUWP devices
    // report nothing but a name) they remain indistinguishable in the data
    // itself, and merging is then the honest outcome.
    final deviceMap = <String, MidiDevice>{};

    MidiDevice deviceFor(MidiPortRef ref) {
      final name = ref.deviceName.isNotEmpty ? ref.deviceName : ref.displayName;
      // Only the serial may join the key. manufacturer and product are
      // per-endpoint properties on CoreMIDI, and a device's source and
      // destination can report them differently — including them split one
      // device into an input-only and an output-only entry, and selecting the
      // input one left the app with no output to send to.
      final key = ref.serial.isNotEmpty ? '$name|${ref.serial}' : name;
      final device = deviceMap.putIfAbsent(key, () => MidiDevice(name));
      device.manufacturer ??= ref.manufacturer;
      device.serial ??= ref.serial;
      device.transportType ??= ref.transportType;
      return device;
    }

    for (final port in inputs) {
      final ref = _toPortRef(port);
      deviceFor(ref).inputPorts.add(ref);
    }

    for (final port in outputs) {
      final ref = _toPortRef(port);
      deviceFor(ref).outputPorts.add(ref);
    }

    return deviceMap.values.toList()..sort((a, b) => a.name.compareTo(b.name));
  }

  @override
  Future<MidiInputConnection> openInput(MidiPortRef port) async {
    return LibremidiInputConnection(
      await LibremidiFlutter.openInputAsync(port.nativePort! as MidiPort),
    );
  }

  @override
  Future<MidiOutputConnection> openOutput(MidiPortRef port) async {
    return LibremidiOutputConnection(
      await LibremidiFlutter.openOutputAsync(port.nativePort! as MidiPort),
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
