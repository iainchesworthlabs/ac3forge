# Ac3ForgeNullSink: the Desktop Atmos Demo's silent output device

A root-enumerated virtual audio adapter with one render endpoint, "Speakers (Desktop Atmos)",
that advertises 7.1 at 48 kHz and discards everything it is given. The Desktop Atmos Demo
(`../`, roadmap UX11, [docs/platforms/windows-demo.md](../../../docs/platforms/windows-demo.md))
makes it the Windows default output so that every application renders into a device nobody
hears while the demo taps each one individually; a game that can render surround renders 7.1
into it and reaches the demo's bed intact.

## Licence: separate from the rest of the repository

This directory is derived from Microsoft's **Simple Audio Sample** driver
(`audio/simpleaudiosample` in
[microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples)), which
is licensed under the **Microsoft Public License (MS-PL)**, reproduced in [LICENSE](LICENSE). The
MS-PL is a free licence but the FSF lists it as incompatible with the GPL, under which the rest
of ac3forge is licensed. That is fine here because this driver is a separate work: a kernel-mode
binary that shares no code with the GPL application, is not linked into it, and is reached only
through public Windows APIs. The modifications in this directory are offered under the same
MS-PL terms as the sample. Nothing in here is `#include`d, linked or copied anywhere else in the
repository, and nothing from the rest of the repository is used here.

## What was changed from the sample

The sample ships a speaker and a microphone array, generates a test tone on the capture pin
and saves rendered audio to files. The null sink keeps the sample's adapter, topology and WaveRT
stream machinery and cuts the rest; every cut is small enough to read as a diff against the
sample:

- `Filters/minipairs.h`: the mic-array endpoint is gone; `g_cCaptureEndpoints` is 0 and the
  capture-enumeration loop in `Main/adapter.cpp` starts from nothing.
- `Filters/speakerwavtable.h`: the one device format is 8 channels, 48 kHz, 16-bit,
  `KSAUDIO_SPEAKER_7POINT1_SURROUND`.
- `Main/minwavertstream.cpp`: no tone generator, no file saving. `ReadBytes`/`WriteBytes` are
  no-ops; the DMA position still advances at the nominal rate so the audio engine sees a device
  consuming exactly what it produces.
- `Main/common.cpp`, `Main/adapter.cpp`: the file-saving work items and the tone-generator
  registry switch are gone.
- `Main/Ac3ForgeNullSink.inx`: one render endpoint, hardware id `ROOT\Ac3ForgeNullSink`,
  device description "Desktop Atmos" (Windows shows the endpoint as "Speakers (Desktop Atmos)",
  which is the name the demo recognises).
- Everything else, including the sample's internal class names, is as Microsoft wrote it.

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
to `x64\Release-kasan\`). The build was confirmed on the
EWDK for Windows 11 26H1 (kit 10.0.28000, VS 2026 build tools): `inf2cat`'s signability test
passes and `infverif /w` reports the INF valid.

Diffing against the sample: the touched files had trailing whitespace stripped when the cuts
were applied, so use `diff -w` (or `--ignore-trailing-space`) to see only the real changes.

## Analysis and verification

A kernel driver is held to the WDK quality standard, which is two tiers: static analysis of
the source at build time, and dynamic verification of the running driver. The static tier is
`Analyze-Driver.ps1`; the dynamic tier is `..\driver-vm\Verify-Driver.ps1`, which runs in the
throwaway guest so a bugcheck is a guest reboot.

**Static.** `Analyze-Driver.ps1` runs, and fails on anything reported by, all three:

1. **Code Analysis with the driver rule set.** A rebuild with `RunCodeAnalysis` on and the
   WDK's `DriverRecommendedRules.ruleset`, which is the successor to PREfast for Drivers (the
   `/analyze` engine plus the driver-specific `__drv_` annotation and concurrency rules). It
   writes one `*.nativecodeanalysis.xml` per file; the driver is clean against it. The rule
   set is copied to a path without spaces first, because the MSBuild property does not survive
   one through `cmd`.
2. **CodeQL with `microsoft/windows-drivers`.** A database built from the same rebuild,
   analysed with the pack's `mustfix` and `recommended` suites into SARIF. Get the CLI and the
   pack once: a CodeQL CLI on `PATH` (or point `-CodeQL` at it) and
   `codeql pack download microsoft/windows-drivers`.
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

Both tiers are clean as of 2026-09-03. One caution the exercise earned: the first pass at
C6387 deleted the sample's port-class stream-resource probe, and that stopped every device
start (`CM_PROB_FAILED_START`, `0xC000000D`) without any bugcheck to point at it. The probe
is load-bearing, so it stays and the rule is answered where it pointed, at the possibly-null
physical device object. Re-run `..\driver-vm\Test-Driver.ps1` after any change here before
trusting it, static-analysis fixes included.

Static Driver Verifier (SDV) is deliberately absent: the current kit ships a stub that says
SDV is no longer included and is incompatible with VS2022 and later, and directs you to
CodeQL, which is what step 2 is.

**Dynamic.** `..\driver-vm\Verify-Driver.ps1` reverts the guest to its clean snapshot, arms
Driver Verifier for this driver with the standard checks plus the KMDF verification flags,
turns the KMDF framework verifier on (handle tracking and verbose framework logging, what
`wdfverifier.exe` sets), reboots, installs, then exercises the driver: makes it the default
endpoint, renders system sounds and speech through it, restarts the device repeatedly while
idle and once under a live stream, opens three streams at once, surprise-removes the device
under a live stream, and reinstalls from scratch. It then reports the verifier state, the
service and device, and any bugcheck or minidump. `-Kasan` runs the same against a
KASAN-instrumented package on the guest's KASAN-enabled kernel, which catches out-of-bounds
and use-after-free in pool the other checks miss. `-Ddi` adds Driver Verifier's DDI
compliance checking, off by default because it targets pure WDF drivers and fails a PortCls
miniport's device start (`CM_PROB_FAILED_START`, no bugcheck; this driver is WDM/PortCls and
uses KMDF only for its entry), while the memory, IRQL, pool, I/O, DMA and security checks
run without it. `-NoVerifier` leaves Driver Verifier off and keeps the WDF verifier and, with
`-Kasan`, the KASAN kernel, which a KASAN proof needs because special pool catches an overrun
on its guard page before the sanitizer sees it; `-NoExercise` installs under the verifiers
and reports without the exercise; `-ReportOnly` just reports; `-VmDir`, `-Name` and
`-Workstation` name the guest and the Workstation install. Driver Verifier bugchecks on a
violation, which the report reads back from the guest rather than taking the workstation
down.

## Installing (test-signed, your own machine only)

A test-signed driver loads only with test signing on and memory integrity off:

1. Windows Security > Device security > Core isolation > Memory integrity: off, then reboot.
2. From an administrator prompt: `bcdedit /set testsigning on`, then reboot.
3. Trust the test certificate the build produced (`certmgr` or the Certificates snap-in:
   import into Trusted Root and Trusted Publishers of the local machine).
4. `install.ps1` from an administrator PowerShell: stages the package with `pnputil` and creates
   the root-enumerated device with `devcon` (from the WDK's `Tools\<kit>\x64`).
5. `remove.ps1` reverses it.

Attestation signing through an EV certificate and Partner Center is what would let this load on
other people's machines with memory integrity on; that is a later phase of the plan.
