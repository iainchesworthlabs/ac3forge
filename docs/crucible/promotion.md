# Promoting the demo to AC3Forge Crucible

!!! note "Status: plan, written 2026-09-04"
    This page plans the promotion of the Windows Desktop Atmos Demo
    ([`docs/platforms/windows-demo.md`](../platforms/windows-demo.md), roadmap UX11) from a
    Windows-only demo into **AC3Forge Crucible**, a desktop product on Windows, Linux and
    macOS. It keeps the shape the demo page kept from
    [the Android page](../platforms/android.md): the design sections say what changes and why,
    each phase carries an exit criterion and will carry its progress record, and
    [What cannot be verified](#what-cannot-be-verified-and-why) says plainly which claims this
    work will not be able to make. Roadmap item **UX12**.

The demo works. It taps every application playing on a Windows PC, lets each be dragged to a
position in a room, and streams the result as live E-AC-3 JOC to a receiver. What it is not
yet is a product: it is named after a trademark it does not own, it exists only on Windows, it
is documented as a footnote under Platform notes, and half its engine is welded to
`platform/windows/` headers. This page is the work that closes each of those.

## The name

The app becomes **AC3Forge Crucible**. Binary `ac3crucible`, namespace `ac3::crucible`, CMake
option `AC3FORGE_BUILD_CRUCIBLE`, ctest label `crucible`, virtual device "Crucible" in the
system sound picker.

Two reasons, in order of weight.

**The current name is a trademark it does not own.** "Desktop Atmos Demo" and "Desktop Atmos
Speakers" use Dolby's registered trademark as the name of a product and of a system-wide audio
*device*. Describing a stream as Dolby Atmos where that is what it factually is remains correct
and stays. Naming the application and the endpoint with it does not survive the move from demo
to product, and the endpoint name is the sharper problem of the two, because it is what appears
in every user's sound settings. The demo page already lists the name as an open question; this
settles it before the driver's attestation signing is paid for, since the device name is inside
the package that gets signed.

**A crucible is where separate materials are combined under heat into one melt**, which is what
the application does to the sounds on a desk. It sits in the forge metaphor the project already
uses, it is distinctive enough to find instantly in a device list, and it names the place the
work happens rather than the result.

What the rename touches is [Phase 1](#phase-1-identity).

## What promotion requires

The gap between what exists and a product, in the order the phases take them.

| Area | Where it stands | What promotion needs |
|---|---|---|
| Identity | `ac3desk`, `ac3::windemo`, "Desktop Atmos Speakers" | one name, applied to namespace, targets, packages, translations, device and docs |
| Engine portability | `AudioDevices` seam done; four couplings to `platform/windows/` remain | four more seams, and a `platform/<os>/` tree |
| Library backends | `process_loopback` and `device_watch` on Windows only | PipeWire and CoreAudio implementations; a corrected capability report |
| The silent device | Windows driver, test-signed | Linux null sink (no driver); macOS tap mute (no driver); Windows attestation signing |
| Product qualities | a demo's first-run, no diagnostics, mechanical translations | first run, log export, settings migration, accessibility, licence notices |
| Docs | one 89 KB design record under Platform notes | its own docs section with a user guide, per-platform install and troubleshooting |
| CI and packaging | built, tested and packaged on Windows | the same on three platforms |
| Verification | Windows workstation and a throwaway guest | a hardware matrix, and honesty where there is none |

## Platform feasibility

The application needs four things from an operating system. It is worth setting them out
separately, because the platforms fail and succeed at different ones, and because two of the
four turn out to need no driver anywhere except Windows.

1. **Enumerate** which applications are playing, with a name and an icon.
2. **Tap** each one separately, without the others.
3. **Silence** their direct output, so the only thing heard is what this application emits.
4. **Bitstream** the encoded result to a receiver in a format it will accept.

| | Windows | Linux (PipeWire) | macOS | Android | WASM |
|---|---|---|---|---|---|
| Enumerate | `IAudioSessionManager2` | PipeWire registry | `kAudioHardwarePropertyProcessObjectList` | partial | no |
| Tap | process loopback, build 20348+ | link to the app's node | `AudioHardwareCreateProcessTap` | opt-in only | no |
| Silence | **null-sink driver** | **null sink module, no driver** | **tap mute, no driver** | not possible | no |
| Bitstream | WASAPI exclusive | ALSA `iec958` (confirmed) / PipeWire | CoreAudio | confirmed on Shield | no |
| Verdict | **done** | **all four, cheapest** | **all four, unverifiable** | premise breaks at 3 | premise breaks at 1 |

**Linux is the cheapest platform and the only fully verifiable one.** PipeWire's registry names
every stream with its application name and process id; a capture stream links to an
application's output node the way `pw-record --target` and OBS's per-application capture do;
and the silent device is `support.null-audio-sink`, a module load rather than a kernel driver,
so the entire signing problem that gates Windows does not exist. Passthrough over ALSA `iec958`
is the one part of this project already confirmed against a real Atmos receiver
([Raspberry Pi](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver), 2026-08-20).

**macOS needs no driver either, which was not obvious.** `CATapDescription` carries a
`muteBehavior`, and `.mutedWhenTapped` routes a process's audio through the tap instead of to
the output device. That is the same job the Windows null-sink driver exists to do, done by the
capture API itself. So macOS needs no kernel extension, no AudioServerPlugIn and no
BlackHole-style HAL plugin — a large cost that a first reading of the problem would have
assumed. What macOS does still need is in
[What cannot be verified](#what-cannot-be-verified-and-why), and none of it is a driver.

**Android and WASM are out.** Android breaks at step 3: an Android device cannot be made the
system output for other applications, and `AudioPlaybackCapture` only reaches applications that
opted in. A browser breaks at step 1. Neither is a reduced version of this application; both
would be different applications, and the Shield app already occupies the Android side of the
project. They stay out of scope, recorded in [Not in scope](#deliberately-not-in-scope).

## What is already portable

The demo was built with the seam in it, which is why this is a promotion rather than a rewrite.

`apps/windows/engine/audio_devices.hpp` declares an abstract factory — `render_devices()` plus
`BurstSink`, `PcmSink`, `ObjectSink` and `TapSource` — and the production implementation
(`platform/windows/wasapi_devices.cpp`, 146 lines) forwards to `ac3::audio`. The fakes in
`tests/windemo/fake_devices.hpp` script endpoints and record submissions, so the frame loop,
the five output routes, the bypass fold and a mid-stream mode switch already run in a plain
Catch2 process on a Linux CI leg that has no audio hardware. Seventy-two cases run there today,
with five more driving the window itself where Qt is present.

So the Linux and macOS work is: implement that same factory over PipeWire and over CoreAudio.
The engine above it does not change.

Four couplings remain, and they are the whole of [Phase 2](#phase-2-the-seams):

| Coupled file | Includes | What it needs |
|---|---|---|
| `engine/engine.cpp` | `platform/windows/session_monitor.hpp`, `foreground.hpp` | a `SessionMonitor` and a `Foreground` interface |
| `runner/main.cpp` | `platform/windows/default_device.hpp` | a `DefaultDevice` interface |
| `ui/desk_controller.cpp` | `default_device.hpp`, `driver_tools.hpp` | both of the above |
| `ui/desk_controller.hpp` | `driver_tools.hpp` | a `VirtualDevice` interface, in a **header** |

The last is the one that bites: a Windows-only type in a UI controller's header means the
controller cannot compile anywhere else, so it takes the interface first.

## The silent device, per platform

The single largest difference between the platforms, and the reason Windows was the hard one to
do first rather than the easy one.

**Windows** keeps the `Ac3ForgeNullSink` ACX driver
([its own page](../platforms/windows-driver-acx.md)). It is built, test-signed and Code-Analysed
in CI, and verified in a throwaway guest. It loads only where test signing is on until an EV
certificate and attestation submission exist. Nothing in this plan changes that; the driver's
device name changes with the rename, which is [a coordination
point](#coordination-with-the-driver-signing-session).

**Linux** loads a null sink. No driver, no signing, no elevation: a PipeWire
`support.null-audio-sink` node the application creates and tears down, or an existing one the
user points it at. This removes, on Linux, every step that makes the Windows path
high-friction. The application still needs the default sink moved to it, which is the
`default.audio.sink` metadata key.

**macOS** uses `muteBehavior = .mutedWhenTapped` on the process tap, so there is no separate
device at all: each tapped application is silenced at the point it is tapped, individually,
which is a cleaner model than either of the other two. The application never becomes the
default output and never moves the user's default device, so the whole "restore the previous
default on exit" concern disappears on macOS.

One consequence worth stating: **the three platforms silence differently enough that the UI's
signal path must say so.** The demo's three-station path ("applications play to", "Crucible",
"you hear it on") is a Windows story. On macOS the first station is per-application, and the
component needs to render that, not a device name. That is a UI change, not just a backend one,
and it is in [Phase 4](#phase-4-linux-platform-half) and [Phase 5](#phase-5-macos).

## Library additions

`ac3::audio`'s capability report is the contract, and one of its strings is currently wrong.
`process_loopback`'s reason on every non-Windows backend reads "no other backend has an
equivalent". PipeWire has had one for years and macOS has had one since 14.2, so that text
became false and is corrected as part of this work whatever else lands.

| Capability | windows | pipewire | alsa | macos | android | posix |
|---|---|---|---|---|---|---|
| `capture` | yes | yes | yes | yes | no | no |
| `passthrough` | yes | yes | yes | yes | yes | no |
| `monitor` | yes | yes | yes | yes | yes | no |
| `spatial` | yes | **no, and no plan** | no | **no, and no plan** | no | no |
| `process_loopback` | yes | **Phase 3** | no, no per-app concept | **Phase 5** | no | no |
| `device_watch` | yes | **Phase 3** | no, would be a udev listener | **Phase 5** | no | no |

### ALSA or PipeWire

`ac3audio` compiles **exactly one** backend, and on Linux that is ALSA whenever both sets of
headers are present — which is most desktops, and the Raspberry Pi this project verifies on.
That default is right for the library, whose discriminating feature is passthrough: ALSA
expresses the IEC 60958 non-audio bit as arguments on a device name and works the moment the
hardware does, while a PipeWire sink only offers a compressed codec once WirePlumber's
`iec958Codecs` has been populated, which the library cannot do on a caller's behalf
([Why ALSA still comes first](../building.md#why-alsa-still-comes-first)).

It is the wrong default for **Crucible**, and not by a little. The application's whole premise is
tapping each application separately, and ALSA has no per-application concept — its
`process_loopback` is a flat no rather than a gap waiting to be filled. So Crucible on Linux is
a PipeWire build or it is nothing.

Two consequences follow, and both are uncomfortable enough to be worth stating rather than
discovering.

**A default build would be silently broken.** The Linux platform half talks to PipeWire directly,
independently of which backend the library chose, so on an ALSA build the application would still
enumerate applications, still draw them in the room, still move the default device — and never
capture a single one of them, because `Capture::start_process_loopback()` would refuse. That is a
worse failure than not building at all, so `apps/crucible/CMakeLists.txt` makes it a configure
error naming the two flags that fix it.

**Crucible on Linux inherits the passthrough path that is *not* confirmed.** The Pi's August run
against a real Atmos receiver — every stream shape locked and identified, zero underruns — was
ALSA. Forcing PipeWire trades that for a path this project has never once seen work against
hardware, and which additionally needs a WirePlumber codec rule the user has to supply. So
DR9's PipeWire row is not a gap adjacent to this work; it is **on Crucible's critical path on
Linux**, and the first hardware run has to answer it. Until it does, the honest position is that
Crucible can tap applications on Linux and may have nowhere to send the result.

`spatial` is the one capability with no cross-platform answer. `SpatialObjectSink` wraps
Windows' `ISpatialAudioObjectRenderStream`; neither Linux nor macOS exposes an OS object
renderer a third party can hand Atmos objects to. On those platforms the headphone route
decodes and folds instead, and the mode table loses its Headphones row. The roadmap already
rules an in-repo binaural renderer out of scope and this plan does not reopen it.

## Phases

Each ends with something that runs and a written exit criterion, in the order they unblock each
other. Phases 1 to 4 are the overnight tranche; 5 onward are recorded here so the shape is
complete.

### Phase 1: identity

The rename, mechanically and completely: `apps/windows/` to `apps/crucible/`, `ac3::windemo` to
`ac3::crucible`, `ac3desk` to `ac3crucible`, `ac3windemo` to the same binary's console mode,
`tests/windemo/` to `tests/crucible/`, `AC3FORGE_BUILD_WINDEMO` to `AC3FORGE_BUILD_CRUCIBLE`,
the `desk`/`windemo` ctest labels to `crucible`, `ac3desk_*.ts` to `ac3crucible_*.ts`, the CPack
component and its archive name, and `tools/ci/check_windemo_package.py` with them.

**Exit:** all cases green under the new labels; the package check passes against a local
`cpack`; no occurrence of `windemo`, `ac3desk` or "Desktop Atmos" outside the CHANGELOG, the
records, and the driver subtree. The driver's own naming is held back — see
[Coordination](#coordination-with-the-driver-signing-session).

!!! success "Done 2026-09-04"
    The tree moved (`apps/windows/{engine,runner,ui,translations,spikes}` to `apps/crucible/`,
    `tests/windemo/` to `tests/crucible/`), the namespace with it (`ac3::windemo` to
    `ac3::crucible`, and the window's own `ac3::desk` to `ac3::crucible::ui`), the binaries
    became `ac3crucible` and `ac3crucible-run`, and the option, CPack component, archive name,
    coverage script, CI matrix flag and ctest labels (`windemo` and `desk` to `crucible` and
    `crucible-ui`) followed. Baseline before: 72 `windemo` plus 5 `desk`, full suite green.
    After: 72 `crucible` plus 5 `crucible-ui`, full suite green in 134 s.

    Three things the rename turned up. **The settings store is not where the application name
    said it was**: the window sets `setApplicationName("Desktop Atmos")` but the controller
    builds its `QSettings` with the four-argument constructor and the application name
    `DesktopAtmos`, no space — so a migration keyed to the displayed name would have quietly
    lost every signing-key path and endpoint choice. The migration in `ui/main.cpp` uses the
    four-argument form for the same reason the controller does, which also makes it a no-op
    under the QML tests' INI isolation. **The About box credited the wrong sample**: it named
    Microsoft's Simple Audio Sample, true of the PortCls miniport but not of the ACX driver
    that replaced it on 2026-09-04; corrected to the AudioCodec ACX sample, which is what the
    driver's own README and version resource say. And **`.github/` was invisible to the first
    sweep**, because the exclusion list held `.git` and `'.github'.startswith('.git')`; the
    workflow was rewritten in a second pass.

### Phase 2: the seams

Extract `SessionMonitor`, `Foreground`, `DefaultDevice` and `VirtualDevice` as interfaces beside
`AudioDevices`, move the Windows implementations behind them, and add fakes for each to
`tests/crucible/`. `platform_services.hpp` gains one factory per seam, one
`platform/<os>/` definition each, and `wasapi_devices()` is renamed
`platform_audio_devices()` with it, so nothing above the seam names an operating system.

The fourth seam is a bigger change than the other three and is taken separately, as **Phase 2b**.
`SessionMonitor`, `Foreground` and `DefaultDevice` are pure relocations: the Windows code moves
behind an interface and the callers dereference a pointer instead of calling a free function,
with no change to what anything displays. `VirtualDevice` is not. The UI currently shows
`testSigningOn`, `memoryIntegrityOn` and `codeIntegrityKnown` as properties of their own, and
those are facts about a Windows kernel that have no counterpart on either other platform — Linux
loads a module and macOS has no silent device at all. Generalising the seam therefore means
changing what the Settings page shows, not merely where it comes from: the three booleans
collapse into `SilentDeviceState`'s `blocker` and `detail` text, and the QML and its tests move
with them.

**Exit (2a):** `engine.cpp` and `runner/main.cpp` include no `platform/<os>/` header; the engine
and its tests build and run on a Linux CI leg; the three new fakes are exercised by at least one
case each.

**Exit (2b):** the UI controller's *header* includes no `platform/<os>/` header, which is the
coupling that stops it compiling anywhere else; the Settings page reads `SilentDeviceState`
rather than three Windows booleans; the QML tests move with them.

!!! success "Done 2026-09-04"
    All four seams are in. Nothing in `apps/crucible/` above `engine/platform/` names an
    operating system: `platform_services.hpp` hands back an `AudioDevices`, a `SessionMonitor`,
    a `Foreground`, a `DefaultDevice` and a `VirtualDevice`, one `platform/<os>/` definition
    each, and `tests/crucible/platform_services_stub.cpp` answers inertly where no platform half
    is built.

    2b was the one that changed the window rather than only its plumbing. `SettingsPage.qml` had
    been composing the Windows advice itself out of `testSigningOn`, `memoryIntegrityOn` and
    `codeIntegrityKnown` — "turn test signing on (`bcdedit /set testsigning on`, then restart)
    and turn memory integrity off ..." — a sentence with no counterpart on either other
    platform. It is composed in `WindowsVirtualDevice` now, where those facts are read, and the
    view prints `silentDeviceBlocker`. A platform with nothing in the way sends an empty
    blocker; one that needs no silent device at all sends `needed = false` and the Settings
    block hides itself, which is what macOS will do.

    Two smaller decisions. `SilentDeviceQuery` carries the two facts that come from
    `DefaultDevice` — whether a matching endpoint exists and whether it is the default — rather
    than having `VirtualDevice` enumerate endpoints a second time. And `set_package_dir()`
    defaults to a no-op, so the two platforms that do not install from a built package need not
    implement it.

    Full suite green: 82 `crucible` cases, and all five `crucible-ui` QML suites, which are the
    ones this phase put at risk.

### Phase 3: library, Linux

`Capture::start_process_loopback` and `DeviceWatcher` in the PipeWire backend; `DeviceWatcher`
in the ALSA backend where it is expressible; the corrected capability strings everywhere.

The mechanism, checked against PipeWire's own documentation rather than assumed: a capture is a
`pw_stream` in `PW_DIRECTION_INPUT` whose properties carry `PW_KEY_TARGET_OBJECT` set to the
target node's `PW_KEY_OBJECT_SERIAL` (or its `PW_KEY_NODE_NAME`), connected with
`PW_STREAM_FLAG_AUTOCONNECT`. Targeting an application's own output node is the per-application
tap; `PW_KEY_STREAM_CAPTURE_SINK` set to `"true"` is the whole-sink monitor variant, which is
the ordinary loopback this backend already wants elsewhere. There is a shipped reference for
the per-application case — OBS's `obs-pipewire-audio-capture` plugin does exactly this — so the
question for this phase is fitting it to `Capture`'s existing shape, not whether it can be done.

**Exit:** `audio_backend()` reports `process_loopback` and `device_watch` on PipeWire; the
backend contract test exercises both without touching a device, as it does on Windows; builds
clean in the WSL2 loop with `-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON`.

!!! success "Done 2026-09-05"
    Both landed. `Capture::start_process_loopback()` walks the registry for the
    `Stream/Output/Audio` node whose `application.process.id` matches — a value the daemon fills
    in from the client's credentials, so it is the kernel's answer and not the client's claim —
    and links a capture stream to it through `PW_KEY_TARGET_OBJECT`. `DeviceWatcher` turns the
    registry's `global`/`global_remove` into added and removed, keeping an id-to-device-id map
    because `global_remove` carries only the number; the default changing is not a registry
    event at all, so it binds PipeWire's `default` metadata object and listens to its `property`
    event, which is where `default.audio.sink` lives and how `wpctl` reads it.

    Two refusals are deliberate rather than unfinished. `kExcludeProcessTree` has no counterpart
    here: PipeWire links a stream to a target, and assembling "everything except this one" out
    of the rest of the graph would mean following every node that came and went for the life of
    the tap. And a process that owns no audio stream is `kProcessNotFound`, not a tap that
    delivers silence for ever, which is what a stream linked to nothing does.

    **Both capabilities are the machine's answer, not the build's**, and the contract test is
    what forced that. It requires `audio_backend().process_loopback.available` to equal
    `process_loopback_available()` on every platform. A hardcoded `true` would have disagreed on
    any container or CI runner, where the library is built against libpipewire but no session
    daemon is running — so the PipeWire table is computed once at first use, the way the Windows
    one already is for its build-number check. The contract's device-watch case also gained a
    second refusal: a backend can be unavailable because it was never built, or because the
    session it needs is not running, and those are different facts.

    One claim was corrected wherever it appeared: every non-Windows backend said per-process
    loopback had "no other backend has an equivalent", which stopped being true when PipeWire
    gained per-node capture and macOS shipped Core Audio process taps in 14.2.

    `Capture::start()` and the new tap now share `Impl::connect_stream()`; only the target and
    the format differed.

    Verified by compiling and running on Linux, in the WSL2 Ubuntu 26.04 loop against
    libpipewire-0.3 1.6.2 and GCC 15.2, with `-DAC3FORGE_WITH_ALSA=OFF
    -DAC3FORGE_WITH_PIPEWIRE=ON`. **What that does not establish**: WSL2 has no PipeWire session,
    so every path here took its "no session" arm. A tap actually delivering one application's
    audio, and a watcher actually reporting a default change, wait on a desktop Linux session —
    the Pi is the machine for it, and it is the same run DR9's PipeWire row needs.

### Phase 4: Linux platform half

`platform/linux/`: `pipewire_devices()` behind `AudioDevices`; the session monitor over the
PipeWire registry, where a stream node already carries `application.name` and
`application.process.id`, so the process-tree walk Windows needs has no counterpart here; the
default-sink move and restore through the `default.audio.sink` metadata key; null-sink create
and tear-down; and application icons from `.desktop` entries and the icon theme. The foreground full-screen check is X11-only and refuses
cleanly under Wayland, which has no way for one client to ask about another's windows — a real
gap, stated in the UI rather than worked around.

**Exit:** `ac3crucible` builds and starts on Linux; the console runner taps two applications,
places one, and encodes; the signal path renders with the null sink as the default.

!!! note "Started 2026-09-05: the AudioDevices half is done, the four seams are not"
    Phase 4 turned out to be smaller than this plan assumed, because one of the five services
    was never platform-specific in the first place. `platform/windows/wasapi_devices.cpp` named
    no Windows API at all: `PassthroughSink`, `MonitorSink`, `SpatialObjectSink` and `Capture`
    are the library's own cross-platform classes, and `enumerate_render_devices()` and
    `probe_spatial_capability()` answer for whichever backend was built. So it moves up to
    `engine/library_devices.cpp` and every platform gets it, rather than each writing a twin.
    A platform that cannot do one of these gets the refusal from the library: a Linux build has
    no spatial backend, so the object sink fails to start and the output policy never chooses
    the headphone route.

    **What Phase 4 still needs**, all four under `platform/linux/`, and each with a decision in
    it rather than only work:

    - **`SessionMonitor`** — a registry walk for `Stream/Output/Audio` nodes, which
      `pipewire_support.hpp` now has the helpers for (`is_output_stream`, `node_process_id`,
      `node_application_name`). The closest to ready. Windows groups sessions by process tree
      because a browser's audio comes from a utility process; PipeWire tags each stream with the
      client's own pid, so that walk has no counterpart here and the grouping question needs
      re-answering rather than porting.
    - **`DefaultDevice`** — endpoints come from the library's own
      `enumerate_render_devices()`; reading and writing the default is the `default.audio.sink`
      metadata key, which means a metadata write from the application rather than a read-only
      registry walk.
    - **`Foreground`** — X11 can answer through `_NET_WM_STATE_FULLSCREEN`, at the cost of a
      libX11 dependency; Wayland cannot answer at all. The `ForegroundSupport` reason field
      exists for exactly this, so the honest first cut refuses on both and X11 is a follow-up.
    - **`VirtualDevice`** — loading and unloading a `support.null-audio-sink` module.
      No driver, no signing, no elevation, but it is a module load from inside the application
      rather than anything the library already does.

    The application also gains a direct PipeWire dependency on Linux for three of those four,
    which is the same shape Windows already has: the app talks to COM and WASAPI itself for
    session enumeration and the default device, because those are demo policy rather than audio
    I/O. Consistent, but worth naming, since the library otherwise keeps PipeWire entirely to
    itself.

!!! success "Done 2026-09-05: the engine runs on Linux"
    All four seams are written, `apps/crucible` builds on Linux, and `ac3crucible-run` starts,
    runs its frame loop at the 32 ms cadence and answers `status` and `list`. On a machine with
    no PipeWire session everything degrades by saying so: "no render endpoint can carry any
    mode", no signing key, no applications. The root guard is now
    `WIN32 OR (UNIX AND NOT APPLE)`; macOS is excluded until Phase 5 gives it a platform half,
    rather than being allowed to configure and then fail to link.

    Two decisions were taken rather than deferred, both stated in the code:

    **The full-screen check does not take libX11.** Wayland gives a client no way to ask which
    window is full-screen — that is its security model, not a gap, and no portal exposes it — so
    X11 would be a dependency for a rule that half of Linux desktops can never honour. The Linux
    `Foreground` refuses on both and says which of the two reasons applies. Reporting "nothing is
    full-screen" instead would have been a different and wrong claim: the engine would quietly
    stop pinning a full-screen game to the bed and nobody would be told why. X11 stays a purely
    additive follow-up, which is what `ForegroundSupport` exists to allow.

!!! success "Done 2026-09-05: X11 full-screen detection"
    The follow-up the previous block left open is taken. The Linux `Foreground` answers under
    X11 and refuses, with the reason, everywhere else; the window, the runner and the probe
    now print that reason instead of only the docs saying it. Four decisions, each stated in
    the code:

    **libxcb, linked directly through pkg-config, optional.** `ac3crucible_engine` has no Qt,
    and the runner and the probe link it, so Qt's native interface was never reachable from
    where `Foreground` lives; and even in the window it hands over an `xcb_connection_t*`
    whose property reads are libxcb calls anyway. libxcb is thread-safe without
    `XInitThreads`, returns `BadWindow` as a per-request reply rather than through a
    process-global handler, and is already on every machine that runs Qt's xcb platform
    plugin, so the `.deb`'s shlibdeps gain nothing new. `AC3FORGE_CRUCIBLE_X11` (AUTO/ON/OFF)
    follows the audio backends' shape; exactly one of `xcb_window_reader.cpp` and
    `no_xcb_window_reader.cpp` is compiled, and the second says at runtime that the build
    has no X11 support. The configure summary prints `Crucible X11   : xcb|none` and the
    Linux Crucible CI leg asserts `xcb`.

    **The read moved onto the session-monitor thread, on every platform.** `fullscreen_pid()`
    is called exactly once in `engine.cpp`, beside `sessions->refresh()`, and the pid is
    handed across `session_mutex` with the list it is matched against, so the frame loop
    never waits on a server round trip and the match is against the processes that existed
    at that instant. The Windows calls have no thread affinity and ride along unchanged.

    **The seat's own word wins in both directions.** `display_session.hpp` classifies from
    `XDG_SESSION_TYPE` first, then `WAYLAND_DISPLAY` or a `wayland-*` socket, then `DISPLAY`;
    an explicit `x11` beats a stale compositor socket (and is how a nested Xephyr on the Pi's
    Wayland seat is told it is X11), Wayland keeps its refusal even with Xwayland's `DISPLAY`
    set, and neither variable at all is a third reason, "no graphical session", rather than
    the wrong X11 one an ssh login used to get.

    **`_NET_WM_PID` validated by `WM_CLIENT_MACHINE`; XRes is the follow-up.** A client in a
    pid namespace (Flatpak, Snap) reports a namespace pid, which matches nothing: a false
    negative, the same as before. `xcb-res` with `XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID`
    removes it and is named in the reader's header rather than taken now.

    The Linux session monitor's `session_pids` now carries the stream's pid and every
    same-executable ancestor of it (`process_tree.hpp`, the twin of the Windows `root_of()`
    walk, sixteen hops at most), so a browser's window process matches its audio process
    while a full-screen terminal never pins the player it launched; `app` stays the stream
    pid because the tap targets it exactly. `EngineStatus` gained `fullscreen_rule_available`
    and `fullscreen_rule_reason`; the Room note (`fullscreenRuleNote`) reads "The full-screen
    rule is off here: ..." when there is a reason and states the rule otherwise.

    Tested without a display: `[crucible][x11]` cases over a fake reader hold the
    pid-only-while-full-screen rule, the first-use connect and its 20-read retry cadence, a
    lost display and its recovery, the classification table and the ancestor walk; the Room
    note has a Qt Quick case that takes the available branch on Windows and the no-display
    reason on the Linux leg.

    [Hardware, to be recorded by the integrator after the Pi run: the Xephyr recipe
    (`Xephyr :2`, `openbox`, `mpv --fs`; `DISPLAY=:2 XDG_SESSION_TYPE=x11 WAYLAND_DISPLAY=
    /tmp/probe 20`) - the WM, the `full-screen pid -> session pid (mpv)` line, `none` after
    `wmctrl -r mpv -b remove,fullscreen`, and `none` for a full-screen xterm running
    pw-play; the unchanged Wayland refusal on the normal session; optionally the window in a
    real X11 session.]

Table under "What this plan cannot verify" (keep the Wayland row; add):

| X11 full-screen detection | yes, in an X11 session or a nested Xephyr on the Pi | none |

    **The silent device is ephemeral, and better for it.** It is a `libpipewire-module-adapter`
    node loaded on the application's own connection with `object.linger=false`, so it exists
    exactly as long as Crucible runs and is gone when it exits. Windows leaves an installed
    driver and an endpoint behind until somebody uninstalls them; on Linux a person who tries
    Crucible and quits has their machine back as they found it, and `remove()` is not an
    uninstall because there is nothing to uninstall. The UI wording for that block should follow
    the difference rather than assume the Windows shape.

    One thing the Linux session monitor drops on purpose. Windows groups audio sessions by
    process tree, because a browser renders its audio from a utility process under the browser;
    PipeWire tags every stream with the client's own `application.process.id` from its
    credentials, so an application is one process id and the walk has no counterpart. What goes
    with it is Windows' `has_window` test — there is no portable way to ask on Linux and no way
    at all under Wayland — so anything with an audio stream is listed. A background process that
    plays sound is rare on a desktop, and listing one is a smaller error than hiding a real
    application.

    **Verified on hardware 2026-09-05**, on the Raspberry Pi 4B this project already uses for
    DR9 — Pi OS 13 (Trixie), kernel 6.18.39, aarch64, PipeWire 1.4.2 with WirePlumber, a live
    Wayland session. `tools/checks/crucible_platform_probe.cpp` exercises each seam against the
    real machine; it is a tool rather than a test because it needs a session, and it is
    read-mostly — it creates and removes the silent device and never moves the default output.

    ```
    process_loopback_available() = true
    session monitor:  pid 30183  pw-play  active=1 session=1
    per-app tap:      116736 frames, 0 silence-filled, 0 dropped
    default device:   alsa_output.platform-fe00b840.mailbox.stereo-fallback
    foreground:       Wayland gives a client no way to ask which window is full-screen
    device watcher:   added ac3forge_crucible_sink / removed ac3forge_crucible_sink   (5 events)
    ```

    The watcher line is the one worth reading twice: an independent client saw the silent device
    arrive and depart, which verifies the watcher and the silent device at once.

    **Five defects, none of which any amount of building could have found.** Every one needed a
    session to exist, and on WSL2 `pw_context_connect()` fails first and the code below it never
    runs.

    1. **The passthrough probe deadlocked on every success.** `probe_connect()` destroyed its
       stream after unlocking the loop but before stopping it — a `pw_stream` call from the wrong
       context, which wedged the loop so the `pw_thread_loop_stop()` that followed never
       returned. This is library code, older than this work, on the path every probe of a real
       node takes: `enumerate_render_devices()` hung the application at startup. The order is
       unlock, stop, destroy, and `Capture::stop()` had it right all along.
    2. **A capture teardown freed the wrong handle.** After the move into the member, the local
       is empty, so the not-ready path destroyed nothing and left a live stream to outlive the
       loop it was created on. Introduced by this work, in the Phase 3 refactor that renamed the
       local to avoid a shadow warning.
    3. **The metadata helper hung on its second round trip.** `pw_core_sync()` returns a fresh
       sequence each time and the `done` handler quits only for the one it expects, so a caller
       that synced and ran the loop itself waited for a sequence nobody was matching. The helper
       owns both round trips now.
    4. **The process id is not on the stream node.** A `Stream/Output/Audio` node carries
       `application.name`, `media.class` and a `client.id` — and no pid. The process is on the
       **Client** object, as `pipewire.sec.pid`, which the daemon takes from the socket
       credentials. Reading `application.process.id` off the node, which is the obvious thing and
       what this work did, matches nothing: the session list was empty for ever and
       `start_process_loopback()` could never find a process to tap. Both the library and the
       application go through `output_stream_nodes()` now, which does the join.
    5. **The silent device did not exist.** `pw_context_load_module()` loads the adapter into
       *this process's own context*: it returns a module, reports no error, and creates a node no
       other client can see, target, or be made to play into. The state query then reported the
       device present on the strength of that module load. It is `pw_core_create_object()` now,
       which asks the daemon for the object, and the state query asks the graph rather than a
       flag. The failure was visible only because the device watcher, which should have seen the
       device arrive, stayed silent.

    Defect 5 is the one worth keeping as a lesson: the code returned success, the state said
    present, and nothing existed. A second, independent observer of the same fact — the watcher —
    is what made the difference between believing it and knowing it.

    **Later the same night the receiver came on**, and the second half of the run followed.

    The kernel saw it at once — `HDMI-A-1: connected`, ELD `monitor_name: AV Receiver`, SADs
    for AC-3 (6 ch), E-AC-3 (8 ch), TrueHD, DTS-HD — and PipeWire did not. WirePlumber had been
    running since 17 August; its hot-plug activation died with "Object activation aborted:
    PipeWire proxy destroyed", `wpctl set-profile` changed nothing, and the card offered only
    `off` and `pro-audio`. A restart of the user's PipeWire services fixed it instantly: the
    proper `hdmi-stereo` profile appeared with `iec958.codecs = [PCM, DTS, AC3, EAC3, TrueHD,
    DTS-HD]` read from the ELD. So the WirePlumber `iec958Codecs` rule this project expected a
    user to write is not needed on a receiver that advertises its codecs; what is needed is a
    session manager that has not been up for three weeks. (A guess along the way — that
    `vc4-hdmi`'s ALSA format being only `IEC958_SUBFRAME_LE` was the cause — was wrong, and is
    recorded so it is not made twice.)

    Then the library's own probe, `enumerate_render_devices()`, against the real graph:

    ```
    *alsa_output.platform-fe00b840.mailbox.stereo-fallback ac3=YES eac3=YES  "Built-in Audio Stereo"
     alsa_output.platform-fef00700.hdmi.hdmi-stereo         ac3=YES eac3=YES  "Digital Stereo (HDMI)"
    ```

    The second line is DR9's PipeWire row answered. The first line is a **false positive**: a
    3.5 mm headphone jack cannot carry a bitstream, and the probe said it could, because the
    connect it makes is accepted by PipeWire's adapter, which would render the bursts as PCM
    noise. Crucible's output policy would have chosen it. Both enumeration and `start()`'s
    auto-pick are now gated on the node's `iec958.codecs`, which is WirePlumber's judgement from
    the sink's own ELD, and the jack — which has none — is never probed.

    `tools/checks/passthrough_probe.cpp` then streamed the 5.1 E-AC-3 fixture to the HDMI sink
    for 25 s: 824 IEC 61937 bursts over 11 loops, 0 dropped. **Whether the receiver locked is the
    receiver's display's to say**, and that reading is the one thing this record still lacks.

    **The window, on Linux.** With Quick 3D, Shader Tools and Linguist Tools installed from apt,
    `ac3crucible` built on the Pi (6.1 MB, aarch64) after one platform split — the application
    icon provider asks the Windows shell for an executable's icon and is the only Windows-only
    file in the UI; its Linux twin returned none and the monogram showed, until Phase 6 gave it the icon theme and the `.desktop` entries. Rendered headless with
    `--shot`, the room, both views, the three-station signal path and the status bar all drew,
    with the encoder at **9.56 ms per frame** on the Pi 4B and zero underruns. The screenshot is
    what exposed the next four defects, which no log had:

    1. **Station 1 said "install the Desktop Atmos driver" and station 2 was labelled "DESKTOP
       ATMOS".** The silent device's name lived in the window as a default rather than in the
       platform that owns it. `VirtualDevice` now carries `device_name()` and
       `how_to_get_one()` — "Desktop Atmos" and the driver advice on Windows, "Crucible (silent)"
       and "Crucible creates it" on Linux — and station 2 is simply Crucible.
    2. **The applications rail listed "ac3forge probe."** Crucible's own probe streams are
       PipeWire streams like any other. Windows never lists another instance of this program;
       the Linux session monitor now skips its own pid.
    3. **The tray was absent, with "Qt Labs Platform requires Qt Widgets".** `Qt.labs.platform`'s
       tray is `QSystemTrayIcon` underneath, a Widgets class; Windows tolerated a
       `QGuiApplication` because its native menu needs no widgets, Linux refused. It is a
       `QApplication` now, on every platform.
    4. **The first frame took 7.1 s** (`worst 7121.5 ms` in the status bar). The output probe ran
       on the frame thread, and on PipeWire every answer is a real connect with a 2 s timeout —
       instant on Windows, seconds here. `OutputStage::reprobe()` is now `enumerate()`, which any
       thread may run, plus `apply()`, which starts and stops sinks and stays on the frame
       thread; the engine runs the first on a probe thread of its own, the same shape as its
       session-monitor thread, and applies the facts at the next frame boundary.

    Two more from the same run's logs. Every `stop()` destroyed its stream *after* stopping the
    loop, which is the right order, but without the loop lock held, which PipeWire's context
    check still wants — "called from wrong context" on every teardown. And the Linux silent
    device's teardown never destroyed its node proxy before the core, which is "impl_ext_end_proxy:
    Device or resource busy". Both fixed; the `rendered` statistic, which had read 0 through a
    run that plainly played because each callback asks for less than one burst and integer
    division per callback rounds to nothing, now accumulates bytes and counts whole bursts.

    **Re-verified on the Pi after these fixes**, 2026-09-05: enumeration reads the headphone jack as `ac3=no eac3=no` and the HDMI sink as `ac3=YES eac3=YES`, from the bound node info rather than the registry dictionary; the window renders with 0 wrong-context warnings and a worst frame of 59 ms; and its own Qt Quick suite passes there (the next section).

    What the Linux window lacked when this was written - application icons and the full-screen
    rule - both landed in Phase 6: icons come from the icon theme and the `.desktop` entries,
    and the rule is answered under X11 and refused, with the reason, under Wayland. Worth
    knowing before the next hardware run: the Pi's own
    checkout sits on `bugfix/vc4-hdmi-device-classification` — HDMI audio classification on
    this exact hardware has bitten the ALSA backend once already.

    **The window's own tests, on Linux — and what they found.** The Qt Quick suite ran on the
    Pi for the first time: four of five passed and the settings suite failed twice, both on the
    same line of thinking. The settings page asserted that pointing the driver folder at a
    place that does not exist means there is no package to install — true on Windows, and
    meaningless on a platform with no package, where the application makes the silent device
    itself and "found" is the platform's word for "can make one". The page was Windows-shaped
    in four places besides: it said applications play into "the Windows default output", named
    "the Desktop Atmos driver" in its own prose, offered a driver folder and `install.ps1`
    advice under Advanced, and labelled its buttons Install driver and Remove driver. The seam
    had the fact (`set_package_dir()` is a no-op on Linux) and nothing told the window, so
    `VirtualDevice` now says it outright — `from_package()`, true on Windows, false on Linux —
    and the page is worded by it: the two-stage note names whatever the platform calls the
    device, the folder and its notes exist only where a package does, and Linux reads Create
    device and Remove device. The tests now open Advanced and assert the shape on both
    platforms rather than skipping one. That took a second lesson: a Qt Quick `TestCase` item
    is invisible by design, an Item's `visible` reads the *effective* value, so nothing
    parented to the test case can ever be seen — the earlier tests only ever read `enabled`
    and `text` and never noticed. The page under test is parented to the window's root item.
    On the Pi, after the fix, all five suites pass — language, output, room (35 s), settings (17 s: seven passed, one skipped by name) and shell — the suite Windows runs in six seconds.

    **Packaging on Linux, from the Pi.** The first `cpack` there died in Qt's own deploy
    script, which `qt_generate_deploy_qml_app_script` runs at install time: on Linux it copies
    the distribution's QML plugins into the package and then fails to rewrite an RPATH they
    never had. WSL2 never reached it, having no Qt. The script is Windows and macOS only now —
    Linux ships no Qt, the loader finds the system's — and the `crucible` component packages
    alone from a build that has built nothing else (`cpack -D CPACK_COMPONENTS_ALL=crucible`;
    the runtime component's install rules want a `libac3forge.so` the Pi never built).
    `ac3crucible` links the library statically, so the package is self-contained apart from
    Qt and PipeWire, which `dpkg-shlibdeps` resolves from the binary:
    `ac3forge-crucible-0.0.0-dev-Linux-aarch64.tar.gz`: 16 entries, 7 MB unpacked, 2.8 MB compressed; `check_crucible_package.py` passes, and not one entry under `qml/`.
    `ac3forge-crucible_0.0.0_arm64.deb` (2.8 MB): Package `ac3forge-crucible`, Section sound, Depends `pipewire, wireplumber | pipewire-media-session` plus what shlibdeps read off the binary — libpipewire-0.3, Qt 6 Core/Gui/Qml/Quick/QuickControls2/Widgets, libstdc++6 — and six files: the two binaries, the launcher, the AppStream record and the two icons.

    **CI, the Linux half.** The Linux LLVM leg's Crucible pass no longer stops at the runner.
    It installs distro Qt the way the GUI leg does, builds the window and its test binary,
    runs the engine's Catch2 tags and the PipeWire backend's contract tests in that tree — the
    only place they run on PipeWire — then the Qt Quick suite headless with `--no-tests=error`,
    packages the component, runs `check_crucible_package.py` on the tarball and checks the
    `.deb`'s name, and uploads both as a run artifact (not a release asset: the release legs
    build against ALSA and cannot produce it, which is the next step for `docs/releasing.md`'s
    table). The fleet's Linux image is Ubuntu 26.04 with Qt 6.10; when `decide-runner` falls
    back to GitHub's 24.04 and its Qt 6.4, below the window's 6.8, the step warns by name and
    skips the window half rather than fail the leg for something unrelated to the change.

### Phase 5: macOS

The library half is an Objective-C++ translation unit — `CATapDescription` has no C entry point —
implementing the tap with `muteBehavior = .mutedWhenTapped`, plus a `DeviceWatcher` over
`kAudioHardwarePropertyDevices` and `kAudioHardwarePropertyDefaultOutputDevice` property
listeners. The platform half reads `kAudioHardwarePropertyProcessObjectList`, takes the
frontmost application from `NSWorkspace`, and renders a per-application signal path because
macOS has no silent device to point at. `system_audio_tap_api_available()` already exists as the
version gate to refuse on; the exact floor (the repo records 14.2, some sources say 14.4) gets
pinned here.

**Exit:** compiles and links on both macOS CI legs and survives the universal merge. **It cannot
be run**; see below.

### Phase 6: product qualities

First-run explanation of what the application is about to do to the sound settings; a log export
that carries no key material; a review pass over the six mechanically translated languages;
third-party licence notices per platform; and an accessibility pass over a UI that has only ever
been driven with a mouse.

Settings migration is done early, in Phase 1, because the rename moves the store: the demo kept
its settings under `ac3forge/DesktopAtmos` and the product keeps them under `ac3forge/Crucible`,
so without a copy on first run a machine that ran the demo would silently lose its signing-key
path, endpoint choice and appearance.

One licence notice is already wrong and is corrected with them: the About box credits the driver
to Microsoft's Simple Audio Sample, which was true of the PortCls miniport but not of the ACX
driver that replaced it on 2026-09-04. The driver's own README and version resource say
AudioCodec ACX sample; the window is the one place that still says otherwise.

!!! success "Done 2026-09-05: first-run explanation and restore-on-quit"
    `FirstRunDialog.qml` opens once over a fresh settings store, one event-loop turn after
    `Main.qml`'s `Component.onCompleted`, and says what Crucible does to the sound settings
    before it does it. Every sentence that names the silent device or how this platform gets
    one comes from the controller's seams (`nullSinkName`, `silentDeviceAdvice`,
    `silentDeviceBlocker`, `silentDeviceFromPackage`, `silentDeviceNeeded` and a new
    `movesDefault` over `DefaultDevice::moves_default()`), so the QML carries no platform word
    and the same file says the macOS-shaped thing (nothing in the sound settings changes) where
    the default never moves. Three ways out - Send applications, Not now, Open Settings - and a
    tick that is `behaviour/moveDefaultOnLaunch`; every way out, Escape included, writes
    `firstRun/acknowledgedVersion`, an int against `kFirstRunVersion`, so the endpoint rename
    after driver signing can show it once more. On the one launch it has not been seen, the
    launch-time automatic move waits behind the dialog; a profile migrated from the demo (now
    marked `migration/fromDesktopAtmos` by `migrate_demo_settings()`) sees it once too, with a
    sentence saying the settings were carried over. `--shot` runs suppress it;
    `--page firstrun` captures it.

    Two corrections rode along. The restore on quit that `SettingsPage`, `SignalPath`,
    `install.md`, `signal-path.md` and this page's own Phase 1 record promised was implemented
    only in the console runner: `CrucibleController::quit()` (tray Quit, and the window's close
    with keep-running off) now restores the previous default when Crucible itself moved it
    (`moved_default_by_us_`), `QCoreApplication::aboutToQuit` runs the same idempotent restore,
    and `stop()` still never touches the default, so the QML suites' `stop()` calls cannot move
    a developer's output. And on Linux `moveDefaultToNullSink()` creates the node first where
    the application makes the silent device itself, so the seam's "Crucible creates it when you
    send applications to it" is what happens and the three Send buttons are enabled while
    `silentDeviceCanCreate`. Tests: `tst_firstrun.qml` (nine cases, none presses Send),
    additions to `tst_settings.qml`, `tst_shell.qml` and `test_platform_seams.cpp`. Not done:
    the six `.ts` files still need the central lupdate pass for the FirstRunDialog context, and
    the Linux create-on-send wait needs a Pi run to confirm the node is found within its
    bounded 500 ms.
!!! success "Done 2026-09-05"
    The log export. A Qt-free module, `apps/crucible/engine/diagnostics.{hpp,cpp}`, holds a
    thread-safe ring of the last 512 one-line notes and a renderer over named fields. The
    engine, the controller and a Qt message handler in `ui/main.cpp` write to one process-wide
    ring: engine start and stop with their counters, the signing outcome as a sentence that
    names no file, the device watcher's refusal, each output change with its reason, tap
    refusals once per application, encode refusals on the transition, catch-ups every
    hundredth, setting changes, default-output moves and silent-device actions. Settings
    block 07 saves the report as `crucible-diagnostics-<date>-<time>.txt` through a save
    dialog; the file carries the version and platform (with the audio backend's capabilities
    and whether the full-screen rule can work), the signing facts, the engine's counters
    including the catch-ups, tap backlog and sink queue the window never showed, the
    endpoints, the applications by name and description, the two devices of the signal path,
    a whitelisted settings list and the recent messages.

    Redaction is structural first: `EngineStatus::signing` (which names the key file for the
    Settings page) and `AppStatus::image_path` are never read, the settings are a fixed list
    with anything under `signing/` written as withheld, and the environment is never
    enumerated. Then the finished text is scrubbed of every spelling of the key path and of
    the inline `AC3FORGE_SIGNING_KEY` value, for lines that arrived through the message ring.
    `tests/crucible/test_diagnostics.cpp` holds the rule over the renderer on every CI leg and
    `tst_settings.qml` holds it over the window with a chosen key file; `tst_shell.qml` checks
    the engine's own notes reach the report and the status line that names the file never
    does. `SigningHook` gained `source_kind()` and `failure()` so the engine can say how the
    key was obtained without saying where it is. Not captured, and the troubleshooting page
    says so: PipeWire's own stderr output and the decoder's AP11 events.
!!! success "Done 2026-09-05: third-party licence notices per platform"
    Every Crucible package now carries a `NOTICES.txt` written for it, and the About box's
    Licences… button shows the same text. The file is generated at configure time by
    `cmake/Notices.cmake` from `apps/crucible/notices/`: shared fragments, a component list per
    platform directory (`platform/windows/`, `platform/linux/` - the same selection rule as
    `engine/platform/`, so no fragment, QML or C++ file names an operating system), verbatim
    licence texts under `licences/`, and the versions CMake already holds (`Qt6_VERSION`, the
    `{fmt}` and PipeWire versions, `PROJECT_VERSION_FULL`). The Qt Quick 3D and Tracy sections
    are inserted by the same build facts that gate `Room3DView.qml` and `ac3::tracy`. A missing
    fragment, a missing licence file or an unreplaced `{{TOKEN}}` fails configure by name.

    The Windows zip carries `NOTICES.txt` and `LICENSE.txt` at its root: the bundled Qt modules,
    the LGPL-3 text with the relinking statement and the download.qt.io source location for the
    exact version, the third-party code inside the Qt libraries transcribed from the 6.8.3 kit's
    SPDX documents, the five runtime files windeployqt places (Mesa llvmpipe under the MIT, the
    DirectX Shader Compiler under the NCSA licence, three Microsoft redistributables under their
    own terms), `{fmt}`, the OFL 1.1 with the Archivo and Noto copyright lines, and the
    Ac3ForgeNullSink driver's MS-PL for the three scripts the package carries. The Linux tarball
    and `.deb` carry theirs under `share/doc/ac3forge-crucible/` with `LICENSE.txt` and once more
    as `copyright`, the name Debian tools look for: built against the system Qt, linking
    libpipewire-0.3, `{fmt}`, the fonts. The same file is embedded as `:/notices/NOTICES.txt` and
    read by `CrucibleController.licenceNotices`, so the dialog cannot say something the package
    does not; `--page licences` captures it.

    One finding to keep: the kit's SBOM concludes Qt Quick 3D, Quick3DRuntimeRender and
    Quick3DUtils as "Commercial OR GPL-3.0-only", where every other module is also offered under
    the LGPL-3. The About box had said LGPL for all of Qt. A GPL-3.0-or-later application
    satisfies that, and the notices say so, but a Windows build with the 3D room is a GPL-3
    combined work; any later plan to offer Crucible under other terms, or through a store that
    refuses the GPL, would have to drop or replace the 3D view. The driver credit is corrected
    with the rest: the notices and the About box say AudioCodec ACX sample, and
    `apps/windows/driver/README.md` records that the three scripts travel into the package under
    that directory's terms.

    `tools/ci/check_crucible_package.py` reads the notices as well as the archive's names: each
    platform's required and forbidden phrases, a filled Qt version, and on Windows the Qt Quick
    3D section present exactly when `qml/QtQuick3D/` shipped; `tools/ci/test_check_crucible_package.py`
    pins those rules, and `tst_about.qml` holds the embedded text to `silentDeviceFromPackage`
    and `has3D` and opens the dialog from About. Not done here: the Windows zip still carries
    Qt6Test, Qt6QuickTest and `qml/QtTest`, which the deploy picked up from the test QML; the
    notices list them until the deploy is narrowed.

!!! success "Measured 2026-09-05: what the tests reach"

    The first coverage figures since the rename, from
    `tools/checks/coverage_crucible.ps1` over a `config-windows-llvm-coverage` build running the
    `crucible` and `crucible-ui` labels - 99 cases between them. Over `apps/crucible`:
    **76.3% of lines** (1,036 of 4,369 missed), **62.2% of branches** and 84.2% of functions.
    The demo's last measurement, under its old name, was 72% of lines and 60% of branches from
    57 cases, so the figure held while the code grew by half.

    The shape matters more than the total. What this pass added is among the best covered:
    `engine/diagnostics.cpp` 91% of lines, `ui/desktop_entries.cpp` 94%, `engine/slots.cpp` 96%,
    `engine/tap_pool.cpp` 97%. What is thin is thin for reasons that are on this page already:
    `ui/main.cpp` is 0%, because the Qt Quick harness has an entry point of its own and never
    runs the application's; `engine/platform/windows/driver_tools.cpp` is 30%, because the rest
    of it launches an elevated PowerShell script that no test may run; and
    `platform/windows/{default_device,foreground}.cpp` sit near 46%, because their other half
    is what a machine with a real endpoint and a real front window does.

    The Linux platform half does not appear here, and it is worth being exact about why. The
    Catch2 binary links `tests/crucible/platform_services_stub.cpp`, so no platform directory is
    compiled into it on any operating system except `x11_foreground.cpp`, which is pure policy
    over an injected reader. Everything else - the Linux session monitor, default device and
    silent device - is reached only through the Qt Quick suites, which run the real controller
    against the real seams. Those numbers therefore exist only where a Qt build runs
    `crucible-ui`: this Windows machine, the Raspberry Pi and the Linux CI leg. Measuring the
    Linux side means a coverage build on one of the latter two, and the machine-facing parts of
    those files stay outside any of it - `tools/checks/crucible_platform_probe.cpp` is what
    exercises them, by hand, on hardware.

    No floor is set. `coverage_report.sh` gates the library per component, and the same is worth
    doing here once a second measurement says which of these numbers are stable.

!!! success "Done 2026-09-05: the accessibility pass"
    The window can be operated without a mouse, and what it offers a screen reader is asserted
    rather than assumed. Every hand-drawn control is a tab stop that Space and Return press
    (`CrucibleButton`, `CrucibleCheck`, `BedChip`, the header pill, the Advanced disclosure);
    the shared `SegmentedControl` is one tab stop with Left/Right/Home/End choosing inside it,
    the way a radio group behaves elsewhere. A new `ui/qml/RoomKeys.qml`, a `FocusScope` around
    both room views, moves whichever application is selected: arrows across and front to back
    (0.05, Shift 0.01, Ctrl 0.25), Page Up and Page Down for height, Home to recentre, Enter to
    place one that is in the bed, Delete to return it, plus and minus for size — the same keys
    whichever picture is on screen, because nothing in it asks which. `Ctrl+1/2/3` switch pages,
    F1 opens About, Escape closes a dialog. A new shared `apps/gui/qml/FocusRing.qml` draws a
    two-pixel accent-derived ring outside a control's own border while it has the keyboard;
    clicking a button does not take focus, so a mouse user sees no rings, and clicking a row, a
    marker or a chip does, because that is where the arrows continue from.

    Names: markers, application rows, bed chips, endpoint rows and both buttons on each of them,
    the signal path's three stations, the two combo boxes and the pin, the driver-folder and
    silent-device fields, the applications list and the room scope all carry a role, a name and a
    description composed from the same live property the view draws — never a second, typed copy.
    Position words moved into a `RoomWords` singleton the card, the rows, the markers and the
    announcements share. An `A11y` singleton and one transition hook in `Main.qml` turn engine
    state, the hearing line, the signing status, the default output and driver messages into one
    sentence each, compared with the last one said for that fact so a 60 ms poll repeats nothing,
    and send it both to `Accessible.announce` and, through a new `CrucibleController::note`, into
    the diagnostics ring, so a report carries what the window said as well as what the engine did.

    Contrast and text size: `Theme` gained `luminance`/`contrast` (compositing the foreground's
    alpha over the background, which is what a reader sees — measuring the unmixed token reports
    about 15:1 for a colour that reads at 5), `accentInk`, focus-ring tokens, `textMuted` at 68%
    and `divider` at 50%, and `accentText` is now whichever end of the palette reads better on
    the accent. Every literal `font.pixelSize` under `apps/crucible/ui/qml` is a `Theme` token
    times a new `Theme.fontScale`, and the header, footer, buttons, chips, fields and combo boxes
    derive their heights from their labels; the exceptions are the three Texts inside the 3D
    scene graph, which are scene units and say so. A Text size setting (100/125/150/175/System,
    `appearance/textScale`) drives the scale. **100% is the default.** "System" reads the point
    size the platform theme reports and counts 9 pt as 100%, which is the base size on Windows
    and what its own Text size setting scales; GNOME, KDE and Ubuntu report 10 or 11 pt with
    nothing about text size touched, so System starts the window 11 to 22% larger there. Making
    it the default would have changed the layout on the platform this window was verified on,
    silently — so it is a choice a person makes, the setting's note says what it reads, and the
    default draws the window at the size the mockups and the `--shot` captures pin.

    **One number does not reach AA, and it is a design decision on the record.** The label on a
    primary button's accent fill is the better of the two ends of the palette on that accent:
    5.7-8.6:1 in the dark modes and in ink light, but 3.95:1 in signal light and 4.22:1 in
    console light, above the 3:1 floor for a control and below the 4.5:1 one for small text. The
    fill stays the design system's colour; the alternative, filling the button with `accentInk`
    and keeping the pale label, reaches 4.5:1 and darkens the button instead. `tst_accessibility`
    asserts ">= 3, and the better of the two", `docs/crucible/accessibility.md` states both
    numbers plainly, and `apps/gui` inherits whichever way this goes, so the design owner's
    answer belongs here before it merges.

    Qt floor raised to 6.8 (`Accessible.announce` is `Q_REVISION(6, 8)`) in all four
    `find_package` calls, `qt_standard_project_setup`, the tests' `find_package`, `install.md`
    and the CI warning; every verified platform is already above it (6.8 on the Pi, 6.10 on
    Windows and the fleet). The harness gained a scripted machine: a `TestServices` singleton
    over the engine's existing fakes and a plain C++ `set_test_services` on the controller,
    reachable from neither QML nor the shipped binary, so the keys-only room flow runs where
    there is no audio session. A null seam restores the platform's own and `TestServices.clear()`
    hands the machine back, which both suites do in `cleanup()`: the default device and the
    silent device are held by the controller rather than passed to the engine, and `movesDefault`
    and `silentDeviceFromPackage` are CONSTANT properties that never re-read after a swap.

    Two suites carry it: `tst_keyboard.qml` (the keys-only flows, the tab order, the focus ring,
    the text scale) and `tst_accessibility.qml` (names, roles and descriptions from live data,
    the announcer, and the palette contrast floors for signal, ink and console in both modes).
    One trap is worth keeping: a `ListView` writes its own `currentIndex` back to 0 whenever the
    value of its model changes, and the controller replaces the applications list on every poll
    where the membership or the sound-first order moved. With the row leading the selection, an
    application starting, exiting or going quiet handed the room's arrow keys to whatever had
    floated to the top of the rail. The selection leads now, and the row follows it.

    **Verified:** by the two suites, which CI runs on the Windows legs and the Linux LLVM leg
    (`ctest -L crucible-ui`, offscreen and `QT_QUICK_BACKEND=software`). **Not verified:** no
    screen reader has been run against the window by hand — neither NVDA on Windows nor Orca on
    the Pi — so the suites prove the window offers the right names, roles and announcements and
    not that a reader speaks them as intended; and no `--shot` capture has been taken at 150% to
    confirm nothing clips at the largest text size (the default draws the window unchanged).
    **Left for later:** the six `.ts` catalogues do not carry this pass's new strings and land
    with the translation review; the 3D camera's orbit and zoom and moving one side of a split
    pair on its own stay mouse-only, and `accessibility.md` says so; and the Signal path page
    still names a platform in three sentences a reader now hears out loud ("the Windows default
    output", "Headphones (Windows Spatial Sound)", "System follows Windows"), which wants the
    same controller-property treatment `fullscreenRuleReason` and `silentDeviceAdvice` already
    have.

!!! note "Still open in Phase 6"
    One of the five items is not done. The **review of the six mechanically translated
    languages** is a pass of its own: the catalogues are stale against the source, fifteen
    current strings have no entry and sixty-five rename-era entries sit as vanished, and the
    window has no `LayoutMirroring` root, so Arabic, Hebrew and Yiddish flip their text and keep
    a left-to-right layout. It lands last, after every item that adds a string - and the
    accessibility pass above added a good many.

    No screen reader has been run against this window by anyone. The suites assert that every
    role, name and description exists and follows the live data; whether NVDA or Orca speaks
    them in a sensible order is a thing a person has to sit down and listen to.

    Three things wait on a machine rather than on work here: the Linux create-on-send wait needs
    a Pi run to confirm the node appears inside it, the application icons need the Pi to say
    which rung each application hits, and no screen reader has been run against the window on
    any platform.

### Phase 7: docs

`docs/crucible/`, taking the application out of Platform notes: an index, install and first run
per platform, the room, the output modes, the signal path, the signing key, the silent device
per platform, localisation, and troubleshooting. `windows-demo.md` stays as the historical
design and phase record, retitled and cross-linked rather than deleted, the way this project
keeps its records. `mkdocs.yml` gains a "Crucible guide" section beside "CLI reference" and
"GUI guide"; the three desktop platform pages link into it instead of owning it.

!!! success "Done 2026-09-05, first cut"
    `docs/crucible/` is a guide now, not a plan: [What it is](index.md),
    [Install and first run](install.md), [The signal path](signal-path.md) and
    [Troubleshooting](troubleshooting.md), with this page kept beside them as the record. The nav
    carries all five under "Crucible guide", and the demo page's entry under Platform notes is
    relabelled as the record it is. `windows.md` links to the guide instead of the demo page, and
    `linux.md` gained the one thing a Linux reader has to know before anything else — that
    Crucible cannot use the backend that page otherwise recommends.

    Every claim in the guide is one this work verified or one the demo's record verified; the
    platform table on the index says "not yet confirmed" where that is the truth, and the Linux
    build command in the install page was run before it was written down. Not yet written: the
    room, output modes and settings pages the GUI guide's shape would want — those describe the
    window, and the window exists on one platform, so they wait for the pass that reviews it as
    a product rather than the demo it was.

### Phase 8: CI and packaging

Build, test and package on three platforms: the existing Windows legs, the Linux GCC and LLVM
legs with a Qt kit, and the two macOS legs feeding the universal merge. The package check
generalises to all three layouts. Installer work is per platform — NSIS exists, `.dmg` exists,
Linux gets AppImage and `.deb` alongside the GUI's.

!!! success "Done 2026-09-05, for Linux"
    One extra pass inside the "Linux LLVM (clang)" leg builds the Linux platform half against
    PipeWire and asserts the runner binary exists — the same "extra pass, not a matrix entry"
    shape the ALSA fallback step uses. That leg and not a GCC one, because the GCC legs carry
    `alsa_fallback`, whose assertion (disabling ALSA falls back to posix) is only true while no
    PipeWire headers are installed, and this pass installs them.

    It deliberately runs no tests, and the reason is worth keeping. The `crucible` ctest label
    drives the engine over the fakes and links the platform-services *stub*, not the Linux half
    this pass exists to build, and every other Linux leg runs it already. An earlier draft ran
    that label without building `ac3tests`; `catch_discover_tests` registers cases at build
    time, so nothing matched, and `ctest` exits 0 when nothing matches. **It passed without
    running a single case.** What replaced it is an assertion on the binary, checked both ways:
    against a real build, and against the binary moved aside.

    Not done: Windows packaging is unchanged (the driver is still test-signed, so the archive
    stays separate), there is no Linux package or installer for Crucible yet, and macOS has
    nothing to build.

### Phase 9: verification

The hardware matrix, and the roadmap edits that follow from it: DR9's Windows and PipeWire rows,
UX7's macOS row, and the warning in `platforms/windows.md`.

## What cannot be verified, and why

Stating this before the work rather than after it, because three of these will still be true
when the code is finished, and a plan that implies otherwise is worth less than one that does
not.

| Claim | Can it be verified | Blocker |
|---|---|---|
| Linux per-application tap and null sink | **confirmed 2026-09-05** on the Pi | none |
| Linux bitstream to a receiver over ALSA | **already confirmed**, 2026-08-20 | none |
| Linux bitstream over PipeWire `iec958` | yes, with a WirePlumber codec rule | needs a session; DR9 |
| Windows bitstream to a real receiver | not yet | an HDMI cable; DR9 |
| Windows driver on a normal machine | not yet | EV certificate and attestation; a separate session |
| macOS anything, at runtime | **no** | no Mac has ever run this backend; DR9 |
| macOS tap consent prompt | **no** | the prompt is keyed to code-signing identity and does not fire unsigned; DR6 |
| Wayland full-screen foreground detection | **no**, by design | Wayland does not let a client ask about another's windows |
| X11 full-screen foreground detection | yes, in an X11 session or a nested Xephyr on the Pi | none |
| Linux application icons, per application | yes, on a desktop with applications playing | needs the Pi; which rung each hits is machine-dependent |

Compiling on a second platform pays for itself before any of it runs. Phase 3 was written on
Windows and built clean there; the first Linux build then rejected it three times, and each was
a real defect rather than a dialect quarrel: a local that shadowed a member after a refactor
(`-Wshadow`), a partial designated initialiser MSVC accepts and GCC does not
(`-Wmissing-field-initializers`), and a capability that claimed to be available on machines
where it could not be. Only the third would have reached a user, and none of the three was
visible from the platform the code was written on.

The macOS row is the one to hold in mind while reading Phase 5. The code can be written and
compiled on CI, and its device-free logic can be unit tested, and none of that establishes that
it works. The demo page's discipline applies: what is claimed is what was checked.

## Coordination with the driver-signing session

Driver signing is being worked in a separate session against `apps/windows/driver/`. Two
consequences for this plan, and neither is optional:

**The driver subtree's naming is not renamed tonight.** The application rename in Phase 1 stops
at the driver's door. `apps/windows/driver/` keeps its paths, its INF and its
`Ac3ForgeNullSink` identity so that work in flight does not collide with a rename underneath it.

**The device name should change before attestation is paid for, not after.** The endpoint
currently advertises "Speakers (Desktop Atmos)". That is the trademark exposure, and it lives
inside the package that gets attestation-signed — so changing it afterwards means submitting
and paying again. The right order is: the signing session lands its work, then a single
coordinated change renames the INF's device name to "Crucible" and rebuilds, then attestation is
submitted once. This plan does not make that change unilaterally; it flags it as the thing to
sequence.

## Deliberately not in scope

- **Android and iOS.** The premise breaks at silencing on Android and at enumeration on iOS.
  The Shield app already covers Android for this project.
- **A browser version.** A page cannot see other applications' audio.
- **An in-repo HRTF or binaural renderer**, on any platform. Windows renders headphones through
  the OS; Linux and macOS fold instead.
- **Per-application rerouting through undocumented policy interfaces**, carried over from the
  demo's scope.
- **Store distribution.** Unchanged from the demo: the Windows driver cannot ship through MSIX,
  and the macOS tap's consent model does not fit the sandbox.
- **Per-tab or per-stream separation inside one application.**
