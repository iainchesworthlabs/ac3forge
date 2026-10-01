# Windows

ICL Forge builds and is tested on Windows with two toolchains, MSVC and clang-cl, CLI and GUI
alike: Windows MSVC in the merge queue, and both in the run after a merge to main (see
[CI for many agents](../ci-agentic.md)). This page covers what is specific to Windows; for the
full preset reference, options list and troubleshooting, see [Building from source](../building.md).
Crucible's kernel driver and driver VM live under
[`apps/windows/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/apps/windows/README.md),
separate from the application in `apps/crucible/`.

## Status

| | |
|---|---|
| What runs here | The library, `forge`, `forge-gui`, Hearth (`hearth`) and Crucible |
| Build | MSVC and clang-cl, x64, both building the GUI; MSVC in the merge queue, both in the run after a merge to main. The GUI is on by default |
| Capture and monitor playback | Confirmed on real hardware — a Realtek endpoint, live microphone capture through encode to playback |
| Windows Spatial Sound (`forge spatial`) | Confirmed on real hardware, with Windows Sonic enabled; nobody has listened to check the positions. E-AC-3 objects only |
| IEC 61937 passthrough output | Confirmed on real hardware — an Onkyo TX-RZ740 over HDMI locks AC-3 (Dolby Digital 5.1), E-AC-3 (Dolby Digital Plus 5.1) and signed Atmos (JOC objects, decoded to 5.0.4) through `PassthroughSink` itself |
| AC-4 | Decoded and encoded by `forge` and `forge-gui`, decoded by Hearth; `forge-gui`'s live capture takes AC-3 and E-AC-3 only. WASAPI has no IEC 61937 subformat for AC-4, so `PassthroughSink` refuses it and `forge play` decodes it to PCM. v0.10.0-beta.1, the latest release, predates the AC-4 decoder and encoder |
| Passthrough capture | **Never confirmed** — no HDMI or S/PDIF capture card has been available |
| Crucible's null sink | A kernel driver, **test-signed only**; a default-settings machine refuses to load it — see [the driver page](windows-driver-acx.md) |
| ARM64 | One CI leg, still marked experimental and run in the nightly run; it builds the CLI only, and its packages have shipped since v0.10.0-beta.1 |

The table below separates x64 from ARM64. Package status and runtime evidence are separate: a
package can exist even where its hardware-facing paths have not been exercised.

--8<-- "docs-snippets/generated/platform-windows.md"

## Toolchains

CI builds with **MSVC 14.51** (the Visual Studio 2026 Build Tools) and **clang-cl 22.1**; the
development workstation is Windows 11 (build 26200).

Every Windows preset chainloads a toolchain file that locates `cl.exe`/`clang-cl.exe` and
`link.exe` itself (via `vswhere` and `vcvarsall.bat` if a Developer PowerShell hasn't already
set one up), so configuring works from an ordinary shell — no need to launch a Developer
Command Prompt first. clang-cl is MSVC-ABI compatible, so it links the same vcpkg packages and
the same prebuilt Qt kit as the MSVC build. See
[The compiler is pinned, not PATH-found](../building.md#the-compiler-is-pinned-not-path-found)
for the mechanics.

## Audio backend: WASAPI

On Windows, the five pieces that touch sound hardware are all implemented over **WASAPI**:

- **`iclforge::audio`** — live input/loopback capture through a lock-free SPSC ring.
- **`iclforge::iec61937::PassthroughDetector`** — recognising, from that same capture, that the
  endpoint is handing over IEC 61937 bursts (AC-3, E-AC-3 or AC-4) rather than PCM.
- **`iclforge::audio::PassthroughSink`** — exclusive-mode/direct bitstream output, for both AC-3 and
  E-AC-3 burst framing (IEC 61937). AC-4 (IEC 61937-14) is refused here with
  `kUnsupportedFormat`: WASAPI asks for a compressed format by its `KSDATAFORMAT_SUBTYPE_IEC61937_*`
  subformat, and the Windows SDK (`ksmedia.h`, 10.0.26100) defines none for AC-4.
- **`iclforge::audio::MonitorSink`** — shared-mode PCM playback: a non-bitstreamed preview/monitor
  path that decodes what is being encoded and plays it back on an ordinary output. It is also
  where `forge monitor` and `forge play` send an AC-4 stream, decoded, and where an AC-4
  stream's objects arrive rendered to speakers by the layout renderer.
- **`iclforge::audio::SpatialObjectSink`** — `ISpatialAudioObjectRenderStream`: decoded
  Atmos objects go out as dynamic objects at their real OAMD positions, and the bed's LFE (never
  a JOC output, TS 103 420 §6.3.2.2) as a static one. Behind `forge spatial`. This is the one
  path that lets Dolby's own renderer engage with this project's reconstructed objects at all — a
  licensed decoder otherwise refuses object decoding without a signing key this project doesn't
  ship (see [Object signing](../concepts/object-signing.md)) — and needs nothing but a spatial-
  sound-capable endpoint to do it, no AVR and no key. It takes E-AC-3 only: `forge spatial`
  reads no AC-4 stream (on a spatial-enabled endpoint it answers "not a valid E-AC-3 stream", exit
  code 2).

What each can carry:

| Path | Carries | Does not carry |
|---|---|---|
| Capture (`iclforge::audio`) | PCM from an input, an endpoint's loopback or one process tree; IEC 61937 bursts that arrive as PCM, which the detector recognises for AC-3, E-AC-3 and AC-4 | |
| `MonitorSink` | PCM the library decoded: AC-3, E-AC-3 (an Atmos stream's bed) and AC-4, whose objects the layout renderer puts on speakers | A bitstream |
| `PassthroughSink` | IEC 61937 bursts of AC-3 and E-AC-3, the latter with its JOC objects | AC-4, refused with `kUnsupportedFormat` |
| `SpatialObjectSink` | E-AC-3 object streams, as dynamic objects and a static LFE | AC-4 objects |

These five are not equally verified on hardware, and the project's own documentation
is explicit about the difference.

!!! note "MonitorSink is confirmed on hardware"
    `forge monitor` and `forge live`'s monitor leg have played decoded AC-3 and E-AC-3
    (including an Atmos stream's 5.1 bed) through a Realtek output in real time, and a live
    microphone capture→encode→monitor session has run end to end. Building this path on
    hardware surfaced two bugs that neither unit tests nor silent/synthetic input
    would have caught — a fixed submit-readiness threshold smaller than one chunk, which
    let the ring buffer silently perform a partial write while reporting failure, and the live
    pipeline's Atmos metering step writing past the end of a buffer sized for the object count
    rather than the bed's fixed six channels. Both are fixed; see
    `src/audio/src/backend/windows/monitor.cpp` and `run_live` in
    `apps/cli/commands/live_audio.cpp`.

!!! note "MonitorSink: a format refusal is told apart from a WASAPI failure, and any rate plays"
    `MonitorSink::start()` reports `MonitorError::kFormatRejected` for `AUDCLNT_E_UNSUPPORTED_FORMAT`
    (`0x88890008`), checked on both the `IAudioClient3` low-latency path and the ordinary
    fallback; every other failure in `start()` still reports `kComFailure`. That was added on
    2026-09-22, debugging why `iclforge-tests "[monitor-unplug]"` would not open the "AV Receiver
    (NVIDIA High Definition Audio)" HDMI endpoint the exclusive-mode passthrough confirmation
    below used: `start()` had no way to say why beyond "a Windows audio (WASAPI/COM) call
    failed", and a standalone WASAPI probe written outside this codebase found the refusal at
    48 kHz on an endpoint whose shared-mode engine ran at 192 kHz, its Advanced-tab "Default
    Format", while the same formats at 192 kHz opened.

    On 2026-09-26 Hearth would not play a 44.1 kHz AC-3 file on a machine whose only output,
    "Speakers (Realtek(R) Audio)", runs its shared-mode engine at 48 kHz: every play ended in
    `kFormatRejected`. The earlier reading, that such an endpoint "converts bit depth but not
    sample rate", was incomplete. That is what WASAPI's shared mode does for a client that asks
    for nothing more: it takes the mix format's own rate and channel count and refuses any other
    with `AUDCLNT_E_UNSUPPORTED_FORMAT`, unless the stream is initialised with
    `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`, which puts the engine's own resampler and channel
    matrixer in front of the mix (`AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY` picks its better
    filter). `start()` set neither flag, so every rate but the endpoint's own was refused, and a
    caller that opens at each item's own rate - Hearth - could not play a song there at all.
    A probe of that Realtek endpoint gave `0x88890008` at 44.1 kHz and at 96 kHz without the
    flag and success at both with it; 48 kHz opened either way. `start()` now sets both flags on
    its ordinary shared-mode initialise. The `IAudioClient3` low-latency path is unchanged, and
    still falls back to that initialise when the engine will not run the format at its smallest
    period, so a format that needs converting plays through the fallback. `kFormatRejected`
    stays for what the converter cannot take. `"[monitor-live]"` opens at 44.1, 48 and 96 kHz on
    the default output, with silence, and needs each to play; `"[hearth-device]"` does the same
    through Hearth's own output sink, which no other test opens on a real device.

!!! note "Playback position, pause and flush are confirmed; a multichannel patch is not"
    `MonitorSink`'s playback position, `pause()`/`resume()` and `flush()` have been exercised
    against the default Realtek endpoint by `iclforge-tests "[monitor-live]"` — a hidden case, since it
    needs a sound card and makes a noise: the position advances with the device's own clock, a
    pause holds it while the queue goes on taking frames, a flush drops both buffers and the
    count restarts, and playback resumes from the next submit. `forge identify` walked the tone
    across that endpoint's own speakers, over a 5.1 layout it can place only two channels of, and
    with the pair swapped.

    What that machine cannot show is a **multichannel** patch: both its endpoints are stereo, and
    which speaker an output actually reaches is exactly what a two-channel device cannot
    disprove. That check needs an 8-channel endpoint — an AVR over HDMI as LPCM — and is
    `forge identify 0 7.1.4 3` heard from the speaker each printed line names, then the same
    command with a patch that swaps two channels heard to swap those two speakers and nothing
    else.

!!! note "SpatialObjectSink is confirmed against a real spatial endpoint"
    `forge spatial` has activated `ISpatialAudioObjectRenderStream` and rendered a real Atmos
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
    documented shape); a third-party bed-plus-objects Annex E stream would be needed to
    exercise the rest of `oba::bed_labels()` against a verified coded-channel-order mapping.

!!! note "Exclusive-mode passthrough bitstreaming is confirmed against a real receiver on Windows"
    An Onkyo TX-RZ740 was cabled to a Windows workstation via an Nvidia GPU's HDMI audio
    endpoint ("AV Receiver (NVIDIA High Definition Audio)"). `IsFormatSupported`
    answered yes for both `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL` and
    `..._DOLBY_DIGITAL_PLUS` — the first real device this has ever happened on — and `forge
    play` through `PassthroughSink` itself, not the PCM16-WAV workaround, locked the receiver
    onto AC-3 (Dolby Digital 5.1), then E-AC-3 (Dolby Digital Plus 5.1), then a signed Atmos
    stream (`forge atmos ... sign-objects`), which the receiver decoded as **Atmos/DD+, 48 kHz
    in, 5.0.4 out**, with the object's motion confirmed audible — the same result the Shield app
    got on this same receiver (see `docs/platforms/android.md`). Clean delivery throughout
    (0 underruns on the confirming Atmos run).

    Getting there surfaced two real defects in `PassthroughSink`, both fixed: a cross-thread
    WASAPI crash (`Activate`/`Initialize` on the caller's thread, `Start`/`GetBuffer` on a
    worker thread — fine for `MonitorSink`'s shared-mode path, fatal deep inside `AUDIOSES.DLL`
    for a real exclusive-mode bitstream client) and a stats bug where the per-callback
    "bursts rendered" counter truncated to zero almost every WASAPI callback, hanging the CLI's
    drain-wait loop forever after real playback had already finished. See
    `src/audio/src/backend/windows/passthrough.cpp`.

    The AC-3 bursts are byte-exact against FFmpeg's `spdif` muxer, and the E-AC-3 burst framing
    (data type 0x15, the 24576-byte/4x-carrier-rate burst, multi-syncframe accumulation, `Pd`
    in bytes not bits) is independently verified against both FFmpeg's `spdif_header_eac3` and
    Microsoft's own "Representing Formats for IEC 61937 Transmissions" documentation, plus
    round-trip and real-audio unit tests — all of which held up against the real device too.

!!! note "An endpoint that goes away mid-stream"
    Unplugging the cable, switching the receiver off or disabling the endpoint invalidates the
    stream: WASAPI answers `AUDCLNT_E_DEVICE_INVALIDATED` to the render thread's next call, and
    stops signalling the event it waits on, so a wait that times out asks the endpoint for its
    padding rather than waiting again. Either answer stops the sink — `running()` turns false,
    `position()` reports nothing, `submit()` refuses — and `start()` opens again with no
    `stop()` first, on the same endpoint once it is back. `iclforge-tests "[passthrough-unplug]"` and
    `"[monitor-unplug]"` are hidden cases that take a person through it.

    **`[passthrough-unplug]` is confirmed**, against the same AV Receiver endpoint the exclusive-
    mode validation above used: the cable was pulled mid-stream, and the sink noticed on its own —
    `running()` false, `position()`/`can_submit()`/`submit()`/`paused()` all answering as a stopped
    sink would, `flush()` returning at once, `pause()`/`resume()` both refusing with `kNotRunning`
    — then a second `start()`, with no `stop()` in between, reopened the same endpoint and played
    real frames once the cable went back in. `[monitor-unplug]` (`MonitorSink`'s shared-mode path)
    has not yet been reported from this workstation's own receiver — its own hidden case opens the
    system's current *default* render endpoint rather than a named one, so what it actually
    exercises depends on whatever that is at the time.

    `SpatialObjectSink` answers the same way, over its own two calls:
    `BeginUpdatingAudioObjects` failing outright, or — a removed endpoint need never signal the
    render-ready event again either — a wait that times out reading back
    `GetMaxDynamicObjectCount` on the `ISpatialAudioClient` instead of `GetCurrentPadding`. Not
    the stream's own `GetAvailableDynamicObjectCount`: Microsoft's reference for that call says
    not to use it once streaming has started, since `BeginUpdatingAudioObjects` already provides
    the same count from then on - the client-level call carries no such restriction.
    `iclforge-tests "[spatial-unplug]"` is its own hidden case, not yet run against real hardware.

!!! note "No EDID/ELD backend on Windows"
    `forge play` asks a chosen sink what it actually accepts before committing to a format —
    see [CLI → Following the sink](../forge/cli/commands.md#following-the-sink) — and that read
    (`iclforge::audio::sink_capabilities`) exists today for ALSA and for PipeWire (see
    [Linux](linux.md#reading-a-sinks-own-edideld)). WASAPI answers "will this
    endpoint accept this format" (`IsFormatSupported`, what `enumerate_render_devices()` already
    uses) but does not re-expose the sink's own raw EDID-carried Short Audio Descriptors to
    user-mode code — the driver consumes them internally to decide what to offer and no
    documented public API was found that hands the source data back. `play` falls back to the
    same `IsFormatSupported` probe here, exactly as it always has.

    The one avenue checked and ruled out: WMI's `root\wmi` monitor provider
    (`WmiMonitorID`/`WmiMonitorDescriptor`) does expose raw EDID bytes on Windows, but it is a
    *display* API keyed to the desktop/monitor topology, not an audio one. Tried against the
    Onkyo TX-RZ740 used for the passthrough confirmation above:
    `Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID` sees only the two actual
    desktop monitors on this machine (an Acer and a Samsung, both on DisplayPort); the receiver,
    despite having a live HDMI audio endpoint WASAPI opens and plays through, is not registered
    as a display Windows extends onto at all, so there is no WMI monitor entry to read its SADs
    from even if this project wired one up.

### Per-process loopback and device notifications

Two more WASAPI paths, added to the shared audio layer for [Crucible](../crucible/index.md)
and available to anything else that links it, both Windows-only in the backend tree:

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

`iclforge::iec61937::PassthroughDetector` recognises the framing from those floats — a `Pa`/`Pb`
preamble at a repetition period with a syncframe behind it (`0x0B77` for AC-3 and E-AC-3, the
AC-4 sync word for AC-4) — and `forge record`
switches to writing the elementary stream instead of encoding the bursts as audio; `forge
live` stops with an error instead. `forge unspdif` does the same job on a capture already
saved to disk. `carrier_from_capture` is the conversion back to PCM16 words, exact for all
65536 of them.

!!! warning "Passthrough capture has never been confirmed against a real capture device"
    No HDMI or S/PDIF capture card, and no loopback of a bitstreaming player, has been
    available during development — the same gap the passthrough *output* side has, from the same
    missing hardware. What is verified: the burst de-framing itself round-trips byte-exactly
    against both this project's own wrapper and FFmpeg's `spdif` muxer, for AC-3 and E-AC-3 and
    for both 16-bit word orders; the float→PCM16 recovery is exact for every int16 value; and
    the detector reaches "not a bitstream" on real audio and silence alike. What is not: that a
    real device's samples reach `Capture` unmodified in the first place. A shared-mode endpoint
    that resamples or mixes would destroy the bursts before anything here saw them, which shows
    up as no detection rather than as wrong output.

## Qt (the windows: GUI, Hearth, Crucible)

`forge-gui` needs a **prebuilt Qt 6.5+ kit**, discovered by `cmake/FindQt6.cmake` — never from
vcpkg. Hearth's and Crucible's windows need Qt 6.8 or later; on an older kit each is skipped with
a warning and its engine still builds. CI installs Qt 6.10.3. `ICLFORGE_BUILD_GUI` defaults **ON**
on Windows. `FindQt6.cmake` widens
`CMAKE_PREFIX_PATH` to the usual install roots (`C:/Qt`, `%USERPROFILE%/Qt`, `D:/Qt`); to point
at a specific kit explicitly:

```bash
cmake --preset config-windows-msvc-debug -DICLFORGE_QT_ROOT=D:/Qt/6.8.3/msvc2022_64
```

See [Qt](../building.md#qt) for the full discovery order and options.

## Building

```bash
cmake --preset config-windows-msvc-debug
cmake --build --preset build-windows-msvc-debug
ctest --preset test-windows-msvc-debug
```

Drop `-debug` for a Release build. The window of Hearth builds by default (`ICLFORGE_BUILD_HEARTH`
is on); Crucible is opt-in with `-DICLFORGE_BUILD_CRUCIBLE=ON`. Swap `msvc` for `llvm`
throughout to build with clang-cl instead:

```bash
cmake --preset config-windows-llvm-debug
cmake --build --preset build-windows-llvm-debug
ctest --preset test-windows-llvm-debug
```

`VCPKG_ROOT` must point at a vcpkg checkout (it supplies Catch2 and {fmt}, mbedTLS, cpp-httplib,
libFLAC, Opus and mdns through the `hearth` feature the desktop presets select, and Boost and
Tracy only if you opt into the `adm`/`profiling` features; see [building.md](../building.md)). See
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
(below). `windows-msvc` is the only leg packaged in ordinary runs — CI packages it in every run
that builds it (the run after a merge, the nightly run) and uploads the result as a workflow
artifact, a standing smoke test of the packaging path; a tagged release packages every
`release_package` leg (Windows x64 and ARM64, Linux x64 and arm64, macOS). See
[Packaging](../building.md#packaging).

An x64 installer built from main (`iclforge-<version>-win64.exe`) carries `forge`, `forge-gui` and
`hearth`; the one in v0.10.0-beta.1, the latest release, predates Hearth's packaging and
carries the first two. The Start Menu folder has an entry for `forge-gui`, one for `hearth`, and
a "forge command prompt" with the install's `bin` on `PATH`. The ZIP splits by component: the
runtime archive, an `iclforge-dev-*` library archive, an `iclforge-hearth-*` archive and an
`iclforge-crucible-*` archive, of which the release has the first two. Crucible stays out of the
installer while its driver is test-signed (`cmake/CPackProjectConfig.cmake`).

The NSIS installer also registers `.ac3` and `.ec3` as `IclForge.Stream`, pointing
`shell\open\command` at the installed `hearth.exe` (`forge-gui.exe` in a build with no Hearth)
and nudging Explorer to pick up the change with `SHChangeNotify`, and reverses both keys on
uninstall — `CPACK_NSIS_EXTRA_INSTALL_COMMANDS`/`_UNINSTALL_COMMANDS` in `cmake/Packaging.cmake`.
Nothing registers `.ac4`. The installer and every binary in it are unsigned (Authenticode signing
waits on a certificate), so SmartScreen may warn on install. CI builds and verifies the installer
itself in every run that packages Windows; running it and double-clicking a `.ac3` file to confirm the file association
end to end is still a manual, unautomated check.

## Windows Firewall

`hearth` and `hearth-testsink` each open a socket a Windows Firewall rule has to allow:
`hearth`'s mDNS browse for `_sendspin._tcp` players, and `hearth-testsink`'s own Sendspin
listener and mDNS advertisement. Rather than leave this to Windows' own "these features have been
blocked" prompt, `iclforge::sendspin::firewall::ensure_inbound_rule()`
(`src/sendspin/include/iclforge/sendspin/firewall.hpp`) adds the rule itself, through the same
`INetFwPolicy2` COM policy object the Settings app's firewall page edits, the first time it finds
none already there for that executable and port.

Adding a rule needs an elevated token. `ensure_inbound_rule()` adds it directly when the calling
process is already elevated; otherwise, on an interactive desktop session, it relaunches its own
executable once with a UAC prompt (`--ac3-sendspin-firewall-helper`,
`maybe_run_as_firewall_helper_and_exit()` in the same header) to make just that one COM call, then
returns to running unelevated. A non-interactive session - a service, a CI runner - is left for
Windows' own prompt instead, same as before this existed. Every rule is scoped to the private and
domain network profiles only, never public, and to `LocalSubnet`: this only ever opens LAN
discovery/streaming traffic to the executable that is actually listening, never to the internet or
to a different program.

A loopback-only bind needs none of this - Windows does not gate loopback traffic - and is skipped
before `ensure_inbound_rule()` is ever called. `hearth-testserver`'s own `ServerHost` and
reference sink are loopback-only today and so never reach it; `iclforge-tests`' own `ServerHost`
fixtures are the same. Linux and macOS build a no-op implementation of the same two functions and
never show a prompt of any kind.

## ARM64

A third Windows leg, `windows-msvc-arm64`, runs on GitHub's hosted `windows-11-vs2026-arm` runner,
an ARM64 host. It shares every file the two x64 legs above use; only
the vcpkg triplet and the resolved MSVC tools directory differ. It is `experimental: true`
(`continue-on-error`, so a failure fails no run) and belongs to the nightly tier, so it runs in
the nightly run and in a release, not after each merge (`.github/ci/legs.jsonc`). Its packages
have shipped since v0.10.0-beta.1.

**Toolchain.** `cmake/vcpkg/triplets/arm64-windows-msvc.cmake` sets `VCPKG_TARGET_ARCHITECTURE
arm64` (same CRT/library linkage policy as `x64-windows-msvc.cmake`).
`cmake/toolchains/windows.msvc.toolchain.cmake` resolves the target-appropriate MSVC tools
subdirectory the same way `linux.gcc.toolchain.cmake`/`macos.llvm.toolchain.cmake` already resolve
their own arm64 legs — generically, not hardcoded — and, for the arm64 case specifically, tries
more than one candidate directory: `bin/Hostarm64/arm64` (a native ARM64-hosted toolset) first,
falling back to `bin/Hostx64/arm64` (the x64-hosted cross toolset, which also produces ARM64
binaries, via x64 tools running under Windows' x64 emulation).
`cmake/toolchains/windows.msvc.environment.cmake`'s `vcvarsall.bat` bootstrap and
`.github/actions/setup-msvc-env`'s CI-side environment loader probe the same pair of
`vcvarsall.bat` arguments (`arm64` native, then `amd64_arm64` cross), since the target
architecture's CRT/Windows SDK library directories have to match whichever compiler actually got
picked, or linking fails outright with a machine-type mismatch.

On the runner's `windows-11-vs2026-arm64` image (version 20260920.164.1) the first candidate
wins. The leg's log of 2026-09-28 shows `vcvarsall.bat arm64` succeeding and MSVC toolset
14.51.36231 under `bin\HostARM64\ARM64`, the toolset version the x64 legs report. That run built
the CLI and passed all 2,984 ctest cases. The image before it, `windows-11-arm`, carried MSVC
14.44.35207 (VS2022), which is why the "Report and assert toolchain versions" step in
`.github/actions/build-leg/action.yml` only prints the toolset for this leg instead of asserting
the pin the x64 legs share; the exception is still in place.

**CLI-only, for now.** Unlike every other packageable Windows/Linux/macOS leg, `windows-msvc-arm64`
does not build `forge-gui` — `ICLFORGE_BUILD_GUI` is off in `CMakePresets.json`'s
`windows-msvc-arm64` preset. Qt's only Windows ARM64 kit for the pinned 6.10.3 (Qt has offered one
since 6.8) is `win64_msvc2022_arm64_cross_compiled` (`python -m aqt list-qt windows desktop --arch
6.10.3`) — a cross-compile kit that expects a paired `win64_msvc2022_64` install to supply its
host build tools (`moc`/`uic`/`rcc`), and `aqtinstall`/`jurplel/install-qt-action` have a
documented CI bug against exactly that combination (`qtpaths.bat` pointing at the wrong x64
setup). That is complexity a *native*-ARM64-host build doesn't need for a first pass, so this
leg stays CLI-only. Revisiting this is a natural fast-follow once Qt ships a native-hosted ARM64
Windows kit.

**Gold-reference gate.** `choco`'s `ffmpeg` package is x64-only, so this leg installs a static
`win-arm64` FFmpeg build from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases) (a dated `autobuild-*`
release, pinned in the "Install ffmpeg (Windows ARM64)" step of `_ci-windows.yml`) instead of
`choco install ffmpeg`.

**Unsigned binaries.** Like every other Windows binary this project ships today, this leg's output
is unsigned — Authenticode signing is blocked project-wide on acquiring a
certificate, not on code, and that applies here exactly as it does to the x64 legs. It is worth
stating plainly for ARM64 specifically: SmartScreen will warn on install, and an ARM64 user has
fewer alternative trusted sources to fall back on than an x64 user does.

## CI

`windows-msvc` and `windows-llvm` run in the run after a merge to main, and `windows-msvc-arm64`
in the nightly run; Windows MSVC also builds in the merge queue. The stages are set out in
[CI for many agents](../ci-agentic.md#the-stages), and the full matrix, including the Linux and
macOS legs and the coverage and FFmpeg-validation jobs, is in
[Verified configuration](../building.md#verified-configuration). CI runs the CLI and GUI on both
x64 Windows legs; the ARM64 leg runs the CLI only (see above) and fails no run while it proves
itself out. A separate `windows-driver` job builds Crucible's null-sink driver, test-signed, on
GitHub's `windows-latest` whenever the Windows lane runs (see [the driver
page](windows-driver-acx.md)).
