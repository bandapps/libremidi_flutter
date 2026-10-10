import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart' show malloc;

import 'package:flutter/material.dart' hide Size;
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

/// The device part of an Android port id (device id << 32 | port index).
int deviceOf(MidiPort p) => p.portId >> 32;

/// An input and an output of the same loopback device. There are two
/// identical loopback devices installed, so "the first of each" may pair
/// different devices, and a message sent to one never echoes on the other.
(MidiPort, MidiPort)? loopPair(MidiObserver o) {
  final outs = o.getOutputPorts().where(isLoop).toList();
  for (final i in o.getInputPorts().where(isLoop)) {
    for (final out in outs) {
      if (deviceOf(out) == deviceOf(i)) return (i, out);
    }
  }
  return null;
}

Future<void> roundTrip(MidiObserver obs, String label) async {
  final pair = loopPair(obs);
  check(pair != null, '$label loopback ports listed');
  if (pair == null) return;

  final input = obs.openInput(pair.$1);
  final output = obs.openOutput(pair.$2);
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
    final pair = loopPair(o);
    if (pair == null) continue;
    final input = o.openInput(pair.$1);
    try {
      final output = o.openOutput(pair.$2);
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

/// Creates and disposes hotplug observers back to back for 75 s.
int observerChurn() {
  final end = DateTime.now().add(const Duration(seconds: 75));
  var n = 0;
  while (DateTime.now().isBefore(end)) {
    MidiObserver.withHotplug().dispose();
    n++;
  }
  return n;
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
    final pair = loopPair(obs)!;
    var ticks = 0;
    final timer = Timer.periodic(const Duration(milliseconds: 1), (_) => ticks++);
    final inputF = obs.openInputAsync(pair.$1);
    final outputF = obs.openOutputAsync(pair.$2);
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

  // A device whose service takes 7 s to start: the first open times out at
  // 5 s, an immediate retry must join the open still in progress and get the
  // device. Without that, the retry started a second open, which MidiService
  // rejects while it is still binding the service.
  await guarded('slow device', () async {
    final slow = obs.getOutputPorts().where((p) => p.displayName.startsWith('SlowLoop')).toList();
    check(slow.isNotEmpty, 'slow loopback listed');
    if (slow.isEmpty) return;
    var first = 'opened';
    try {
      (await obs.openOutputAsync(slow.first)).dispose();
    } on MidiException catch (e) {
      first = e.message;
    }
    check(first.contains('timed out'), 'first open of a 7 s device times out ($first)');
    final out = await obs.openOutputAsync(slow.first);
    out.send(MidiMessageBuilder.noteOn(channel: 0, note: 2, velocity: 2));
    out.dispose();
    check(true, 'retry received the device that was still opening');
  });

  // A virtual device provided by this app: opening it synchronously from the
  // main thread can never finish (its service starts on that same thread), so
  // it must fail at once with a reason; the async open works.
  await guarded('own device', () async {
    final own = obs.getOutputPorts().where((p) => p.displayName.startsWith('OwnLoop')).toList();
    check(own.isNotEmpty, "app's own virtual device listed");
    if (own.isEmpty) return;
    final watch = Stopwatch()..start();
    var message = 'opened';
    try {
      obs.openOutput(own.first).dispose();
    } on MidiException catch (e) {
      message = e.message;
    }
    check(message.contains('provided by this app') && watch.elapsedMilliseconds < 1000,
        'sync open of own device refused at once (${watch.elapsedMilliseconds} ms: $message)');
    final out = await obs.openOutputAsync(own.first);
    out.dispose();
    check(true, 'own device opens asynchronously');
  });

  // A message that arrives in pieces — status in one packet, data in the
  // next, a clock byte in between, then running status — must come out as
  // whole messages. The loopback echoes every send as its own packet.
  await guarded('fragmented input', () async {
    final pair = loopPair(obs)!;
    final input = obs.openInput(pair.$1, receiveTiming: true);
    final output = obs.openOutput(pair.$2);
    final got = <MidiMessage>[];
    final sub = input.messages.listen(got.add);
    for (final piece in [
      [0x93],
      [0x30],
      [0xF8],
      [0x50],
      [0x31, 0x51], // running status: another note on, channel 4
    ]) {
      output.send(Uint8List.fromList(piece));
      await Future<void>.delayed(const Duration(milliseconds: 30));
    }
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await sub.cancel();
    input.dispose();
    output.dispose();
    final notes = got.where((m) => m.isNoteOn).map((m) => m.data.toList()).toList();
    final clocks = got.where((m) => m.status == 0xF8).length;
    check(
        notes.length == 2 &&
            notes[0].join(',') == '147,48,80' &&
            notes[1].join(',') == '147,49,81' &&
            clocks == 1 &&
            got.length == 3,
        'fragmented input reassembled ($notes, clocks $clocks, total ${got.length})');
  });

  // Cross-plugin bridging: another plugin gets the raw pointer through
  // handleAddress and the MidiOutput object itself is dropped. The garbage
  // collector must not free the native output underneath that plugin.
  await guarded('handleAddress', () async {
    final pair = loopPair(obs)!;
    final input = obs.openInput(pair.$1);
    final got = <MidiMessage>[];
    final sub = input.messages.listen(got.add);
    final address = obs.openOutput(pair.$2).handleAddress; // object dropped
    await collectGarbage();

    final lib = DynamicLibrary.open('liblibremidi_flutter.so');
    final send = lib.lookupFunction<Int32 Function(Pointer<Void>, Pointer<Uint8>, Size),
        int Function(Pointer<Void>, Pointer<Uint8>, int)>('lrm_midi_out_send');
    final close = lib.lookupFunction<Void Function(Pointer<Void>),
        void Function(Pointer<Void>)>('lrm_midi_out_close');
    final bytes = malloc<Uint8>(3);
    bytes[0] = 0x9E; // note on, channel 15
    bytes[1] = 44;
    bytes[2] = 4;
    final result = send(Pointer.fromAddress(address), bytes, 3);
    malloc.free(bytes);
    await Future<void>.delayed(const Duration(milliseconds: 300));
    close(Pointer.fromAddress(address)); // the other plugin is done
    await sub.cancel();
    input.dispose();
    check(result == 0 && got.any((m) => m.note == 44 && m.channel == 14),
        'raw handle still valid after its MidiOutput was collected (send $result)');
  });

  // Two identical devices (same name, manufacturer, product, no serial) must
  // be told apart: distinct port ids, and a message sent to the second one
  // arrives on the second one only.
  await guarded('identical devices', () async {
    final ins = obs.getInputPorts().where(isLoop).toList();
    final outs = obs.getOutputPorts().where(isLoop).toList();
    check(ins.length == 2 && outs.length == 2,
        'two identical loopback devices listed (${ins.length}/${outs.length})');
    if (ins.length != 2 || outs.length != 2) return;
    check(ins[0].stableId == ins[1].stableId,
        'the two devices are indistinguishable by their data');
    check(ins[0].portId != ins[1].portId, 'identical devices have distinct port ids');
    check({ins[0], ins[1]}.length == 2, 'identical devices are different MidiPorts (==)');
    final inA = obs.openInput(ins[0]);
    final inB = obs.openInput(ins[1]);
    final outB = obs.openOutput(outs.firstWhere((o) => deviceOf(o) == deviceOf(ins[1])));
    final gotA = <MidiMessage>[];
    final gotB = <MidiMessage>[];
    final subA = inA.messages.listen(gotA.add);
    final subB = inB.messages.listen(gotB.add);
    outB.send(MidiMessageBuilder.noteOn(channel: 9, note: 33, velocity: 3));
    await Future<void>.delayed(const Duration(milliseconds: 500));
    await subA.cancel();
    await subB.cancel();
    inA.dispose();
    inB.dispose();
    outB.dispose();
    check(gotB.any((m) => m.note == 33) && gotA.isEmpty,
        'second identical device addressed on its own (A ${gotA.length}, B ${gotB.length})');
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
  // Observers are created and torn down without pause while devices come and
  // go, here and on a worker isolate, so teardown keeps landing in the middle
  // of a hotplug notification. And the visible loopback count is sampled: with
  // one of the two identical devices removed it must drop to 1.
  var minLoops = 99, maxLoops = 0, churned = 0;
  var windowOpen = true;
  final churnWorker = Isolate.run(observerChurn);
  // The pattern apps use: on a hotplug event, throw the observer away and
  // make a new one. The event for the input arrives while the native side is
  // still delivering the output's change on the same observer.
  var reactiveRecreated = 0;
  late void Function() armReactive;
  MidiObserver? reactive;
  armReactive = () {
    final o = MidiObserver.withHotplug();
    reactive = o;
    o.onHotplug.first.then((_) {
      if (!windowOpen) return;
      o.dispose();
      reactiveRecreated++;
      armReactive();
    });
  };
  armReactive();
  final sampler = () async {
    while (windowOpen) {
      obs.refresh();
      final n = obs.getInputPorts().where(isLoop).length;
      if (n < minLoops) minLoops = n;
      if (n > maxLoops) maxLoops = n;
      final churn = MidiObserver.withHotplug();
      await Future<void>.delayed(Duration(milliseconds: churned % 20));
      churn.dispose();
      churned++;
    }
  }();
  log('HOTPLUG-WINDOW open for 75s');
  await Future<void>.delayed(const Duration(seconds: 75));
  windowOpen = false;
  await sampler;
  final workerChurned = await churnWorker;
  reactive?.dispose();
  heldInput?.dispose();
  check(reactiveRecreated >= 5,
      'observer disposed from its own hotplug event ($reactiveRecreated times)');
  check(minLoops == 1 && maxLoops == 2,
      'removing one of two identical devices is seen (loopbacks $minLoops..$maxLoops)');
  check(churned > 100 && workerChurned > 100,
      'observer teardown during hotplug ($churned here, $workerChurned on a worker)');
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
  final orphanDevice = await Isolate.run(() {
    final o = MidiObserver();
    final pair = loopPair(o);
    if (pair != null) o.openInput(pair.$1); // never disposed
    return pair == null ? 0 : deviceOf(pair.$1);
  });
  log('ORPHAN isolate exited with an open input; sending into it');
  final outs = obs.getOutputPorts().where((p) => isLoop(p) && deviceOf(p) == orphanDevice).toList();
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
