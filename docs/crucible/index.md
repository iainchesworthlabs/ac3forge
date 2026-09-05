# AC3Forge Crucible

Every application making sound on your machine becomes an object in a Dolby Atmos scene. Drag
each one to a place in the room; what leaves for your receiver is a live E-AC-3 JOC stream in
which the browser is behind you, the game is in the corner, and the chat client is at your left
ear.

A crucible is where separate materials are combined under heat into one melt, which is what this
does to the sounds on a desk.

!!! note "Status: Windows works; Linux is new; macOS is not built yet"
    The application began as a Windows demo (roadmap UX11) and was promoted to a product on
    2026-09-04 (roadmap UX12). Windows is the mature platform. The Linux half is new: its
    per-application capture, silent device and window are confirmed on real hardware, and its
    bitstream path has reached a receiver's HDMI sink, with the receiver's own lock still to be
    read off its display. macOS has a design and no implementation.
    [Where each platform stands](#where-each-platform-stands) is exact about this, and
    [the promotion plan](promotion.md) carries the full record.

## What it does

1. **Install it**, and its silent output device.
2. **Send applications to that device** — one click, or your system's sound settings. Every
   application now plays into a device nobody hears.
3. **Crucible taps each one separately** and shows it in a room, as an icon with a level ring.
   Applications appear when they start playing and stay while they run.
4. **Drag one anywhere** — in plan, and in elevation. That application is now a dynamic object at
   that position. Drag it back to the tray, and it returns to the bed.
5. **What you hear follows your hardware.** An Atmos receiver over HDMI gets E-AC-3 JOC with the
   objects intact. A Dolby Digital receiver gets AC-3 5.1 with the positions panned onto the ring.
   A TV gets decoded multichannel PCM. Headphones get the decoded objects through the OS
   renderer, where there is one. Plugging or unplugging switches modes without a restart.

Anything you have not placed, and whichever application is full-screen in front, is mixed into
the 5.1 bed.

## The four things it needs from a system

Worth setting out, because it is what makes one platform easy and another impossible, and it
explains the shape of every page that follows.

1. **Enumerate** which applications are playing.
2. **Tap** each one separately, without the others.
3. **Silence** their direct output, so the only thing you hear is what Crucible sends.
4. **Bitstream** the encoded result to a receiver.

Step 3 is the one that surprises people. Tapping an application does not stop it also playing out
of your speakers, so without it you would hear everything twice. Each platform solves it
differently, and [Install and first run](install.md) is mostly about that difference.

## Where each platform stands

| | Windows | Linux | macOS |
|---|---|---|---|
| Enumerate and tap | yes | yes, confirmed on hardware | designed, not built |
| Silence | a signed driver, [see below](#the-silent-device) | a PipeWire node, nothing to install | the tap mutes; no device needed |
| Bitstream to a receiver | not yet confirmed | bursts reach the HDMI sink; receiver lock not yet confirmed | not built |
| The window | yes | yes, headless-verified on the Pi | no |

**Windows** is the platform the application was built on and the one with the longest record:
the room, the tray, the driver. Its silent device is a kernel driver that is **test-signed only** today: it
loads on a machine with test signing turned on, and refuses on a normal one. That closes when an
EV certificate and attestation submission are in place.

**Linux** has the engine, the console runner, the window, and all four platform services over
PipeWire. Its per-application tap, session list, default-device control and silent device are
confirmed against a live session on real hardware, and the window builds and renders there. The
library's PipeWire backend has streamed E-AC-3 bursts to a real HDMI sink with an Atmos receiver
on the end of it; whether the receiver locked is the receiver's display's to say, and that reading
is still open. It matters more than it sounds, because Crucible **cannot** use the ALSA backend
(no per-application tap), so it is forced onto the one passthrough path this project had not
confirmed before. [The plan](promotion.md#alsa-or-pipewire) is blunt about that. What the Linux
window still lacks: application icons (the monogram stands in), and the full-screen rule, which
Wayland cannot answer.

**macOS** needs no driver — its process taps can mute an application where they tap it, which is
the job the Windows driver exists to do — but nothing is implemented. It is blocked on a Mac to
run it and a certificate to sign it, not on knowing what to write.

## The silent device

The thing that stops you hearing everything twice, and the biggest difference between platforms.

**On Windows** it is a virtual audio device called "Desktop Atmos" that discards whatever it is
given. It arrives with the application and stays installed until you uninstall it.

**On Linux** there is nothing to install. Crucible creates a PipeWire node named
"Crucible (silent)" while it runs, and the node disappears when it exits. No driver, no signing,
no password, and nothing left on your machine afterwards.

**On macOS** there will be no silent device at all: the tap mutes each application individually
as it captures it, so there is no default output to move and nothing to restore.

## Objects, and what the bed is

The encoder carries a 5.1 bed plus up to 15 dynamic objects, and the bed is made *from* five of
those object slots pinned to the L, R, C, Ls and Rs speaker positions. That leaves **ten slots
for placed applications**, which is more than a desk needs.

Each application is folded to one signal before it goes into its slot, so one application is one
object. You can ask for a **split** instead, where a stereo application becomes two objects
placed either side of its position — that costs a second slot, and Crucible refuses when the
budget would be exceeded rather than silently dropping one.

Two things always go to the bed: every application you have not placed, and the full-screen
application in front, because a full-screen game rendering 7.1 *is* the bed. On Linux the
full-screen rule is off, because no Wayland client can ask which window is full-screen.

## Objects need a signing key

An unsigned-but-present object container is a hard refusal on a validating decoder, not a
graceful fallback. So with no key, Crucible sets no object metadata and streams plain 5.1 — your
placements still pan within the bed, but height does nothing.

The key is resolved at runtime, from a path in Settings or the same environment variables
`ac3cli` reads. It is never built in, never shipped in a package, and never written to a log.
See [Object signing](../concepts/object-signing.md).

## Where to go next

- [Install and first run](install.md) — per platform, including what to do about the silent device
- [The signal path](signal-path.md) — the two devices Crucible depends on, and why they are two
- [Troubleshooting](troubleshooting.md) — when you hear nothing, or hear everything twice
- [The promotion plan](promotion.md) — the design record, phase by phase, and what is unverified
