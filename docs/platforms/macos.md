# macOS (Apple Silicon and Intel, Homebrew LLVM)

!!! note "Verified in CI only — no Mac host is available to this project"
    There is no macOS host available to this project locally; everything on this page has been
    exercised exclusively by two required CI legs: `macos-llvm`, on GitHub's `macos-latest` (Apple
    Silicon) runners, configuring the `config-macos-llvm` / `config-macos-llvm-debug` preset pair,
    and `macos-llvm-x64`, on GitHub's `macos-15-intel` runners (real native Intel hardware, not
    Rosetta emulation — confirmed against
    [docs.github.com's hosted-runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)),
    configuring `config-macos-llvm-x64` / `config-macos-llvm-x64-debug`. Neither is experimental any
    more. `macos-llvm`'s first-ever run surfaced one fully-understood issue (Homebrew's
    unpinned `llvm` formula flagging Catch2's `__COUNTER__` usage under `-Wc2y-extensions` — see
    `cmake/CompilerWarnings.cmake`), fixed in one commit, followed by two consecutive clean runs.
    `macos-llvm-x64` (DR8's new leg, on a brand-new `macos-15-intel` runner label never exercised
    before this project used it) went three consecutive clean runs — two `release.yml` dry runs and
    this feature branch's own required PR CI — at real gold-reference SNR numbers
    (67.80/67.82/67.76 dB, matching the x86 baseline every other non-arm64 leg reports) before its
    own `continue-on-error` escape hatch came off the same way
    (see [`.github/workflows/_build.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/_build.yml)),
    so a failure on either leg blocks like every other required leg now.

    **One section of this page rests on less than that.**
    [Per-application capture](#per-application-capture-the-core-audio-process-tap) describes code
    added on this branch. Both legs compile it and one test exercises its version gate; the tap
    itself has never been created, on a runner or anywhere else. That section says what each part
    rests on.

## Toolchain

Homebrew-installed LLVM (`cmake/toolchains/macos.llvm.toolchain.cmake` prefers it over Apple's
bundled clang), on the `arm64-macos-llvm` (Apple Silicon) or `x64-macos-llvm` (Intel) vcpkg
triplet — the toolchain file itself needed no change to support the second architecture; it
already resolved its target from `VCPKG_TARGET_ARCHITECTURE`, falling back to `uname -m` outside a
vcpkg port-build context. Unlike the Linux/Windows LLVM legs, neither is pinned to an exact
version: Homebrew's core `llvm` formula has no versioned sibling to pin against the way
`apt.llvm.org` or the official Windows installer do, so CI installs and reports whatever Homebrew
currently ships rather than asserting a specific one.

## Universal binaries (DR8)

A release's macOS package is a single **universal (arm64 + x86_64) `.dmg`**, not two per-arch
ones. `macos-llvm` and `macos-llvm-x64` each build and `cmake --install` their own single-arch
tree; a separate `package-macos-universal` job (also CI-only — it runs on `macos-latest`, any
macOS label works since `lipo`/`hdiutil` are the only tools it needs) `lipo -create`s every
Mach-O file the two trees have in common — `ac3cli`, `ac3gui`, and every dylib/framework binary
`qt_generate_deploy_qml_app_script` copies into `ac3gui.app/Contents/Frameworks/` — and packages
the merged tree with `hdiutil` directly, the same call CPack's own DragNDrop generator makes under
the hood. Proven for real in CI: `lipo -info` on the merged `ac3cli`/`ac3gui` binaries and at
least one bundled Qt framework binary reports both `x86_64` and `arm64` present in the same file —
see that job's own log, not just its exit code. This was a deliberate reversal of the original
roadmap text, which called a macOS universal binary "a separate decision, not a given" on the
assumption that Intel demand was doubtful and no Intel hosted runner existed; `macos-15-intel`
turned out to already exist, be free for public repos, and be real native hardware rather than
Rosetta, which removed the actual blocker (needing to cross-compile x86_64 from Apple Silicon, or
pay for a self-hosted Intel Mac) entirely.

Each leg's own single-arch `.dmg` still exists as a fast per-push packaging smoke test
(`packageable: true` never went away), it just isn't what a release publishes any more — see
[Packaging](#packaging) below and
[docs/releasing.md](../releasing.md#what-gets-published).

## Audio backend: CoreAudio

`src/audio/CMakeLists.txt` selects a real CoreAudio backend on macOS, `src/audio/src/backend/macos/`
— capture, monitor playback and IEC 61937 passthrough are built on the Audio HAL
(`AudioObjectID`/`AudioDeviceIOProc`), the same layer WASAPI and ALSA occupy on their own
platforms, rather than the no-backend stub that used to fall back to here. Its passthrough
mechanism differs from both: CoreAudio has no per-open bitstream flag the way
WASAPI's exclusive-mode subformat or ALSA's channel-status device name are, so bitstreaming means
taking hog mode on a digital output and retuning its *physical* stream format
(`kAudioStreamPropertyPhysicalFormat`) to `kAudioFormat60958AC3` for AC-3 — see
`src/audio/src/backend/macos/passthrough.cpp`'s own header for the full mechanism, cross-checked
against three independent real-world implementations of the same thing (MythTV, mpv, VLC) while
writing it, since there was no Mac available locally to try it on directly. For E-AC-3, the same
walk additionally probes a stream's available physical formats for `kAudioFormatEnhancedAC3`:
Apple's own documentation confirms Dolby Digital Plus/Atmos HDMI passthrough exists on Apple
Silicon Macs without documenting the HAL mechanism behind it, so where a driver doesn't publish
that format (older hardware, a non-HDMI output, an Intel Mac) the backend reports E-AC-3
passthrough unavailable rather than claiming it everywhere — see `passthrough.cpp`'s own "AC-3
and E-AC-3" section.

Passthrough **capture** — an input carrying somebody else's bitstream — needs none of that
machinery, on macOS or anywhere else: IEC 61937 bursts arrive as ordinary PCM samples, and
recognising them is `ac3::iec61937::PassthroughDetector`, which works off whatever interleaved
floats the backend delivers rather than off any HAL property. `ac3cli record` uses it to write
the elementary stream instead of encoding the bursts as audio, `ac3cli live` to stop rather than
encode a session of noise, and `ac3cli unspdif` does the same job on a capture already saved to
disk. That part is platform-independent and shares the verification the framing has — see
[Windows](windows.md#passthrough-capture) for what is and is not confirmed.

The backend is CI-verified only: the parts that need no live device — enumeration on a machine
with none, format matching, sample conversion — run under `ac3tests` on the hosted runner, same
as everywhere else without real hardware, but no real Mac has ever run this code against an
actual digital output, and no receiver has been asked to lock onto its output.

**No EDID/ELD backend here either (roadmap UX9).** `ac3cli play` asks a chosen sink what it
actually accepts before committing to a format (see
[CLI → Following the sink](../cli/commands.md#following-the-sink)), and that read
(`ac3::audio::sink_capabilities`) is real today only on ALSA (see
[Linux](linux.md#reading-a-sinks-own-edideld-roadmap-ux9)). CoreAudio's device properties and
IOKit's `IODisplayEDID` are both real APIs, but neither is documented to expose the CEA-861
Short Audio Descriptor block for an HDMI *audio* endpoint specifically, and a pure optical
output has no display EDID to read in the first place. `play` falls back to the live
`enumerate_render_devices()` probe here, the same as before this roadmap item existed.

## Per-application capture: the Core Audio process tap

`ac3cli devices` never lists a loopback entry here, and `ac3cli record`/`live --loopback` refuse
outright rather than silently opening a microphone instead — unlike
[Windows](windows.md) (any render endpoint reopened via WASAPI loopback) or
[Linux/PipeWire](linux.md#audio-backend-alsa-or-pipewire) (a sink's monitor), the Audio HAL this
backend otherwise uses has no "capture what a render device is playing" concept at all.

What macOS 14.2 (Sonoma) added instead is the per-*process* tap, and
`Capture::start_process_loopback(pid, mode, format)` is built on it:
`AudioHardwareCreateProcessTap` over a `CATapDescription`, carried by a private aggregate device
whose `AudioDeviceIOProcID` reads the way an input device's does. Because `CATapDescription` is an
Objective-C class with no C entry point, `src/audio/src/backend/macos/process_tap.mm` is the
library's one Objective-C++ translation unit, behind the plain-C++ header `process_tap.hpp` that
the rest of the backend includes; `src/audio/CMakeLists.txt`'s `APPLE` block enables `OBJCXX` for
that one file.

The tree holds **three `.mm` files and two directories that enable `OBJCXX`**. The other two are
Crucible's, for AppKit rather than Core Audio — `apps/crucible/engine/platform/macos/foreground.mm`
(NSWorkspace) and `apps/crucible/ui/platform/macos/app_icon_provider.mm` (NSWorkspace and NSImage)
— and `apps/crucible/CMakeLists.txt`'s `APPLE` arm makes its own `enable_language(OBJCXX)` call
rather than relying on this one. Both call sites pin `CMAKE_OBJCXX_COMPILER` to the C++ compiler
the toolchain file chose, and `cmake/toolchains/macos.llvm.toolchain.cmake` sets
`CMAKE_OBJCXX_FLAGS_INIT` beside `CMAKE_CXX_FLAGS_INIT` so a `.mm` resolves the standard library
headers to the same libc++ its neighbours use.

The tap is created with `muteBehavior = CATapMutedWhenTapped`, and that single choice is why
[Crucible](../crucible/index.md) needs no silent device on this platform. On Windows an
application's audio has to be sent to an installed silent driver, and on Linux to a PipeWire node
created at run time, so that the sound reaches the mixer instead of the speakers; here the mute
rides on the tap itself. Nothing in the sound settings changes and there is nothing to restore on
quit.

**Three ways this contract is narrower than the Windows one**, all of them properties of the API
rather than shortcuts:

- **One process, not a tree.** `ProcessLoopbackMode::kIncludeProcessTree` is honoured as "this
  process" and `kExcludeProcessTree` as "everything except this process". Windows' activation
  walks the target's children because a browser renders its audio from a utility process; a
  `CATapDescription` names audio process objects, which carry no descendant relation to follow.
  PipeWire's tap has the same property for its own reason, so Windows is the only one of the
  three backends with a tap where that mode name is literal.
- **Mono or stereo, and nothing else.** WASAPI lets a caller state any format and has the audio
  engine convert to it. Core Audio's mixdown descriptions are mono and stereo with no converter
  behind them, so `ProcessLoopbackFormat::channels` of 1 or 2 is accepted and anything else —
  including the eight channels that keep a surround-rendering application's bed intact — is
  refused with `kFormatUnsupported`.
- **The rate is the machine's, and is checked rather than converted.** A mixdown tap runs at the
  rate of the device it mixes down to. `capture.hpp` promises samples land in the ring at exactly
  the format the caller asked for, so a tap whose own `kAudioTapPropertyFormat` disagrees is
  refused rather than quietly delivering something else.

A fourth difference is in what is *reported* rather than what is promised, and it rests on an
assumption worth naming. `CaptureStats::frames_silence_filled` stays zero for a tap here, where
the Windows and PipeWire taps both count wall-clock gaps they had to fill for a quiet process.
The reason given in the code is that the aggregate device is clocked by the output device it
names as its main sub-device, not by the tapped application, so its `AudioDeviceIOProcID` is
called every device period whether that application is playing or not. That reading of the API
has not been checked against a running one. If it turns out to be wrong — if a tap delivers
nothing at all while its process is quiet — this backend needs the wall-clock fill the other two
have, and has not written it.

`process_loopback_available()` is the OS version gate, and `audio_backend().process_loopback`
reports the same answer — as an empty reason where it is yes, and where it is no as a sentence
naming the version a person needs. The floor is pinned in
one place — `ac3::coreaudio::kSystemAudioTapMinimumOs` in
`src/audio/src/backend/macos/coreaudio_names.hpp` — and it is **14.2** rather than the 14.4 some
third-party write-ups require. Apple's SDK annotates the API `API_AVAILABLE(macos(14.2))`, which
is what `@available` and the weak-linked symbols are keyed to, and taking 14.4 would mean
refusing a machine whose own operating system declares the API present, on the strength of a
report nobody on this project can check. That constant's comment records the other figure and why
it was not taken.

**Two things this cannot settle, and neither is a code question.** Creating a tap raises a TCC
consent prompt under its own permission category (`SystemAudioCaptureRequests`, driven by an
`NSAudioCaptureUsageDescription` Info.plist key, separate from microphone access). That prompt is
keyed to the requesting binary's code-signing identity and, per every report surveyed, never
fires at all for an unsigned binary — and `ac3cli`, `ac3gui` and Crucible ship unsigned today
(ROADMAP.md's DR6, blocked on certificates). A denial and a prompt that never appeared arrive
identically, as one refusal from `AudioHardwareCreateProcessTap`, reported as `kComFailure`. And
DR9 still records what CI cannot stand in for: no Mac has opened a device, a stream or a tap
through this backend. What the runners do execute of it is named at the end of this section and
under [Device notifications](#device-notifications).

A third thing here *is* a code question, and it is not written: **no bundle in this tree declares
`NSAudioCaptureUsageDescription`**. `apps/gui/Info.plist.in` is the only custom template and
carries the `.ac3`/`.ec3` document types alone; `apps/crucible/CMakeLists.txt`'s `APPLE` arm sets
`MACOSX_BUNDLE` and the `.icns` and takes CMake's default template, which has no such key. Adding
it needs no Mac. Checking that it does anything does, since the prompt it drives is behind the
signing identity nobody here has either.

**So the most that is claimed is that it compiles, and that its version gate answers.** The tap,
the gate and the aggregate device are new on this branch. The first macOS CI attempt at them
never reached a compiler: it stopped during configure, at an `install(TARGETS ac3crucible)` rule
that named no `BUNDLE DESTINATION` for a target with `MACOSX_BUNDLE` on. With that fixed, both
legs compiled `process_tap.mm` and linked it into `ac3audio`, and their `ctest` runs covered
exactly two things here — `tests/backend/macos/test_macos_support.cpp` checks that the runner's
own OS build is above the 14.2 floor, which is the one place the `__builtin_available` lowering
is actually executed rather than merely compiled; and the backend contract case in
`tests/audio/test_audio_backend.cpp` checks that the capability report and
`process_loopback_available()` agree and that process id 0 is refused as `kProcessNotFound`.
Neither touches a tap, a consent prompt or a device.

Nothing past that has been observed. `AudioHardwareCreateProcessTap` has never been called: not
on a runner, which has no audio device and no way to grant the consent prompt, and not on anyone's
desk, because nobody here has a Mac.

## Device notifications

`DeviceWatcher` (`ac3/audio/device_watcher.hpp`) is implemented here over HAL property listeners
on `kAudioObjectSystemObject`: `kAudioHardwarePropertyDevices` for endpoints arriving and
leaving, and `kAudioHardwarePropertyDefaultOutputDevice`/`…DefaultInputDevice` for the two
defaults moving. Core Audio says only "the device list changed", so the watcher keeps the
previous list of device UIDs and diffs against it to produce `kAdded` and `kRemoved` — the
bookkeeping Windows gets from `IMMNotificationClient` and Linux from PipeWire's registry for
free.

`kStateChanged` is never raised, which is a difference rather than an omission: it exists because
a Windows endpoint can stay in the enumerator while becoming disabled or unplugged, where a HAL
device that goes away leaves the device list altogether and is already reported as
`kRemoved`.

Callbacks arrive on the HAL's own notification thread rather than a realtime one, so the property
reads the diff makes are allowed there. The watcher does **not** set
`kAudioHardwarePropertyRunLoop`, and the file says why at length: the older listener API delivered
on the main run loop, which a program that never runs one — `ac3cli`, Crucible's console runner, a
test binary — would register for and then never hear, but that property is process-wide, the newer
`AudioObjectAddPropertyListener` used here delivers on a HAL thread of its own, and the selector
carries a deprecation annotation in recent SDKs that nobody here can check against a `-Werror`
build. If it turns out notifications do not reach a run-loop-less process, that property is the
lever to pull. Registration itself needs no device and no
session, so `audio_backend().device_watch` reports available on any Mac, and the contract case in
`tests/audio/test_audio_backend.cpp` exercises that on the runners: it starts a watcher, checks it
is running, checks a second start is refused, stops it, starts it again and stops it again. That
case passed on both macOS legs, so this is the one part of the backend that has run on a Mac. It
is also the least of it. No callback has ever been seen to arrive, because a hosted runner's
device list does not change while a test is running, so the diff, `kAdded`, `kRemoved` and the
run-loop question above are all still unobserved.

## Building

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset config-macos-llvm-debug
cmake --build --preset build-macos-llvm-debug
ctest --preset test-macos-llvm-debug
```

Swap `macos-llvm` for `macos-llvm-x64` throughout on Intel; drop `-debug` from all three preset
names for a Release build either way. The `ci-macos-llvm`/`ci-macos-llvm-x64` workflow presets
each chain the same three steps in one command. Homebrew's `llvm` formula must be installed (CI
runs `brew install llvm`), and `VCPKG_ROOT` must point at a vcpkg checkout — it supplies Catch2,
plus Boost and Tracy only if you opt into the `adm`/`profiling` features (see
[building.md](../building.md)). `AC3FORGE_BUILD_GUI` defaults **OFF** on both presets, as on Linux
— see [GUI on macOS](#gui-on-macos) below to opt in.

## GUI on macOS

`config-macos-llvm`/`config-macos-llvm-x64` both default `AC3FORGE_BUILD_GUI` to `OFF` for the
same reason the Linux presets do (see [GUI on Linux](../building.md#gui-on-linux)): a Qt kit
isn't assumed present on every Mac, not because `ac3gui` cannot be built here. `cmake/FindQt6.cmake`
already searches both Homebrew prefixes (`/opt/homebrew/opt/qt`/`/opt/homebrew/opt/qt6` on Apple
Silicon, `/usr/local/opt/qt`/`/usr/local/opt/qt6` on Intel), and `apps/gui/CMakeLists.txt`'s
`APPLE` branch — `MACOSX_BUNDLE`, the `.icns` bundle icon, and `qt_generate_deploy_qml_app_script()`
for packaging — was written for this from the start; it was never exercised until the
`macos-llvm` CI leg turned the option on. Opt in explicitly once Qt is installed:

```bash
brew install qt
cmake --preset config-macos-llvm-debug -DAC3FORGE_BUILD_GUI=ON
```

Homebrew's `qt` formula is the umbrella Qt6 package — one install pulls in QtDeclarative/QtQuick
and their build-time tooling (`qmlcachegen`) alongside QtCore/QtGui, unlike apt's split
`qt6-base-dev`/`qt6-declarative-dev` packages. The built app is a bundle,
`build/config-macos-llvm/bin/ac3gui.app` (or `build/config-macos-llvm-x64/...` on Intel);
`ac3gui --smoke` (the same headless check the other platforms run — see
[Verified configuration](../building.md#verified-configuration)) lives at
`ac3gui.app/Contents/MacOS/ac3gui`, not directly under `bin/`, because `MACOSX_BUNDLE` relocates
the executable there — the same property Windows' `WIN32_EXECUTABLE` sits beside but which only
takes effect on `APPLE`.

## Packaging

```bash
cpack --preset pack-macos-llvm       # or pack-macos-llvm-x64 on Intel
```

Produces a DragNDrop image on top of a plain ZIP when the packaging tool is found, the same way
NSIS is on Windows and DEB/RPM are on Linux — each leg's own single-arch `.dmg`/`.zip`, useful as
a fast per-push packaging smoke test of that architecture alone. Neither `macos-llvm` nor
`macos-llvm-x64` carries `release_package` any more, though: a real tagged release
(`release.yml`, `do_package: true`) instead runs a separate `package-macos-universal` job that
`lipo`-merges both legs' install trees into one universal `.dmg` and ships that as the canonical
macOS package — see [Universal binaries (DR8)](#universal-binaries-dr8) above and
[docs/releasing.md](../releasing.md#what-gets-published). That path has been exercised for real on
the arm64 half: nine beta releases, v0.2.0-beta.1 through v0.9.0-beta.1, shipped a macOS package
through the tag-triggered workflow before the universal merge existed. `cmake/Packaging.cmake`
needed no change for `ac3gui` to join either leg's own `.dmg`: which targets end up in a package
is decided entirely by which `install()` rules ran, and `ac3gui`'s already runs whenever
`AC3FORGE_BUILD_GUI` is `ON` — the DragNDrop generator itself is unconditional on `APPLE`, GUI or
not. No stable (non-beta) release has been tagged yet. See [Packaging](../building.md#packaging).

The `.app` bundle also declares `CFBundleDocumentTypes`/`UTExportedTypeDeclarations` for `.ac3`
and `.ec3` — a custom `Info.plist.in` rather than CMake's default template, since neither
extension is a system-known UTI and each needs its own `UTTypeConformsTo: public.audio`
declaration tying it to `audio/ac3`/`audio/eac3`. Configure/build-verified only, like the rest of
this file's GUI coverage below — nobody has opened a real `.ac3` file from Finder on real hardware
yet.

## CI: what has and has not been verified

Build, `ctest` (see [Verified configuration](../building.md#verified-configuration) for how the
suite's composition differs from Windows/Linux) and the [gold-reference correctness
gate](../building.md#gold-reference-correctness-gate) all pass on real GitHub Actions runners —
not a simulation or a local guess, on `macos-llvm`. `ac3gui_qmltests` registers and passes there
too: 582 ctest entries total, 100% passing, that one entry in 39.74s of a 56.81s total run - the
leg's first-ever GUI run, confirmed clean on a second push after two real fixes
(`QSG_RENDER_LOOP=basic` for a Qt Quick render-loop deadlock, and forcing the `Fusion` style in
the test binary for a native-`ComboBox`-under-offscreen hang - see [GUI on macOS](#gui-on-macos)
above and `apps/gui/tests/CMakeLists.txt`/`qml_test_main.cpp` for the full detail). Real SNR
numbers from the CI run that first proved the gate on macOS: 61.81/61.82 dB, against 67.84/67.82
dB on Linux and Windows for the same material, comfortably clear of the gate's 30 dB floor. That
gap is **not** a Homebrew-LLVM-libm-vs-glibc/MSVC difference, despite what this page and
`ci.yml` used to say — roadmap VX11 (see `docs/building.md`'s "Floating-point contraction"
section) traced it to every real arm64/aarch64 CI leg, `macos-llvm` included, landing on the
same ~6.0 dB offset from every x86 leg regardless of OS or C library, which rules out a
macOS-specific explanation; the gap tracks CPU architecture, and the actual mechanism is still
open pending real hardware access neither this project nor `qemu-user` emulation can substitute
for. `macos-llvm-x64` turned out to be new, confirmed data for exactly this question: same OS,
same Homebrew Qt/LLVM stack as `macos-llvm`, x86_64 instead of arm64, and its real gold-reference
numbers land with the other x86 legs — 67.80/67.82/67.76 dB across three separate real runs — not
with the ~61.8 dB every arm64/aarch64 leg (`macos-llvm` included) reports. That is further
evidence the offset is architecture-bound rather than OS-bound: two macOS legs on the same
toolchain now sit on opposite sides of the split, purely by CPU architecture. See `ci.yml`'s VX11
comment and [ROADMAP.md](../roadmap.md)'s VX11 entry for the fuller record.

**Crucible on macOS, as of 2026-09-06.** Both legs build it — every `.mm`, every file under
`apps/crucible/engine/platform/macos/`, and `bin/ac3crucible.app/Contents/MacOS/ac3crucible` —
and both run its Qt Quick tests, which drive fakes rather than any macOS platform code. The Intel
leg passed all of them. The Apple Silicon leg timed out at 300 s on three —
`ac3crucible_qml_tests_firstrun`, `_room` and `_shell`, which the Intel leg passed in 8.3 s,
12.8 s and 6.2 s — the same three suites, the same binary, one architecture apart. There is
precedent for that shape on this runner: `macos-llvm`'s first-ever GUI run deadlocked in the
threaded Qt Quick render loop, which is why `apps/gui/tests/CMakeLists.txt` sets
`QSG_RENDER_LOOP=basic` on `APPLE` (see [GUI on macOS](#gui-on-macos)). What is being tried for
Crucible's suites is in `apps/crucible/ui/tests/CMakeLists.txt`'s own comment; until an arm64 leg
runs green, this is an open failure and a red leg says nothing about the platform half beneath
it. The application itself has never been launched on either leg.

---

If you get a Mac, that's still useful information for this project — running these instructions
on real local hardware, or actually launching `ac3gui.app` and using it (CI's `--smoke` run
proves it starts, loads its QML and drives a real encode headlessly, not that the interactive
experience is right), would be new information. Consider filing an issue with what you found.
