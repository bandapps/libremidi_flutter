# Virtual MIDI loopback for the integration tests

Several integration tests send a message and check what comes back. That needs
a **virtual loopback**: a pair of software ports where what goes into one
arrives at the other. Without it those tests skip, and the run still ends in
`All tests passed!`.

That is not a hypothetical cost. On Windows the three loopback-dependent tests
had been skipping unnoticed, and two genuine SysEx conversion bugs — one of
them froze the app outright — survived in a branch whose test suite was green.
`loopback-backed coverage is enabled` now says out loud when the coverage is
missing.

The tests never touch hardware. `safeTestInput` and `virtualLoopbackPair`
return virtual ports only: opening and closing a connected USB device from a
test run is how a device ends up needing to be re-plugged.

## macOS

Enable the **IAC Driver** in *Audio MIDI Setup* → *Window* → *Show MIDI Studio*
→ double-click *IAC Driver* → tick *Device is online*. One port is enough; the
IAC bus echoes a device onto itself, which is the shape `virtualLoopbackPair`
matches by equal display name.

## Windows

Needs Windows MIDI Services, which is a separate matter from the loopback: on a
machine where the feature was never enabled, `midi.exe` reports
`The Windows MIDI Services feature does not appear to be enabled on this PC`
and the plugin falls back to WinUWP, where no virtual ports exist at all.
`mididiag.exe` and `midicheckservice.exe` (in
`C:\Program Files\Windows MIDI Services\Tools\`) report the state;
`midifixreg.exe`, run as Administrator, is Microsoft's tool for enabling it.
That rewrites `HKLM\...\CurrentVersion\Drivers32` and affects every MIDI
application on the machine, so read what it proposes before agreeing, and
export the key first.

With the service running, `windows_create.ps1` creates the pair:

```powershell
powershell -File tool/midi_loopback/windows_create.ps1
```

The endpoints exist until the service or the PC restarts. For a permanent pair
use the MIDI Settings app.

A Windows loopback is a *pair* — what is sent to A is received by B, never by A
itself — which is why `virtualLoopbackPair` matches those across their two
names rather than by equal display name.

## Linux

Not set up here. ALSA's `snd-virmidi` is the usual route; the plugin selects
`ALSA_SEQ`, a MIDI 1.0 backend, so the SysEx conversion paths these tests cover
are not reachable there in the first place.
