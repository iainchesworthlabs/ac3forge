# Windows (Desktop Atmos Demo)

!!! warning "Status: plan, nothing built yet"
    This page is the design and phase plan for a Windows demo app that does not exist yet
    (roadmap UX11). It is written in the shape of [the Android page](android.md) so that, as
    phases land, each section is rewritten from "will" to "does" and the
    [verification section](#what-has-and-has-not-been-verified) fills in. Until then, every
    claim below about how Windows behaves is either a documented API contract or an open
    question that [Phase 0](#phase-0-spikes) exists to answer, and the text says which.

The Windows demo is a different animal from the Shield app. The Shield app plays one authored
stream and lets a controller move one object. The Windows app, **Desktop Atmos Demo**
(`apps/windows/`, working name), installs itself as the PC's output device the way FxSound does,
takes every application that is playing sound, and lets the user drag each one to a position in
the room. What comes out over HDMI is a live E-AC-3 JOC (Atmos) stream in which each application
is an object at the position it was dragged to. Chrome playing a video can be put up and behind
the seat, a game in the corner, a chat client at the left ear, and an AV receiver renders exactly
that. A full-screen application becomes the bed.

It is a joke app in the sense that nobody needs their spreadsheet to be overhead. It is a serious
demo in the sense that it exercises, live and in real time, the parts of the library the Shield
app does not: per-application capture, a dynamic object count, a headphone path, output
hot-switching, and the Windows exclusive-mode bitstream path that
[roadmap DR9](../roadmap.md) still lists as unconfirmed on real hardware.

## The user story

1. Install the app. Optionally install its virtual audio device, "Desktop Atmos Speakers".
2. Pick "Desktop Atmos Speakers" as the Windows default output, or let the app do it with one
   click. Every application now renders into a device that nobody hears.
3. The app's room view shows an icon for every application that is currently making sound,
   pulled from the same session list the Windows Volume Mixer uses. New applications appear
   when they start playing; they leave when they stop.
4. Drag an icon anywhere in the room, in plan and in elevation. That application is now a
   dynamic object at that position. Drag it back to the centre tray, or press reset, and it
   returns to the bed.
5. Whatever the app is not told to position, and whichever application is full-screen in the
   foreground, is mixed into the 5.1 bed.
6. The output follows the hardware. An Atmos-capable receiver over HDMI gets E-AC-3 JOC with the
   objects intact. A Dolby Digital receiver gets AC-3 5.1 with the positions panned onto the
   ring. A TV or PCM-only sink gets decoded multichannel PCM. Headphones get the decoded objects
   through Windows Spatial Sound. Plugging or unplugging HDMI switches modes without a restart.
7. Minimise it and it lives in the tray.

## What's reused, what's new

The point of writing this down first is that almost the whole pipeline exists. The table is the
inventory the plan is built on, with the header each item lives in.

| Piece | Status | Where |
|---|---|---|
| E-AC-3 JOC encoder, 5.1 bed plus up to 15 dynamic objects, count fixed at construction | exists | `ac3::oba::AtmosEncoder`, `src/forge/include/ac3/oba/atmos.hpp` |
| Live per-object placement surface the UI pushes into once per frame | exists | `ac3::oba::SceneCursor`, `src/forge/include/ac3/oba/scene.hpp`; the three-call pattern is `examples/osc_object_control.cpp` |
| 5.1 bed panner for the AC-3-only receiver case | exists | `ac3::spatial::BedRenderer`, shown in `examples/spatial_objects.cpp` |
| IEC 61937 burst framing for AC-3 and E-AC-3 | exists | `ac3::iec61937`, `src/forge/include/ac3/iec61937/iec61937.hpp` |
| Exclusive-mode HDMI/S/PDIF bitstream sink, with a per-device format probe | exists, unconfirmed on a real receiver on Windows | `ac3::audio::PassthroughSink`, `enumerate_render_devices()`, `src/audio/include/ac3/audio/passthrough.hpp` |
| Shared-mode multichannel PCM sink | exists | `ac3::audio::MonitorSink`, `monitor.hpp` |
| Windows Spatial Sound object sink (headphones) | exists, confirmed against a real spatial endpoint | `ac3::audio::SpatialObjectSink`, `spatial.hpp` |
| Whole-endpoint WASAPI loopback capture | exists | `ac3::audio::Capture` with `DeviceKind::kLoopback`, `src/audio/src/backend/windows/capture.cpp` |
| Decoder with §7.8 downmix for the honest headphone path | exists | `ac3::OutputStage`, `src/forge/include/ac3/decoder/output.hpp` |
| Object signing hook pattern | exists, on Android | `apps/android/app/src/main/cpp/shield_signing_hook.hpp` |
| Draggable room widget, plan plus elevation | exists, in the GUI's Live tab | `apps/gui/qml/Main.qml` (`liveRoom`), `SoundfieldView.qml` |
| Reference live encode loop | exists, twice | `apps/cli/commands/live_audio.cpp` (`run_live`), `apps/android/.../live_cursor.cpp` |
| **Per-process loopback capture** | **landed** (Phase 1) | `Capture::start_process_loopback`, Windows backend only, see [Library additions](#library-additions) |
| **Render-device arrival and removal notifications** | **landed** (Phase 1) | `ac3::audio::DeviceWatcher`, Windows backend only |
| **Audio session enumeration** (who is playing, PID, name, icon) | **new, app-level** | `apps/windows/` |
| **Virtual null-sink audio device** | **new, separate driver project** | `apps/windows/driver/` |
| **Output-mode state machine with hot switching** | **new, app-level** | `apps/windows/` |
| **Low-latency mode** | **new, configuration of existing knobs** | `AtmosConfig::numblkscod`, capture buffer size |
| Tray-resident Qt Quick UI | new | `apps/windows/` |

Nothing in `src/forge/` changes. The library additions are two Windows-backend files and their
`kNoBackend` twins in every other backend, per the
[platform-tree convention](raspberry-pi.md#why-theres-no-raspberry-pi-specific-code).

## Routing: two problems, not one

"Register as an output device and see every application's audio separately" is two separate
problems on Windows, and it helps to keep them apart because they have different solutions.

**Separation** is solved without a driver. Windows 11 (build 20348 and later) has process
loopback capture: `ActivateAudioInterfaceAsync` with
`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` opens a capture stream that carries only what one
process tree renders. One client per application, each on its own thread, each delivering that
application's contribution to the mix in the endpoint's mix format. The Volume Mixer's session
list (`IAudioSessionManager2` and friends) tells the app who is playing, with process ID,
display name and icon, and raises a notification when a session appears or changes state.
Granularity is the application, not the browser tab: Chrome renders all tabs from one utility
process.

**Silencing** is the problem the driver solves. If the app taps Chrome and re-emits it over
HDMI, Chrome also still plays out of whatever the default device is. Muting the session in the
mixer mutes the tap as well, because the tap sits after session volume in the engine (S1
confirmed it: the tap went silent within a second of the mute). Opening the default endpoint
in exclusive mode while other applications are rendering to it is worse than useless: S1 found
the open is refused with `AUDCLNT_E_DEVICE_IN_USE` and the applications' streams are invalidated
anyway, so they stop playing and the taps deliver silence from then on. The clean
answer is the one FxSound arrived at: a **virtual render endpoint that discards what it is
given**. Make it the system default and every application renders into a device nobody hears,
while the per-process taps pick each one off individually. The driver has no smarts at all. It
is a null sink whose only job is to exist and to advertise a format.

That format is the second thing the driver buys. It advertises **7.1 at 48 kHz**, so a game
that can render surround does, and its tap arrives as eight channels that drop straight into the
bed. Without the driver the taps arrive in whatever the user's real default device offers,
usually stereo.

### The driver-free mode

The app must work, in a degraded way, with no driver installed, because driver installation
is the highest-friction step and because CI will not have one. Driver-free mode means:

- the user picks any endpoint they cannot hear as the default (a monitor with no speakers, an
  idle virtual cable, a muted device), or accepts hearing the direct mix as well;
- taps still work, positions still work, every output mode still works;
- the app says plainly, in the UI, why the direct mix is still audible and what fixes it.

FxSound is installed on the development workstation. Its endpoint, "Speakers (FxSound Audio
Enhancer)", is a production-signed virtual render device that goes nowhere when the FxSound
application is not running. It is the development stand-in for our own driver until Phase 4.
S1 used it to prove the whole model: applications rendering to it were tapped identically to
applications on the real default, and with them there the workstation's Realtek endpoint could
be taken in exclusive mode (`S_OK`) without the taps noticing.

### Rerouting: explicit, like FxSound

The undocumented per-application routing interface that EarTrumpet uses
(`IAudioPolicyConfigFactory::SetPersistedDefaultAudioEndpoint`) is deliberately **not** used.
The app changes the system default output, with a confirmation, and restores the previous
default on exit. Windows has no documented API for that either: the route every
default-switcher utility takes is the well-known `IPolicyConfig::SetDefaultEndpoint`, stable
since Vista and used by FxSound, SoundSwitch and EarTrumpet alike. It is still unsupported, so
the app treats it as a convenience with a fallback: if the call is refused, it opens the Sound
settings page and asks the user to pick the device, and detects the change through the
`DeviceWatcher`. Per-application rerouting would let the app work alongside a normal default
device, but it is a second, less-travelled undocumented surface, and it is not what FxSound
does. It stays on the list as a possible later phase.

## Objects and the bed

The encoder's object count is fixed when it is constructed, so the app allocates **all 15
dynamic slots at start** and feeds silence to the idle ones, the same way the Shield app keeps
three objects alive for every scene. An application being dragged out of the bed takes the
lowest free slot; dragging it back frees the slot. Nothing is ever rebuilt mid-stream.

**The bed is five of those slots.** `AtmosEncoder` has no bed input of its own: its 5.1 bed is
rendered *from* the objects, and it knows nothing of OAMD's bed labels (those live in
`SceneObject::bed` for authoring and reach the wire, but the encoder's `ObjectPlacement` has
no such field). So the demo's bed is five objects pinned to the L, R, C, Ls and Rs speaker
positions with `snap` set, plus `lfe_send` for the LFE, and the bed mixer sums bed applications
into those five. That leaves **10 slots for positioned applications**, not 15. It is still
more than a desk needs, and it is honest about what the encoder does.

**Mono per application** is the default. Each tap's channels are folded to one signal
(a plain L+R sum for a stereo render; the §7.8 fold for a surround render) before it goes into
its slot. That gives up to 10 positioned applications alongside the five-slot bed.
The stretch, as a per-application user choice, is **split**: a stereo render becomes two
objects placed either side of the application's position, and a surround render becomes a
cluster. Split costs slots, so the slot allocator refuses when the budget would be exceeded and
the UI says so.

**What goes to the bed:**

- every application the user has not positioned;
- the foreground full-screen application, always, even if it was positioned, because a
  full-screen game that renders 7.1 is the bed. The UI shows why its icon snapped back.

Bed applications are mixed by the app, not by Windows, because the taps are per process and
there is no "everything except these" tap (the exclude mode takes one process tree, not a
list). A stereo bed application goes to the L and R bed slots; a 5.1 render maps one-to-one
onto the five plus the LFE send; a 7.1 render folds the side and rear pairs into Ls and Rs.
Total bed contributors are unbounded; total taps are one per playing session, and S1 ran
sixteen at once without strain.

Positions use the library's room coordinates (`ac3::oba::Position`): `x` from 0 at the left
wall to 1 at the right, `y` from 0 at the front wall to 1 at the back, `z` from -1 at the floor
to +1 at the ceiling. The seat is at the centre. The UI's plan view sets `x` and `y`; the
elevation view sets `z`. Moves are pushed through `SceneCursor::push()` once per frame, and a
drag is smoothed across a few frames because a 32 ms step in position is a click.

## Output modes and hot switching

| Mode | Chosen when | What positions become | What the sink is |
|---|---|---|---|
| **Atmos** (E-AC-3 JOC) | a render endpoint accepts E-AC-3 in exclusive mode and a signing key is present | objects, intact | `PassthroughSink`, E-AC-3 bursts |
| **DD+ 5.1** (E-AC-3, no object metadata) | as above, no signing key | panned onto the bed by `BedRenderer` | `PassthroughSink`, E-AC-3 bursts |
| **DD 5.1** (AC-3) | an endpoint accepts AC-3 but not E-AC-3 | panned onto the bed | `PassthroughSink`, AC-3 bursts |
| **PCM surround** | an endpoint offers 6 or 8 shared-mode channels and no bitstream format | encoded, then decoded and rendered | `MonitorSink`, 5.1 or 7.1 |
| **Headphones** | the default endpoint has a spatial format enabled (Windows Sonic, Dolby Atmos for Headphones, DTS Headphone:X) | encoded, then decoded, objects handed to the OS renderer at their OAMD positions | `SpatialObjectSink` |
| **Stereo** | nothing above applies | encoded, then decoded, Lo/Ro fold | `MonitorSink`, 2 channels |

Detection on Windows is a live probe, not an EDID read: `enumerate_render_devices()` asks each
endpoint whether it accepts AC-3 and E-AC-3 exclusive formats and how many shared-mode channels
it has, and `probe_spatial_capability()` asks whether a spatial format is on. Windows does not
expose EDID short audio descriptors to user mode, so `read_sink_capabilities()` keeps returning
`kNoBackend` there and the demo does not pretend otherwise.

Hot switching is driven by the new render-device notifications (`IMMNotificationClient`
underneath): a device arriving or leaving, or the default changing, re-runs the probe and, if
the best mode changed, fades the current sink down over one frame, stops it, starts the next,
and fades up. The encoder keeps running through the switch; only the sink changes. The user can
pin a mode to stop the app changing its mind.

One rule the output stage must hold, from S1: **the probe is safe, the open is not.**
`IsFormatSupported` in exclusive mode can be asked of any endpoint at any time, live streams or
not. `Initialize` in exclusive mode on an endpoint that other applications are rendering to is
refused and still invalidates their streams. So the app takes HDMI exclusively only after the
default has been moved to the null sink and the session list confirms nothing is left on HDMI.

**The headphone and PCM paths decode what was encoded.** It would be less work and lower latency
to hand the taps' PCM and positions straight to the spatial sink, but then those modes would
never exercise the codec, and the demo would be making a claim about the encoder it cannot back.
A "bypass codec" switch in settings exists precisely so the difference can be demonstrated.

### Low-latency mode

The normal chain will sit somewhere around 150 to 200 ms behind the picture once a receiver has
decoded it, which is a visible lip-sync error on a video. That is accepted for the joke, but a
user-selectable **low-latency mode** trades the following, all existing knobs:

| Knob | Normal | Low latency |
|---|---|---|
| E-AC-3 frame (`AtmosConfig::numblkscod`) | 6 blocks, 32 ms | 1 block, 5.3 ms |
| Capture buffer | 20 ms | 10 ms or the engine minimum |
| Sink pre-roll | two frames | one frame |
| Bitrate | 448 kb/s | about 1.5 Mb/s, because 15 objects' metadata has to fit every 5.3 ms (S4 found 640 kb/s refused at 1 block, 1536 kb/s fine) |

The encoder's own object latency (832 samples in the QMF domain, about 17 ms) and the
receiver's decode delay do not move. S4 showed the encoder itself is not the constraint in
either mode. Phase 5 measures both chains end to end with a loopback capture and writes the
numbers here.

## Object signing

The rule is the Shield app's, unchanged, and the reasons are in
[Object signing](../concepts/object-signing.md): an unsigned-but-present object container is a
hard refusal on a validating decoder, not a graceful fallback, so **with no key the app sets
`emit_object_metadata = false` and streams plain 5.1**. The mode table above shows this as the
difference between the Atmos and DD+ 5.1 rows.

On the desktop the key is resolved **at runtime**, from an explicit path in settings or the
same environment variables `ac3cli` reads (`AC3FORGE_SIGNING_KEY_FILE`, `AC3FORGE_SIGNING_KEY`),
never from a file next to the executable and never
from an installer. Nothing about the build changes with or without a key. The release job's
"no key in the package" assertion carries over from Android to whatever packages this app.

The user supplies the key once and the app remembers it across runs. What the settings store
persists is the **path** to the key file by default; optionally the key bytes themselves,
protected with DPAPI so only the same Windows user on the same machine can read them back,
for a user who would rather not keep a key file lying around. Plaintext key material never
lands in a settings file, a log, or a crash dump, and the settings screen shows only whether a
key is loaded and where it came from. Changing or clearing the key restarts the encoder,
because objects-or-nothing is decided at construction.

## UI

Qt Quick, module `Ac3ForgeDesk` (working name), sharing `Theme.qml` and the room widgets with
the GUI rather than copying them. Screens:

- **Room**: plan view and elevation view side by side, the same layout as the GUI's Live tab and
  the Shield dashboard. Every playing application is an icon with a level ring. Unpositioned
  icons sit in a tray at the bottom labelled "bed"; drag one in to position it, drag it back or
  right-click reset to return it. The foreground full-screen application shows with a lock.
- **Output**: what mode is active, which endpoint, why (the probe result in one line each), and
  a pin. The default-device switch lives here with its confirmation.
- **Settings**: low-latency mode, bypass codec, split per application, signing-key path,
  bitrate, driver install status with the install and remove buttons that shell out to
  `pnputil`.
- **Tray**: minimise to tray, tray menu with output mode, pin, and quit. Closing the window
  keeps the engine running; quitting restores the previous default device.

The 3D room view (Qt Quick 3D, app icons floating in a wireframe room) is a fast follow after
the 2D views work, not part of the first cut.

## Structure

```
apps/windows/
  CMakeLists.txt                  in-tree, behind option(AC3FORGE_BUILD_WINDEMO) and if(WIN32)
  engine/                         headless, namespace ac3::windemo, no Qt
    slots.{hpp,cpp}               the plan: 10 positioned + 5 bed slots, full-screen rule   [landed]
    bed_mixer.{hpp,cpp}           stereo/5.1/7.1 taps -> mono object or the 5 bed slots      [landed]
    placement.{hpp,cpp}           UI targets -> per-frame ObjectPlacement, smoothed          [landed]
    output_policy.{hpp,cpp}       probe facts + pin -> mode, endpoint, reason (the S1 rule)  [landed]
    tap_pool.{hpp,cpp}            one Capture::start_process_loopback per application  [landed]
    output_stage.{hpp,cpp}        probe, policy, sink ownership, the five routes        [landed]
    signing_hook.{hpp,cpp}        runtime key resolution, the Shield rule               [landed]
    engine.{hpp,cpp}              the frame loop and the UI's command/status surface    [landed]
    platform/windows/             the half that talks to Windows, this directory only
      session_monitor             IAudioSessionManager2 -> applications by process tree [landed]
      default_device              read/move/restore the default output (IPolicyConfig) [landed]
      foreground                  SHQueryUserNotificationState + foreground window      [landed]
  runner/main.cpp                 ac3windemo, the console runner (Phase 2's exit)       [landed]
  ui/                             ac3desk: main.cpp, desk_controller, qml/ (module Ac3ForgeDesk) [landed]
    qml/shared/                   generated at configure time from apps/gui/qml, git-ignored
  translations/                   ac3desk_{fr,de,es,ar,he,yi}.ts, the GUI's language set    [landed]
  spikes/                         Phase 0 programs, kept as documented experiments
  driver/                         the null-sink driver, separately licensed (see below)
tests/windemo/                    the pure modules' tests, on every CI leg, ungated
```

The pure modules compile into `ac3tests` on every platform, the way `apps/common` does, so
the rules the UI hangs on (the slot budget, what a full-screen application does, which output
wins when) hold on a Linux CI leg that could never run the demo. The `platform/windows/`
half builds only under the option.

The library gains, in `src/audio`:

- **`Capture::start_process_loopback(pid, mode, format)`**, implemented in
  `src/audio/src/backend/windows/capture.cpp` with the activation-params route, sharing the
  capture thread with the endpoint paths; every other backend refuses it with `kNoBackend` or
  `kProcessLoopbackUnavailable`, and `process_loopback_available()` answers for the machine
  rather than the build. **Landed 2026-09-03.**
- **`DeviceWatcher`** (`ac3/audio/device_watcher.hpp`), a new backend file in every
  directory, delivering default-changed, added, removed and state-changed events on a
  caller-supplied callback; non-Windows backends return `kNoBackend`. **Landed 2026-09-03.**
- Both are reported by `audio_backend()` as `process_loopback` and `device_watch`, and the
  backend contract test exercises both without touching a device (the watcher is actually
  started and stopped wherever the backend exists, since registering needs no endpoint).

Session enumeration, default-device switching and the foreground check are **app-level**, under
`apps/windows/engine/`, because they are demo policy rather than audio I/O and because one of
them touches policy-config COM interfaces that have no business in the library.

Android wraps the repo root from its own Gradle build because Gradle owns that build. Here the
toolchain is the same one the CLI and GUI use, so the demo is an in-tree option like the GUI,
defaulting OFF, with the same pre-seeded `OFF` list for the targets it does not need.

### The driver, and its licence

`apps/windows/driver/` is derived from Microsoft's Simple Audio Sample virtual audio driver
(the smaller sibling of SysVAD, same WaveRT machinery), cut down to one render endpoint that
discards its input and advertises 7.1 at 48 kHz. The sample is licensed
under the Microsoft Public License, which the FSF lists as free but **not GPL-compatible**. That
is fine here because the driver is a separate work: a kernel-mode binary that shares no code
with the GPL-3.0-or-later application and is only ever reached through public Windows APIs.
The `driver/` subtree carries its own `LICENSE` (MS-PL, plus the project's modifications under
the same terms) and its own `README` saying so, and nothing in it is `#include`d, linked, or
copied anywhere else in the repository. FxSound's driver is AGPL and is not used or consulted.

Signing has two stages, in order. First, **test-signed on the development workstation** with
test-signing mode on and memory integrity off, installed with `pnputil`. Second, an EV
code-signing certificate and a Partner Center account for **attestation signing**, so the
driver loads on other people's machines with memory integrity on. The second stage gates the
driver's presence in any published package; until then the package ships without it and the
docs explain how to build and test-sign it locally.

## Phases

Each phase ends with something that runs, and each has a written exit criterion. Prerequisites
that are the developer's to-do rather than code are listed where they bite.

### Phase 0: spikes

Throwaway console programs in `apps/windows/spikes/`, each answering one question, each
leaving its answer in this page. None of it is reused as code.

| Spike | Question | Exit |
|---|---|---|
| **S1 taps** | Does process loopback work against N processes at once, what format arrives, does it keep delivering when the session is muted, when the app is routed to the FxSound endpoint, and when the default endpoint is held exclusively by us? | **Done 2026-09-03**, results in `apps/windows/spikes/README.md`: 16 taps separate exactly at 48 kHz float, mute kills a tap, the FxSound null-sink model works, exclusive on another endpoint is fine, exclusive on the apps' own endpoint is refused and destructive, the probe alone is harmless |
| **S2 bitstream** | Does `PassthroughSink` in E-AC-3 and AC-3 exclusive mode lock on a real Atmos receiver from this workstation? This is DR9's Windows row. | the receiver's front panel reads DD+ Atmos, then DD, at zero underruns for a minute |
| **S3 headphones** | With Windows Sonic enabled on the Realtek endpoint, does encode then decode then `SpatialObjectSink` produce audible height and rear movement by ear? | yes or no, and the measured round-trip latency |
| **S4 throughput** | Does a 15-object `AtmosEncoder` plus 16 taps plus the bed mix hold 32 ms cadence on this machine with margin? | **Done 2026-09-03**, results in `apps/windows/spikes/README.md`: p99 1.8 ms of the 32 ms budget in normal mode; the 1-block frame p99 0.7 ms of 5.3 ms, but needs at least about 1.5 Mb/s to carry 15 objects' metadata (640 kb/s is refused) |

Prerequisites: a long HDMI cable to the receiver (S2, the developer's to-do); Windows Sonic
enabled on the headphone endpoint (S3). S3 and S4 do not wait for S2.

### Phase 1: library additions

The process-loopback capture entry point and the device watcher, both in the Windows backend
with `kNoBackend` twins elsewhere, the device-free logic (parameter validation, refusal paths,
watcher lifecycle) covered by the unconditional backend contract test rather than a new
`tests/backend/windows/` directory, and `docs/platforms/windows.md` updated. Exit: a program
tapping one process through the library and reading the tone back. **Done 2026-09-03**:
`s1_library_tap` in the spikes tree reads the spawned player's tone through
`start_process_loopback` at the expected level and frequency, refuses a process id nobody
owns, and starts and stops a `DeviceWatcher`.

### Phase 2: engine

`apps/windows/engine/`, headless. Session monitor, tap pool, slot allocator, bed mixer,
placement, output stage with the mode state machine, signing hook, and the frame loop modelled
on `run_live`. Everything that does not touch Windows is unit-tested on CTest; everything that
does is behind an interface the tests can fake. Exit: a console runner that takes positions on
stdin, streams Atmos to the receiver, and switches to headphones when HDMI is pulled.

**Progress, 2026-09-03:** the pure half landed first, with its tests (36 cases, every
platform): the slot plan with the full-screen rule and a waiting list for the eleventh
application, the bed mixer and mono fold with the channel maps above, placement smoothing, and
the output policy. The policy holds the S1 rule and one more the tests forced: shared-mode
output on the endpoint applications render to (hearing the direct mix alongside ours) is
chosen only when no other endpoint can carry anything at all. The Windows half and the frame
loop are next.

**Progress, later the same day:** the rest of the engine and the console runner, `ac3windemo`.
The session monitor groups sessions into applications by process tree (Chrome's audio utility
process rolls up into chrome.exe) and leaves the engine's own process out, because a monitor
sink's output is itself a session and the first smoke run tapped it. The tap pool keeps one
process-loopback capture per application; the output stage owns whichever sink the policy
chose and routes each access unit down one of five paths (E-AC-3 bursts, an AC-3 leg encoded
from the encoder's own bed, decoded 5.1 PCM, decoded objects to Windows Spatial Sound, decoded
Lo/Ro); the signing hook applies the Shield rule; the frame loop paces on the taps, folds each
application into its slot, encodes, signs and submits, and republishes a status snapshot the
UI will read. On the workstation, with a tone player rendering to the idle FxSound endpoint:
the player was listed, tapped, moved into slot 0 on a `pos` command and back to the bed on
`bed`, the stream ran at frame cadence with no starved reads and no underruns, and the
decoded stereo fold played to a non-default endpoint. Headphone mode is a with-key mode (no
key means no object container, so nothing for the spatial renderer to place). Not yet
exercised: any bitstream mode (no receiver, and this workstation's HDMI endpoint accepts
neither format), the spatial path (Windows Sonic is off here), and a real device-arrival
switch. Fades on a mode switch are not written; the switch is a stop and a start.

### Phase 3: UI

The Qt Quick shell, the room in plan and elevation, the bed tray, the output screen, settings,
and tray residency. Exit: the user story above, minus the driver, works end to end.

**Progress, 2026-09-03:** built to the design canvas after a human-factors pass on it.
`ac3desk` (`apps/windows/ui/`, QML module `Ac3ForgeDesk`) has the three pages, the tray icon
with its output submenu and default-output switch, the status strip, and the `--shot`
capture aid borrowed from the GUI's smoke modes. The GUI's Theme, Card, RailBlock and
SegmentedControl are not copied: the demo's CMake rewrites each one's module import at
configure time into an ignored directory and registers the result, so an edit to
`apps/gui/qml/Theme.qml` reaches both apps. `DeskController` is a QML singleton that polls the
engine's status snapshot a few times a second and republishes it as properties, and persists
settings through QSettings under the GUI's organisation. Localisation shares the GUI's
`LanguageManager` (given a translation basename so a second app can use it, and a way back to
following the system locale) and the same six languages, mechanically translated for now, with
Windows' locale as the default and a chooser in Settings. Verified by `--shot` captures in
English, French, Arabic and Hebrew: layout, mirroring and the translations all render; the
engine runs underneath and the room reflects the live session list. Not yet done: real
Windows icons (monograms stand in), the 3D room, split-per-application, and the codec-bypass
switch, which is shown disabled until the engine has the path.

### Phase 4: driver

The SysVAD-derived null sink, test-signed, `pnputil` install and remove from the settings
screen, driver-free mode still working when it is absent. Prerequisite: the Windows Driver Kit
installed on `D:`. Exit: "Desktop Atmos Speakers" appears in Sound settings, becomes the
default from the app, and the direct mix is silent.

**Progress, 2026-09-03:** built, not yet installed. The base turned out to be Microsoft's
*Simple Audio Sample* rather than SysVAD: the same WaveRT virtual-device machinery at a fifth
of the size. `apps/windows/driver/` carries it under its own MS-PL `LICENSE` with a `README`
that lists every cut: the mic-array endpoint, the tone generator and the file-saving path are
gone, the speaker is 7.1 at 48 kHz, rendered data is discarded while the DMA position still
advances at the nominal rate, and the INF is rewritten for one endpoint under the hardware id
`ROOT\Ac3ForgeNullSink` with device description "Desktop Atmos", so Windows shows the endpoint
as "Speakers (Desktop Atmos)", which is what the demo's default null-sink name matches. It
builds under the Enterprise WDK (kit 10.0.28000, mounted from an ISO on `D:`, nothing installed
on `C:`) to a test-signed package: `inf2cat`'s signability test passes, `infverif` reports the
INF valid, and the build generates its own WDK test certificate. `install.ps1` and `remove.ps1`
stage the package with `pnputil` and create or remove the root-enumerated device with the
WDK's `devcon`. Loading it needs test signing on and memory integrity off, both reboots and
both the developer's to do; until then the FxSound endpoint remains the stand-in. The
Settings screen's install and remove buttons are not wired yet; the scripts are the path.

### Phase 5: fast follows

In this order: low-latency mode with measured numbers, split per application, the 3D room,
and per-application width and size. Each is its own PR.

### Phase 6: docs, CI, release

This page rewritten from plan to record; a roadmap record; a CHANGELOG entry; the demo built
(driver excluded) on the self-hosted Windows runners; the WDK on the runners and the driver in
CI only once it builds locally without surprises; the EV certificate and attestation step as
the last item, after which the driver can join the package.

## What has and has not been verified

!!! note "S1 verified on the development workstation, 2026-09-03"
    Process-loopback capture behaves as the plan needs: sixteen concurrent taps each read only
    their own process, at 48 kHz float, first packet within 20 ms; a tap opened before the
    process starts playing picks it up; applications rendering to the idle FxSound virtual
    endpoint are tapped identically; and with them there, the Realtek endpoint could be taken
    in exclusive mode without disturbing the taps. Two hazards confirmed: session mute silences
    the tap, and an exclusive open on an endpoint with live shared streams is refused *and*
    kills those streams. The full table is in `apps/windows/spikes/README.md`.

!!! note "S4 verified on the development workstation, 2026-09-03"
    Fifteen objects, sixteen taps folded and mixed, encode and burst-wrap: p99 1.8 ms of the
    32 ms frame in normal mode, 0.7 ms of the 5.3 ms one-block frame. The one-block frame
    refuses below roughly 1.5 Mb/s because the object metadata no longer fits. Same file.

Everything else is still a plan. When S2 runs, the "Windows/WASAPI exclusive: unconfirmed"
line in roadmap DR9 and the warning in [Windows](windows.md) are the first two things to change.

## Open questions this plan does not settle

- How the app confirms every session has left HDMI before it opens HDMI exclusively, given
  that a premature open kills the streams still there. The session list per endpoint is the
  obvious answer; whether it updates fast enough after a default-device switch is not yet known.
- Whether the foreground full-screen check should use the shell's full-screen notification
  (`SHQueryUserNotificationState`) or a window-rect comparison. Either is app-level; the
  first is cheaper and the plan starts there.
- The app's real name. "Desktop Atmos Demo" and "Desktop Atmos Speakers" are placeholders.

## Deliberately not in scope

- Per-tab or per-stream separation inside one application.
- An in-repo HRTF or binaural renderer. The roadmap rules it out; the OS renders headphones.
- Per-application rerouting through undocumented policy interfaces.
- The Shield extras: scenes and tour, phone remote, path record, rumble. None of them earn
  their place on a desk.
- Store distribution. The driver cannot ship through MSIX anyway.
