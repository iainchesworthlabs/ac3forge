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

Produces a ZIP, plus an NSIS installer on top if `makensis` is on `PATH`. `pack-windows-llvm` is
the clang-cl equivalent. `windows-msvc` is the only leg packaged continuously — CI packages it on
every push and uploads the result as a workflow artifact, a standing smoke test of the packaging
path; tagged releases package all four `release_package` legs (Windows, Linux x64/arm64, macOS).
See [Packaging](../building.md#packaging).

The NSIS installer also registers `.ac3` and `.ec3` as `AC3Forge.Stream`, pointing
`shell\open\command` at the installed `ac3gui.exe` and nudging Explorer to pick up the change with
`SHChangeNotify`, and reverses both keys on uninstall — `CPACK_NSIS_EXTRA_INSTALL_COMMANDS`/
`_UNINSTALL_COMMANDS` in `cmake/Packaging.cmake`. Configure/build-verified only: no `makensis`
locally to actually run the installer and double-click a `.ac3` file afterwards.

## CI

Both Windows legs — `windows-msvc` and `windows-llvm` — run on every push and are **required**,
alongside every other required leg — see the full matrix in [Verified
configuration](../building.md#verified-configuration), including the Linux and macOS legs and
the coverage/FFmpeg-validation legs. CI runs the CLI and GUI on both Windows legs.
