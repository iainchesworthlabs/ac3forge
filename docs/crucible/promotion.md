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
| `device_watch` | yes | **Phase 3** | **Phase 3** | **Phase 5** | no | no |

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

### Phase 7: docs

`docs/crucible/`, taking the application out of Platform notes: an index, install and first run
per platform, the room, the output modes, the signal path, the signing key, the silent device
per platform, localisation, and troubleshooting. `windows-demo.md` stays as the historical
design and phase record, retitled and cross-linked rather than deleted, the way this project
keeps its records. `mkdocs.yml` gains a "Crucible guide" section beside "CLI reference" and
"GUI guide"; the three desktop platform pages link into it instead of owning it.

### Phase 8: CI and packaging

Build, test and package on three platforms: the existing Windows legs, the Linux GCC and LLVM
legs with a Qt kit, and the two macOS legs feeding the universal merge. The package check
generalises to all three layouts. Installer work is per platform — NSIS exists, `.dmg` exists,
Linux gets AppImage and `.deb` alongside the GUI's.

### Phase 9: verification

The hardware matrix, and the roadmap edits that follow from it: DR9's Windows and PipeWire rows,
UX7's macOS row, and the warning in `platforms/windows.md`.

## What cannot be verified, and why

Stating this before the work rather than after it, because three of these will still be true
when the code is finished, and a plan that implies otherwise is worth less than one that does
not.

| Claim | Can it be verified | Blocker |
|---|---|---|
| Linux per-application tap and null sink | **yes**, on the Pi or a desktop Linux session | none; the code is in, the run is not |
| Linux bitstream to a receiver over ALSA | **already confirmed**, 2026-08-20 | none |
| Linux bitstream over PipeWire `iec958` | yes, with a WirePlumber codec rule | needs a session; DR9 |
| Windows bitstream to a real receiver | not yet | an HDMI cable; DR9 |
| Windows driver on a normal machine | not yet | EV certificate and attestation; a separate session |
| macOS anything, at runtime | **no** | no Mac has ever run this backend; DR9 |
| macOS tap consent prompt | **no** | the prompt is keyed to code-signing identity and does not fire unsigned; DR6 |
| Wayland full-screen foreground detection | **no**, by design | Wayland does not let a client ask about another's windows |

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
