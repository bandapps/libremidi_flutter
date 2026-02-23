/// Cross-platform MIDI device access for Flutter using libremidi.
library;

import 'dart:convert' show utf8;
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';
import 'dart:async' show StreamController;

import 'package:ffi/ffi.dart';

import 'libremidi_flutter_bindings_generated.dart';

// =============================================================================
// Library loading
// =============================================================================

const String _libName = 'libremidi_flutter';

final DynamicLibrary _dylib = () {
  if (Platform.isMacOS || Platform.isIOS) {
    try {
      return DynamicLibrary.open('$_libName.framework/$_libName');
    } catch (_) {
      // SPM statically links the library into the process.
      return DynamicLibrary.process();
    }
  }
  if (Platform.isAndroid || Platform.isLinux) {
    return DynamicLibrary.open('lib$_libName.so');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('$_libName.dll');
  }
  throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
}();

final LibremidiFlutterBindings _bindings = LibremidiFlutterBindings(_dylib);

// =============================================================================
// MidiTransportType - Type of MIDI connection
// =============================================================================

/// Type of MIDI transport/connection.
enum MidiTransportType {
  unknown,
  software,
  loopback,
  hardware,
  usb,
  bluetooth,
  pci,
  network;

  static MidiTransportType fromValue(int value) {
    if (value & 128 != 0) return MidiTransportType.network;
    if (value & 64 != 0) return MidiTransportType.pci;
    if (value & 32 != 0) return MidiTransportType.bluetooth;
    if (value & 16 != 0) return MidiTransportType.usb;
    if (value & 8 != 0) return MidiTransportType.hardware;
    if (value & 4 != 0) return MidiTransportType.loopback;
    if (value & 2 != 0) return MidiTransportType.software;
    return MidiTransportType.unknown;
  }

  String get displayName {
    switch (this) {
      case MidiTransportType.unknown:
        return 'Unknown';
      case MidiTransportType.software:
        return 'Software';
      case MidiTransportType.loopback:
        return 'Loopback';
      case MidiTransportType.hardware:
        return 'Hardware';
      case MidiTransportType.usb:
        return 'USB';
      case MidiTransportType.bluetooth:
        return 'Bluetooth';
      case MidiTransportType.pci:
        return 'PCI';
      case MidiTransportType.network:
        return 'Network';
    }
  }
}

// =============================================================================
// MidiPort - Represents a MIDI port
// =============================================================================

/// Represents a MIDI port (input or output device endpoint).
class MidiPort {
  /// Cross-platform stable ID (survives hotplug/reorder).
  ///
  /// This is a hash of port_name|manufacturer|product|serial, consistent
  /// across all platforms. Use this to identify "the same port" after
  /// device reconnect or app restart, rather than [index] which may change.
  final int stableId;

  /// Unique port ID (CoreMIDI: kMIDIPropertyUniqueID).
  final int portId;

  /// API client handle.
  final int clientHandle;

  /// The index of the port in the enumeration.
  final int index;

  /// Full display name (e.g. "IAC Driver Bus 1").
  final String displayName;

  /// Port name (e.g. "Bus 1").
  final String portName;

  /// Device/model name (e.g. "IAC Driver").
  final String deviceName;

  /// Manufacturer name.
  final String manufacturer;

  /// Product name.
  final String product;

  /// Serial number (often empty).
  final String serial;

  /// The transport type of the port.
  final MidiTransportType transportType;

  /// The raw transport type value from libremidi (bitmask).
  final int rawTransportType;

  /// Whether this is an input port.
  final bool isInput;

  /// Whether this is a virtual/software port.
  final bool isVirtual;

  const MidiPort._({
    required this.stableId,
    required this.portId,
    required this.clientHandle,
    required this.index,
    required this.displayName,
    required this.portName,
    required this.deviceName,
    required this.manufacturer,
    required this.product,
    required this.serial,
    required this.transportType,
    required this.rawTransportType,
    required this.isInput,
    required this.isVirtual,
  });

  /// The display name (alias for displayName for backward compatibility).
  String get name => displayName;

  /// Whether this is an output port.
  bool get isOutput => !isInput;

  /// Whether this is a hardware port (USB or generic hardware).
  bool get isHardware =>
      transportType == MidiTransportType.hardware ||
      transportType == MidiTransportType.usb;

  /// Returns a stable identifier that's reliable for reconnection logic.
  ///
  /// Uses [stableId] (hash-based) when the device provides enough identifying
  /// info (product or serial). Falls back to [portId] (platform-specific unique
  /// ID) when hash would be unreliable due to missing device info.
  ///
  /// Use this instead of [stableId] directly when you need to persist port
  /// selections across app restarts or reconnects.
  int get effectiveStableId =>
      (serial.isEmpty && product.isEmpty) ? portId : stableId;

  /// Returns all port info as a Map (keys match libremidi naming).
  Map<String, dynamic> toMap() => {
    'stable_id': stableId,
    'effective_stable_id': effectiveStableId,
    'port': portId,
    'client': clientHandle,
    'index': index,
    'display_name': displayName,
    'port_name': portName,
    'device_name': deviceName,
    'manufacturer': manufacturer,
    'product': product,
    'serial': serial,
    'type': transportType.name,
    'raw_transport_type': rawTransportType,
    'is_input': isInput,
    'is_virtual': isVirtual,
  };

  @override
  String toString() =>
      'MidiPort($displayName, ${isInput ? "input" : "output"}, ${transportType.displayName})';

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is MidiPort &&
          runtimeType == other.runtimeType &&
          stableId == other.stableId &&
          isInput == other.isInput;

  @override
  int get hashCode => stableId.hashCode ^ isInput.hashCode;
}

// =============================================================================
// MidiMessage - Represents a MIDI message
// =============================================================================

/// Represents a MIDI message.
class MidiMessage {
  /// The raw MIDI data bytes.
  final Uint8List data;

  /// The timestamp of the message (in microseconds).
  ///
  /// The clock source is platform-dependent:
  /// - macOS/iOS: CoreMIDI host time (mach_absolute_time based)
  /// - Windows: WinMM timeGetTime (milliseconds, converted to microseconds)
  /// - Linux: ALSA sequencer timestamp
  /// - Android: AMidi timestamp (nanoseconds, converted to microseconds)
  final int timestamp;

  const MidiMessage(this.data, {this.timestamp = 0});

  /// The status byte.
  int get status => data.isNotEmpty ? data[0] : 0;

  /// The message type (status byte high nibble).
  int get type => status & 0xF0;

  /// The channel (status byte low nibble) for channel messages.
  int get channel => status & 0x0F;

  /// Whether this is a Note On message.
  bool get isNoteOn => type == 0x90 && data.length >= 3 && data[2] > 0;

  /// Whether this is a Note Off message.
  bool get isNoteOff =>
      type == 0x80 || (type == 0x90 && data.length >= 3 && data[2] == 0);

  /// Whether this is a Control Change message.
  bool get isControlChange => type == 0xB0;

  /// Whether this is a Program Change message.
  bool get isProgramChange => type == 0xC0;

  /// Whether this is a Pitch Bend message.
  bool get isPitchBend => type == 0xE0;

  /// Whether this is a SysEx message.
  bool get isSysEx => data.isNotEmpty && data[0] == 0xF0;

  /// Whether this is a Channel Aftertouch message.
  bool get isAftertouch => type == 0xD0;

  /// Whether this is a Polyphonic Aftertouch message.
  bool get isPolyAftertouch => type == 0xA0;

  /// The note number for Note On/Off messages.
  int get note => data.length >= 2 ? data[1] : 0;

  /// The velocity for Note On/Off messages.
  int get velocity => data.length >= 3 ? data[2] : 0;

  /// The controller number for Control Change messages.
  int get controller => data.length >= 2 ? data[1] : 0;

  /// The value for Control Change messages.
  int get value => data.length >= 3 ? data[2] : 0;

  @override
  String toString() {
    final hex = data.map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');
    return 'MidiMessage($hex)';
  }
}

// =============================================================================
// MidiException - MIDI-related errors
// =============================================================================

/// Exception thrown when a MIDI operation fails.
class MidiException implements Exception {
  final String message;
  final int? errorCode;
  final String? nativeFunction;

  const MidiException(this.message, {this.errorCode, this.nativeFunction});

  @override
  String toString() {
    final parts = <String>['MidiException: $message'];
    if (nativeFunction != null) {
      parts.add('in $nativeFunction');
    }
    if (errorCode != null) {
      parts.add('(code: $errorCode)');
    }
    return parts.join(' ');
  }
}

// =============================================================================
// HotplugEvent - Device connection events
// =============================================================================

/// Type of hotplug event.
enum HotplugEventType {
  unknown,
  inputAdded,
  inputRemoved,
  outputAdded,
  outputRemoved;

  static HotplugEventType fromValue(int value) {
    switch (value) {
      case 0:
        return HotplugEventType.inputAdded;
      case 1:
        return HotplugEventType.inputRemoved;
      case 2:
        return HotplugEventType.outputAdded;
      case 3:
        return HotplugEventType.outputRemoved;
      default:
        return HotplugEventType.unknown;
    }
  }
}

// =============================================================================
// MidiObserver - Enumerate MIDI ports
// =============================================================================

/// Observer for enumerating available MIDI ports.
class MidiObserver {
  Pointer<LrmObserver>? _handle;
  bool _disposed = false;
  NativeCallable<Void Function(Pointer<Void>, Int32)>? _hotplugCallable;
  final StreamController<HotplugEventType> _hotplugController =
      StreamController<HotplugEventType>.broadcast();

  /// Creates a new MIDI observer without hotplug detection.
  MidiObserver() {
    _handle = _bindings.lrm_observer_new();
    if (_handle == nullptr) {
      throw const MidiException('Failed to create MIDI observer');
    }
  }

  /// Creates a new MIDI observer with hotplug detection.
  MidiObserver.withHotplug() {
    _hotplugCallable =
        NativeCallable<Void Function(Pointer<Void>, Int32)>.listener(
          _onHotplugEvent,
        );

    _handle = _bindings.lrm_observer_new_with_callbacks(
      _hotplugCallable!.nativeFunction,
      nullptr,
    );

    if (_handle == nullptr) {
      _hotplugCallable?.close();
      throw const MidiException('Failed to create MIDI observer');
    }
  }

  void _onHotplugEvent(Pointer<Void> context, int eventType) {
    if (!_disposed) {
      _hotplugController.add(HotplugEventType.fromValue(eventType));
    }
  }

  /// Stream of hotplug events (device added/removed).
  ///
  /// Note: Events originate from native callbacks which may be invoked on
  /// a different thread. The stream delivers events asynchronously to the
  /// Dart isolate, so UI updates should be safe.
  Stream<HotplugEventType> get onHotplug => _hotplugController.stream;

  void _checkDisposed() {
    if (_disposed) {
      throw StateError('MidiObserver has been disposed');
    }
  }

  /// Gets all available MIDI input ports.
  List<MidiPort> getInputPorts() {
    _checkDisposed();
    final count = _bindings.lrm_observer_get_input_count(_handle!);
    final ports = <MidiPort>[];

    final info = calloc<LrmPortInfo>();
    try {
      for (var i = 0; i < count; i++) {
        final result = _bindings.lrm_observer_get_input(_handle!, i, info);
        if (result == LRM_OK) {
          ports.add(_portInfoToMidiPort(info.ref));
        }
      }
    } finally {
      calloc.free(info);
    }

    return ports;
  }

  /// Gets all available MIDI output ports.
  List<MidiPort> getOutputPorts() {
    _checkDisposed();
    final count = _bindings.lrm_observer_get_output_count(_handle!);
    final ports = <MidiPort>[];

    final info = calloc<LrmPortInfo>();
    try {
      for (var i = 0; i < count; i++) {
        final result = _bindings.lrm_observer_get_output(_handle!, i, info);
        if (result == LRM_OK) {
          ports.add(_portInfoToMidiPort(info.ref));
        }
      }
    } finally {
      calloc.free(info);
    }

    return ports;
  }

  /// Converts native LrmPortInfo to MidiPort.
  MidiPort _portInfoToMidiPort(LrmPortInfo info) {
    return MidiPort._(
      stableId: info.stable_id,
      portId: info.port_id,
      clientHandle: info.client_handle,
      index: info.index,
      displayName: _arrayToString(info.display_name),
      portName: _arrayToString(info.port_name),
      deviceName: _arrayToString(info.device_name),
      manufacturer: _arrayToString(info.manufacturer),
      product: _arrayToString(info.product),
      serial: _arrayToString(info.serial, maxLength: 128),
      transportType: MidiTransportType.fromValue(info.transport_type),
      rawTransportType: info.transport_type,
      isInput: info.is_input,
      isVirtual: info.is_virtual,
    );
  }

  /// Opens a MIDI output connection to the specified port.
  MidiOutput openOutput(MidiPort port) {
    _checkDisposed();
    if (port.isInput) {
      throw ArgumentError('Port must be an output port');
    }
    return MidiOutput._(_handle!, port.index);
  }

  /// Opens a MIDI input connection to the specified port.
  ///
  /// By default, SysEx messages are received, while timing (MIDI clock) and
  /// active sensing messages are filtered out.
  MidiInput openInput(
    MidiPort port, {
    bool receiveSysex = true,
    bool receiveTiming = false,
    bool receiveSensing = false,
  }) {
    _checkDisposed();
    if (!port.isInput) {
      throw ArgumentError('Port must be an input port');
    }
    return MidiInput._(
      _handle!,
      port.index,
      receiveSysex: receiveSysex,
      receiveTiming: receiveTiming,
      receiveSensing: receiveSensing,
    );
  }

  /// Refreshes the internal port list cache.
  ///
  /// Call this to manually update the port list. Note that this does NOT
  /// trigger hotplug events - it only updates the internal cache.
  /// Hotplug events are only fired when the OS notifies about device changes.
  void refresh() {
    _checkDisposed();
    _bindings.lrm_observer_refresh(_handle!);
  }

  /// Disposes the observer and releases resources.
  void dispose() {
    if (!_disposed && _handle != null) {
      // Order matters to avoid race conditions:
      // 1. Mark disposed first to reject new callbacks
      _disposed = true;
      // 2. Close Dart stream controller
      _hotplugController.close();
      // 3. Close native callable (stops callbacks from native)
      _hotplugCallable?.close();
      // 4. Finally free native resources
      _bindings.lrm_observer_free(_handle!);
      _handle = null;
    }
  }

  /// Helper to convert char array to String (UTF-8 safe).
  String _arrayToString(Array<Char> array, {int maxLength = 256}) {
    final bytes = <int>[];
    for (var i = 0; i < maxLength; i++) {
      final byte = array[i];
      if (byte == 0) break;
      bytes.add(byte & 0xFF);
    }
    return utf8.decode(bytes, allowMalformed: true);
  }
}

// =============================================================================
// MidiOutput - Send MIDI messages
// =============================================================================

/// Handles MIDI output to a connected port.
class MidiOutput {
  Pointer<LrmMidiOut>? _handle;
  bool _disposed = false;

  MidiOutput._(Pointer<LrmObserver> observer, int portIndex) {
    _handle = _bindings.lrm_midi_out_open(observer, portIndex);
    if (_handle == nullptr) {
      throw const MidiException('Failed to open MIDI output');
    }
  }

  /// The raw native pointer address for cross-plugin bridging.
  ///
  /// Use with `Pointer<LrmMidiOut>.fromAddress(address)` in another
  /// FFI plugin that shares the same [LrmMidiOut] struct layout.
  /// Returns 0 if disposed.
  int get handleAddress => _disposed ? 0 : (_handle?.address ?? 0);

  void _checkDisposed() {
    if (_disposed) {
      throw StateError('MidiOutput has been disposed');
    }
  }

  /// Whether the output is currently connected.
  bool get isConnected {
    if (_disposed || _handle == null) return false;
    return _bindings.lrm_midi_out_is_connected(_handle!);
  }

  /// Sends a raw MIDI message.
  void send(Uint8List data) {
    _checkDisposed();
    if (data.isEmpty) return; // Guard: don't send empty messages
    final ptr = calloc<Uint8>(data.length);
    try {
      for (var i = 0; i < data.length; i++) {
        ptr[i] = data[i];
      }
      final result = _bindings.lrm_midi_out_send(_handle!, ptr, data.length);
      if (result != LRM_OK) {
        throw MidiException('Failed to send MIDI message', errorCode: result);
      }
    } finally {
      calloc.free(ptr);
    }
  }

  /// Sends a Note On message.
  void sendNoteOn({
    required int channel,
    required int note,
    required int velocity,
  }) {
    assert(channel >= 0 && channel < 16, 'Channel must be 0-15');
    assert(note >= 0 && note < 128, 'Note must be 0-127');
    assert(velocity >= 0 && velocity < 128, 'Velocity must be 0-127');
    send(
      Uint8List.fromList([
        0x90 | (channel & 0x0F),
        note & 0x7F,
        velocity & 0x7F,
      ]),
    );
  }

  /// Sends a Note Off message.
  void sendNoteOff({
    required int channel,
    required int note,
    int velocity = 0,
  }) {
    assert(channel >= 0 && channel < 16, 'Channel must be 0-15');
    assert(note >= 0 && note < 128, 'Note must be 0-127');
    assert(velocity >= 0 && velocity < 128, 'Velocity must be 0-127');
    send(
      Uint8List.fromList([
        0x80 | (channel & 0x0F),
        note & 0x7F,
        velocity & 0x7F,
      ]),
    );
  }

  /// Sends a Control Change message.
  void sendControlChange({
    required int channel,
    required int controller,
    required int value,
  }) {
    assert(channel >= 0 && channel < 16, 'Channel must be 0-15');
    assert(controller >= 0 && controller < 128, 'Controller must be 0-127');
    assert(value >= 0 && value < 128, 'Value must be 0-127');
    send(
      Uint8List.fromList([
        0xB0 | (channel & 0x0F),
        controller & 0x7F,
        value & 0x7F,
      ]),
    );
  }

  /// Sends a Program Change message.
  void sendProgramChange({required int channel, required int program}) {
    assert(channel >= 0 && channel < 16, 'Channel must be 0-15');
    assert(program >= 0 && program < 128, 'Program must be 0-127');
    send(Uint8List.fromList([0xC0 | (channel & 0x0F), program & 0x7F]));
  }

  /// Sends a Pitch Bend message.
  /// Value is 14-bit (0-16383), center is 8192.
  void sendPitchBend({required int channel, required int value}) {
    assert(channel >= 0 && channel < 16, 'Channel must be 0-15');
    assert(value >= 0 && value <= 16383, 'Pitch bend value must be 0-16383');
    final lsb = value & 0x7F;
    final msb = (value >> 7) & 0x7F;
    send(Uint8List.fromList([0xE0 | (channel & 0x0F), lsb, msb]));
  }

  /// Sends Bank Select (CC 0 = MSB, CC 32 = LSB) followed by Program Change.
  ///
  /// Uses the standard GM/GS/XG convention: CC 0 for bank MSB, CC 32 for LSB.
  /// Some instruments only respond to MSB (bank 0-127), others use the full
  /// 14-bit range. The [bank] parameter is split: MSB = bank >> 7, LSB = bank & 0x7F.
  void sendBankSelect({
    required int channel,
    required int bank,
    required int program,
  }) {
    assert(channel >= 0 && channel < 16, 'Channel must be 0-15');
    assert(bank >= 0 && bank < 16384, 'Bank must be 0-16383');
    assert(program >= 0 && program < 128, 'Program must be 0-127');
    final msb = (bank >> 7) & 0x7F;
    final lsb = bank & 0x7F;
    sendControlChange(channel: channel, controller: 0, value: msb);
    sendControlChange(channel: channel, controller: 32, value: lsb);
    sendProgramChange(channel: channel, program: program);
  }

  /// Sends a SysEx message.
  ///
  /// By default, [data] should NOT include 0xF0/0xF7 framing bytes - they
  /// will be added automatically. Set [alreadyFramed] to true if [data]
  /// already includes the framing bytes (starts with 0xF0, ends with 0xF7).
  void sendSysEx(Uint8List data, {bool alreadyFramed = false}) {
    if (alreadyFramed) {
      send(data);
    } else {
      final message = Uint8List(data.length + 2);
      message[0] = 0xF0;
      message.setRange(1, data.length + 1, data);
      message[data.length + 1] = 0xF7;
      send(message);
    }
  }

  /// Sends Channel Aftertouch (Channel Pressure).
  void sendAftertouch({required int channel, required int pressure}) {
    assert(channel >= 0 && channel < 16, 'Channel must be 0-15');
    assert(pressure >= 0 && pressure < 128, 'Pressure must be 0-127');
    send(Uint8List.fromList([0xD0 | (channel & 0x0F), pressure & 0x7F]));
  }

  /// Sends Polyphonic Aftertouch (Key Pressure).
  void sendPolyAftertouch({
    required int channel,
    required int note,
    required int pressure,
  }) {
    assert(channel >= 0 && channel < 16, 'Channel must be 0-15');
    assert(note >= 0 && note < 128, 'Note must be 0-127');
    assert(pressure >= 0 && pressure < 128, 'Pressure must be 0-127');
    send(
      Uint8List.fromList([
        0xA0 | (channel & 0x0F),
        note & 0x7F,
        pressure & 0x7F,
      ]),
    );
  }

  /// Closes the output connection and releases resources.
  void dispose() {
    if (!_disposed && _handle != null) {
      _bindings.lrm_midi_out_close(_handle!);
      _handle = null;
      _disposed = true;
    }
  }
}

// =============================================================================
// MidiInput - Receive MIDI messages
// =============================================================================

/// Handles MIDI input from a connected port.
class MidiInput {
  Pointer<LrmMidiIn>? _handle;
  bool _disposed = false;
  NativeCallable<Void Function(Pointer<Void>, Pointer<Uint8>, Size, Int64)>?
  _callback;
  final StreamController<MidiMessage> _messageController =
      StreamController<MidiMessage>.broadcast();

  MidiInput._(
    Pointer<LrmObserver> observer,
    int portIndex, {
    required bool receiveSysex,
    required bool receiveTiming,
    required bool receiveSensing,
  }) {
    _callback =
        NativeCallable<
          Void Function(Pointer<Void>, Pointer<Uint8>, Size, Int64)
        >.listener(_onMidiMessage);

    _handle = _bindings.lrm_midi_in_open(
      observer,
      portIndex,
      _callback!.nativeFunction,
      nullptr,
      receiveSysex,
      receiveTiming,
      receiveSensing,
    );

    if (_handle == nullptr) {
      _callback?.close();
      throw const MidiException('Failed to open MIDI input');
    }
  }

  void _onMidiMessage(
    Pointer<Void> context,
    Pointer<Uint8> data,
    int length,
    int timestamp,
  ) {
    if (_disposed) return;

    final bytes = Uint8List(length);
    for (var i = 0; i < length; i++) {
      bytes[i] = data[i];
    }

    _messageController.add(MidiMessage(bytes, timestamp: timestamp));
  }

  /// Stream of incoming MIDI messages.
  ///
  /// Note: Messages originate from native callbacks which may be invoked on
  /// a different thread. The stream delivers events asynchronously to the
  /// Dart isolate, so UI updates should be safe.
  ///
  /// **Backpressure warning:** This is an unbounded broadcast stream. If your
  /// listener processes messages slower than they arrive (e.g., high-speed
  /// SysEx dumps), messages will queue in memory. Consider using
  /// [messagesFiltered] or processing messages efficiently.
  Stream<MidiMessage> get messages => _messageController.stream;

  /// Stream of incoming MIDI messages with optional filtering.
  ///
  /// Filters out messages that exceed [maxBytes] (default: 1024). This is
  /// useful to prevent memory issues from large SysEx dumps while still
  /// receiving normal MIDI messages.
  ///
  /// Set [excludeSysEx] to true to filter out all SysEx messages.
  ///
  /// Example:
  /// ```dart
  /// // Only receive messages up to 256 bytes
  /// input.messagesFiltered(maxBytes: 256).listen(...);
  ///
  /// // Exclude all SysEx
  /// input.messagesFiltered(excludeSysEx: true).listen(...);
  /// ```
  Stream<MidiMessage> messagesFiltered({
    int maxBytes = 1024,
    bool excludeSysEx = false,
  }) {
    return _messageController.stream.where((msg) {
      if (excludeSysEx && msg.isSysEx) return false;
      if (msg.data.length > maxBytes) return false;
      return true;
    });
  }

  /// Whether the input is currently connected.
  bool get isConnected {
    if (_disposed || _handle == null) return false;
    return _bindings.lrm_midi_in_is_connected(_handle!);
  }

  /// Closes the input connection and releases resources.
  void dispose() {
    if (!_disposed && _handle != null) {
      // Order matters to avoid race conditions:
      // 1. Mark disposed first to reject new callbacks
      _disposed = true;
      // 2. Close Dart stream controller
      _messageController.close();
      // 3. Close native callable (stops callbacks from native)
      _callback?.close();
      // 4. Finally close native MIDI input
      _bindings.lrm_midi_in_close(_handle!);
      _handle = null;
    }
  }
}

// =============================================================================
// LibremidiFlutter - High-level convenience API
// =============================================================================

/// High-level MIDI manager for simple use cases.
class LibremidiFlutter {
  static MidiObserver? _observer;
  static final Map<int, MidiInput> _openInputs = {}; // portId -> MidiInput
  static final Map<int, MidiOutput> _openOutputs = {}; // portId -> MidiOutput

  LibremidiFlutter._();

  static MidiObserver get _ensureObserver {
    _observer ??= MidiObserver.withHotplug();
    return _observer!;
  }

  /// Whether the library has been initialized (observer created).
  static bool get isInitialized => _observer != null;

  /// Gets the library version.
  static String get version {
    final ptr = _bindings.lrm_get_version();
    return ptr.cast<Utf8>().toDartString();
  }

  /// Stream of hotplug events (device added/removed).
  static Stream<HotplugEventType> get onHotplug => _ensureObserver.onHotplug;

  /// Gets all available MIDI input ports.
  static List<MidiPort> getInputPorts() {
    return _ensureObserver.getInputPorts();
  }

  /// Gets all available MIDI output ports.
  static List<MidiPort> getOutputPorts() {
    return _ensureObserver.getOutputPorts();
  }

  /// Opens a MIDI output connection to the specified port.
  ///
  /// Throws [StateError] if the port is already open.
  static MidiOutput openOutput(MidiPort port) {
    if (_openOutputs.containsKey(port.portId)) {
      throw StateError('Output port ${port.displayName} is already open');
    }
    final output = _ensureObserver.openOutput(port);
    _openOutputs[port.portId] = output;
    return output;
  }

  /// Opens a MIDI input connection to the specified port.
  ///
  /// By default, SysEx messages are received, while timing (MIDI clock) and
  /// active sensing messages are filtered out.
  ///
  /// Throws [StateError] if the port is already open.
  static MidiInput openInput(
    MidiPort port, {
    bool receiveSysex = true,
    bool receiveTiming = false,
    bool receiveSensing = false,
  }) {
    if (_openInputs.containsKey(port.portId)) {
      throw StateError('Input port ${port.displayName} is already open');
    }
    final input = _ensureObserver.openInput(
      port,
      receiveSysex: receiveSysex,
      receiveTiming: receiveTiming,
      receiveSensing: receiveSensing,
    );
    _openInputs[port.portId] = input;
    return input;
  }

  /// Disconnects a specific MIDI input.
  static void disconnectInput(MidiInput input) {
    input.dispose();
    _openInputs.removeWhere((_, v) => v == input);
  }

  /// Disconnects a specific MIDI output.
  static void disconnectOutput(MidiOutput output) {
    output.dispose();
    _openOutputs.removeWhere((_, v) => v == output);
  }

  /// Disconnects all open MIDI connections (inputs and outputs).
  static void disconnectAll() {
    for (final input in _openInputs.values) {
      input.dispose();
    }
    _openInputs.clear();

    for (final output in _openOutputs.values) {
      output.dispose();
    }
    _openOutputs.clear();
  }

  /// Returns the number of currently open input connections.
  static int get openInputCount => _openInputs.length;

  /// Returns the number of currently open output connections.
  static int get openOutputCount => _openOutputs.length;

  /// Disposes all resources (disconnects all and releases observer).
  ///
  /// This method is idempotent - calling it multiple times is safe.
  /// After disposal, [isInitialized] returns false and the library can
  /// be used again (a new observer will be created on next API call).
  static void dispose() {
    disconnectAll();
    _observer?.dispose();
    _observer = null;
  }
}
