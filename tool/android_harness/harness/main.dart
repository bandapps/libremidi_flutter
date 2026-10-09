import 'dart:async';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:libremidi_flutter/libremidi_flutter.dart';

// Release-build harness for libremidi_flutter on Android. Results go to
// logcat with the prefix "HARNESS". Expects the midi_loopback app installed
// (one port that echoes input to output).

void log(String s) => debugPrint('HARNESS $s');

int failures = 0;
void check(bool ok, String what) {
  if (!ok) failures++;
  log('${ok ? "PASS" : "FAIL"} $what');
}

Future<void> guarded(String label, Future<void> Function() body) async {
  try {
    await body();
  } catch (e) {
    check(false, '$label threw: $e');
  }
}

Future<void> sendWhenReady(MidiOutput out, Uint8List data) async {
  // Android opens asynchronously; sending before the open completes throws.
  for (var i = 0; i < 50; i++) {
    try {
      out.send(data);
      return;
    } on MidiException {
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
  }
  throw StateError('output never became ready');
}

bool isLoop(MidiPort p) => p.displayName.startsWith('Loopback');

Future<void> roundTrip(MidiObserver obs, String label) async {
  final ins = obs.getInputPorts().where(isLoop).toList();
  final outs = obs.getOutputPorts().where(isLoop).toList();
  check(ins.isNotEmpty && outs.isNotEmpty,
      '$label loopback ports listed (in=${ins.length} out=${outs.length})');
  if (ins.isEmpty || outs.isEmpty) return;

  final input = obs.openInput(ins.first);
  final output = obs.openOutput(outs.first);
  final received = <MidiMessage>[];
  final sub = input.messages.listen(received.add);

  try {
    // The input may also still be opening; resend until something arrives.
    final note = MidiMessageBuilder.noteOn(channel: 3, note: 60, velocity: 100);
    final deadline = DateTime.now().add(const Duration(seconds: 5));
    while (received.isEmpty && DateTime.now().isBefore(deadline)) {
      await sendWhenReady(output, note);
      await Future<void>.delayed(const Duration(milliseconds: 200));
    }
    check(received.any((m) => m.isNoteOn && m.note == 60 && m.channel == 3),
        '$label note round trip (${received.length} msgs)');

    received.clear();
    final payload =
        Uint8List.fromList([0x7D, for (var i = 0; i < 200; i++) i & 0x7F]);
    final sysex = MidiMessageBuilder.sysEx(payload);
    await sendWhenReady(output, sysex);
    await Future<void>.delayed(const Duration(milliseconds: 500));
    check(
        received.any((m) =>
            m.isSysEx &&
            m.data.length == sysex.length &&
            m.data[100] == sysex[100]),
        '$label sysex round trip (${sysex.length} bytes)');
  } finally {
    await sub.cancel();
    input.dispose();
    output.dispose();
  }
}

// Top-level so that Isolate.run does not capture the main isolate's state.
int openCloseCycles(MidiObserver o, int n) {
  var ok = 0;
  for (var i = 0; i < n; i++) {
    final ins = o.getInputPorts().where(isLoop).toList();
    final outs = o.getOutputPorts().where(isLoop).toList();
    if (ins.isEmpty || outs.isEmpty) continue;
    final input = o.openInput(ins.first);
    try {
      final output = o.openOutput(outs.first);
      try {
        output.send(MidiMessageBuilder.noteOn(channel: 0, note: 1, velocity: 1));
        ok++;
      } finally {
        output.dispose();
      }
    } finally {
      input.dispose();
    }
  }
  return ok;
}

/// Allocates until the collector has run, so finalizers of unreachable
/// objects get their turn.
Future<void> collectGarbage() async {
  var keep = 0;
  for (var round = 0; round < 40; round++) {
    final garbage = List.generate(64, (_) => Uint8List(1 << 20));
    keep += garbage.length;
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }
  if (keep < 0) log('unreachable');
}

/// Starts both async opens and returns at once, so the isolate ends while the
/// device is still being opened. Neither the observer nor the opens are ever
/// completed or disposed here.
int abandonAsyncOpens() {
  final o = MidiObserver();
  final ins = o.getInputPorts().where(isLoop).toList();
  final outs = o.getOutputPorts().where(isLoop).toList();
  if (ins.isEmpty || outs.isEmpty) return 0;
  o.openInputAsync(ins.first).ignore();
  o.openOutputAsync(outs.first).ignore();
  return 1;
}

int workerCycles() {
  final o = MidiObserver();
  try {
    return openCloseCycles(o, 50);
  } finally {
    o.dispose();
  }
}

Future<void> run() async {
  log('START');
  final obs = MidiObserver.withHotplug();
  final hotplugEvents = <HotplugEventType>[];
  obs.onHotplug.listen(hotplugEvents.add);
  await Future<void>.delayed(const Duration(seconds: 1));
  check(obs.hotplugAvailable, 'hotplugAvailable');
  log('inputs: ${obs.getInputPorts().map((p) => p.displayName).toList()}');
  log('outputs: ${obs.getOutputPorts().map((p) => p.displayName).toList()}');

  await guarded('main-isolate', () => roundTrip(obs, 'main-isolate'));
  for (var i = 0; i < 5; i++) {
    await guarded('repeat-$i', () => roundTrip(obs, 'repeat-$i'));
  }

  // Async open: the calling isolate keeps running while the port opens, and
  // the ports work like synchronously opened ones. The device is left closed
  // first, so the open has to bind it again.
  await guarded('async open', () async {
    await Future<void>.delayed(const Duration(seconds: 2));
    final ins = obs.getInputPorts().where(isLoop).toList();
    final outs = obs.getOutputPorts().where(isLoop).toList();
    var ticks = 0;
    final timer = Timer.periodic(const Duration(milliseconds: 1), (_) => ticks++);
    final inputF = obs.openInputAsync(ins.first);
    final outputF = obs.openOutputAsync(outs.first);
    final input = await inputF;
    final output = await outputF;
    timer.cancel();
    final received = <MidiMessage>[];
    final sub = input.messages.listen(received.add);
    output.send(MidiMessageBuilder.noteOn(channel: 5, note: 70, velocity: 7));
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await sub.cancel();
    input.dispose();
    output.dispose();
    check(ticks > 0, 'async open kept the isolate running ($ticks ticks)');
    check(received.any((m) => m.isNoteOn && m.note == 70 && m.channel == 5),
        'async open round trip');
  });

  // Polling on the main thread within one event: local references created
  // here are only reclaimed when the thread returns to Java. refresh() is what
  // re-enumerates natively; getInputPorts() alone reads a Dart-side cache.
  await guarded('main poll', () async {
    var n = 0;
    for (var i = 0; i < 2000; i++) {
      obs.refresh();
      n += obs.getInputPorts().length + obs.getOutputPorts().length;
    }
    check(n > 0, 'main-thread poll x2000');
  });

  // Polling on a Dart worker thread: attached by the plugin, never returns
  // to Java, so a per-call local reference leak overflows the table (512 on
  // API 29) well before 2000 iterations.
  await guarded('worker poll', () async {
    final n = await Isolate.run(() {
      final o = MidiObserver();
      var n = 0;
      for (var i = 0; i < 2000; i++) {
        o.refresh();
        n += o.getInputPorts().length + o.getOutputPorts().length;
      }
      o.dispose();
      return n;
    });
    check(n > 0, 'worker-thread poll x2000');
  });

  // Round trip from a background isolate too.
  await guarded('worker roundtrip', () async {
    final ok = await Isolate.run(() async {
      final o = MidiObserver();
      try {
        await roundTrip(o, 'worker');
      } finally {
        o.dispose();
      }
      return failures == 0;
    });
    check(ok, 'worker isolate round trip');
  });

  // Main isolate and a worker open and close the same device at the same
  // time: shared device lease, concurrent first opens, close during open.
  await guarded('concurrent open/close', () async {
    final worker = Isolate.run(workerCycles);
    final mine = openCloseCycles(obs, 50);
    final theirs = await worker;
    check(mine == 50 && theirs == 50, 'concurrent open/close x50 each ($mine/$theirs)');
  });

  // The workers above are retired by the VM once idle. A thread that exits
  // while still attached to the JVM aborts the process.
  log('waiting for worker threads to retire');
  await Future<void>.delayed(const Duration(seconds: 70));
  check(true, 'process alive after worker thread retirement');

  // An input stays open while the script removes and re-adds the device
  // several times; hotplug dispatch must cope with that.
  final held = obs.getInputPorts().where(isLoop).toList();
  final heldInput = held.isEmpty ? null : obs.openInput(held.first);
  hotplugEvents.clear();
  log('HOTPLUG-WINDOW open for 75s');
  await Future<void>.delayed(const Duration(seconds: 75));
  heldInput?.dispose();
  final removals = hotplugEvents.where((e) => e == HotplugEventType.inputRemoved).length;
  final additions = hotplugEvents.where((e) => e == HotplugEventType.inputAdded).length;
  check(removals >= 5 && additions >= 5,
      'hotplug cycles seen (removed $removals, added $additions)');

  // After re-adding, the device must be usable again.
  await guarded('after-hotplug', () => roundTrip(obs, 'after-hotplug'));

  // An isolate that ends in the middle of an async open: the opens finish on
  // their native threads with nobody left to take the ports, so they must be
  // closed again. The connection count read at DONE catches it if not.
  await guarded('isolate ends mid-open', () async {
    await Future<void>.delayed(const Duration(seconds: 3)); // device idle
    final started = await Isolate.run(abandonAsyncOpens);
    check(started == 1, 'abandoned async opens started');
    await Future<void>.delayed(const Duration(seconds: 6)); // opens finish
    await collectGarbage();
    await Future<void>.delayed(const Duration(seconds: 2));
    check(true, 'process alive after abandoned async opens');
  });

  // LibremidiFlutter.dispose() while its async opens are still running: the
  // futures fail, nothing is registered, and the ports are closed natively —
  // the connection count at DONE catches a port left open.
  await guarded('dispose during async open', () async {
    await Future<void>.delayed(const Duration(seconds: 3)); // device idle
    final ins = LibremidiFlutter.getInputPorts().where(isLoop).toList();
    final outs = LibremidiFlutter.getOutputPorts().where(isLoop).toList();
    final inputF = LibremidiFlutter.openInputAsync(ins.first);
    final outputF = LibremidiFlutter.openOutputAsync(outs.first);
    LibremidiFlutter.dispose();
    var stateErrors = 0;
    for (final f in [inputF, outputF]) {
      try {
        await f;
      } on StateError {
        stateErrors++;
      }
    }
    check(stateErrors == 2, 'async opens abandoned by dispose fail ($stateErrors/2)');
    check(LibremidiFlutter.openInputCount == 0 &&
            LibremidiFlutter.openOutputCount == 0,
        'nothing registered after dispose');
    LibremidiFlutter.dispose();
    await Future<void>.delayed(const Duration(seconds: 2));
  });

  log('DONE failures=$failures');
  // Give the runner time to read MidiService's connection count before the
  // probe below deliberately leaves an input open.
  await Future<void>.delayed(const Duration(seconds: 5));

  // Probe, deliberately last: an isolate that ends with an input still open.
  await Isolate.run(() {
    final o = MidiObserver();
    final ins = o.getInputPorts().where(isLoop).toList();
    if (ins.isNotEmpty) o.openInput(ins.first); // never disposed
    return 0;
  });
  log('ORPHAN isolate exited with an open input; sending into it');
  final outs = obs.getOutputPorts().where(isLoop).toList();
  if (outs.isNotEmpty) {
    final out = obs.openOutput(outs.first);
    for (var i = 0; i < 20; i++) {
      out.send(MidiMessageBuilder.noteOn(channel: 0, note: 64, velocity: 1));
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
    out.dispose();
  }
  log('ORPHAN survived');
  obs.dispose();

  // The orphaned input and observer are unreachable now; let the collector
  // run so their finalizers close them natively.
  await collectGarbage();
  await Future<void>.delayed(const Duration(seconds: 2));
  log('ORPHAN-GC done');
}

void main() {
  runApp(const MaterialApp(home: Scaffold(body: Center(child: Text('harness')))));
  run();
}
