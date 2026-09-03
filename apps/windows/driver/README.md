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
Output: `Package\x64\Release\package\` holding the `.sys`, `.inf` and `.cat`, with the test
certificate beside it as `Package\x64\Release\package.cer`. The build was confirmed on the
EWDK for Windows 11 26H1 (kit 10.0.28000, VS 2026 build tools): `inf2cat`'s signability test
passes and `infverif /w` reports the INF valid.

Diffing against the sample: the touched files had trailing whitespace stripped when the cuts
were applied, so use `diff -w` (or `--ignore-trailing-space`) to see only the real changes.

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
