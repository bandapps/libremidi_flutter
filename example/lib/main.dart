import 'dart:async';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

/// MIDI demo with hotplug

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  SystemChrome.setPreferredOrientations([
    DeviceOrientation.portraitUp,
    DeviceOrientation.portraitDown,
  ]);
  runApp(
    const MaterialApp(debugShowCheckedModeBanner: false, home: MidiDemoPage()),
  );
}

/// Groups input/output ports into a single device.
class MidiDevice {
  final String name;
  final List<MidiPort> inputPorts = [];
  final List<MidiPort> outputPorts = [];
  String? manufacturer;
  MidiTransportType? transportType;
  int? _stableId;

  MidiDevice(this.name);

  int get inputCount => inputPorts.length;
  int get outputCount => outputPorts.length;

  /// Stable ID based on all port IDs + name (survives hotplug).
  int get stableId {
    if (_stableId != null) return _stableId!;
    var hash = name.hashCode;
    for (final port in inputPorts) {
      hash = hash ^ (port.effectiveStableId * 31);
    }
    for (final port in outputPorts) {
      hash = hash ^ (port.effectiveStableId * 37);
    }
    _stableId = hash;
    return _stableId!;
  }

  String get transportName => transportType?.name ?? '';
}

class MidiDemoPage extends StatefulWidget {
  const MidiDemoPage({super.key});

  @override
  State<MidiDemoPage> createState() => _MidiDemoPageState();
}

class _MidiDemoPageState extends State<MidiDemoPage> {
  List<MidiDevice> _devices = [];
  MidiDevice? _selectedDevice;
  bool _deviceConnected = false;
  MidiInput? _midiInput;
  MidiOutput? _midiOutput;

  // Multi-port: selected port indices
  int _selectedInputIndex = 0;
  int _selectedOutputIndex = 0;

  // MIDI values
  int _channel = 0;
  int _cc = 1;
  int _ccValue = 64;
  int _bank = 0;
  int _pc = 0;
  int _note = 60;
  int _velocity = 100;

  String _midiFunction = 'CC';

  late final TextEditingController _sysexController;

  // Note-off timers (key: channel << 8 | note)
  final Map<int, Timer> _noteOffTimers = {};

  // Log buffers and streams
  final List<String> _outLog = [];
  final List<String> _inLog = [];
  final _outLogController = StreamController<List<String>>.broadcast();
  final _inLogController = StreamController<List<String>>.broadcast();

  // Throttling for incoming messages
  final List<String> _inLogBuffer = [];
  Timer? _inLogFlushTimer;
  static const _logFlushInterval = Duration(milliseconds: 16);

  // Debounce for port reconnection
  Timer? _reconnectDebounce;
  static const _reconnectDelay = Duration(milliseconds: 100);

  @override
  void initState() {
    super.initState();
    _sysexController = TextEditingController(text: 'F0 7E 7F 06 01 F7');
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _refreshDevices();
      // Listen for device connect/disconnect events
      LibremidiFlutter.onHotplug.listen((_) => _refreshDevices());
    });
  }

  void _refreshDevices() {
    // Get all available MIDI ports
    final inputs = LibremidiFlutter.getInputPorts();
    final outputs = LibremidiFlutter.getOutputPorts();

    // Group ports by device name
    final deviceMap = <String, MidiDevice>{};

    for (final port in inputs) {
      final name = port.deviceName.isNotEmpty
          ? port.deviceName
          : port.displayName;
      deviceMap.putIfAbsent(name, () => MidiDevice(name));
      deviceMap[name]!.inputPorts.add(port);
      deviceMap[name]!.manufacturer ??= port.manufacturer;
      deviceMap[name]!.transportType ??= port.transportType;
    }

    for (final port in outputs) {
      final name = port.deviceName.isNotEmpty
          ? port.deviceName
          : port.displayName;
      deviceMap.putIfAbsent(name, () => MidiDevice(name));
      deviceMap[name]!.outputPorts.add(port);
      deviceMap[name]!.manufacturer ??= port.manufacturer;
      deviceMap[name]!.transportType ??= port.transportType;
    }

    final newDevices = deviceMap.values.toList()
      ..sort((a, b) => a.name.compareTo(b.name));

    String? statusMsg;
    bool connected = _deviceConnected;
    MidiDevice? newSelectedDevice = _selectedDevice;

    if (_selectedDevice != null) {
      final stillExists = newDevices.any(
        (d) => d.stableId == _selectedDevice!.stableId,
      );

      if (!stillExists && _deviceConnected) {
        statusMsg = 'Disconnected: ${_selectedDevice!.name}';
        if (_midiInput != null) {
          LibremidiFlutter.disconnectInput(_midiInput!);
          _midiInput = null;
        }
        if (_midiOutput != null) {
          LibremidiFlutter.disconnectOutput(_midiOutput!);
          _midiOutput = null;
        }
        connected = false;
      } else if (stillExists && !_deviceConnected) {
        newSelectedDevice = newDevices.firstWhere(
          (d) => d.stableId == _selectedDevice!.stableId,
        );
        _connectDeviceInternal(newSelectedDevice);
        statusMsg = 'Reconnected: ${newSelectedDevice.name}';
        connected = true;
      } else if (stillExists && _deviceConnected) {
        newSelectedDevice = newDevices.firstWhere(
          (d) => d.stableId == _selectedDevice!.stableId,
        );
      }
    }

    if (statusMsg != null) {
      _addOutLog(statusMsg);
      _addInLog(statusMsg);
    }
    setState(() {
      _devices = newDevices;
      _selectedDevice = newSelectedDevice;
      _deviceConnected = connected;
    });
  }

  void _connectDeviceInternal(MidiDevice device) {
    // Open MIDI input and listen for incoming messages
    if (device.inputPorts.isNotEmpty) {
      final idx = _selectedInputIndex.clamp(0, device.inputPorts.length - 1);
      try {
        _midiInput = LibremidiFlutter.openInput(device.inputPorts[idx]);
        _midiInput!.messages.listen(_onMidiMessage);
        _addInLog(
          'Connected: ${device.name} [In ${idx + 1}/${device.inputCount}]',
        );
      } catch (e) {
        _addInLog('Error ${device.name}: $e');
      }
    }

    // Open MIDI output for sending messages
    if (device.outputPorts.isNotEmpty) {
      final idx = _selectedOutputIndex.clamp(0, device.outputPorts.length - 1);
      try {
        _midiOutput = LibremidiFlutter.openOutput(device.outputPorts[idx]);
        _addOutLog(
          'Connected: ${device.name} [Out ${idx + 1}/${device.outputCount}]',
        );
      } catch (e) {
        _addOutLog('Error ${device.name}: $e');
      }
    }
  }

  void _onMidiMessage(MidiMessage msg) {
    _inLogBuffer.add(_formatMidiMessage(msg));
    _inLogFlushTimer ??= Timer(_logFlushInterval, _flushInLog);
  }

  void _flushInLog() {
    _inLogFlushTimer = null;
    if (_inLogBuffer.isEmpty) return;
    _inLog.insertAll(0, _inLogBuffer.reversed);
    _inLogBuffer.clear();
    if (_inLog.length > 100) _inLog.removeRange(100, _inLog.length);
    _inLogController.add(List.from(_inLog));
  }

  void _connectDevice(MidiDevice? device) {
    // Disconnect previous device
    if (_selectedDevice != null &&
        (_midiInput != null || _midiOutput != null)) {
      final oldName = _selectedDevice!.name;
      if (_midiInput != null) {
        LibremidiFlutter.disconnectInput(_midiInput!);
        _midiInput = null;
      }
      if (_midiOutput != null) {
        LibremidiFlutter.disconnectOutput(_midiOutput!);
        _midiOutput = null;
      }
      _addOutLog('Disconnected: $oldName');
      _addInLog('Disconnected: $oldName');
    }

    _selectedInputIndex = 0;
    _selectedOutputIndex = 0;

    bool connected = false;
    if (device != null) {
      _connectDeviceInternal(device);
      connected = _midiInput != null || _midiOutput != null;
    }

    setState(() {
      _selectedDevice = device;
      _deviceConnected = connected;
    });
  }

  String _formatMidiMessage(MidiMessage msg) {
    if (msg.isNoteOn)
      return 'Note ${msg.note} vel:${msg.velocity} ch:${msg.channel + 1}';
    if (msg.isNoteOff) return 'NoteOff ${msg.note} ch:${msg.channel + 1}';
    if (msg.type == 0xB0)
      return 'CC ${msg.controller}=${msg.value} ch:${msg.channel + 1}';
    if (msg.type == 0xC0) return 'PC ${msg.data[1]} ch:${msg.channel + 1}';
    if (msg.type == 0xE0)
      return 'PitchBend ${msg.data[1] | (msg.data[2] << 7)} ch:${msg.channel + 1}';
    if (msg.isSysEx) {
      final total = msg.data.length;
      final show = total > 256 ? msg.data.sublist(0, 256) : msg.data;
      final hex = show
          .map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase())
          .join(' ');
      if (total > 256) {
        return 'SysEx: $hex ... ($total bytes)';
      }
      return 'SysEx: $hex';
    }
    return msg.data.map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');
  }

  void _addOutLog(String msg) {
    _outLog.insert(0, msg);
    if (_outLog.length > 100) _outLog.removeLast();
    _outLogController.add(List.from(_outLog));
  }

  void _addInLog(String msg) {
    _inLog.insert(0, msg);
    if (_inLog.length > 100) _inLog.removeLast();
    _inLogController.add(List.from(_inLog));
  }

  // Send Note On, then auto Note Off after 300ms
  void _sendNote() {
    if (_midiOutput == null) return;

    final channel = _channel;
    final note = _note;
    final key = (channel << 8) | note;

    _noteOffTimers[key]?.cancel();
    _midiOutput!.sendNoteOn(channel: channel, note: note, velocity: _velocity);
    _addOutLog('Note $note vel:$_velocity ch:${channel + 1}');

    _noteOffTimers[key] = Timer(const Duration(milliseconds: 300), () {
      _noteOffTimers.remove(key);
      _midiOutput?.sendNoteOff(channel: channel, note: note);
    });
  }

  // Send Control Change message
  void _sendCC() {
    if (_midiOutput == null) return;
    _midiOutput!.sendControlChange(
      channel: _channel,
      controller: _cc,
      value: _ccValue,
    );
    _addOutLog('CC $_cc=$_ccValue ch:${_channel + 1}');
  }

  // Send Program Change (with optional Bank Select)
  void _sendPC() {
    if (_midiOutput == null) return;
    if (_bank > 0) {
      _midiOutput!.sendBankSelect(channel: _channel, bank: _bank, program: _pc);
      _addOutLog('Bank $_bank PC $_pc ch:${_channel + 1}');
    } else {
      _midiOutput!.sendProgramChange(channel: _channel, program: _pc);
      _addOutLog('PC $_pc ch:${_channel + 1}');
    }
  }

  // Send raw SysEx bytes (parsed from hex string)
  void _sendSysEx() {
    if (_midiOutput == null) return;
    try {
      final bytes = _sysexController.text
          .split(RegExp(r'[\s,]+'))
          .where((s) => s.isNotEmpty)
          .map((s) => int.parse(s, radix: 16))
          .toList();
      _midiOutput!.send(Uint8List.fromList(bytes));
      _addOutLog(
        'SysEx: ${bytes.map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase()).join(' ')}',
      );
    } catch (e) {
      _addOutLog('SysEx error: $e');
    }
  }

  @override
  void dispose() {
    _sysexController.dispose();
    _outLogController.close();
    _inLogController.close();
    _inLogFlushTimer?.cancel();
    _reconnectDebounce?.cancel();
    for (final timer in _noteOffTimers.values) {
      timer.cancel();
    }
    _noteOffTimers.clear();
    // Clean up MIDI connections
    if (_midiInput != null) LibremidiFlutter.disconnectInput(_midiInput!);
    if (_midiOutput != null) LibremidiFlutter.disconnectOutput(_midiOutput!);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: LayoutBuilder(
          builder: (context, constraints) {
            final logHeight = (constraints.maxHeight * 0.35).clamp(150.0, 300.0);
            return SingleChildScrollView(
              padding: const EdgeInsets.all(12.0),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.all(12.0),
                      child: Column(
                        children: [
                          _buildDeviceDropdown(),
                          const SizedBox(height: 12),
                          _buildPortSelectors(),
                        ],
                      ),
                    ),
                  ),
                  const SizedBox(height: 12),
                  _buildControls(),
                  const SizedBox(height: 12),
                  SizedBox(
                    height: logHeight,
                    child: Row(
                      children: [
                        Expanded(
                          child: _buildLogPanel(
                            'MIDI Out',
                            _outLog,
                            _outLogController.stream,
                            onClear: () {
                              _outLog.clear();
                              _outLogController.add([]);
                            },
                          ),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: _buildLogPanel(
                            'MIDI In',
                            _inLog,
                            _inLogController.stream,
                            onClear: () {
                              _inLog.clear();
                              _inLogController.add([]);
                            },
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            );
          },
        ),
      ),
    );
  }

  Widget _buildChannelDropdown({bool enabled = true}) {
    return DropdownButtonFormField<int>(
      initialValue: _channel,
      decoration: const InputDecoration(
        labelText: 'CH',
        contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 12),
        border: OutlineInputBorder(),
      ),
      items: List.generate(
        16,
        (i) => DropdownMenuItem(value: i, child: Text('${i + 1}')),
      ),
      onChanged: enabled ? (v) => setState(() => _channel = v ?? 0) : null,
    );
  }

  Widget _buildDeviceItem(
    MidiDevice d, {
    bool showStatus = false,
    bool connected = false,
  }) {
    final statusColor = connected ? Colors.green : Colors.red;
    final displayName = showStatus
        ? '${d.name} ${connected ? '[connected]' : '[disconnected]'}'
        : d.name;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(
          displayName,
          overflow: TextOverflow.ellipsis,
          style: showStatus
              ? TextStyle(color: statusColor, fontWeight: FontWeight.w500)
              : null,
        ),
        Text(
          'In: ${d.inputCount}  Out: ${d.outputCount}  ${d.manufacturer ?? ""}  ${d.transportName}'
              .trim(),
          style: TextStyle(fontSize: 11, color: Colors.grey.shade600),
          overflow: TextOverflow.ellipsis,
        ),
      ],
    );
  }

  Widget _buildPortSelectors() {
    final device = _selectedDevice;
    final inputEnabled = device != null && device.inputCount > 0;
    final outputEnabled = device != null && device.outputCount > 0;

    final outputDropdown = DropdownButtonFormField<int>(
      initialValue: outputEnabled
          ? _selectedOutputIndex.clamp(0, device.outputCount - 1)
          : null,
      isExpanded: true,
      decoration: const InputDecoration(
        labelText: 'Output Port',
        contentPadding: EdgeInsets.symmetric(horizontal: 8, vertical: 8),
        border: OutlineInputBorder(),
      ),
      items: outputEnabled
          ? List.generate(
              device.outputCount,
              (i) => DropdownMenuItem(
                value: i,
                child: Text(
                  device.outputPorts[i].displayName.isNotEmpty
                      ? device.outputPorts[i].displayName
                      : 'Port ${i + 1}',
                  overflow: TextOverflow.ellipsis,
                ),
              ),
            )
          : [],
      onChanged: outputEnabled
          ? (v) {
              if (v != null && v != _selectedOutputIndex) {
                setState(() => _selectedOutputIndex = v);
                _reconnectPorts();
              }
            }
          : null,
    );

    final inputDropdown = DropdownButtonFormField<int>(
      initialValue: inputEnabled
          ? _selectedInputIndex.clamp(0, device.inputCount - 1)
          : null,
      isExpanded: true,
      decoration: const InputDecoration(
        labelText: 'Input Port',
        contentPadding: EdgeInsets.symmetric(horizontal: 8, vertical: 8),
        border: OutlineInputBorder(),
      ),
      items: inputEnabled
          ? List.generate(
              device.inputCount,
              (i) => DropdownMenuItem(
                value: i,
                child: Text(
                  device.inputPorts[i].displayName.isNotEmpty
                      ? device.inputPorts[i].displayName
                      : 'Port ${i + 1}',
                  overflow: TextOverflow.ellipsis,
                ),
              ),
            )
          : [],
      onChanged: inputEnabled
          ? (v) {
              if (v != null && v != _selectedInputIndex) {
                setState(() => _selectedInputIndex = v);
                _reconnectPorts();
              }
            }
          : null,
    );

    final screenWidth = MediaQuery.of(context).size.width;
    if (screenWidth < 600) {
      return Column(
        children: [
          outputDropdown,
          const SizedBox(height: 8),
          inputDropdown,
        ],
      );
    }
    return Row(
      children: [
        Expanded(child: outputDropdown),
        const SizedBox(width: 8),
        Expanded(child: inputDropdown),
      ],
    );
  }

  void _reconnectPorts() {
    _reconnectDebounce?.cancel();
    _reconnectDebounce = Timer(_reconnectDelay, _doReconnectPorts);
  }

  void _doReconnectPorts() {
    if (_selectedDevice == null) return;
    final device = _selectedDevice!;

    if (_midiInput != null) {
      LibremidiFlutter.disconnectInput(_midiInput!);
      _midiInput = null;
    }
    if (_midiOutput != null) {
      LibremidiFlutter.disconnectOutput(_midiOutput!);
      _midiOutput = null;
    }

    _connectDeviceInternal(device);
    setState(() {
      _deviceConnected = _midiInput != null || _midiOutput != null;
    });
  }

  Widget _buildDeviceDropdown() {
    final dropdownDevices = List<MidiDevice>.from(_devices);
    if (_selectedDevice != null &&
        !_devices.any((d) => d.stableId == _selectedDevice!.stableId)) {
      dropdownDevices.insert(0, _selectedDevice!);
    }

    return DropdownButtonFormField<MidiDevice>(
      initialValue: _selectedDevice,
      isExpanded: true,
      menuMaxHeight: 400,
      decoration: const InputDecoration(
        labelText: 'MIDI Device',
        contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 12),
        border: OutlineInputBorder(),
      ),
      isDense: false,
      hint: const Text('Select device'),
      selectedItemBuilder: (context) => dropdownDevices
          .map(
            (d) => Align(
              alignment: Alignment.centerLeft,
              child: _buildDeviceItem(
                d,
                showStatus: true,
                connected:
                    _deviceConnected && d.stableId == _selectedDevice?.stableId,
              ),
            ),
          )
          .toList(),
      items: dropdownDevices
          .map(
            (d) => DropdownMenuItem(
              value: d,
              child: Padding(
                padding: const EdgeInsets.symmetric(vertical: 4),
                child: _buildDeviceItem(d),
              ),
            ),
          )
          .toList(),
      onChanged: _connectDevice,
    );
  }

  Widget _buildControls() {
    final enabled = _midiOutput != null;

    return Opacity(
      opacity: enabled ? 1.0 : 0.5,
      child: Card(
        child: Padding(
          padding: const EdgeInsets.all(12.0),
          child: AbsorbPointer(
            absorbing: !enabled,
            child: Column(
              children: [
                Row(
                  children: [
                    Expanded(
                      child: DropdownButtonFormField<String>(
                        initialValue: _midiFunction,
                        isExpanded: true,
                        decoration: const InputDecoration(
                          labelText: 'MIDI Function',
                          border: OutlineInputBorder(),
                          contentPadding: EdgeInsets.symmetric(
                            horizontal: 8,
                            vertical: 8,
                          ),
                        ),
                        items: const [
                          DropdownMenuItem(
                            value: 'CC',
                            child: Text('Control Change', overflow: TextOverflow.ellipsis),
                          ),
                          DropdownMenuItem(
                            value: 'PC',
                            child: Text('Program Change', overflow: TextOverflow.ellipsis),
                          ),
                          DropdownMenuItem(
                            value: 'SysEx',
                            child: Text('SysEx', overflow: TextOverflow.ellipsis),
                          ),
                          DropdownMenuItem(
                            value: 'Note',
                            child: Text('Note', overflow: TextOverflow.ellipsis),
                          ),
                        ],
                        onChanged: enabled
                            ? (v) => setState(() => _midiFunction = v ?? 'CC')
                            : null,
                      ),
                    ),
                    const SizedBox(width: 12),
                    SizedBox(
                      width: 80,
                      child: _buildChannelDropdown(enabled: enabled),
                    ),
                  ],
                ),
                const SizedBox(height: 12),
                ..._buildFunctionControls(enabled: enabled),
              ],
            ),
          ),
        ),
      ),
    );
  }

  List<Widget> _buildFunctionControls({bool enabled = true}) {
    switch (_midiFunction) {
      case 'Note':
        return [
          _buildTwoSliderRow(
            'Note',
            _note,
            0,
            127,
            (v) => setState(() => _note = v),
            'Vel',
            _velocity,
            0,
            127,
            (v) => setState(() => _velocity = v),
          ),
          const SizedBox(height: 8),
          ElevatedButton(
            onPressed: enabled ? _sendNote : null,
            child: const Text('Send'),
          ),
        ];
      case 'CC':
        return [
          _buildTwoSliderRow(
            'CC#',
            _cc,
            0,
            127,
            (v) => setState(() => _cc = v),
            'Value',
            _ccValue,
            0,
            127,
            (v) => setState(() => _ccValue = v),
          ),
          const SizedBox(height: 8),
          ElevatedButton(
            onPressed: enabled ? _sendCC : null,
            child: const Text('Send'),
          ),
        ];
      case 'PC':
        return [
          _buildTwoSliderRow(
            'Bank',
            _bank,
            0,
            127,
            (v) => setState(() => _bank = v),
            'Prog',
            _pc,
            0,
            127,
            (v) => setState(() => _pc = v),
          ),
          const SizedBox(height: 8),
          ElevatedButton(
            onPressed: enabled ? _sendPC : null,
            child: const Text('Send'),
          ),
        ];
      case 'SysEx':
        return [
          TextField(
            decoration: const InputDecoration(
              labelText: 'SysEx Data (hex)',
              hintText: 'F0 7E 7F 06 01 F7',
              border: OutlineInputBorder(),
            ),
            controller: _sysexController,
            enabled: enabled,
          ),
          const SizedBox(height: 8),
          ElevatedButton(
            onPressed: enabled ? _sendSysEx : null,
            child: const Text('Send'),
          ),
        ];
      default:
        return [];
    }
  }

  Widget _buildLogPanel(
    String title,
    List<String> buffer,
    Stream<List<String>> stream, {
    VoidCallback? onClear,
  }) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Text(
              title,
              style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 12),
            ),
            const Spacer(),
            SizedBox(
              width: 24,
              height: 24,
              child: IconButton(
                padding: EdgeInsets.zero,
                iconSize: 16,
                icon: const Icon(Icons.clear_all),
                tooltip: 'Clear',
                onPressed: onClear,
              ),
            ),
          ],
        ),
        const SizedBox(height: 4),
        Expanded(
          child: Container(
            decoration: BoxDecoration(
              border: Border.all(color: Colors.grey.shade300),
              borderRadius: BorderRadius.circular(4),
            ),
            child: StreamBuilder<List<String>>(
              stream: stream,
              initialData: List.from(buffer),
              builder: (context, snapshot) {
                final log = snapshot.data ?? [];
                return ListView.builder(
                  padding: const EdgeInsets.all(8),
                  itemCount: log.length,
                  itemBuilder: (_, i) => Text(
                    log[i],
                    style: const TextStyle(
                      fontSize: 13,
                      fontFamily: 'monospace',
                    ),
                  ),
                );
              },
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildSlider(
    String label,
    int value,
    int min,
    int max,
    ValueChanged<int> onChanged,
  ) {
    return Row(
      children: [
        SizedBox(width: 75, child: Text('$label: $value')),
        Expanded(
          child: Slider(
            value: value.toDouble(),
            min: min.toDouble(),
            max: max.toDouble(),
            divisions: max - min,
            onChanged: (v) => onChanged(v.round()),
          ),
        ),
      ],
    );
  }

  Widget _buildTwoSliderRow(
    String label1,
    int value1,
    int min1,
    int max1,
    ValueChanged<int> onChanged1,
    String label2,
    int value2,
    int min2,
    int max2,
    ValueChanged<int> onChanged2,
  ) {
    return Row(
      children: [
        Expanded(child: _buildSlider(label1, value1, min1, max1, onChanged1)),
        const SizedBox(width: 8),
        Expanded(child: _buildSlider(label2, value2, min2, max2, onChanged2)),
      ],
    );
  }
}
