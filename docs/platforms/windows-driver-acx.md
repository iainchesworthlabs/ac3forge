# The null-sink driver on ACX: plan

The Desktop Atmos Demo's silent output device ([windows-demo.md](windows-demo.md), "Routing:
two problems, not one") is a kernel driver because Windows offers no other way to make an
audio endpoint. The one in the tree today is derived from Microsoft's Simple Audio Sample: a
PortCls/WaveRT miniport, about 9,700 lines of sample code kept so that a few dozen lines of
ours can discard what they are given. It works, it is verified to the WDK's standard, and it
proved the model. This page plans its replacement on ACX, Microsoft's current framework for
audio drivers, decided on 2026-09-04 after a review of the options (recorded under
[Phase 6](windows-demo.md#phase-6-docs-ci-release)).

## Why ACX, in three sentences

Microsoft's own pages call PortCls "the current legacy model" and say ACX 1.1 "is recommended
for all new driver development"; it runs on Windows 10 version 2004 and later, below the
floor the demo already needs for process loopback (build 20348). An ACX driver is a plain
KMDF driver, so Driver Verifier's DDI compliance checking, which cannot be run against the
PortCls miniport today, applies in full. And the signing step ahead (an EV certificate, an
attestation submission, and from March 2027 an SBOM and VEX statement with it) is a per-binary
cost with a paper trail, so it is paid once, on the driver we intend to keep.

## What does not change

The demo, its scripts and its verification harness never see the framework. These stay
exactly as they are, and the port is not done until each is confirmed unchanged:

- The hardware id `ROOT\Ac3ForgeNullSink`, the service name `Ac3ForgeNullSink`, the device
  description "Desktop Atmos" and so the endpoint name "Speakers (Desktop Atmos)", which the
  demo's default silent-device filter matches. Every script under `apps/windows/driver/` and
  `apps/windows/driver-vm/` keys on these names.
- The one device format: 8 channels, 48 kHz, 16-bit, `KSAUDIO_SPEAKER_7POINT1_SURROUND`, so a
  game that can render surround reaches the demo's bed intact.
- The behaviour: rendered data is discarded and the position advances at the nominal rate,
  so the audio engine sees a device consuming exactly what it produces. The position and
  timing simulation is the one piece of real logic in the current driver and is carried
  across, not rewritten.
- The install shape (`pnputil /add-driver`, a root-enumerated device) and the package layout
  (`x64\Release\package\` with `.sys`, `.inf`, `.cat` and the test certificate beside it), so
  `Test-Driver.ps1`, `Verify-Driver.ps1`, `Deploy-Desk.ps1` and the Settings page's install
  and remove buttons work without edits.
- The licence: the ACX samples carry the same MS-PL as the Simple Audio Sample, so
  `apps/windows/driver/LICENSE` and the separate-work reasoning in its README stand.
- The build: the EWDK already used (kit 10.0.28000) carries the ACX headers and
  `acxstub.lib` under `km\acx\km\1.0\`; nothing new is installed.

## What the new driver is

Derived from `audio/Acx/Samples/AudioCodec` in microsoft/Windows-driver-samples, the way the
current one is derived from `audio/simpleaudiosample`: a virtual, root-enumerated audio
device that needs no hardware. The sample is a codec with a render circuit and a capture
circuit, a tone generator, a file writer, a keyword detector and a peak meter; the null sink
keeps the render circuit and the stream engine's shape and cuts everything else. Expected
size: a few hundred lines of ours over the sample's `CircuitHelper` and stream-engine
plumbing, against 9,700 today.

| File | Role | From the sample |
|---|---|---|
| `Driver.cpp` | `DriverEntry`, `EvtDriverDeviceAdd`, `AcxDriverInitialize` | as is |
| `Device.cpp` | device context, PnP and power callbacks, one static render circuit added in `EvtDevicePrepareHardware`, removed in `EvtDeviceReleaseHardware` | capture circuit, `CSaveData`, `CWaveReader` work items removed |
| `RenderCircuit.cpp` | the circuit: a host pin and a bridge pin, the format list, `EvtAcxPinSetDataFormat`, stream creation | volume and mute elements kept as pass-through state (the engine expects an endpoint to answer them); jack, peak meter removed |
| `NullStream.cpp` | the stream engine: `AllocateRtPackets`/`FreeRtPackets` from non-paged pool, `Run`/`Pause`, `GetPresentationPosition` and `GetLinearBufferPosition` from the timing simulation carried over from `minwavertstream.cpp` | tone generator and save-to-file paths removed |
| `Ac3ForgeNullSink.inx` | the current INF's names and ids on the sample's KMDF shape (`KmdfService`, `KmdfLibraryVersion`), gated at `10.0...22000` as today | the sample's `[Version]` and `.Wdf` sections |

Three things the current INF carries are dropped on the way, having been found to be sample
leftovers during the review: `SignatureAttributes.DRM` with `DRMLevel = 1300` and `PETrust`,
which are for protected-content paths and complicate signing for nothing; and the
`wdmaud`/`swmidi`/`redbook` associated filters and `Drivers\wave|midi|mixer` mappings, which
are legacy WDM audio plumbing. The last of those is checked rather than assumed (step 2
below), since `winmm` applications are still real.

## Steps

Each step ends at something the harness can confirm, and none starts before the one before it
has.

1. **Prototype** (about a day). The ACX sample's render circuit and stream engine, cut down to
   the null sink, under the current names, built by the current `.sln` shape. Exit:
   `Test-Driver.ps1` reports the "Desktop Atmos" device and its endpoint in the guest, the
   service running, WAV playback and speech synthesis rendering into it, no bugcheck. That is
   the same exit Phase 4 had, and it is the whole proof that ACX carries a virtual device on
   this machine and in VMware before anything else is spent.
2. **Parity** (about a day). The position and timing simulation carried across verbatim and
   compared: the current driver's `GetPosition` and `UpdatePosition` against the new engine's
   `GetPresentationPosition`, under the same playback. The 7.1 format the only one offered.
   The INF cruft dropped, then a `winmm` player run in the guest to confirm legacy
   applications still find the endpoint without the `wdmaud` entries; if they do not, the
   entries come back with a comment saying why. Exit: `Deploy-Desk.ps1` runs the window in
   the guest against the new driver and the signal path reads as it did on 2026-09-03.
3. **Verification** (one to two days). `Analyze-Driver.ps1` over the new tree: Code Analysis
   at the driver ruleset, CodeQL's `mustfix` and `recommended` suites, the DVL; every finding
   fixed or waived with a reason, as before. `Verify-Driver.ps1` with three additions that the
   framework makes possible or the review found wanting: `-Ddi` on by default, since the
   driver is now pure WDF; Driver Verifier's code-integrity checks (flag `0x02000000`), so the
   HVCI compliance an attestation-signed driver needs on a default Windows 11 install is
   demonstrated rather than assumed; and the exercise widened to the gaps the current record
   admits, namely a guest sleep and resume under a live stream, a sample-rate change on the
   endpoint, and driver unload with a stream open. Then the KASAN build and the one-past-the-end
   proof that the sanitizer is live, repeated. Exit: both tiers clean, written up on the plan
   page the way Phase 4's are.
4. **Install path** (half a day). `devcon` replaced by `SwDeviceCreate`: a small elevated
   helper (or the demo itself, elevated for the step) stages the package with `pnputil` and
   creates the root-enumerated device through the documented software-device API, and removes
   it the same way. `devcon` is a WDK sample tool and awkward to redistribute; the packaged
   demo's `bin/driver/` then carries scripts that need nothing beyond Windows. Exit:
   `install.ps1` and `remove.ps1` work on a machine with no WDK.
5. **Switch-over** (half a day). The ACX tree replaces `apps/windows/driver/Source`; the
   README rewritten as "what was cut from the ACX sample"; the plan page's driver sections
   and the CHANGELOG updated; the PortCls driver lives on in history, not beside the new one.
   Exit: `Test-Driver.ps1`, `Verify-Driver.ps1 -Kasan` and `Deploy-Desk.ps1` all pass from a
   clean checkout, and CI is green.
6. **Signing** stays where it is: the last item of Phase 6, on this driver. Nothing in this
   plan is signed.

About a week end to end. The harness is what makes it that short: every step's exit is a
script that already exists.

## Risks, and what answers each

- **ACX virtual devices in VMware.** The AudioCodec sample is documented against real
  machines. Step 1 is the answer, and it comes first for that reason; if the endpoint does not
  appear in the guest, the plan stops there and the reason is written down.
- **What the audio engine expects of an endpoint.** The sample's render circuit has volume,
  mute and jack elements; a null sink needs none of them for its own sake, but the engine may
  refuse or misreport an endpoint without some. Step 1 keeps volume and mute as pass-through
  state, and step 2 tries removing them one at a time.
- **The timing simulation.** It is the one place the PortCls driver has logic of its own, and
  the one place a port can quietly change behaviour (a position that runs fast or slow shows
  up as the audio engine's glitch counters, not as an error). Step 2 compares the two under the
  same playback rather than trusting the port.
- **HVCI.** The verification VM runs with memory integrity off, because test signing needs
  it; the first real test of the driver under HVCI is a signed build on a default install. The
  code-integrity verifier flag in step 3 is the closest thing available before that, and the
  HLK's HVCI readiness test is the check to run before submission.
- **Header versions.** The EWDK lays the ACX headers under a `1.0` directory while the sample
  builds with `ACX_VERSION_MAJOR=1`, `ACX_VERSION_MINOR=1`; step 1 confirms which version the
  kit actually carries and pins the project to it.

## Findings from the current driver that carry over

Phase 4's verification of the PortCls driver ([the record](windows-demo.md#phase-4-driver))
produced findings that are about kernel code derived from a Microsoft sample, not about
PortCls, and each applies to the ACX port from its first line. Rolled in here so the port
starts where the current driver ended rather than rediscovering them.

- **Every member initialised, at the declaration.** Seventy of the 157 Code Analysis findings
  against the sample were uninitialised members (C6001 and its relatives), fixed with default
  member initialisers in the class bodies. The ACX sample's contexts (`CODEC_DEVICE_CONTEXT`,
  the circuit and stream contexts, the stream engine) are written in the same style and get the
  same treatment as they are copied in, not after the analyser asks.
- **Pool is never executable.** The sample's `operator new` passed the caller's pool flags
  straight to `ExAllocatePool2`, and C28160 pointed out that a non-paged request could carry
  `POOL_FLAG_NON_PAGED_EXECUTE`. The current driver masks that flag off at the one allocator
  (`newdelete.cpp`), with the suppression explained beside it. The ACX sample's
  `Common/NewDelete.cpp` is the same allocator; the port carries the mask, and the RT packet
  allocations in the stream engine (`AllocateRtPackets`) use `POOL_FLAG_NON_PAGED` explicitly.
  This is also what HVCI enforces, so it is not cosmetic.
- **An ignored status can be load-bearing; a "dead" probe can be the thing that starts the
  device.** The lesson of the exercise: deleting the sample's stream-resource-manager probe
  to satisfy C6387 stopped every device start with `CM_PROB_FAILED_START` and problem status
  `0xC000000D`, no bugcheck, and the cause was found only by bisecting hunk by hunk against the
  guest. ACX has no `IPortClsStreamResourceManager`, so that probe itself does not carry over;
  the discipline does. Every cut from the ACX sample is a separate change re-installed in the
  guest before the next (`Test-Driver.ps1` with no verifier, device `OK` and the endpoint
  present, *then* `Verify-Driver.ps1`), and a status the analyser says to examine is examined
  by logging and downgrading when the step is optional, never by turning a warning into a
  failed start. When a start fails under Driver Verifier, `-NoExercise` and a no-verifier
  install separate the verifier's interaction from a driver that simply does not start.
- **Sample code for endpoints that no longer exist is removed, not left to run over nothing.**
  The Simple Audio Sample's capture-endpoint loop ran with `g_cCaptureEndpoints` at zero and
  the analyser flagged the resulting dead indexing; it went, and the same goes for the ACX
  sample's capture circuit, keyword detector, `SaveData`, `WaveReader` and tone generator,
  which are cut entirely rather than compiled in behind a zero.
- **Optional platform steps are best-effort.** The sample's device-interface template
  migration (`MigrateDeviceInterfaceTemplateParameters`) and its ETW helper query were made
  non-fatal to the endpoint. ACX registers interfaces itself, so neither step exists in the
  port, but the ACX sample's `EvtDevicePrepareHardware` chains several `RETURN_NTSTATUS_IF_FAILED`
  calls of its own (power policy, the work-item initialisers); each is classified as required or
  optional when it is copied, and the optional ones log rather than fail.
- **The static tier's waivers are per finding, with the reason in the source.** The current
  driver has three suppressions, each with a comment saying what the rule saw and why the code
  is right anyway (`6387` for a parameter documented to take null, `28160` beside the pool
  mask, `28118` for a paging annotation). The port allows itself the same: a waiver in
  `Analyze-Driver.ps1`'s `$known` list or a `#pragma warning(suppress:)` only with the reason
  beside it, and the CodeQL `init-not-cleared` waiver re-examined against the new code rather
  than carried over blind.
- **The dynamic exercise list is the baseline, not the ceiling.** Default-role change, twelve
  system sounds and three spoken passages, three device restarts idle and one under a live
  stream, three concurrent streams, surprise removal under a stream, reinstall on top; special
  pool accounting for every allocation with none untagged. All of it runs against the ACX
  driver unchanged, plus the three additions in step 3 above.
- **The KASAN proof is repeated, not assumed.** The instrumented build loading is not evidence
  the checks are live; the deliberate one-past-the-end read at driver entry is, with its two
  signatures (`0x50` under special pool, `0x1F2 KASAN_ILLEGAL_ACCESS` with the verifier off).
  Same build, same two runs, then the fault build deleted.
- **Kernel coverage is not measurable, so the logic that can be lifted out is.** The current
  record notes that the timing simulation is the one piece of the driver that is ours and that
  it could be tested in user mode behind a seam. The port is the moment to do that: the
  position and timing simulation goes into a header with no kernel dependencies, the stream
  engine calls it, and a `tests/windemo` case drives it with synthetic QPC values and checks the
  position advances at the nominal rate and never runs backwards. That is the one addition to
  scope this section makes.
- **Harness lessons stand.** `runScriptInGuest` never returns when the guest bugchecks under
  it, so every guest step has a timeout and the report reads the bugcheck out of the event
  log; a vmrun interpreter argument must be `''`, not `'""'`; the package is found by its INF
  rather than a fixed path. None of this changes, which is why the harness is the reason the
  port is a week and not a month.

## What is deliberately not in this plan

- Any change to what the driver does. It discards audio and advertises 7.1; it did before and
  it does after.
- A capture endpoint, a loopback, or anything that would let the driver carry the demo's
  taps. Process loopback in user mode does that and the plan page says why.
- Keeping both drivers. The PortCls one is not maintained past step 5.
- HLK certification. Attestation is the signing route unless the product decision changes;
  the plan page's Phase 6 records the difference.
