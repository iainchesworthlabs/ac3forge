# apps/windows: the Windows-only pieces of Crucible

The application itself moved to [`apps/crucible/`](../crucible/) when it was promoted from a
Windows demo to a cross-platform product (Crucible cross-platform promotion,
[docs/crucible/design/promotion.md](../../docs/crucible/design/promotion.md)). What is left here is the part
that cannot move, because it is Windows and nothing else:

| Directory | What it is |
|---|---|
| [`driver/`](driver/) | `IclForgeNullSink`, the silent render endpoint applications play into ("Speakers (Crucible Silent Output)"). A kernel-mode ACX driver, **separately licensed** (MS-PL, derived from Microsoft's ACX AudioCodec sample) — see its own `LICENSE` and `README`. Nothing in it is included, linked or copied anywhere else in the repository. |
| [`driver-vm/`](driver-vm/) | The throwaway VMware guest the driver is verified in: create, install, test and verify scripts, plus `Deploy-Desk.ps1`, which pushes a built `crucible` into that guest. |

## Why the driver did not move with the application

Windows is the only one of the three platforms that needs a driver at all. Linux makes its
silent device with a PipeWire `support.null-audio-sink` module load, and macOS needs no silent
device because its process taps mute each application where they tap it. So the driver is not
one platform's implementation of a shared idea; it is a Windows-only answer to a Windows-only
problem, and it keeps a directory of its own.

## The driver's names

The driver kept its first names through the promotion to Crucible and through the renaming of the
programs (stage N1A of the re-layout), and took its own on 2026-10-01 (change N1D):

| | Before | Now |
|---|---|---|
| The identity: the hardware id, the service, the SYS, INF, CAT, SLN, INX and RC names, the scripts that build, install, remove and verify it | `ROOT\Ac3ForgeNullSink`, `Ac3ForgeNullSink` | `ROOT\IclForgeNullSink`, `IclForgeNullSink` |
| The .NET namespace its scripts compile for themselves | `Ac3Forge` | `IclForge` |
| The endpoint and the device description | "Speakers (Desktop Atmos)", "Desktop Atmos" | "Speakers (Crucible Silent Output)", "Crucible Silent Output" |
| The INF's provider and manufacturer | `ac3forge` | "ICL Forge" |

The old names were kept for two reasons: they sit inside the package that attestation signing
will sign, so changing them afterwards means submitting and paying again, and a new hardware id
leaves every installed copy orphaned. Neither held on 2026-10-01. The driver has never been
signed and has been installed nowhere but the test guest in `driver-vm/`, so nothing was locked
and nothing was orphaned. The endpoint's old name was also the reason to change it: "Desktop
Atmos" used a trademark of Dolby's to name a system-wide device. The new one is not "Crucible"
alone because Crucible's signal path calls its own station that, and a device of the same name
would stand beside it.

Crucible finds the device by its name. `kWindowsSilentDeviceName`
(`apps/crucible/engine/virtual_device.hpp`) is the one constant for it in the application, read by
the engine's and the output stage's default filter and by the Windows `VirtualDevice`; the INF's
`DeviceDesc` and the guest scripts carry the same words. One run of
`tools/n1b/n1d_driver_names.py` changes any of the names in every place that holds it.

Still not done: the driver is unsigned, so it loads only with test signing on and memory
integrity off, which is why it is installed in the guest and never on a workstation; and it has
run only in that guest, never on a real machine and never under memory integrity.
