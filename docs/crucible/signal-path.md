# The signal path

Crucible depends on **two** audio devices, and confusing them is the single most common way to
end up with silence or with everything playing twice. This page is that distinction, and nothing
else.

## Three stations

```
    applications play to  →  Crucible  →  you hear it on
      the silent device                    a receiver, a TV,
      (nobody hears it)                    headphones, speakers
```

**Station 1, where applications play.** This has to be a device you cannot hear. Crucible taps
each application there, individually. If it is a real device, you will hear every application
directly *as well as* through Crucible — the doubling that station 1 exists to prevent.

**Station 2, Crucible itself.** It takes each tap, places it where you dragged it, and encodes
the room as one stream.

**Station 3, what you hear.** One endpoint: your receiver over HDMI, a TV, headphones, or plain
speakers. This is the only station that makes sound.

The window shows all three, with a warning against whichever one is not yet what the path needs,
and one action that fixes it.

## Why it is two devices and not one

Because tapping an application does not silence it. On Windows and Linux, an application's audio
goes to the system's default output whether or not anyone is also capturing it. So Crucible has
to give the applications somewhere silent to play, and take a *different* device for itself.

That is also why Crucible moves your default output, and why it puts it back when it exits.

## How the stations differ per platform

The three stations are not the same shape everywhere, and the window says so rather than
pretending otherwise.

### Windows

Station 1 is the **"Desktop Atmos" driver**, a virtual device that discards its input. Crucible
offers to make it the system default, and restores your previous default on exit.

The driver is test-signed today, so it loads only on a machine with test signing on. Until an EV
certificate and attestation land, a normal machine has no silent device and Crucible tells you
so on the Settings page.

### Linux

Station 1 is a PipeWire node named **"Crucible (silent)"**, created by Crucible itself while it
runs and gone when it exits. Nothing is installed, nothing needs a password, and nothing is left
behind.

Crucible sets it as the default sink through PipeWire's `default.audio.sink` metadata — the same
thing `wpctl set-default` writes — and restores the previous default on exit.

### macOS

**There is no station 1.** macOS process taps can mute an application at the point they capture
it, so each application is silenced individually and no default output is moved at all. When the
macOS half exists, the window will show two stations rather than three, because the first one has
nothing to be.

## What the mode line means

Station 3's line names the mode Crucible chose and why:

| Mode | Chosen when | What your placements become |
|---|---|---|
| **Atmos** (E-AC-3 JOC) | an endpoint takes E-AC-3 exclusively **and** a signing key is loaded | objects, intact |
| **DD+ 5.1** | as above, but no signing key | panned onto the bed |
| **DD 5.1** (AC-3) | an endpoint takes AC-3 but not E-AC-3 | panned onto the bed |
| **PCM surround** | an endpoint offers 6 or 8 channels and no bitstream format | encoded, then decoded and rendered |
| **Headphones** | the endpoint has a spatial format enabled | objects handed to the OS renderer |
| **Stereo** | nothing above applies | encoded, then decoded, folded down |

Two of these need saying plainly.

**Without a signing key you get DD+ 5.1, not Atmos.** Placements still pan, but height does
nothing. That is a deliberate refusal rather than a degradation: an unsigned object container is
a hard error on a validating decoder.

**Headphones is Windows-only.** It needs an OS object renderer, and neither Linux nor macOS
exposes one a third party can hand Atmos objects to. On those platforms the headphone route
decodes and folds instead.

You can **pin** a mode to stop Crucible changing its mind, and **choose the endpoint** you hear
it on rather than letting the policy pick. An impossible pin leaves the automatic choice standing
and the reason line says so.

## The one rule that is not obvious

Probing an endpoint is safe. **Opening one is not.**

Crucible can ask any endpoint whether it accepts a format at any time, whatever else is playing.
But *taking* an endpoint exclusively while other applications are rendering to it is refused, and
invalidates their streams in the process — they stop playing, and the taps deliver silence from
then on.

So Crucible takes an output exclusively only after the default has moved to the silent device and
nothing is left on the endpoint it wants. If you see it decline to take your receiver, this is
usually why: something is still playing to it.
