# Stress harness (macOS, development only)

Drives the plugin's real C API — `apple/Classes/libremidi_flutter.cpp`, the
CoreMIDI implementation shared by iOS and macOS — without Flutter, Dart or
physical MIDI hardware, and hammers the teardown paths under a sanitizer.

It creates its own virtual CoreMIDI source with `MIDISourceCreate`, so it needs
no IAC Driver setup and no connected device.

```sh
./build.sh asan            # or: tsan, plain
./stress_asan midiin 20    # or: observer, all
```

## Scenarios

| Scenario   | What it does | What it targets |
|------------|--------------|-----------------|
| `midiin`   | Dense channel messages plus large SysEx while an input is opened and closed in a tight loop with jitter | The window in `LrmMidiIn::~LrmMidiIn`: a delivery starting while libremidi closes the port |
| `observer` | Virtual endpoints appearing and disappearing (a real CoreMIDI hotplug storm) while observers with hotplug callbacks are created and freed | `LrmObserver::~LrmObserver`, the dedicated run-loop thread teardown, and port-vector lifetime |

Typical throughput per 30 s run: ~9,400 open/close cycles with ~6.4 M delivered
messages (~130 MB) for `midiin`; ~900 observer create/free cycles over ~630
published endpoints and ~1,250 device add/remove events for `observer`.

**Read the counters, not just the exit code.** `observer` reports how many
virtual endpoints CoreMIDI actually accepted and how many of the hotplug events
were real device add/remove notifications rather than the one `setupChanged`
every observer emits by itself. A run that creates no endpoints, or that sees no
add/remove event, tested plain observer churn and not the hotplug path — the
harness fails such a run instead of reporting success, because it did not
exercise what its name claims.

## Why it exists

Code review found a hang in the observer teardown *by reasoning*; the fix that
followed was wrong, and only this harness proved it: the main thread sat in
`pthread_join` while the MIDI thread happily kept running `CFRunLoopRun`,
because a stop enqueued before the loop started was silently dropped. Anything
touching the CoreMIDI thread lifecycle should be re-run through this harness
before release.

## Reading the results

- **ASan** catches use-after-free and double free, including on the message
  buffers handed to the callback (the harness reads every byte and returns them
  through `lrm_free`, exactly like the Dart listener does).
- **TSan** catches data races. Note that races reported inside
  `libremidi::midi_in_core` — its destructor against CoreMIDI's read callback —
  are a known upstream limitation, not plugin state. The plugin's own guards
  (`closing`, `callback_mutex`) should never appear in a report; if they do,
  something in `LrmMidiIn` regressed.

Not part of the published package (excluded via `.pubignore`).

## When CoreMIDI starts refusing clients

Long or repeated runs can leave the MIDI server unhappy, and calls then fail
with `-10833` (`kMIDINoConnection` — not a start error, which would be
`-10839`). It is transient: `MIDIServer` is launched on demand, so simply
waiting, or letting the current runs finish, is usually enough. Neither a
reboot nor killing the daemon is normally needed.

This state cannot make a run look successful: the `observer` scenario fails
when no endpoint could be created, and `sendtest` fails when nothing reaches
the destination.

An earlier version of this harness *caused* the condition by creating one
MIDIClient per endpoint — around 1100 of them in half a minute. It now uses a
single long-lived client, but if you extend the harness, keep client creation
out of hot loops.

## Before you run this

Every scenario drives the live CoreMIDI server. On a machine with MIDI
hardware attached this has repeatedly left a connected USB device needing a
physical re-plug — including `midiin` and `sendtest`, which publish only a
single endpoint and which I had wrongly assumed were harmless. `observer` is
the worst of them: it publishes and removes hundreds.

Nothing runs without `LRM_STRESS_ALLOW_MIDI_DISRUPTION=1`:

```sh
LRM_STRESS_ALLOW_MIDI_DISRUPTION=1 ./stress_asan sendtest
```

Set it only on a machine whose MIDI devices you are willing to re-plug.

On top of that the harness refuses to start when it finds any non-virtual port,
and names what it found. The variable guards against starting this by accident;
the hardware check guards against the case where someone set the variable and
forgot to unplug.
