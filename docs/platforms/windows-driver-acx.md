# The null-sink driver on ACX: plan and record

Crucible's silent output device ([windows-demo.md](windows-demo.md), "Routing:
separation and silencing") is a kernel driver because Windows offers no other way to make an
audio endpoint. The driver in the tree is an ACX driver, about 1,800 lines of C++. On 2026-09-04
it replaced a PortCls/WaveRT miniport derived from Microsoft's Simple Audio Sample, which held
about 9,700 lines of sample code so that a few dozen lines of ours could discard what they are
given. ACX is Microsoft's current framework for audio drivers, and it was chosen on 2026-09-04
after a review of the options (recorded under
[Phase 6](windows-demo.md#phase-6-docs-ci-release)).

| | |
|---|---|
| Driver | An ACX driver on KMDF in `apps/windows/driver/`; [its README](https://github.com/iainchesworthlabs/iclforge/blob/main/apps/windows/driver/README.md) says what each file does |
| Names | `IclForgeNullSink` (the hardware id `ROOT\IclForgeNullSink`, the service, the file names) and the endpoint "Speakers (Crucible Silent Output)", since 2026-10-01; `Ac3ForgeNullSink` and "Speakers (Desktop Atmos)" before. [The rename](#the-rename-2026-10-01) |
| Signing | **Test-signed only.** It loads with test signing on and memory integrity off. The EV certificate and the attestation submission are not done |
| CI | The `windows-driver` job builds and test-signs the package, runs Code Analysis at the driver rule set and uploads the package as `iclforge-nullsink-driver-testsigned`. It installs nothing. A `ci.yml` dispatch with `-f legs=windows-driver` runs it alone |
| Verification | Static tier and dynamic tier (Driver Verifier with DDI compliance and code-integrity checking, the KMDF verifier, KASAN) clean in a throwaway VMware guest on 2026-09-04; the static tier and the dynamic tier without KASAN clean again on the renamed driver on 2026-10-01. A guest sleep and resume is not done |
| Under HVCI | Not exercised. The guest runs with memory integrity off, so the first test under HVCI is a signed build on a default install |
| Distribution | Not a release asset. Crucible's Windows archive carries the install scripts and no driver |

*The plan below is kept as it was written; what each step found, where it departed
from the plan (the install API, the header version, the timing simulation lifted into a
testable header) and the verification record are under [Progress](#progress) at the end. The
plan's file names did not survive: `Source/Main/` holds `driver.cpp`, `device.cpp`,
`circuit.cpp`, `stream.cpp`, `stream.h`, `position.h`, `nullsink.h` and `NewDelete.cpp`. The
names it uses for the driver and its endpoint (`Ac3ForgeNullSink`, "Desktop Atmos") are those of
2026-09-04; [the rename of 2026-10-01](#the-rename-2026-10-01) changed them.*

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
  engine calls it, and a `tests/crucible` case drives it with synthetic QPC values and checks the
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

## Progress

**2026-09-04, steps 1 to 5 in one sitting.** The ACX driver is in the tree and the PortCls
one is history. What each step found, in the order the plan gave them:

1. **Prototype.** The kit carries ACX under `km\acx\km\1.1` (not `1.0` as the risk list
   guessed), so the project pins `ACX_VERSION_MAJOR=1`, `ACX_VERSION_MINOR=1` and KMDF 1.31.
   The endpoint appeared in the guest on the third install. The first two failed AddDevice
   with `CM_PROB_FAILED_ADD` and `0xC000000D`, which is the PortCls lesson again: a status
   with no location. Rather than bisect, the driver now names the step: every call that
   builds the device or the circuit goes through `NOTE_AND_RETURN_IF_FAILED`, and a failure
   is written to the service's `Parameters` key (`LastFailedStep`, `LastFailedStatus`, first
   note wins) before it is returned; `Test-Driver.ps1` prints it. One run then said
   `AcxJackCreate`: ACX refuses (`STATUS_INVALID_PARAMETER`) a jack given a presence callback
   without the jack-detection flag, and a device with no socket wants neither, so the jack has
   neither and ACX reports it always connected. Three build-time findings on the way, each
   kept in the source: MSVC mangles `__declspec(code_seg("PAGE"))` into a C++ function's
   name, so the ACX callback typedefs only link when the callbacks are declared `extern "C"`;
   `inf2cat` refuses a UTF-16 INF without a byte-order mark ("no installation INF"), and
   `stampinf` preserves whatever encoding the `.inx` has; and `inf2cat` rejects a `DriverVer`
   date later than the current UTC date, so a build in Australian morning hours fails with
   the default local-date stamp, and the project stamps the UTC date instead. Exit met:
   device `OK`, "Speakers (Desktop Atmos)" present, service running, WAV playback and speech
   rendering into it, no bugcheck.
2. **Parity.** The timing simulation was not carried across verbatim but lifted out: the
   PortCls stream's `GetPosition`/`UpdatePosition` (a QPC delta scaled to bytes per second
   at the nominal rate) is `PositionClock` in `position.h`, with no kernel dependencies, and
   `tests/crucible/test_nullsink_position.cpp` pins it (nine cases: nothing moves before run,
   exactly the nominal rate, never backwards, pause and resume continuous, stop to zero,
   completions as packets pass, a late timer owes each missed packet once without sliding
   the schedule, completions after a pause line up, an unconfigured packet size owes
   nothing). The comparison the plan asked for is therefore a proof rather than a
   side-by-side run. The one format offered is 7.1 at 48 kHz. The INF cruft is gone; `winmm`
   playback still reaches the endpoint without the `wdmaud` entries (`System.Media.SoundPlayer`
   in the exercise is a `winmm` client), so they stay gone.
3. **Verification.** `Verify-Driver.ps1` arms DDI compliance by default now (`-NoDdi` drops
   it) and Driver Verifier's code-integrity checking (`0x02000000`) with it; the exercise
   gained the idle power-down and return (the S0 idle policy, five seconds with no stream,
   which is the D0 exit and entry a sleep would give; VMware cannot be woken from S3 under
   script control, so a guest sleep is the one thing still done by hand), a format change
   on the endpoint (`Set-DefaultToNullSink.ps1 -TryFormat 44100`, the set-data-format path
   under a real request), and removal under a live stream now runs `remove.ps1`, which
   deletes the package and so unloads the driver with the stream open. Results are below.
4. **Install path.** Not `SwDeviceCreate` after all: on the guest it returns
   `HRESULT 0x8007007E` (`ERROR_MOD_NOT_FOUND`) for every variant tried, including
   Microsoft's IddSampleDriver recipe verbatim, from the session-0 batch logon `vmrun` gives
   and from an elevated scheduled task on the desktop alike, while an unelevated call gets
   the expected access-denied, and the same P/Invoke from the workstation gets access-denied
   unelevated too; the cause was not found in the time it deserved, and an install path
   cannot rest on it. `NullSinkDevice.ps1` instead performs the SetupAPI sequence `devcon
   install` performs (`SetupDiGetINFClass`, `SetupDiCreateDeviceInfo`, the hardware id,
   `DIF_REGISTERDEVICE`, `UpdateDriverForPlugAndPlayDevices`), which is equally documented and
   keeps the instance path `ROOT\MEDIA\0000`; removal is `pnputil /remove-device`. `devcon` is
   gone from the scripts, the VM tooling and the demo's Settings page, and the packaged demo's
   `bin/driver/` carries the three scripts. One thing learnt on the way, for the next time a
   guest step needs the desktop: `vmrun`'s guest programs run in session 0, and
   `runProgramInGuest -interactive` lands on the desktop unelevated; an elevated desktop
   process comes from a scheduled task (`schtasks /it /rl highest`).
5. **Switch-over.** Done in the same commits: the ACX source replaces `Source/`, the README
   is "what was cut from the ACX sample", the CHANGELOG and this page say so, the CodeQL
   `init-not-cleared` waiver went with the PortCls code it excused.

### Verification results

**Static tier (`Analyze-Driver.ps1`, 2026-09-04).** Code Analysis at the driver rule set:
five reports, zero defects, after a first pass that reported thirty and taught three things
about ACX code under the analyser, each kept in the source: a member function with no
parameters must not carry `_Use_decl_annotations_` (C28213); the kernel's own
`KeAcquireSpinLock`/`KeReleaseSpinLock` pair is what the IRQL and lock analysis reads, so a
tiny helper that wrapped the acquire produced a dozen C28167/C28122/C26110 findings for
nothing; and `DriverEntry` in the INIT segment has no paged segment to assert on (C28172).
CodeQL `mustfix` and `recommended`: clean, with no waivers (one `recommended` finding,
padding bytes of the RT packet array leaving the driver uninitialised, answered with an
explicit `RtlZeroMemory` on a block `ExAllocatePool2` had already zeroed). The DVL is
produced. Two harness fixes on the way: CodeQL's tracer runs the build in an environment of
its own in which the EWDK's `SetupBuildEnv.cmd` cannot find `vswhere` or `msbuild`, so the
script now imports the EWDK environment into its own process and hands the tracer a bare
`msbuild`; and `dvl.exe /create` takes the current directory as the project directory.

**Dynamic tier (`Verify-Driver.ps1`, defaults: Driver Verifier's standard checks, DDI
compliance and code-integrity checking, the KMDF framework verifier).** The first run found a
real bug: two bugchecks `0xD1` (`DRIVER_IRQL_NOT_LESS_OR_EQUAL`, IRQL 2, access type 8, that
is an execute fault) at the same image offset, at install time, before the exercise began.
The stream engine's `Run`, `Pause`, the position and packet queries and the destructor were
in the paged segment and took a spin lock; Driver Verifier trims a driver's pageable code the
moment IRQL rises, so a paged function holding a spin lock faults on its own next
instruction. Those functions are non-paged now (the destructor because WDF's destroy callback
may run at DISPATCH_LEVEL), and the comment in `stream.h` says why. With that fixed the run is
clean: the endpoint appears under the verifiers, takes the default role, plays twelve system
sounds and three spoken passages, goes idle to D3 after five seconds and comes back for the
next stream, refuses a 44.1 kHz format request with `AUDCLNT_E_UNSUPPORTED_FORMAT` and keeps
rendering afterwards, survives three device restarts idle and one under a live stream and
three concurrent streams, is removed and unloaded under a live stream (`remove.ps1`, service
gone), and reinstalls from scratch. Special pool accounted for every allocation, 132 of 132,
none untagged, untracked or failed; six loads and five unloads, the sixth still running; no
bugcheck, no minidump.

**KASAN.** The instrumented package (45,736 bytes against the ordinary 32,824) on the KASAN
kernel, under the same verifier settings and the same exercise: clean, 132 of 132 allocations
in special pool, no bugcheck. The proof that the sanitizer is live is repeated below with a
throwaway build carrying a deliberate one-past-the-end read of a 16-byte pool block at driver
entry, never staged past the run.

**The proof that the sanitizer is live, repeated.** The same throwaway method as Phase 4's,
with one more thing learnt. A build reading one past the end of a 16-byte block at driver
entry, under Driver Verifier on the KASAN kernel, bugchecks `0x50` on a page-aligned address:
special pool's guard page catches it first, as before. With Driver Verifier off and the KASAN
kernel alone, that aligned read at offset 16 of a 16-byte block is *not* flagged on this
build (the driver loads and the endpoint appears): the shadow granule past a 16-byte
allocation is evidently not poisoned at that offset. A second build reading one byte past a
24-byte block (offset 25, unaligned) and then reading the block after freeing it bugchecks
`0x1F2`, `KASAN_ILLEGAL_ACCESS`, on the unaligned address one byte past the block, access
size 1, which is the signature the PortCls record has too (its note said "unaligned"; now we
know why that word matters). The sanitizer is live and the shipped KASAN package passes it.
Both throwaway packages were deleted after the runs and were never staged anywhere the
harness could pick up again.

**Not done.** A guest sleep and resume under a live stream: VMware cannot be woken from S3
under script control, so the S0 idle-to-D3 transition, which exercises the same driver
callbacks, stands in; a sleep on the workstation is a manual check for the day the driver is
installed there. And the timing comparison the plan called for was replaced by the unit test
of the lifted clock rather than run side by side against the PortCls driver.

**Parity (step 2's exit).** `Test-Driver.ps1` on the final package, then `Deploy-Desk.ps1
-Shot -Page output` in the same guest: the window starts against the installed ACX driver,
its endpoint probe lists "Speakers (Desktop Atmos)" beside the guest's HD Audio device, and
the Output page's signal path reads as it did on 2026-09-03 (applications still on the real
device, the one-click move to the silent device offered, stereo on the HD Audio endpoint as
the best the guest can carry).

### The rename, 2026-10-01

The driver took its own names in change N1D, after the programs took theirs (N1A) and the family
became ICL Forge. `Ac3ForgeNullSink` is now `IclForgeNullSink`: the hardware id
`ROOT\IclForgeNullSink`, the service, the `.sln`, `.inx`, `.rc`, `.sys`, `.inf` and `.cat` names,
the scripts that build, install, remove, analyse and verify it, and the artifact the CI job
uploads, `iclforge-nullsink-driver-testsigned`. The device description, and so the endpoint, went
from "Desktop Atmos" to "Crucible Silent Output". The INF's provider and manufacturer read "ICL
Forge"; N1A had recorded them as changed, and its pass had not read the `.inx`, which is UTF-16
with a byte-order mark. `tools/n1b/n1d_driver_names.py` makes the change from three tables of
names, so a later change of a name is one row and one run. The code of the driver is the same: the
sections that hold it (`.text`, `.data`, `INIT`, `.reloc`) match the old build byte for byte,
`PAGE` differs by four bytes of name literals, and the data sections differ by the names, the PDB
path and the version resource.

*Why it could be done now.* The plan above holds the names until attestation signing, because
they sit inside the package that gets signed (a rename afterwards means submitting and paying
again) and because a new hardware id leaves every installed copy orphaned. Neither held on
2026-10-01: the driver had never been signed and had been installed only in the throwaway guest,
and the owner decided that day that the names could change. The endpoint's old name was the other
reason. "Desktop Atmos" used Dolby's trademark to name a system-wide device, which [the promotion
plan](../crucible/design/promotion.md#the-name) names as the sharper of the two problems with the
demo's name.

*Why the endpoint is "Crucible Silent Output" and not "Crucible".* The Output page's signal path
has three stations. The first is titled with the endpoint, the second is the application ("2 ·
CRUCIBLE") and the third is the device you hear. With the bare word the first two would carry the
same name, and the first-run dialog would say that the default output becomes "Crucible". The name
is one constant in the application (`kWindowsSilentDeviceName`,
`apps/crucible/engine/virtual_device.hpp`) and one string in the INF. A test pins that the
engine's and the output stage's defaults are that constant, that it begins with "Crucible", is not
the bare word and has no "Atmos" in it. The cost is on the Room page, whose narrow rail elides the
station's title ("Speakers (Crucible Silent Outp..."). The full name is in the line under it.

**Verification in the guest.** The package was built with the EWDK (kit 10.0.28000) at `/W4 /WX`
in 32 seconds with no warnings; `IclForgeNullSink.sys` is 32,824 bytes, as before. In the guest,
from the `clean-install` snapshot:

- `Test-Driver.ps1`: the device `ROOT\MEDIA\0000` ("Crucible Silent Output") and the endpoint
  "Speakers (Crucible Silent Output)" are `OK`, the service `IclForgeNullSink` runs, the driver's
  own failure note is empty and there is no bugcheck. `setupapi.dev.log` records the hardware id
  `ROOT\IclForgeNullSink`.
- The endpoint takes the console, multimedia and communications roles (set through the call the
  window makes and read back through `IMMDeviceEnumerator`), twelve system sounds and three
  spoken passages play into it, and the guest stays up.
- `Deploy-Desk.ps1` runs a Crucible built from the branch against the installed driver. Its
  Settings page reads "The silent device is installed: an endpoint named like "Crucible Silent
  Output"", so the window finds the device by its new name, and the Output page's first station
  is titled "Speakers (Crucible Silent Output)" beside "2 · CRUCIBLE".
- `remove.ps1` leaves no device, endpoint or staged package, and the reinstall lists "ICL Forge"
  as the package's provider.
- `Verify-Driver.ps1` (Driver Verifier's standard checks, DDI compliance and code-integrity
  checking, and the KMDF verifier) ran the whole exercise twice, once from a revert (482 seconds)
  and once on a guest that had finished its update restarts (`-NoRevert`, 454 seconds). Special
  pool accounted for 132 of 132 allocations in both, the driver loaded six times and unloaded
  five, a 44.1 kHz format request was refused with `AUDCLNT_E_UNSUPPORTED_FORMAT` and rendering
  went on, and there was no bugcheck and no minidump. These are the numbers of 2026-09-04.
- The static tier (`Analyze-Driver.ps1`, with CodeQL): Code Analysis reports five files and no
  defects, CodeQL's `mustfix` and `recommended` suites report nothing and need no waiver, and the
  DVL is produced.

One run from a revert bugchecked the guest, and the driver was not the cause. It stopped after
"restarted the device under a live stream" with `0x124` (`WHEA_UNCORRECTABLE_ERROR`, error source
type `0x10`, a device driver), and the crashing CPU's stack has no frame of the driver. The VM's
own log (`vmware.log`) puts the cause on the host: writes to the guest's virtual NVMe disk took up
to 14.5 seconds in the two minutes before it, and the guest's NVMe driver reset its controller at
06:22:04 and again at 06:22:22 UTC, when the blue screen came up. The guest's disks are files on
the host's `D:`, which other builds were using; what stalled it is not known. The two clean runs
above bracket it. Whether the old-named driver would have been hit as well was not measured. The
log is the first place to look before blaming the driver for a `0x124` in this guest.

Two earlier runs were lost to the guest. A guest reverted to `clean-install` installs the updates
it had downloaded before the snapshot over its first restarts, while Tools already reports
running, so a step that lands inside one comes back with no output and no error.
`Verify-Driver.ps1` now waits for the guest to settle after every restart, repeats the install
until a device exists, and takes `-NoRevert`.

**CI.** The `windows-driver` job could not be run on its own: its condition skipped it for any
dispatch that named legs, so its first run would have been the push to `main`. A `ci.yml`
dispatch now accepts `-f legs=windows-driver` ([the legs](../ci-agentic.md#the-legs)). On the
branch the job ran for two minutes: the package builds and is test-signed with
`IclForgeNullSink.sys`, `IclForgeNullSink.inf` and `iclforgenullsink.cat` and the test certificate
beside them, Code Analysis reports five files and no defects, and the artifact
`iclforge-nullsink-driver-testsigned` is uploaded with four files. The pull-request gate with
Windows MSVC added, and the `windows-msvc`, `windows-llvm` and `linux-llvm` legs of `ci.yml` (the
legs that build Crucible, run its Qt Quick suites and check its package), passed on the branch as
well.

**Not done.** Signing for release: the package is test-signed with the certificate the build
makes. A run on a physical machine and under memory integrity. The KASAN pass, which was not
repeated on the renamed driver. A guest sleep and resume. The move of the driver under
`apps/crucible/`, which Phase 7 of [the recasting
plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/recasting.md) allowed and
nobody took.
