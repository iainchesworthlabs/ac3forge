# Windows

ac3forge is built and tested on Windows today — both toolchains, CLI and GUI alike, are
required, green CI legs. This page covers what is specific to Windows; for the full preset
reference, options list and troubleshooting, see [Building from source](../building.md).

## Toolchains

Built and tested with **MSVC 14.51** and **clang-cl 22.1** on **Windows 11**, via Visual Studio
2026 (MSVC) or clang-cl.

Every Windows preset chainloads a toolchain file that locates `cl.exe`/`clang-cl.exe` and
`link.exe` itself (via `vswhere` and `vcvarsall.bat` if a Developer PowerShell hasn't already
set one up), so configuring works from an ordinary shell — no need to launch a Developer
Command Prompt first. clang-cl is MSVC-ABI compatible, so it links the same vcpkg packages and
the same prebuilt Qt kit as the MSVC build. See
[The compiler is pinned, not PATH-found](../building.md#the-compiler-is-pinned-not-path-found)
for the mechanics.

## Audio backend: WASAPI

On Windows, the three features that touch sound hardware are all implemented over **WASAPI**:

- **`ac3::audio`** — live input/loopback capture through a lock-free SPSC ring.
- **`ac3::iec61937::PassthroughDetector`** — recognising, from that same capture, that the
  endpoint is handing over IEC 61937 bursts rather than PCM.
- **`ac3::audio::PassthroughSink`** — exclusive-mode/direct bitstream output, for both AC-3 and
  E-AC-3 burst framing (IEC 61937).
- **`ac3::audio::MonitorSink`** — shared-mode PCM playback: a non-bitstreamed preview/monitor
  path that decodes what is being encoded and plays it back on an ordinary output.
- **`ac3::audio::SpatialObjectSink`** (roadmap UX8) — `ISpatialAudioObjectRenderStream`: decoded
  Atmos objects go out as dynamic objects at their real OAMD positions, and the bed's LFE (never
  a JOC output, TS 103 420 §6.3.2.2) as a static one. Behind `ac3cli spatial`. This is the one
  path that lets Dolby's own renderer engage with this project's reconstructed objects at all — a
  licensed decoder otherwise refuses object decoding without a signing key this project doesn't
  ship (see [Object signing](../concepts/object-signing.md)) — and needs nothing but a spatial-
  sound-capable endpoint to do it, no AVR and no key.

These four are not equally verified against real hardware, and the project's own documentation
is deliberately explicit about the difference.

!!! note "MonitorSink is confirmed against real hardware"
    `ac3cli monitor` / `ac3cli live --monitor` have actually played decoded AC-3 and E-AC-3
    (including an Atmos stream's 5.1 bed) through a real Realtek output in real time, and a live
    microphone capture→encode→monitor session has run end to end. Building this path against
    real hardware surfaced two genuine bugs that neither unit tests nor silent/synthetic input
    would have caught — a fixed submit-readiness threshold smaller than an actual chunk, which
    let the ring buffer silently perform a partial write while reporting failure, and the live
    pipeline's Atmos metering step writing past the end of a buffer sized for the object count
    rather than the bed's fixed six channels. Both are fixed; see
    `src/audio/src/backend/windows/monitor.cpp` and `run_live` in
    `apps/cli/commands/live_audio.cpp`.

!!! note "SpatialObjectSink is confirmed against a real spatial endpoint"
    `ac3cli spatial` has activated `ISpatialAudioObjectRenderStream` and rendered a real Atmos
    stream (4 orbiting objects, 8 s, this project's own encoder) against the default Realtek
    output after Windows Sonic for Headphones was enabled on it — 250 access units, zero
    underruns, clean shutdown. The `kNoSpatialFormat` refusal was confirmed the same session
    against a second endpoint that still had no spatial format enabled, and against the very
    endpoint used for the successful run, probed *before* Windows Sonic was turned on
    (`GetMaxDynamicObjectCount` read 0 everywhere on this machine at that point). What is not yet
    checked: a listening pass confirming the rendered audio actually arrives from where the OAMD
    positions say it should — nobody running this had ears in the loop, only the OS's own
    accept-and-render behaviour — and the "bed as static objects" branch beyond the LFE, since
    every stream this project's own encoder produces is dynamic-object-only (`oamd.hpp`'s own
    documented shape); a genuine third-party bed-plus-objects Annex E stream would be needed to
    exercise the rest of `oba::bed_labels()` against a verified coded-channel-order mapping.

!!! warning "Exclusive-mode passthrough bitstreaming has never been confirmed against a real receiver on Windows"
    No S/PDIF or HDMI endpoint behind an actual AV receiver has been connected to a Windows
    machine during development. A receiver is available now — the one used for [Raspberry Pi's
    HDMI passthrough
    validation](raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver) — it just hasn't been
    cabled to this workstation yet; see roadmap `DR9` for that as outstanding work.
    `IsFormatSupported` correctly answers no everywhere it has been tried, for
    both `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL` and `..._DOLBY_DIGITAL_PLUS`, and neither
    descriptor has been accepted by a real device. What *is* verified: the exclusive-mode path
    itself works (a Realtek endpoint accepts an exclusive PCM format), the AC-3 bursts are
    byte-exact against FFmpeg's `spdif` muxer, and the E-AC-3 burst framing (data type 0x15, the
    24576-byte/4x-carrier-rate burst, multi-syncframe accumulation, `Pd` in bytes not bits) is
    independently verified against both FFmpeg's `spdif_header_eac3` and Microsoft's own
    "Representing Formats for IEC 61937 Transmissions" documentation, plus round-trip and
    real-audio unit tests.
    A receiver has been confirmed to lock onto AC-3, but only via a different code path (playing
    the bursts as a PCM16 WAV through a passthrough output, not through `PassthroughSink`
    itself); the same trick now exists for E-AC-3 (`ac3cli spdif`/`monitor`/`live`, branching on
    bsid) but has not itself been tried against a receiver either.

!!! note "No EDID/ELD backend on Windows (roadmap UX9)"
    `ac3cli play` asks a chosen sink what it actually accepts before committing to a format —
    see [CLI → Following the sink](../cli/commands.md#following-the-sink) — and that read
    (`ac3::audio::sink_capabilities`) is real today only on ALSA (see
    [Linux](linux.md#reading-a-sinks-own-edideld-roadmap-ux9)). WASAPI answers "will this
    endpoint accept this format" (`IsFormatSupported`, what `enumerate_render_devices()` already
    uses) but does not re-expose the sink's own raw EDID-carried Short Audio Descriptors to
    user-mode code — the driver consumes them internally to decide what to offer and no
    documented public API was found that hands the source data back. `play` falls back to the
    same `IsFormatSupported` probe here, exactly as it always has.

### Per-process loopback and device notifications (roadmap UX11)

Two more WASAPI paths, both added for the [AC3Forge Crucible](../crucible/index.md) and
both Windows-only in the backend tree:

- **`Capture::start_process_loopback(pid, mode, format)`** — `ActivateAudioInterfaceAsync`
  with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, which captures what one process tree
  renders and nothing else, whichever endpoint it renders to. Needs Windows 10 build 20348 or
  later; `process_loopback_available()` asks the kernel (`RtlGetVersion`, not the manifest-bound
  `GetVersionEx`) and `audio_backend().process_loopback` reports the same answer. A
  process-loopback client has no `GetMixFormat`, so the caller states the format and the engine
  converts — 48 kHz float at two or eight channels is confirmed. The tap is polled and
  silence-filled exactly like endpoint loopback, because a quiet process delivers no packets.
  The library refuses a process id nobody owns (`kProcessNotFound`) because the OS does not: such
  a tap activates and delivers zeros forever, as does a tap whose process has since exited.
- **`DeviceWatcher`** — `IMMNotificationClient` registered with the MMDevice enumerator on a
  worker thread that owns the COM apartment. Default-changed events are delivered for the
  console role only (the one every sink in this tree opens; the other two roles would report
  the same change twice more), and property-value changes (a volume nudge, a rename) are
  dropped as noise. The listener is hand-rolled `IUnknown` rather than a WRL `RuntimeClass`,
  for the same include-order reason `capture.cpp` spells its GUIDs out by hand.

!!! note "Both confirmed on the development workstation, 2026-09-03"
    Through the raw WASAPI spike first (`apps/crucible/spikes/README.md`, S1: sixteen taps at
    once, exact separation, the mute and exclusive-mode hazards) and then through these library
    entry points themselves (`s1_library_tap`), on Windows 11 build 26200. Not yet exercised on
    a hosted CI runner beyond the device-free contract in `tests/audio/test_audio_backend.cpp`,
    which does start and stop a real watcher wherever the backend exists.

`MonitorSink::start` also takes a `low_latency` flag, added for the demo's one-block mode: it
asks `IAudioClient3::GetSharedModeEnginePeriod` for the engine's smallest shared-mode period
for the stream's format and opens with `InitializeSharedAudioStream` at that, falling back to
the default period where the interface is missing or the engine refuses the format at that
size; the other backends take the flag and ignore it. On the workstation's Realtek endpoint
it changes nothing, and the spike's `period_probe` (`apps/crucible/spikes/s5_latency/`) says
why: the engine answers 480 frames, 10 ms, for the default, fundamental, minimum and maximum
period alike, for the demo's float format and for the mix format both. `IAudioClient`'s
"minimum 3 ms" device period is the exclusive-mode floor, not a shared-mode offer. A device
that offers 2.7 ms will get it; this one does not.

### Passthrough capture

The reverse direction — a WASAPI endpoint *delivering* IEC 61937 rather than PCM, which is what
an HDMI or S/PDIF capture card gives, and what a render-endpoint loopback gives when the app
playing into it is bitstreaming — is the same framing read backwards, and the same facts govern
it. The bursts arrive as ordinary PCM16 samples: `IAudioClient` has no way to say "this is
Dolby Digital", and `Capture` converts them to float by dividing by 32768, which loses nothing.

`ac3::iec61937::PassthroughDetector` recognises the framing from those floats — a `Pa`/`Pb`
preamble at a repetition period with a `0x0B77` syncframe behind it — and `ac3cli record`
switches to writing the elementary stream instead of encoding the bursts as audio; `ac3cli
live` stops with an error instead. `ac3cli unspdif` does the same job on a capture already
saved to disk. `carrier_from_capture` is the conversion back to PCM16 words, exact for all
65536 of them.

!!! warning "Passthrough capture has never been confirmed against a real capture device"
    No HDMI or S/PDIF capture card, and no loopback of a genuinely bitstreaming player, has been
    available during development — the same gap the passthrough *output* side has, from the same
    missing hardware. What is verified: the burst de-framing itself round-trips byte-exactly
    against both this project's own wrapper and FFmpeg's `spdif` muxer, for AC-3 and E-AC-3 and
    for both 16-bit word orders; the float→PCM16 recovery is exact for every int16 value; and
    the detector reaches "not a bitstream" on real audio and silence alike. What is not: that a
    real device's samples reach `Capture` unmodified in the first place. A shared-mode endpoint
    that resamples or mixes would destroy the bursts before anything here saw them, which shows
    up as no detection rather than as wrong output.

## Qt (GUI only)

`ac3gui` needs a **prebuilt Qt 6.5+ kit**, discovered by `cmake/FindQt6.cmake` — never from
vcpkg. `AC3FORGE_BUILD_GUI` defaults **ON** on Windows. `FindQt6.cmake` widens
`CMAKE_PREFIX_PATH` to the usual install roots (`C:/Qt`, `%USERPROFILE%/Qt`, `D:/Qt`); to point
at a specific kit explicitly:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_QT_ROOT=D:/Qt/6.8.3/msvc2022_64
```

See [Qt](../building.md#qt) for the full discovery order and options.

## Building

```bash
cmake --preset config-windows-msvc-debug
cmake --build --preset build-windows-msvc-debug
ctest --preset test-windows-msvc-debug
```

Drop `-debug` for a Release build. Swap `msvc` for `llvm` throughout to build with clang-cl
instead:

```bash
cmake --preset config-windows-llvm-debug
cmake --build --preset build-windows-llvm-debug
ctest --preset test-windows-llvm-debug
```

`VCPKG_ROOT` must point at a vcpkg checkout (it supplies Catch2 — plus Boost and Tracy only if
you opt into the `adm`/`profiling` features; see [building.md](../building.md)). See
[Presets](../building.md#presets) for the full preset table and the `ci-windows-msvc` /
`ci-windows-llvm` workflow presets that chain all three steps.

## Packaging

```bash
cpack --preset pack-windows-msvc
```

Produces a ZIP, plus an NSIS installer on top if `makensis` is on `PATH` (CI's `windows-msvc` leg
installs it via Chocolatey automatically and fails the leg if the installer doesn't come out the
other end — see [releasing.md](../releasing.md#winget-manifest); locally, install NSIS yourself
or `cpack` falls back to ZIP-only with a `message(WARNING ...)` explaining why).
`pack-windows-llvm` is the clang-cl equivalent, and `pack-windows-msvc-arm64` the ARM64 one
(below). `windows-msvc` is the only leg packaged continuously — CI packages it on every push and
uploads the result as a workflow artifact, a standing smoke test of the packaging path; tagged
releases package every `release_package` leg (Windows x64/arm64, Linux x64/arm64, macOS). See
[Packaging](../building.md#packaging).

The NSIS installer also registers `.ac3` and `.ec3` as `AC3Forge.Stream`, pointing
`shell\open\command` at the installed `ac3gui.exe` and nudging Explorer to pick up the change with
`SHChangeNotify`, and reverses both keys on uninstall — `CPACK_NSIS_EXTRA_INSTALL_COMMANDS`/
`_UNINSTALL_COMMANDS` in `cmake/Packaging.cmake`. CI now builds and verifies the installer itself
on every push; running it and double-clicking a `.ac3` file to confirm the file association end
to end is still a manual, unautomated check.

## ARM64 (roadmap DR8)

A third Windows leg, `windows-msvc-arm64`, targets GitHub's hosted `windows-11-arm` runner — real
ARM64 hardware, not x64 emulation. It shares every file the two x64 legs above use; only the
vcpkg triplet and the resolved MSVC tools directory differ.

**Toolchain.** `cmake/vcpkg/triplets/arm64-windows-msvc.cmake` sets `VCPKG_TARGET_ARCHITECTURE
arm64` (same CRT/library linkage policy as `x64-windows-msvc.cmake`).
`cmake/toolchains/windows.msvc.toolchain.cmake` resolves the target-appropriate MSVC tools
subdirectory the same way `linux.gcc.toolchain.cmake`/`macos.llvm.toolchain.cmake` already resolve
their own arm64 legs — generically, not hardcoded — and, for the arm64 case specifically, tries
more than one candidate directory: `bin/Hostarm64/arm64` (a native ARM64-hosted toolset) first,
falling back to `bin/Hostx64/arm64` (the older x64-hosted cross toolset, which still produces
ARM64 binaries, just via x64 tools running under Windows' x64 emulation). Which one this runner's
VS Build Tools install actually ships was genuinely unconfirmed when this leg was written — it
needed a real CI run to answer, the same "confirmed empirically, not assumed" standard the rest of
this codebase holds itself to (see e.g. `cmake/vcpkg/triplets/arm64-linux-gcc.cmake`'s own
`VCPKG_FORCE_SYSTEM_BINARIES` comment). `cmake/toolchains/windows.msvc.environment.cmake`'s
`vcvarsall.bat` bootstrap and `.github/actions/setup-msvc-env`'s CI-side environment loader probe
the equivalent pair of `vcvarsall.bat` arguments (`arm64` native, then `amd64_arm64` cross) for the
same reason — the target architecture's CRT/Windows SDK library directories have to match whichever
compiler actually got picked, or linking fails outright with a machine-type mismatch. Which of the
two candidates actually wins is still open as of this writing (the leg's first real run failed one
step earlier than Configure — see "Toolset generation" below); this section gets a follow-up update
once that is resolved.

**Toolset generation is older than the x64 images, confirmed empirically.** The `windows-11-arm`
hosted runner's VS Build Tools install carries MSVC 14.44.35207 (VS2022, roughly the 17.14
generation) — older than `windows-latest`'s x64 image, which is on the 14.5x ("VS 2026"/18.x)
toolset every other Windows leg's `msvc_toolset` pin (`.github/toolchain-versions.json`) is written
against. This is a genuine difference between the two runner images' own update cadences, not a
misconfiguration — `vswhere`/`vcvarsall` resolution and the Ninja install both worked fine on the
ARM64 runner in the same run that surfaced this. `_build.yml`'s "Report and assert toolchain
versions" step accordingly does not hard-assert the shared pin for `windows-msvc-arm64` the way it
does for `windows-msvc`; it reports whatever toolset this runner actually has instead, the same
report-only shape already used for `macos-llvm`'s unpinnable Homebrew LLVM, with the reasoning
recorded in that step's own case arm.

**CLI-only, for now.** Unlike every other packageable Windows/Linux/macOS leg, `windows-msvc-arm64`
does not build `ac3gui` — `AC3FORGE_BUILD_GUI` is off in `CMakePresets.json`'s
`windows-msvc-arm64` preset. Qt's only Windows ARM64 kit for the pinned 6.8.3 (the first Qt LTS
with official Windows ARM64 support at all) is `win64_msvc2022_arm64_cross_compiled` — a
cross-compile kit that expects a paired `win64_msvc2022_64` install to supply its host build
tools (`moc`/`uic`/`rcc`), and `aqtinstall`/`jurplel/install-qt-action` have a documented CI bug
against exactly that combination (`qtpaths.bat` pointing at the wrong x64 setup). That is real
complexity a *native*-ARM64-host build does not actually need to take on for a first pass, so this
leg stays CLI-only, the same deliberately-scoped shape `linux-llvm-asan-ubsan` already uses
elsewhere in the matrix for a different reason. Revisiting this is a natural fast-follow once Qt
ships a genuinely native-hosted ARM64 Windows kit.

**Gold-reference gate.** `choco`'s `ffmpeg` package is x64-only, so this leg installs a static
`win-arm64` FFmpeg build from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases/tag/latest) instead of
`choco install ffmpeg` — see the "Install ffmpeg (Windows ARM64)" step in `_build.yml`.

**Status.** New and `experimental: true` (`continue-on-error`) in `_build.yml` until it has proven
green over real runs, the same promotion path `macos-llvm` and the Android leg both went through —
see that file's own header comment for the mechanics. The `windows-11-arm` runner label is
confirmed enabled for this repository/org: the leg's first real run picked up a runner immediately
and passed checkout, "Setup MSVC environment" (`vswhere`/`vcvarsall` resolution) and the Ninja
install, failing only at the toolset-version assertion described above (since relaxed to
report-only for this leg). Iteration continues from there.

**Unsigned binaries.** Like every other Windows binary this project ships today, this leg's output
is unsigned — Authenticode signing (roadmap `DR6`) is blocked project-wide on acquiring a
certificate, not on code, and that applies here exactly as it does to the x64 legs. It is worth
stating plainly for ARM64 specifically: SmartScreen will warn on install, and an ARM64 user has
fewer alternative trusted sources to fall back on than an x64 user does.

## CI

All three Windows legs — `windows-msvc`, `windows-llvm` and (experimentally) `windows-msvc-arm64`
— run on every push; the first two are **required**, alongside every other required leg — see the
full matrix in [Verified configuration](../building.md#verified-configuration), including the
Linux and macOS legs and the coverage/FFmpeg-validation legs. CI runs the CLI and GUI on both x64
Windows legs; the ARM64 leg runs the CLI only (see above) and does not yet block the build while it
proves itself out.
