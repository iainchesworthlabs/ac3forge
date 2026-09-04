# Ac3ForgeNullSink: the Desktop Atmos Demo's silent output device

A root-enumerated virtual audio device with one render endpoint, "Speakers (Desktop Atmos)",
that advertises 7.1 at 48 kHz and discards everything it is given. The Desktop Atmos Demo
(`../`, roadmap UX11, [docs/platforms/windows-demo.md](../../../docs/platforms/windows-demo.md))
makes it the Windows default output so that every application renders into a device nobody
hears while the demo taps each one individually; a game that can render surround renders 7.1
into it and reaches the demo's bed intact.

It is an ACX driver (Audio Class eXtensions, Microsoft's current audio driver framework) on
KMDF: a plain WDF driver of about 1,900 lines, half of them comments, in place of the
PortCls/WaveRT miniport that held this directory until 2026-09-04 and carried 9,700 lines of
sample code to do the same job. Why the port was made, what it keeps and how it was verified
are on [docs/platforms/windows-driver-acx.md](../../../docs/platforms/windows-driver-acx.md).

## Licence: separate from the rest of the repository

This directory is derived from Microsoft's **AudioCodec** ACX sample (`audio/Acx/Samples` in
[microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples)), which
is licensed under the **Microsoft Public License (MS-PL)**, reproduced in [LICENSE](LICENSE). The
MS-PL is a free licence but the FSF lists it as incompatible with the GPL, under which the rest
of ac3forge is licensed. That is fine here because this driver is a separate work: a kernel-mode
binary that shares no code with the GPL application, is not linked into it, and is reached only
through public Windows APIs. The modifications in this directory are offered under the same
MS-PL terms as the sample. Nothing in here is `#include`d, linked or copied anywhere else in the
repository, and nothing from the rest of the repository is used here. (The one exception runs
the other way: `position.h` has no kernel dependencies and the repository's test suite compiles
it, under this directory's licence, to pin the timing down; see "The clock" below.)

## What the driver is, file by file

The sample is a codec with a render circuit and a capture circuit, a tone generator, a file
writer, a keyword detector, a peak meter and a stream engine that services them all. The null
sink keeps the shape of the render circuit and the stream engine and cuts the rest; nothing is
compiled in behind a zero.

| File | What it does |
|---|---|
| `Source/Main/driver.cpp` | `DriverEntry`: `WdfDriverCreate`, then `AcxDriverInitialize`. |
| `Source/Main/device.cpp` | `EvtDeviceAdd` (ACX init of the device init, the device, ACX init of the device, one render circuit built here); `EvtDevicePrepareHardware` adds the circuit (required) and applies the S0 idle policy (optional, logged when refused); `ReleaseHardware` removes it; D0 entry/exit keep the D3-cold exclusion in step with what ACX says the exit latency must be. Also `NullSink_NoteFailure`, below. |
| `Source/Main/circuit.cpp` | The render circuit: a host pin the audio engine streams into, a volume element and a mute element that hold per-channel state without applying it (the engine expects an endpoint to answer both), a bridge pin categorised as a speaker with one always-present jack, the one format on the host pin's raw-mode list, and stream creation, which hands each stream a `CNullStream`. |
| `Source/Main/stream.cpp`, `stream.h` | `CNullStream`: allocates the RT packets (two, non-paged, MDL-backed; the engine writes into them and nothing reads them), runs a WDF high-resolution timer that reports each packet complete to ACX as the clock passes its end (event-driven mode), and answers presentation position, current packet and hardware latency (none). |
| `Source/Main/position.h` | `PositionClock`: the position and timing simulation. Bytes per second and a packet size in, a 100 ns clock value in, the position and the packets owed out. No kernel headers. |
| `Source/Main/nullsink.h` | Kit includes, the WDF contexts, the constants, and every callback's declaration - in an `extern "C"` block, because MSVC mangles `__declspec(code_seg("PAGE"))` into a C++ function's name and the ACX callback typedefs then fail to link. |
| `Source/Main/NewDelete.cpp` | The sample's placement `operator new` on `ExAllocatePool2`, with `POOL_FLAG_NON_PAGED_EXECUTE` masked off so no allocation is ever executable. |
| `Source/Main/Ac3ForgeNullSink.inx` | The INF, stamped to `.inf` at build time. Hardware id `ROOT\Ac3ForgeNullSink`, service `Ac3ForgeNullSink`, device description "Desktop Atmos" (so the endpoint is "Speakers (Desktop Atmos)", the name the demo matches), reference string `Speaker0` (the circuit's name), the endpoint opted in to event-driven mode, Windows 11 (build 22000) and later. UTF-16 with a byte-order mark: `stampinf` preserves the encoding and `inf2cat` refuses UTF-16 without the mark. |
| `Source/Main/Main.vcxproj`, `Package/package.VcxProj`, `Ac3ForgeNullSink.sln` | KMDF 1.31, ACX 1.1, the kit's `acxstub.lib`. `DriverVer`'s date is stamped from the build's **UTC** date: `inf2cat` rejects a date later than the current UTC date, and the default (the local date) fails between midnight and 10:00 in Australia. |

What was cut from the sample, by name: the capture circuit and its microphone, the keyword
detector, `CSaveData` and `CWaveReader` (the file writer and reader), the tone generator, the
peak meter, `CircuitHelper` (the generic element factory; this driver has one circuit and
builds it in place), the streaming-engine class hierarchy (`CStreamEngine` and its render,
capture and keyword subclasses, replaced by `CNullStream`), WPP tracing, and the INF's
capture endpoint. Kept from the sample's INF shape: the `.Wdf` sections and the
`KmdfLibraryVersion` stamp. Dropped from the previous INF, on the review's finding that they
were sample leftovers: `SignatureAttributes.DRM`/`DRMLevel`/`PETrust` (protected-content
paths) and the `wdmaud`/`swmidi`/`redbook` associated-filter and `Drivers\wave|midi|mixer`
mappings (legacy WDM audio plumbing).

### Three things the port learnt, kept in the source

- **A failed AddDevice names its step.** `NOTE_AND_RETURN_IF_FAILED` wraps every call that
  builds the device and the circuit. When one fails, `NullSink_NoteFailure` writes the call's
  text and status under the service's `Parameters` key (`LastFailedStep`, `LastFailedStatus`)
  before returning, first note wins, and `..\driver-vm\Test-Driver.ps1` prints it. Without it,
  a failed start is `CM_PROB_FAILED_ADD` with a status and no location, on a guest with no
  kernel debugger. It found the port's one start failure in one run: `AcxJackCreate` returns
  `STATUS_INVALID_PARAMETER` for a jack given a presence callback without the jack-detection
  flag, so the jack has neither, and ACX reports it always connected, which is right for a
  device with no socket.
- **Optional steps log, required steps fail.** The sample chains `RETURN_NTSTATUS_IF_FAILED`
  through prepare-hardware; here the S0 idle policy is best-effort and only adding the circuit
  can fail the start. The PortCls driver was stopped for a day by a "harmless" status
  promoted to a failure.
- **Pool is never executable**, and the RT packet buffers ask for `POOL_FLAG_NON_PAGED`
  explicitly. HVCI enforces this; the mask in `NewDelete.cpp` is where the sample's allocator
  would have passed a caller's executable flag through.

### The clock

`PositionClock` (`position.h`) is the one piece of the driver that is logic rather than
framework plumbing: it is what makes the audio engine see a device consuming exactly what it
produces. Given the format's bytes per second and the packet size, it runs, pauses and stops
on a 100 ns clock value (QPC converted, in the driver), reports the position at any instant
at exactly the nominal rate, says how many packets are owed and when the next completes, and
never runs backwards, including across a pause, a resume long afterwards, or a timer that
fires late (a debugger break, a suspended guest), where it owes every missed packet once and
does not slide the schedule. `tests/crucible/test_nullsink_position.cpp` drives it with
synthetic clock values under the repository's coverage preset, which is the nearest thing to
measured coverage a kernel driver can have (kernel code cannot be instrumented with public
tooling; the plan page says so at length).

## Building

Needs the Windows Driver Kit. The project builds under the Enterprise WDK without installing
anything: mount the EWDK ISO and run, from its root,

```bat
LaunchBuildEnv.cmd
msbuild <repo>\apps\windows\driver\Ac3ForgeNullSink.sln /p:Configuration=Release /p:Platform=x64
```

The `package` project runs `inf2cat` and test-signs the package with a WDK test certificate.
Output, under this directory: `x64\Release\package\` holding the `.sys`, `.inf` and `.cat`,
with the test certificate beside it as `x64\Release\package.cer` (the KASAN build below goes
to `x64\Release-kasan\`). The build was confirmed on the EWDK for Windows 11 26H1 (kit
10.0.28000, VS 2026 build tools), whose ACX headers and `acxstub.lib` are under
`km\acx\km\1.1`: `inf2cat`'s signability test passes and `infverif /w` reports the INF
valid. The driver compiles at `/W4 /WX`.

**Without the EWDK, and in CI.** The WDK and SDK are also NuGet packages, which is how
Microsoft's own driver-samples CI builds and how this driver is built on every push
(`.github/workflows/_build.yml`, the `windows-driver` job on GitHub's `windows-latest`
image): `packages.config` names the three packages, `Directory.Build.props` imports them
when they are present and is inert when they are not, so the EWDK build above is
unaffected. What the packages do not carry, and the runner image does, is the "Windows
Driver Kit" Visual Studio component (the `WindowsKernelModeDriver10.0` toolset) and the
Spectre-mitigated libraries; a workstation needs both from the Visual Studio installer
before this works there. Then, from a Visual Studio developer shell:

```powershell
nuget restore .\packages.config -PackagesDirectory .\packages
msbuild .\Ac3ForgeNullSink.sln /p:Configuration=Release /p:Platform=x64
```

The job builds and test-signs the package, runs Code Analysis at the driver rule set
with the same zero-defect bar as `Analyze-Driver.ps1`, and uploads the package as the
`ac3forge-nullsink-driver-testsigned` artifact. It is not a release asset: a test-signed
driver loads only with test signing on, and shipping it waits on the EV certificate.

## Analysis and verification

A kernel driver is held to the WDK quality standard, which is two tiers: static analysis of
the source at build time, and dynamic verification of the running driver. The static tier is
`Analyze-Driver.ps1`; the dynamic tier is `..\driver-vm\Verify-Driver.ps1`, which runs in the
throwaway guest so a bugcheck is a guest reboot.

**Static.** `Analyze-Driver.ps1` runs, and fails on anything reported by, all three:

1. **Code Analysis with the driver rule set.** A rebuild with `RunCodeAnalysis` on and the
   WDK's `DriverRecommendedRules.ruleset`, which is the successor to PREfast for Drivers (the
   `/analyze` engine plus the driver-specific `__drv_` annotation and concurrency rules). It
   writes one `*.nativecodeanalysis.xml` per file. The rule set is copied to a path without
   spaces first, because the MSBuild property does not survive one through `cmd`.
2. **CodeQL with `microsoft/windows-drivers`.** A database built from the same rebuild,
   analysed with the pack's `mustfix` and `recommended` suites into SARIF. Get the CLI and the
   pack once: a CodeQL CLI on `PATH` (or point `-CodeQL` at it) and
   `codeql pack download microsoft/windows-drivers`. Waivers are per finding, in the script's
   `$known` list with the reason beside each; the PortCls driver's one waiver
   (`init-not-cleared`, a PortCls false positive) went with PortCls.
3. **The Driver Verification Log** (`dvl.exe`, the kit's `dvl` MSBuild target), which bundles
   the results into `Ac3ForgeNullSink.DVL.XML`, the artefact the HLK Static Tools Logo test
   consumes for submission.

Its switches: `-BuildEnv` (the EWDK's `SetupBuildEnv.cmd`), `-CodeQL` (the CLI, when it is
not on `PATH`), `-RuleSet` (`DriverRecommendedRules.ruleset` by default), `-Database` (where
the CodeQL database is built), `-SkipCodeQL` and `-SkipDvl`.

**KASAN.** The instrumented package is the same solution built with `/p:EnableKasan=true`,
which the kit turns into `/fsanitize=kernel-address`; the result imports the sanitizer's
load, store and shadow routines from `ntoskrnl.exe`, so it loads only on a kernel that
exports them (Windows 11 24H2 and later) and only once the `KasanEnabled` value under the
kernel's Session Manager key is set and the machine rebooted, which `Verify-Driver.ps1
-Kasan` does. Build it into a scratch copy of this tree so the ordinary package stays put,
then stage `package\` and `package.cer` under `x64\Release-kasan\`, where `-Kasan` finds
it:

```bat
msbuild Ac3ForgeNullSink.sln /p:Configuration=Release /p:Platform=x64 /p:EnableKasan=true /t:Rebuild
```

Static Driver Verifier (SDV) is deliberately absent: the current kit ships a stub that says
SDV is no longer included and is incompatible with VS2022 and later, and directs you to
CodeQL, which is what step 2 is.

**Dynamic.** `..\driver-vm\Verify-Driver.ps1` reverts the guest to its clean snapshot, arms
Driver Verifier for this driver with the standard checks, the KMDF verification flags and
DDI compliance checking (the driver is pure WDF, so it applies in full; `-NoDdi` drops it),
turns the KMDF framework verifier on (handle tracking and verbose framework logging, what
`wdfverifier.exe` sets), reboots, installs, then exercises the driver: makes it the default
endpoint, renders system sounds and speech through it, restarts the device repeatedly while
idle and once under a live stream, opens three streams at once, surprise-removes the device
under a live stream, and reinstalls from scratch. It then reports the verifier state, the
service and device, and any bugcheck or minidump. `-Kasan` runs the same against a
KASAN-instrumented package on the guest's KASAN-enabled kernel, which catches out-of-bounds
and use-after-free in pool the other checks miss. `-NoVerifier` leaves Driver Verifier off
and keeps the WDF verifier and, with `-Kasan`, the KASAN kernel, which a KASAN proof needs
because special pool catches an overrun on its guard page before the sanitizer sees it;
`-NoExercise` installs under the verifiers and reports without the exercise; `-ReportOnly`
just reports; `-VmDir`, `-Name` and `-Workstation` name the guest and the Workstation
install. Driver Verifier bugchecks on a violation, which the report reads back from the guest
rather than taking the workstation down.

Re-run `..\driver-vm\Test-Driver.ps1` after any change here before trusting it, static-analysis
fixes included; the driver's own failure note (above) says where a start failed.

## Installing (test-signed, your own machine only)

A test-signed driver loads only with test signing on and memory integrity off:

1. Windows Security > Device security > Core isolation > Memory integrity: off, then reboot.
2. From an administrator prompt: `bcdedit /set testsigning on`, then reboot.
3. `install.ps1` from an administrator PowerShell: trusts the build's test certificate, stages
   the package with `pnputil`, and creates the root-enumerated device through SetupAPI
   (`NullSinkDevice.ps1`: `SetupDiCreateDeviceInfo`, the hardware id, `DIF_REGISTERDEVICE`,
   `UpdateDriverForPlugAndPlayDevices`, the documented sequence the WDK's `devcon install`
   performs), as `ROOT\MEDIA\0000`. Nothing beyond Windows is needed, so the packaged demo
   carries the same three scripts. `SwDeviceCreate`, the newer software-device API, was
   tried first and returns `ERROR_MOD_NOT_FOUND` on the test guest from every session tried;
   the note in `NullSinkDevice.ps1` has the detail.
4. `remove.ps1` reverses it: removes the device (`pnputil /remove-device`) and deletes the
   staged package.

Attestation signing through an EV certificate and Partner Center is what would let this load on
other people's machines with memory integrity on; that is the last item of the demo's plan.
