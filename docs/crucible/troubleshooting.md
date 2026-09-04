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

Not on Linux: the rule is off there, because no Wayland client can ask which window is
full-screen.

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

## Linux: no compressed format is offered on any endpoint

A PipeWire sink only advertises a compressed codec once WirePlumber's `iec958Codecs` has been
populated for it, and that is configuration Crucible cannot do on your behalf. Without it, every
bitstream mode probes as unavailable and Crucible falls back to PCM or stereo.

This is an open gap rather than a solved one — see
[Linux → Audio backend](../platforms/linux.md#audio-backend-alsa-or-pipewire).

## Getting more out of it

The console runner's `status` line reports the frame's own time beside the loop's cadence,
underruns, tap backlog and sink depth. The window shows the same. A build with
`AC3FORGE_ENABLE_TRACY=ON` carries Tracy zones on every stage of the frame loop if you need to
know where the time actually goes.
