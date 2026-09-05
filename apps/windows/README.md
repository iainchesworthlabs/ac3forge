# apps/windows: the Windows-only pieces of AC3Forge Crucible

The application itself moved to [`apps/crucible/`](../crucible/) when it was promoted from a
Windows demo to a cross-platform product (roadmap UX12,
[docs/crucible/promotion.md](../../docs/crucible/promotion.md)). What is left here is the part
that cannot move, because it is Windows and nothing else:

| Directory | What it is |
|---|---|
| [`driver/`](driver/) | `Ac3ForgeNullSink`, the silent render endpoint applications play into. A kernel-mode ACX driver, **separately licensed** (MS-PL, derived from Microsoft's ACX AudioCodec sample) — see its own `LICENSE` and `README`. Nothing in it is included, linked or copied anywhere else in the repository. |
| [`driver-vm/`](driver-vm/) | The throwaway VMware guest the driver is verified in: create, install, test and verify scripts, plus `Deploy-Desk.ps1`, which pushes a built `ac3crucible` into that guest. |

## Why the driver did not move with the application

Windows is the only one of the three platforms that needs a driver at all. Linux makes its
silent device with a PipeWire `support.null-audio-sink` module load, and macOS needs no silent
device because its process taps mute each application where they tap it. So the driver is not
one platform's implementation of a shared idea; it is a Windows-only answer to a Windows-only
problem, and it keeps a directory of its own.

It also keeps its **old names** for now — the device is still "Desktop Atmos", not "Crucible".
That is deliberate and temporary. The device name lives inside the package that attestation
signing will sign, so renaming it after signing would mean submitting and paying again. The
rename happens once, in the same change that rebuilds and re-signs the driver, and until then
the application matches the endpoint by the name it actually advertises (see
`EngineConfig::null_sink_substring`).
