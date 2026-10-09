/// Cross-platform MIDI device access for Flutter using libremidi.
library;

import 'dart:convert' show utf8;
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate' show RawReceivePort;
import 'dart:typed_data';
import 'dart:async' show Completer, Stream, StreamController;

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

/// Dart_PostCObject, for the native side to deliver to a [RawReceivePort].
///
/// Native code posts incoming MIDI and hotplug events to a port instead of
/// calling a NativeCallable: when an isolate ends — or the app is hot
/// restarted — with an input or observer still open, a post to its closed port
/// is dropped, whereas calling a deleted NativeCallable aborts the process.
final LrmPostCObject _postCObject = NativeApi.postCObject.cast();

/// Why the last open on this thread failed; empty when the platform gives no
/// reason (currently only Android does).
String _lastOpenError() =>
    _bindings.lrm_get_last_error().cast<Utf8>().toDartString();

/// Builds the exception for a failed open, with the native reason appended.
MidiException _openFailure(String what, [String? reason]) {
  reason ??= _lastOpenError();
  return MidiException(reason.isEmpty ? what : '$what: $reason');
}

// Native cleanup for objects whose owner never called dispose() — typically
// because their isolate ended or the app was hot restarted. Each runs the
// matching close/free once the Dart object has been garbage collected or the
// isolate group shuts down; dispose() detaches it, so nothing is freed twice.
// An input stays reachable through its receive port while that port is open,
// so an input still in use is never finalized.
NativeFinalizer _finalizerFor(String symbol) => NativeFinalizer(
    _dylib.lookup<NativeFunction<Void Function(Pointer<Void>)>>(symbol));
final NativeFinalizer _observerFinalizer = _finalizerFor('lrm_observer_free');
final NativeFinalizer _inputFinalizer = _finalizerFor('lrm_midi_in_close');
final NativeFinalizer _outputFinalizer = _finalizerFor('lrm_midi_out_close');

final NativeFinalizer _ticketFinalizer = _finalizerFor('lrm_open_ticket_release');

/// The caller's share of a native asynchronous open (see `LrmOpenTicket` in
/// the header). If the awaiting isolate ends before the open finishes, this
/// object becomes unreachable and its finalizer gives up the share; the native
/// side then closes the port once the open completes. Nothing else in the
/// open uses the observer, so ending the isolate mid-open is safe.
final class _OpenTicket implements Finalizable {
  _OpenTicket(this._ticket) {
    _ticketFinalizer.attach(this, _ticket.cast(), detach: this);
  }

  final Pointer<LrmOpenTicket> _ticket;

  /// Takes the opened handle (null on failure) and the failure reason, and
  /// releases the ticket. Call once, after the done message.
  (Pointer<Void>, String) finish() {
    final handle = _bindings.lrm_open_ticket_take(_ticket);
    final error = _bindings.lrm_open_ticket_error(_ticket).cast<Utf8>().toDartString();
    _ticketFinalizer.detach(this);
    _bindings.lrm_open_ticket_release(_ticket);
    return (handle, error);
  }
}

/// Starts a native asynchronous open with [start] (given the native port to
/// notify) and waits for it. [what] names the port kind for the exception.
Future<Pointer<Void>> _openAsync(
  String what,
  Pointer<LrmOpenTicket> Function(int donePort) start,
) async {
  final done = Completer<void>();
  final donePort =
      RawReceivePort((_) => done.complete(), 'libremidi_flutter open');
  final _OpenTicket ticket;
  try {
    final raw = start(donePort.sendPort.nativePort);
    if (raw == nullptr) throw _openFailure(what);
    ticket = _OpenTicket(raw);
  } catch (_) {
    donePort.close();
    rethrow;
  }
  try {
    await done.future;
  } finally {
    donePort.close();
  }
  final (handle, error) = ticket.finish();
  if (handle == nullptr) throw _openFailure(what, error);
  return handle;
}

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

  /// Serializes only the fields needed for fuzzy matching to a JSON-compatible
  /// map. Persist this to later restore a port with [findClosestPort].
  Map<String, String> toMatchJson() => {
        'display_name': displayName,
        'port_name': portName,
        'device_name': deviceName,
        'manufacturer': manufacturer,
        'product': product,
        'serial': serial,
      };

  /// Reconstructs a [MidiPort] from a [toMatchJson] map for fuzzy matching.
  ///
  /// Only the matching-relevant string fields are restored; all other fields
  /// are set to defaults. Returns null if the map is null or carries no usable
  /// identifying information.
  static MidiPort? fromMatchJson(Map<String, dynamic>? json) {
    if (json == null) return null;
    final portName = json['port_name'] as String? ?? '';
    final displayName = json['display_name'] as String? ?? '';
    if (portName.isEmpty && displayName.isEmpty) return null;
    return MidiPort._(
      stableId: 0,
      portId: 0,
      clientHandle: 0,
      index: -1,
      displayName: displayName,
      portName: portName,
      deviceName: json['device_name'] as String? ?? '',
      manufacturer: json['manufacturer'] as String? ?? '',
      product: json['product'] as String? ?? '',
      serial: json['serial'] as String? ?? '',
      transportType: MidiTransportType.unknown,
      rawTransportType: 0,
      isInput: false,
      isVirtual: false,
    );
  }

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

  /// The backend's timestamp for this message, in **nanoseconds**.
  ///
  /// The value is passed through from libremidi unchanged. Earlier versions of
  /// this doc claimed microseconds and a conversion that never happened — the
  /// value has always been what the backend reports:
  ///
  /// - macOS/iOS: CoreMIDI host time converted to nanoseconds
  /// - Android: AMidi timestamp, nanoseconds
  /// - Linux / Windows: the backend's own clock, nanoseconds where it provides
  ///   absolute timestamps
  ///
  /// Only the epoch differs per platform and per session, so treat this as a
  /// monotonic value for measuring *intervals between messages*, not as a wall
  /// clock and not as something comparable across platforms.
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
// RPN / NRPN parsing
// =============================================================================

/// The type of parameter number message represented by [RpnNrpnMessage].
enum RpnNrpnType {
  /// Registered Parameter Number (RPN), selected with CC 101/100.
  rpn,

  /// Non-Registered Parameter Number (NRPN), selected with CC 99/98.
  nrpn,
}

/// The kind of RPN/NRPN data change represented by [RpnNrpnMessage].
enum RpnNrpnChangeType {
  /// Absolute Data Entry value from CC 6 and optional CC 38.
  value,

  /// Relative Data Increment from CC 96.
  increment,

  /// Relative Data Decrement from CC 97.
  decrement,
}

/// A decoded RPN or NRPN value from a Control Change sequence.
///
/// RPN and NRPN are not single MIDI messages. They are decoded from a sequence
/// of Control Change messages that select a parameter and then send Data Entry
/// values or increments/decrements. [source] is the Control Change message that
/// completed this decoded event (usually CC 6, CC 38, CC 96, or CC 97).
class RpnNrpnMessage {
  final RpnNrpnType type;
  final RpnNrpnChangeType changeType;
  final int channel;
  final int parameter;
  final int value;
  final bool fourteenBit;
  final MidiMessage source;

  const RpnNrpnMessage({
    required this.type,
    this.changeType = RpnNrpnChangeType.value,
    required this.channel,
    required this.parameter,
    required this.value,
    required this.fourteenBit,
    required this.source,
  });

  @override
  String toString() =>
      'RpnNrpnMessage(${type.name}, ${changeType.name}, channel: $channel, parameter: $parameter, value: $value, fourteenBit: $fourteenBit)';
}

class _RpnNrpnChannelState {
  RpnNrpnType? selectedType;
  int? rpnMsb;
  int? rpnLsb;
  int? nrpnMsb;
  int? nrpnLsb;
  int? dataMsb;

  int? get selectedParameter {
    switch (selectedType) {
      case RpnNrpnType.rpn:
        final msb = rpnMsb;
        final lsb = rpnLsb;
        return msb == null || lsb == null ? null : (msb << 7) | lsb;
      case RpnNrpnType.nrpn:
        final msb = nrpnMsb;
        final lsb = nrpnLsb;
        return msb == null || lsb == null ? null : (msb << 7) | lsb;
      case null:
        return null;
    }
  }

  void selectRpn() {
    selectedType = RpnNrpnType.rpn;
    nrpnMsb = null;
    nrpnLsb = null;
    dataMsb = null;
  }

  void selectNrpn() {
    selectedType = RpnNrpnType.nrpn;
    rpnMsb = null;
    rpnLsb = null;
    dataMsb = null;
  }

  void deselectRpnIfNullFunction() {
    if (rpnMsb == 127 && rpnLsb == 127) {
      selectedType = selectedType == RpnNrpnType.rpn ? null : selectedType;
      rpnMsb = null;
      rpnLsb = null;
      dataMsb = null;
    }
  }

  void deselectNrpnIfNullFunction() {
    if (nrpnMsb == 127 && nrpnLsb == 127) {
      selectedType = selectedType == RpnNrpnType.nrpn ? null : selectedType;
      nrpnMsb = null;
      nrpnLsb = null;
      dataMsb = null;
    }
  }
}

/// Stateful decoder for incoming RPN and NRPN Control Change sequences.
///
/// MIDI keeps RPN/NRPN selection state per channel, so use one parser instance
/// for each MIDI input stream you want to decode. The parser emits a 7-bit
/// event when CC 6 (Data Entry MSB) arrives, a 14-bit refinement when CC 38
/// (Data Entry LSB) follows for the same selected parameter, and relative
/// increment/decrement events when CC 96/97 arrive.
class RpnNrpnParser {
  final List<_RpnNrpnChannelState> _states = List.generate(
    16,
    (_) => _RpnNrpnChannelState(),
  );

  /// Processes one MIDI message and returns a decoded RPN/NRPN event if the
  /// message completes one.
  RpnNrpnMessage? process(MidiMessage message) {
    if (!message.isControlChange || message.data.length < 3) return null;

    final state = _states[message.channel];
    final controller = message.controller;
    final value = message.value;

    switch (controller) {
      case 101: // RPN MSB
        state.selectRpn();
        state.rpnMsb = value;
        state.deselectRpnIfNullFunction();
        return null;
      case 100: // RPN LSB
        state.selectRpn();
        state.rpnLsb = value;
        state.deselectRpnIfNullFunction();
        return null;
      case 99: // NRPN MSB
        state.selectNrpn();
        state.nrpnMsb = value;
        state.deselectNrpnIfNullFunction();
        return null;
      case 98: // NRPN LSB
        state.selectNrpn();
        state.nrpnLsb = value;
        state.deselectNrpnIfNullFunction();
        return null;
      case 6: // Data Entry MSB
        return _emitDataMsb(state, message, value);
      case 38: // Data Entry LSB
        return _emitDataLsb(state, message, value);
      case 96: // Data Increment
        return _emitRelativeChange(
          state,
          message,
          value,
          RpnNrpnChangeType.increment,
        );
      case 97: // Data Decrement
        return _emitRelativeChange(
          state,
          message,
          value,
          RpnNrpnChangeType.decrement,
        );
      default:
        return null;
    }
  }

  RpnNrpnMessage? _emitDataMsb(
    _RpnNrpnChannelState state,
    MidiMessage source,
    int value,
  ) {
    final type = state.selectedType;
    final parameter = state.selectedParameter;
    if (type == null || parameter == null) return null;

    state.dataMsb = value;
    return RpnNrpnMessage(
      type: type,
      channel: source.channel,
      parameter: parameter,
      value: value,
      fourteenBit: false,
      source: source,
    );
  }

  RpnNrpnMessage? _emitDataLsb(
    _RpnNrpnChannelState state,
    MidiMessage source,
    int value,
  ) {
    final type = state.selectedType;
    final parameter = state.selectedParameter;
    final dataMsb = state.dataMsb;
    if (type == null || parameter == null || dataMsb == null) return null;

    return RpnNrpnMessage(
      type: type,
      channel: source.channel,
      parameter: parameter,
      value: (dataMsb << 7) | value,
      fourteenBit: true,
      source: source,
    );
  }

  RpnNrpnMessage? _emitRelativeChange(
    _RpnNrpnChannelState state,
    MidiMessage source,
    int value,
    RpnNrpnChangeType changeType,
  ) {
    final type = state.selectedType;
    final parameter = state.selectedParameter;
    if (type == null || parameter == null) return null;

    return RpnNrpnMessage(
      type: type,
      changeType: changeType,
      channel: source.channel,
      parameter: parameter,
      value: value,
      fourteenBit: false,
      source: source,
    );
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
  outputRemoved,

  /// Generic setup change (e.g. macOS/iOS kMIDIMsgSetupChanged).
  /// The OS does not specify what changed — clients should re-enumerate ports.
  setupChanged;

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
      case 4:
        return HotplugEventType.setupChanged;
      default:
        return HotplugEventType.unknown;
    }
  }
}

// =============================================================================
// MidiObserver - Enumerate MIDI ports
// =============================================================================

/// Observer for enumerating available MIDI ports.
class MidiObserver implements Finalizable {
  Pointer<LrmObserver>? _handle;
  bool _disposed = false;
  RawReceivePort? _hotplugPort;
  final StreamController<HotplugEventType> _hotplugController =
      StreamController<HotplugEventType>.broadcast();

  /// Creates a new MIDI observer without hotplug detection.
  MidiObserver() {
    _handle = _bindings.lrm_observer_new();
    if (_handle == nullptr) {
      throw const MidiException('Failed to create MIDI observer');
    }
    _observerFinalizer.attach(this, _handle!.cast(), detach: this);
  }

  /// Creates a new MIDI observer with hotplug detection.
  ///
  /// On iOS and macOS the first CoreMIDI contact can be slow (seconds to
  /// minutes on a physical device), so it runs on a dedicated background thread
  /// and this constructor returns immediately. [getInputPorts] / [getOutputPorts]
  /// therefore return empty lists until the first
  /// [HotplugEventType.setupChanged] arrives on [onHotplug]. On Android, Linux
  /// and Windows the port lists are already populated when the constructor
  /// returns. See [onHotplug] for the full per-platform event contract.
  MidiObserver.withHotplug() {
    final port = RawReceivePort(_onHotplugEvent, 'libremidi_flutter hotplug');
    _hotplugPort = port;

    _handle = _bindings.lrm_observer_new_with_port(
      _postCObject,
      port.sendPort.nativePort,
    );

    if (_handle == nullptr) {
      port.close();
      throw const MidiException('Failed to create MIDI observer');
    }
    _observerFinalizer.attach(this, _handle!.cast(), detach: this);
  }

  void _onHotplugEvent(Object? eventType) {
    if (!_disposed && eventType is int) {
      _hotplugController.add(HotplugEventType.fromValue(eventType));
    }
  }

  /// Whether this observer can actually deliver hotplug events.
  ///
  /// False for an observer created without hotplug, and — on iOS/macOS — also
  /// when CoreMIDI refused the notification client (for example when the MIDI
  /// server is unreachable). That failure is otherwise invisible: the port list
  /// is correct and the first [HotplugEventType.setupChanged] still arrives,
  /// but no further event ever will, so an app that relies on hotplug should
  /// check this and fall back to polling [getInputPorts] / [getOutputPorts].
  /// On Android it is also false when the device has no MIDI support, in which
  /// case there are no ports either.
  ///
  /// On iOS/macOS this is false until the observer has finished initialising in
  /// the background — asking right after creating it always returns false.
  /// Check again after the first [HotplugEventType.setupChanged] arrives; that
  /// event is emitted once the notification client is set up, so the answer is
  /// final from then on.
  bool get hotplugAvailable {
    if (_disposed || _handle == null) return false;
    return _bindings.lrm_observer_hotplug_available(_handle!);
  }

  /// Stream of hotplug events (device added/removed).
  ///
  /// Note: Events originate from native callbacks which may be invoked on
  /// a different thread. The stream delivers events asynchronously to the
  /// Dart isolate, so UI updates should be safe.
  ///
  /// **The event pattern differs per platform.** Treat every event as "the
  /// port list changed, re-read it" rather than as an incremental description
  /// of what changed — code that counts added/removed events will not behave
  /// the same everywhere:
  ///
  /// - **iOS / macOS:** the port lists start out empty and a single
  ///   [HotplugEventType.setupChanged] signals that the first enumeration
  ///   finished. Afterwards CoreMIDI reports added/removed events, plus
  ///   [HotplugEventType.setupChanged] for changes it does not classify.
  /// - **Android / Linux / Windows:** the port lists are already filled when
  ///   the observer is created, and each device present at that point is
  ///   reported as an added event — i.e. a burst right after subscribing.
  ///   [HotplugEventType.setupChanged] is never emitted on these platforms.
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
  ///
  /// The port is resolved by [MidiPort.portId], which is stable across
  /// hotplug events (unlike index-based lookup).
  ///
  /// Returns once the port is open, so it can be sent to right away; throws a
  /// [MidiException] otherwise. On Android, opening a Bluetooth or virtual
  /// device for the first time can take a moment while the system connects
  /// it (at most 5 seconds before giving up).
  MidiOutput openOutput(MidiPort port) {
    _checkDisposed();
    if (port.isInput) {
      throw ArgumentError('Port must be an output port');
    }
    return MidiOutput._byId(_handle!, port.portId);
  }

  /// Opens a MIDI input connection to the specified port.
  ///
  /// The port is resolved by [MidiPort.portId], which is stable across
  /// hotplug events (unlike index-based lookup).
  ///
  /// By default, SysEx messages are received, while timing (MIDI clock) and
  /// active sensing messages are filtered out.
  ///
  /// Returns once the port is open; see [openOutput] for how long that can
  /// take on Android.
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
    return MidiInput._byId(
      _handle!,
      port.portId,
      receiveSysex: receiveSysex,
      receiveTiming: receiveTiming,
      receiveSensing: receiveSensing,
    );
  }

  /// Like [openOutput], but the port opens on a native thread while the
  /// calling isolate keeps running. Worth using on Android, where the first
  /// open of a Bluetooth or virtual device can take a moment; elsewhere
  /// opening is quick either way.
  ///
  /// If the calling isolate ends before the open finishes, the port is closed
  /// again as soon as it is open.
  Future<MidiOutput> openOutputAsync(MidiPort port) async {
    _checkDisposed();
    if (port.isInput) {
      throw ArgumentError('Port must be an output port');
    }
    final handle = await _openAsync(
      'Failed to open MIDI output',
      (donePort) => _bindings.lrm_midi_out_open_by_id_async(
          _handle!, port.portId, _postCObject, donePort),
    );
    return MidiOutput._adopt(handle.cast());
  }

  /// Like [openInput], but the port opens on a native thread while the
  /// calling isolate keeps running. See [openOutputAsync].
  Future<MidiInput> openInputAsync(
    MidiPort port, {
    bool receiveSysex = true,
    bool receiveTiming = false,
    bool receiveSensing = false,
  }) async {
    _checkDisposed();
    if (!port.isInput) {
      throw ArgumentError('Port must be an input port');
    }
    // Created first: messages may arrive before the open has been reported,
    // and they belong to this input.
    final input = MidiInput._pending();
    final Pointer<Void> handle;
    try {
      handle = await _openAsync(
        'Failed to open MIDI input',
        (donePort) => _bindings.lrm_midi_in_open_by_id_async(
          _handle!,
          port.portId,
          _postCObject,
          input._port!.sendPort.nativePort,
          donePort,
          receiveSysex,
          receiveTiming,
          receiveSensing,
        ),
      );
    } catch (_) {
      input.dispose();
      rethrow;
    }
    input._adoptHandle(handle.cast());
    return input;
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

  /// Finds the closest matching port using libremidi's heuristic fuzzy matcher.
  ///
  /// Pass a [MidiPort] describing the wanted device (e.g. restored via
  /// [MidiPort.fromMatchJson]) and whether to search inputs or outputs. Returns
  /// the index into the current port list (>= 0) on success, or -1 if no match
  /// was found. The match tolerates driver updates, USB-port changes and
  /// WinMM→WinMIDI migration better than an exact id.
  int findClosestPort(MidiPort port, {required bool isInput}) {
    _checkDisposed();
    final info = calloc<LrmPortInfo>();
    try {
      _fillPortInfo(info.ref, port);
      return _bindings.lrm_find_closest_port(_handle!, info, isInput);
    } finally {
      calloc.free(info);
    }
  }

  /// Disposes the observer and releases resources.
  ///
  /// Inputs and outputs opened from it stay open; dispose them separately.
  /// An async open already in progress is unaffected: it no longer uses the
  /// observer once started.
  void dispose() {
    if (_disposed || _handle == null) return;
    // 1. Mark disposed first to reject new events in Dart.
    _disposed = true;
    // 2. Free native resources (stops the native producer).
    _observerFinalizer.detach(this);
    _bindings.lrm_observer_free(_handle!);
    _handle = null;
    // 3. Close the port. Events already posted to it are dropped.
    _hotplugPort?.close();
    // 4. Close Dart stream controller
    _hotplugController.close();
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

  /// Writes a [MidiPort]'s matching-relevant string fields into a native
  /// [LrmPortInfo] struct for [findClosestPort]. Numeric fields are left as
  /// zeroed by the caller's `calloc`.
  static void _fillPortInfo(LrmPortInfo info, MidiPort port) {
    _stringToArray(info.display_name, port.displayName, 256);
    _stringToArray(info.port_name, port.portName, 256);
    _stringToArray(info.device_name, port.deviceName, 256);
    _stringToArray(info.manufacturer, port.manufacturer, 256);
    _stringToArray(info.product, port.product, 256);
    _stringToArray(info.serial, port.serial, 128);
  }

  /// Writes a Dart string into a fixed-size native char array as UTF-8,
  /// truncating to fit and always null-terminating.
  static void _stringToArray(Array<Char> array, String value, int maxLength) {
    final bytes = utf8.encode(value);
    final len = bytes.length < maxLength - 1 ? bytes.length : maxLength - 1;
    for (var i = 0; i < len; i++) {
      array[i] = bytes[i];
    }
    array[len] = 0; // null terminator
  }
}

// =============================================================================
// MidiMessageBuilder - Wire format used by MidiOutput
// =============================================================================

/// Builds the exact byte sequences that [MidiOutput] puts on the wire.
///
/// Every `MidiOutput.send*` method delegates here, so these builders *are* the
/// wire contract rather than a description of it. They are pure and need no
/// native library, which makes the byte layout (status nibbles, LSB/MSB order,
/// Control Change ordering) testable without an open MIDI port.
///
/// Methods returning a `List<Uint8List>` describe a sequence that is sent as
/// separate MIDI messages, in list order.
///
/// Range validation lives here too, so an out-of-range argument is rejected
/// identically whether it came through [MidiOutput] or a direct builder call.
class MidiMessageBuilder {
  const MidiMessageBuilder._();

  /// Note On: status 0x90 | channel, note, velocity.
  static Uint8List noteOn({
    required int channel,
    required int note,
    required int velocity,
  }) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(note, 0, 127, 'note');
    RangeError.checkValueInInterval(velocity, 0, 127, 'velocity');
    return Uint8List.fromList([0x90 | channel, note, velocity]);
  }

  /// Note Off: status 0x80 | channel, note, velocity.
  static Uint8List noteOff({
    required int channel,
    required int note,
    int velocity = 0,
  }) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(note, 0, 127, 'note');
    RangeError.checkValueInInterval(velocity, 0, 127, 'velocity');
    return Uint8List.fromList([0x80 | channel, note, velocity]);
  }

  /// Control Change: status 0xB0 | channel, controller, value.
  static Uint8List controlChange({
    required int channel,
    required int controller,
    required int value,
  }) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(controller, 0, 127, 'controller');
    RangeError.checkValueInInterval(value, 0, 127, 'value');
    return Uint8List.fromList([0xB0 | channel, controller, value]);
  }

  /// Program Change: status 0xC0 | channel, program. Two bytes, no data byte 2.
  static Uint8List programChange({
    required int channel,
    required int program,
  }) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(program, 0, 127, 'program');
    return Uint8List.fromList([0xC0 | channel, program]);
  }

  /// Pitch Bend: status 0xE0 | channel, then **LSB first, MSB second** — the
  /// opposite order of the 14-bit parameter encodings used by RPN/NRPN.
  static Uint8List pitchBend({required int channel, required int value}) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(value, 0, 16383, 'value');
    return Uint8List.fromList([
      0xE0 | channel,
      value & 0x7F,
      (value >> 7) & 0x7F,
    ]);
  }

  /// Channel Aftertouch (Channel Pressure): status 0xD0 | channel, pressure.
  static Uint8List aftertouch({required int channel, required int pressure}) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(pressure, 0, 127, 'pressure');
    return Uint8List.fromList([0xD0 | channel, pressure]);
  }

  /// Polyphonic Aftertouch (Key Pressure): status 0xA0 | channel, note, pressure.
  static Uint8List polyAftertouch({
    required int channel,
    required int note,
    required int pressure,
  }) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(note, 0, 127, 'note');
    RangeError.checkValueInInterval(pressure, 0, 127, 'pressure');
    return Uint8List.fromList([0xA0 | channel, note, pressure]);
  }

  /// SysEx. Adds the 0xF0/0xF7 framing unless [alreadyFramed] is set.
  static Uint8List sysEx(Uint8List data, {bool alreadyFramed = false}) {
    if (alreadyFramed) return data;
    final message = Uint8List(data.length + 2);
    message[0] = 0xF0;
    message.setRange(1, data.length + 1, data);
    message[data.length + 1] = 0xF7;
    return message;
  }

  /// Bank Select (CC 0 = MSB, CC 32 = LSB) followed by Program Change.
  static List<Uint8List> bankSelect({
    required int channel,
    required int bank,
    required int program,
  }) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(bank, 0, 16383, 'bank');
    RangeError.checkValueInInterval(program, 0, 127, 'program');
    return [
      controlChange(channel: channel, controller: 0, value: (bank >> 7) & 0x7F),
      controlChange(channel: channel, controller: 32, value: bank & 0x7F),
      programChange(channel: channel, program: program),
    ];
  }

  /// RPN (CC 101/100 select) or NRPN (CC 99/98 select) with a Data Entry value.
  ///
  /// [fourteenBit] sends CC 6 (MSB) then CC 38 (LSB); otherwise only CC 6.
  static List<Uint8List> parameterNumber({
    required int channel,
    required int parameter,
    required int value,
    required bool registered,
    bool fourteenBit = true,
    bool deselect = true,
  }) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(parameter, 0, 16383, 'parameter');
    RangeError.checkValueInInterval(
      value,
      0,
      fourteenBit ? 16383 : 127,
      'value',
    );

    final selectMsb = registered ? 101 : 99;
    final selectLsb = registered ? 100 : 98;
    final messages = <Uint8List>[
      controlChange(
          channel: channel,
          controller: selectMsb,
          value: (parameter >> 7) & 0x7F),
      controlChange(
          channel: channel, controller: selectLsb, value: parameter & 0x7F),
    ];

    if (fourteenBit) {
      messages.add(controlChange(
          channel: channel, controller: 6, value: (value >> 7) & 0x7F));
      messages.add(
          controlChange(channel: channel, controller: 38, value: value & 0x7F));
    } else {
      messages
          .add(controlChange(channel: channel, controller: 6, value: value));
    }

    if (deselect) {
      messages
          .add(controlChange(channel: channel, controller: selectMsb, value: 127));
      messages
          .add(controlChange(channel: channel, controller: selectLsb, value: 127));
    }
    return messages;
  }

  /// RPN/NRPN parameter select followed by Data Increment (CC 96) or
  /// Data Decrement (CC 97).
  static List<Uint8List> parameterNumberRelative({
    required int channel,
    required int parameter,
    required bool registered,
    required bool increment,
    int amount = 1,
    bool deselect = true,
  }) {
    RangeError.checkValueInInterval(channel, 0, 15, 'channel');
    RangeError.checkValueInInterval(parameter, 0, 16383, 'parameter');
    RangeError.checkValueInInterval(amount, 0, 127, 'amount');

    final selectMsb = registered ? 101 : 99;
    final selectLsb = registered ? 100 : 98;
    final messages = <Uint8List>[
      controlChange(
          channel: channel,
          controller: selectMsb,
          value: (parameter >> 7) & 0x7F),
      controlChange(
          channel: channel, controller: selectLsb, value: parameter & 0x7F),
      controlChange(
          channel: channel, controller: increment ? 96 : 97, value: amount),
    ];

    if (deselect) {
      messages
          .add(controlChange(channel: channel, controller: selectMsb, value: 127));
      messages
          .add(controlChange(channel: channel, controller: selectLsb, value: 127));
    }
    return messages;
  }
}

// =============================================================================
// MidiOutput - Send MIDI messages
// =============================================================================

/// Handles MIDI output to a connected port.
class MidiOutput implements Finalizable {
  Pointer<LrmMidiOut>? _handle;
  bool _disposed = false;

  /// Invoked once when this output is disposed. Set by [LibremidiFlutter] so
  /// its registry cannot go stale when callers dispose the output directly.
  void Function()? _onDispose;

  MidiOutput._byId(Pointer<LrmObserver> observer, int portId) {
    final handle = _bindings.lrm_midi_out_open_by_id(observer, portId);
    if (handle == nullptr) {
      throw _openFailure('Failed to open MIDI output');
    }
    _adoptHandle(handle);
  }

  MidiOutput._adopt(Pointer<LrmMidiOut> handle) {
    _adoptHandle(handle);
  }

  void _adoptHandle(Pointer<LrmMidiOut> handle) {
    _handle = handle;
    _outputFinalizer.attach(this, handle.cast(), detach: this);
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

  /// Sends every message of [messages] in list order.
  void _sendAll(List<Uint8List> messages) {
    for (final message in messages) {
      send(message);
    }
  }

  /// Sends a Note On message.
  void sendNoteOn({
    required int channel,
    required int note,
    required int velocity,
  }) =>
      send(MidiMessageBuilder.noteOn(
          channel: channel, note: note, velocity: velocity));

  /// Sends a Note Off message.
  void sendNoteOff({
    required int channel,
    required int note,
    int velocity = 0,
  }) =>
      send(MidiMessageBuilder.noteOff(
          channel: channel, note: note, velocity: velocity));

  /// Sends a Control Change message.
  void sendControlChange({
    required int channel,
    required int controller,
    required int value,
  }) =>
      send(MidiMessageBuilder.controlChange(
          channel: channel, controller: controller, value: value));

  /// Sends a Program Change message.
  void sendProgramChange({required int channel, required int program}) =>
      send(MidiMessageBuilder.programChange(
          channel: channel, program: program));

  /// Sends a Pitch Bend message.
  /// Value is 14-bit (0-16383), center is 8192.
  void sendPitchBend({required int channel, required int value}) =>
      send(MidiMessageBuilder.pitchBend(channel: channel, value: value));

  /// Sends Bank Select (CC 0 = MSB, CC 32 = LSB) followed by Program Change.
  ///
  /// Uses the standard GM/GS/XG convention: CC 0 for bank MSB, CC 32 for LSB.
  /// Some instruments only respond to MSB (bank 0-127), others use the full
  /// 14-bit range. The [bank] parameter is split: MSB = bank >> 7, LSB = bank & 0x7F.
  void sendBankSelect({
    required int channel,
    required int bank,
    required int program,
  }) =>
      _sendAll(MidiMessageBuilder.bankSelect(
          channel: channel, bank: bank, program: program));

  /// Sends a Registered Parameter Number (RPN) value.
  ///
  /// RPN uses a standard Control Change sequence:
  /// CC 101/100 select the parameter, CC 6 sets the value MSB, and CC 38
  /// optionally sets the value LSB for 14-bit values. When [deselect] is true,
  /// the RPN parameter is cleared afterwards with CC 101/100 = 127.
  ///
  /// With [fourteenBit] true, [value] must be 0..16383. With [fourteenBit]
  /// false, [value] must be 0..127 and only CC 6 is sent for the value.
  void sendRpn({
    required int channel,
    required int parameter,
    required int value,
    bool fourteenBit = true,
    bool deselect = true,
  }) =>
      _sendAll(MidiMessageBuilder.parameterNumber(
        channel: channel,
        parameter: parameter,
        value: value,
        registered: true,
        fourteenBit: fourteenBit,
        deselect: deselect,
      ));

  /// Sends a Data Increment for a Registered Parameter Number (RPN).
  ///
  /// Sends the RPN parameter select sequence followed by CC 96. [amount] is the
  /// raw 7-bit value byte for CC 96; many devices ignore it or interpret it as
  /// a step amount.
  void incrementRpn({
    required int channel,
    required int parameter,
    int amount = 1,
    bool deselect = true,
  }) =>
      _sendAll(MidiMessageBuilder.parameterNumberRelative(
        channel: channel,
        parameter: parameter,
        registered: true,
        increment: true,
        amount: amount,
        deselect: deselect,
      ));

  /// Sends a Data Decrement for a Registered Parameter Number (RPN).
  ///
  /// Sends the RPN parameter select sequence followed by CC 97. [amount] is the
  /// raw 7-bit value byte for CC 97; many devices ignore it or interpret it as
  /// a step amount.
  void decrementRpn({
    required int channel,
    required int parameter,
    int amount = 1,
    bool deselect = true,
  }) =>
      _sendAll(MidiMessageBuilder.parameterNumberRelative(
        channel: channel,
        parameter: parameter,
        registered: true,
        increment: false,
        amount: amount,
        deselect: deselect,
      ));

  /// Sends a Non-Registered Parameter Number (NRPN) value.
  ///
  /// NRPN uses a Control Change sequence:
  /// CC 99/98 select the parameter, CC 6 sets the value MSB, and CC 38
  /// optionally sets the value LSB for 14-bit values. When [deselect] is true,
  /// the NRPN parameter is cleared afterwards with CC 99/98 = 127.
  ///
  /// With [fourteenBit] true, [value] must be 0..16383. With [fourteenBit]
  /// false, [value] must be 0..127 and only CC 6 is sent for the value.
  void sendNrpn({
    required int channel,
    required int parameter,
    required int value,
    bool fourteenBit = true,
    bool deselect = true,
  }) =>
      _sendAll(MidiMessageBuilder.parameterNumber(
        channel: channel,
        parameter: parameter,
        value: value,
        registered: false,
        fourteenBit: fourteenBit,
        deselect: deselect,
      ));

  /// Sends a Data Increment for a Non-Registered Parameter Number (NRPN).
  ///
  /// Sends the NRPN parameter select sequence followed by CC 96. [amount] is
  /// the raw 7-bit value byte for CC 96; many devices ignore it or interpret it
  /// as a step amount.
  void incrementNrpn({
    required int channel,
    required int parameter,
    int amount = 1,
    bool deselect = true,
  }) =>
      _sendAll(MidiMessageBuilder.parameterNumberRelative(
        channel: channel,
        parameter: parameter,
        registered: false,
        increment: true,
        amount: amount,
        deselect: deselect,
      ));

  /// Sends a Data Decrement for a Non-Registered Parameter Number (NRPN).
  ///
  /// Sends the NRPN parameter select sequence followed by CC 97. [amount] is
  /// the raw 7-bit value byte for CC 97; many devices ignore it or interpret it
  /// as a step amount.
  void decrementNrpn({
    required int channel,
    required int parameter,
    int amount = 1,
    bool deselect = true,
  }) =>
      _sendAll(MidiMessageBuilder.parameterNumberRelative(
        channel: channel,
        parameter: parameter,
        registered: false,
        increment: false,
        amount: amount,
        deselect: deselect,
      ));

  /// Sends a SysEx message.
  ///
  /// By default, [data] should NOT include 0xF0/0xF7 framing bytes - they
  /// will be added automatically. Set [alreadyFramed] to true if [data]
  /// already includes the framing bytes (starts with 0xF0, ends with 0xF7).
  void sendSysEx(Uint8List data, {bool alreadyFramed = false}) =>
      send(MidiMessageBuilder.sysEx(data, alreadyFramed: alreadyFramed));

  /// Sends Channel Aftertouch (Channel Pressure).
  void sendAftertouch({required int channel, required int pressure}) =>
      send(MidiMessageBuilder.aftertouch(
          channel: channel, pressure: pressure));

  /// Sends Polyphonic Aftertouch (Key Pressure).
  void sendPolyAftertouch({
    required int channel,
    required int note,
    required int pressure,
  }) =>
      send(MidiMessageBuilder.polyAftertouch(
          channel: channel, note: note, pressure: pressure));

  /// Closes the output connection and releases resources.
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    if (_handle != null) {
      _outputFinalizer.detach(this);
      _bindings.lrm_midi_out_close(_handle!);
      _handle = null;
    }
    // Tell the LibremidiFlutter registry (if this output came from there) that
    // the port is free again. Without it, disposing directly instead of via
    // LibremidiFlutter.disconnectOutput left a stale entry behind and every
    // later openOutput() for the same port threw "already open" forever.
    final onDispose = _onDispose;
    _onDispose = null;
    onDispose?.call();
  }
}

// =============================================================================
// MidiInput - Receive MIDI messages
// =============================================================================

/// Handles MIDI input from a connected port.
class MidiInput implements Finalizable {
  Pointer<LrmMidiIn>? _handle;
  bool _disposed = false;
  RawReceivePort? _port;
  final StreamController<MidiMessage> _messageController =
      StreamController<MidiMessage>.broadcast();

  /// Invoked once when this input is disposed. Set by [LibremidiFlutter] so
  /// its registry cannot go stale when callers dispose the input directly.
  void Function()? _onDispose;

  MidiInput._byId(
    Pointer<LrmObserver> observer,
    int portId, {
    required bool receiveSysex,
    required bool receiveTiming,
    required bool receiveSensing,
  }) {
    final port = RawReceivePort(_onMidiMessage, 'libremidi_flutter input');
    _port = port;

    final handle = _bindings.lrm_midi_in_open_by_id_with_port(
      observer,
      portId,
      _postCObject,
      port.sendPort.nativePort,
      receiveSysex,
      receiveTiming,
      receiveSensing,
    );

    if (handle == nullptr) {
      // Read the reason before anything else can open on this thread.
      final error = _openFailure('Failed to open MIDI input');
      port.close();
      throw error;
    }
    _adoptHandle(handle);
  }

  /// An input whose native side is still being opened elsewhere; see
  /// [MidiObserver.openInputAsync].
  MidiInput._pending() {
    _port = RawReceivePort(_onMidiMessage, 'libremidi_flutter input');
  }

  void _adoptHandle(Pointer<LrmMidiIn> handle) {
    _handle = handle;
    _inputFinalizer.attach(this, handle.cast(), detach: this);
  }

  /// Receives `[Uint8List bytes, int timestamp]`, posted by the native side.
  /// The bytes were copied into this isolate by the post and are owned here.
  void _onMidiMessage(Object? message) {
    if (_disposed || message is! List || message.length != 2) return;
    final bytes = message[0];
    final timestamp = message[1];
    if (bytes is! Uint8List || timestamp is! int) return;
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

  /// Stream of decoded RPN / NRPN messages.
  ///
  /// The raw Control Change messages remain available through [messages]. This
  /// stream creates a parser for the returned stream and emits parameter events
  /// when a CC sequence selects an RPN/NRPN parameter and sends Data Entry.
  Stream<RpnNrpnMessage> get rpnNrpnMessages {
    final parser = RpnNrpnParser();
    return _messageController.stream.expand((msg) {
      final event = parser.process(msg);
      return event == null ? const <RpnNrpnMessage>[] : [event];
    });
  }

  /// Stream of incoming MIDI messages with optional filtering.
  ///
  /// Filters out messages that exceed [maxBytes] (default: 1024) or,
  /// optionally, all SysEx messages. This is a convenience filter on the
  /// Dart side — note that messages are still received and allocated by the
  /// native callback before filtering. To prevent large SysEx allocations
  /// entirely, disable SysEx at the input level via [MidiObserver.openInput]
  /// with `receiveSysex: false`.
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
    if (_disposed) return;
    // Order matters to avoid use-after-free:
    // 1. Mark disposed first to reject new callbacks in Dart
    _disposed = true;
    // 2. Close the native MIDI input first. This stops the native producer and
    //    waits for a delivery that is already inside the native callback.
    if (_handle != null) {
      _inputFinalizer.detach(this);
      _bindings.lrm_midi_in_close(_handle!);
      _handle = null;
    }
    // 3. Close the port. Messages already posted to it are dropped.
    _port?.close();
    // 4. Close Dart stream controller
    _messageController.close();
    // 5. Tell the LibremidiFlutter registry (if this input came from there)
    //    that the port is free again — otherwise disposing directly instead of
    //    via disconnectInput() left a stale entry and every later openInput()
    //    for the same port threw "already open" forever.
    final onDispose = _onDispose;
    _onDispose = null;
    onDispose?.call();
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
  // Ports with an async open in flight, so a second open is refused as for an
  // open port instead of racing the first.
  static final Set<int> _openingInputs = {};
  static final Set<int> _openingOutputs = {};
  // Bumped by dispose(). An async open that started under an older generation
  // must not register its port when it completes: dispose() promised that
  // everything is released.
  static int _generation = 0;

  static StateError _disposedWhileOpening() => StateError(
      'LibremidiFlutter was disposed while the port was opening');

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

  /// Whether hotplug events can actually be delivered.
  /// See [MidiObserver.hotplugAvailable].
  static bool get hotplugAvailable => _ensureObserver.hotplugAvailable;

  /// Gets all available MIDI input ports.
  static List<MidiPort> getInputPorts() {
    return _ensureObserver.getInputPorts();
  }

  /// Gets all available MIDI output ports.
  static List<MidiPort> getOutputPorts() {
    return _ensureObserver.getOutputPorts();
  }

  /// Finds the closest matching output port using fuzzy heuristic matching.
  /// Returns the port index (>= 0) or -1 if no match.
  static int findClosestOutputPort(MidiPort port) =>
      _ensureObserver.findClosestPort(port, isInput: false);

  /// Finds the closest matching input port using fuzzy heuristic matching.
  /// Returns the port index (>= 0) or -1 if no match.
  static int findClosestInputPort(MidiPort port) =>
      _ensureObserver.findClosestPort(port, isInput: true);

  /// Opens a MIDI output connection to the specified port.
  ///
  /// Throws [StateError] if the port is already open.
  static MidiOutput openOutput(MidiPort port) {
    if (_openOutputs.containsKey(port.portId) ||
        _openingOutputs.contains(port.portId)) {
      throw StateError('Output port ${port.displayName} is already open');
    }
    final output = _ensureObserver.openOutput(port);
    _openOutputs[port.portId] = output;
    // Keep the registry in sync even when the caller disposes the output
    // directly instead of going through disconnectOutput().
    output._onDispose = () => _openOutputs.remove(port.portId);
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
    if (_openInputs.containsKey(port.portId) ||
        _openingInputs.contains(port.portId)) {
      throw StateError('Input port ${port.displayName} is already open');
    }
    final input = _ensureObserver.openInput(
      port,
      receiveSysex: receiveSysex,
      receiveTiming: receiveTiming,
      receiveSensing: receiveSensing,
    );
    _openInputs[port.portId] = input;
    // Keep the registry in sync even when the caller disposes the input
    // directly instead of going through disconnectInput().
    input._onDispose = () => _openInputs.remove(port.portId);
    return input;
  }

  /// Like [openOutput], but keeps the calling isolate responsive while the
  /// port opens. See [MidiObserver.openOutputAsync].
  ///
  /// Completes with a [StateError] if [dispose] runs before the port has
  /// opened; the port is closed again in that case.
  static Future<MidiOutput> openOutputAsync(MidiPort port) async {
    final id = port.portId;
    if (_openOutputs.containsKey(id) || _openingOutputs.contains(id)) {
      throw StateError('Output port ${port.displayName} is already open');
    }
    final generation = _generation;
    _openingOutputs.add(id);
    final MidiOutput output;
    try {
      output = await _ensureObserver.openOutputAsync(port);
    } finally {
      if (generation == _generation) _openingOutputs.remove(id);
    }
    if (generation != _generation) {
      output.dispose();
      throw _disposedWhileOpening();
    }
    _openOutputs[id] = output;
    output._onDispose = () => _openOutputs.remove(id);
    return output;
  }

  /// Like [openInput], but keeps the calling isolate responsive while the
  /// port opens. See [MidiObserver.openOutputAsync] and [openOutputAsync].
  static Future<MidiInput> openInputAsync(
    MidiPort port, {
    bool receiveSysex = true,
    bool receiveTiming = false,
    bool receiveSensing = false,
  }) async {
    final id = port.portId;
    if (_openInputs.containsKey(id) || _openingInputs.contains(id)) {
      throw StateError('Input port ${port.displayName} is already open');
    }
    final generation = _generation;
    _openingInputs.add(id);
    final MidiInput input;
    try {
      input = await _ensureObserver.openInputAsync(
        port,
        receiveSysex: receiveSysex,
        receiveTiming: receiveTiming,
        receiveSensing: receiveSensing,
      );
    } finally {
      if (generation == _generation) _openingInputs.remove(id);
    }
    if (generation != _generation) {
      input.dispose();
      throw _disposedWhileOpening();
    }
    _openInputs[id] = input;
    input._onDispose = () => _openInputs.remove(id);
    return input;
  }

  /// Disconnects a specific MIDI input.
  static void disconnectInput(MidiInput input) {
    // dispose() removes the registry entry through the _onDispose hook. The
    // removeWhere below still covers inputs that were never registered here.
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
    // Iterate over copies: dispose() now mutates these maps through the
    // _onDispose hook, which would throw ConcurrentModificationError if we
    // walked the live map.
    for (final input in _openInputs.values.toList()) {
      input.dispose();
    }
    _openInputs.clear();

    for (final output in _openOutputs.values.toList()) {
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
  ///
  /// Async opens still in progress are abandoned: their ports are closed as
  /// soon as they open, and their futures complete with a [StateError].
  static void dispose() {
    _generation++;
    _openingInputs.clear();
    _openingOutputs.clear();
    disconnectAll();
    _observer?.dispose();
    _observer = null;
  }
}
