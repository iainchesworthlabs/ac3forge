# Troubleshooting

Ordered by what people actually hit, which is mostly the two devices in
[the signal path](signal-path.md).

## I hear everything twice

Applications are still playing to a device you can hear, so you get the direct mix *and*
Crucible's.

Station 1 of the path needs to be a silent device. The header pill says
`⚠ apps heard direct → …` while it is not, and offers the one button that fixes it.

If there is no silent device to send them to:

- **Windows** — the driver is not installed, or will not load. See
  [Install](install.md#the-silent-device); the Settings page reports whether this machine can
  load a test-signed driver at all.
- **Linux** — Crucible creates its own node, so this usually means the node was refused. Check a
  session is running (`wpctl status`) and look for "Crucible (silent)" in the sink list.

## I hear nothing at all

Work down the three stations.

1. **Is the engine running?** There is a Start/Stop beside its state.
2. **Is station 3 an endpoint you can actually hear?** With several endpoints, Crucible picks one
   automatically. Use "Hear it here" on the row you want, or "Automatic" to hand the choice back.
3. **Did it refuse to take the endpoint?** Taking an output exclusively fails while other
   applications are still rendering to it. Make sure applications have moved to the silent device
   first — that ordering is [the one rule](signal-path.md#the-one-rule-that-is-not-obvious) worth
   remembering.
4. **Is the receiver on the right input?** Crucible cannot tell a receiver on the wrong input from
   one that is not listening.

## Placements pan, but height does nothing

You have no signing key, so objects are off and Crucible is streaming plain 5.1. Your placements
pan within the bed; there is nothing to carry height.

This is deliberate. An unsigned-but-present object container is a hard refusal on a validating
decoder, not a graceful fallback, so Crucible sends no object metadata rather than something that
would be rejected outright. The Room page says so where the placing happens, and the elevation
view is dimmed and captioned.

Load a key ([Install](install.md#a-signing-key-on-any-platform)) and the mode line changes from
DD+ 5.1 to Atmos.

## An application is not in the list

- **It is not playing.** Crucible lists every running application with a window, but only ones
  with an audio session are tapped. A silent one shows greyed and reads "no audio".
- **It is a background process.** Anything without a visible window is hidden unless the Behaviour
  setting shows it. It is still in the bed either way.
- **On Linux, it plays through something Crucible cannot attribute.** The process behind a stream
  comes from PipeWire's Client object; a stream the daemon cannot attribute to a process is
  skipped, because there is nothing to tap.

## A placed application snapped back to the bed

It went full-screen. The full-screen application in front is always the bed — a full-screen game
rendering 7.1 *is* the bed — and the icon shows a lock to say why.

On Linux the rule is on under X11, where Crucible reads the active window's `_NET_WM_STATE` and
`_NET_WM_PID`, and off under Wayland, because no Wayland client can ask which window is
full-screen. The Room page's note says which applies; a build configured without libxcb says so
there too.

## The two dots for one application will not move apart

That application is **split** — a stereo application as two objects, one per channel. Each dot is
its own marker and drags where you put it; the pair's centre moves both. "Standard stereo" puts
them back.

Split costs a second object slot. With ten slots for placed applications, Crucible refuses a split
that would exceed the budget and says so rather than silently dropping one.

## Sound is behind the picture

Expect roughly 150–200 ms once a receiver has decoded, which is a visible lip-sync error on video.

**Low-latency mode** trades bitrate and headroom for a one-block frame. It needs roughly
1.5 Mb/s or more — below that the object metadata no longer fits in a single block and the encoder
refuses.

## Linux: it configured against ALSA and refused to build

Working as intended. Crucible cannot use the ALSA backend at all — no per-application tap — and
ALSA wins whenever both sets of headers are present. Reconfigure:

```bash
cmake --preset config-linux-gcc -B build/crucible -DAC3FORGE_BUILD_CRUCIBLE=ON -DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON
```

Note what that trades: ALSA's `iec958` passthrough is the path confirmed against a real receiver,
and PipeWire's is not. See [the plan](promotion.md#alsa-or-pipewire).

## Linux: the receiver is plugged in and on, but there is no HDMI sink

The kernel sees it and PipeWire does not. `wpctl status` lists only the headphone jack; the
HDMI card shows profiles `off` and `pro-audio` and switching to either creates nothing; and
`journalctl --user -u wireplumber` has a line like *"Failed to create alsa_output…hdmi…:
Object activation aborted: PipeWire proxy destroyed"*.

That is WirePlumber's hot-plug handling failing after a long uptime, and it was the first
thing the Raspberry Pi did when its receiver came on after three weeks. Restart the user
services:

```bash
systemctl --user restart pipewire pipewire-pulse wireplumber
```

The proper `hdmi-stereo` profile then appears, with `iec958.codecs` on the sink read from
the receiver's own EDID — for an Atmos receiver, `[PCM, DTS, AC3, EAC3, TrueHD, DTS-HD]`.
Nothing needs configuring by hand on a receiver that advertises its codecs; the earlier
belief that a `iec958Codecs` rule had to be written for WirePlumber turned out not to apply
to one that does.

## Linux: an endpoint that cannot carry a bitstream is offered one

It should not be. Crucible offers AC-3 or E-AC-3 on a sink only when that sink's
`iec958.codecs` lists the codec — WirePlumber's judgement from the display's EDID — and never
on the strength of a connect alone, because PipeWire's adapter will accept an IEC 958 stream
on an analogue jack and render the bursts as noise. If you see a bitstream mode on a
headphone jack, that gate has been bypassed; report it.

## Saving a diagnostics file

Settings, block 07 (DIAGNOSTICS), "Save diagnostics…" writes a plain-text file for a bug
report. The dialog suggests `crucible-diagnostics-<date>-<time>.txt` in your Documents folder,
and once the file is written the page says where it went.

The file holds, in this order: the version and build; the platform (OS, kernel, CPU, Qt, the
audio backend's capabilities, and whether the full-screen rule can work here); how the signing
key was obtained, without saying where it is; the engine's counters, including the catch-ups,
tap backlog and sink queue that the window does not show; the endpoints the last probe found
and what each accepts; the applications the engine lists, by name and description; the two
devices of [the signal path](signal-path.md) and the last silent-device action; this
application's settings; and the last 512 messages the application and its engine left, oldest
first, stamped in seconds since the log began.

It withholds the signing key, the path to the key file (whether chosen in Settings or given
through `AC3FORGE_SIGNING_KEY_FILE`), the value of `AC3FORGE_SIGNING_KEY`, the value of every
other environment variable, and the executable paths of the applications listed. The report is
composed from named fields, and none of them is the key or its path; the settings section is a
fixed list of keys, with anything under `signing/` written as `<withheld>`; and the finished text
is scrubbed of every spelling of the key path and of the inline key value, in case one arrived
through a message. Two tests hold that rule, one over the renderer and one over the window.

It does name your audio devices and their ids, the applications that are running, the OS
version, and on Windows the folder the driver package lives in. Read it before attaching it to
an issue.

Two limits. Only the last 512 messages are kept, so save the file soon after the fault. The
scrub matches the key path wherever it occurs, so a key file at a very short path withholds
that prefix everywhere it appears, which costs some of the report's usefulness and none of its
safety. The audio daemon's own output (PipeWire's log on Linux) is a separate thing and is not
captured here.
## Linux: an application shows a monogram

Crucible found no icon for it. It looks in three places, in order: the icon name the
application set on its own PipeWire client, the `.desktop` entry that matches it, and the icon
theme under the binary's own name.

- `pw-dump | grep -E '"application\.(name|icon-name|process\.binary)"'` shows what the
  application told PipeWire. `application.icon-name` is a theme icon name; most applications set
  only `application.name` and `application.process.binary`.
- The `.desktop` match is by application id (a Flatpak's), then `TryExec`, `Exec`,
  `StartupWMClass` against the name or the binary, then `Name`. An application started through
  a wrapper script has a binary its entry does not name, and matches only through
  `StartupWMClass` or `Name`.
- An icon that exists only as SVG needs Qt SVG (`libqt6svg6`); without it the monogram stays.
- A script, an interpreter or a command-line player (`python3`, `sh`, `aplay`) has no icon of
  its own, and the monogram is the right picture for it.

`QT_LOGGING_RULES="ac3crucible.icons.debug=true" ac3crucible` prints the theme Crucible found
and which of the three answered for each application.

## Getting more out of it

The console runner's `status` line reports the frame's own time beside the loop's cadence,
underruns, tap backlog and sink depth. The window shows the same. A build with
`AC3FORGE_ENABLE_TRACY=ON` carries Tracy zones on every stage of the frame loop if you need to
know where the time actually goes.
