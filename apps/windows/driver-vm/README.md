# A throwaway guest for the null-sink driver

Loading a test-signed kernel driver needs test signing on and memory integrity off, and a
kernel driver that misbehaves takes the machine with it. Neither belongs on the workstation,
so the driver's first runs happen in a VMware Workstation guest: a fresh Windows 11 that
installs itself unattended into exactly that state, with a snapshot to roll back to. A blue
screen in the guest is a guest reboot and nothing more.

## What you supply

One file: a Windows 11 x64 install ISO from Microsoft, saved under `D:\ISOs`. The consumer
download page (Windows 11 multi-edition ISO, English) needs an edition and language chosen by
hand; Microsoft's prebuilt developer VMs are no longer downloadable. The scripts install
Windows 11 Pro from that ISO with a generic installation key, unactivated, which is fine for a
guest that never leaves this machine.

## The scripts

| Script | What it does |
|---|---|
| `New-DriverTestVm.ps1 -WindowsIso <iso>` | Creates the VM under `D:\Virtual Machines\Atmos Driver Test` (EFI, Secure Boot off, no TPM, 4 vCPU, 8 GB, 64 GB NVMe, NAT, HD Audio, console on VNC port 5951), attaches the no-prompt copy of the Windows ISO, an ISO carrying `autounattend.xml`, an ISO carrying the built driver package and the scripts (a convenience for a hand-driven guest; the test scripts copy fresh ones from the working tree), and VMware's Tools ISO, then starts it. |
| `New-NoPromptIso.ps1 -WindowsIso <iso>` | Re-packs Microsoft's ISO as `<stem>-noprompt.iso` beside it, booting through the media's own `efisys_noprompt.bin` instead of `efisys.bin`, because the stock EFI boot stops at "Press any key to boot from CD or DVD" and nobody is there to press it. `New-DriverTestVm.ps1` calls it and reuses the result. A few minutes the first time. |
| `Wait-DriverTestVm.ps1` | Polls until Tools is running and the first-logon marker exists, then takes the `clean-install` snapshot. |
| `Test-Driver.ps1` | Reverts to `clean-install`, copies the working tree's package and scripts into the guest, runs `install.ps1` there, and reports: test signing and HVCI state, MEDIA devices and audio endpoints, whether the driver service is loaded, the driver's own note of a failed device-building step (`LastFailedStep`/`LastFailedStatus` under its service key, which the ACX driver writes before it returns a failure from AddDevice), any bugchecks and minidumps, and the tail of `setupapi.dev.log`. `-NoRevert` skips the revert, `-ReportOnly` just reports. |
| `Deploy-Desk.ps1 -BuildDir <dir>` | Puts the built window and its Qt runtime into the guest, installs the VC++ runtime there, and starts the window on the guest's desktop (auto-logged in as `atmos`), so the demo runs end to end against the installed driver; `-Shot out.png -Page output` also captures a page inside the guest. Run after `Test-Driver.ps1`. |
| `Verify-Driver.ps1` | Reverts to `clean-install`, arms Driver Verifier (the standard checks, DDI compliance and code-integrity checking) and the KMDF framework verifier for the driver, reboots, installs, exercises it (default role, playback, the idle power-down and return, a format change on the endpoint, device restarts idle and under a stream, three concurrent streams, removal and driver unload under a stream, reinstall from scratch) and reports the verifier state, the service and device, and any bugcheck or minidump. `-Kasan` does the same with the KASAN-instrumented package on the KASAN kernel; `-NoDdi`, `-NoVerifier`, `-NoExercise` and `-ReportOnly` are described in `..\driver\README.md`. |
| `Build-Iso.ps1` | Writes an ISO from a directory with Windows' own IMAPI2 (ISO 9660 + Joliet, or UDF with an EFI boot image); the other scripts use it. |
| `guest_console.py` | Screenshots (`shot out.png`), key presses (`key space`, `combo Super_L+r`) and typing (`type text`) on the guest console over Workstation's VNC server, for the stretch before Tools is running when `vmrun` cannot see the guest. Needs Python with Pillow. The VNC server maps keys by the US layout, so the guest is installed with a US keyboard and the helper adds Shift for capitals and symbols itself. |
| `guest/Set-DefaultToNullSink.ps1` | Runs inside the guest (copied there by `Verify-Driver.ps1`): makes "Speakers (Desktop Atmos)" the default output the way the demo does, so the endpoint is exercised as a default without the demo installed there; `-TryFormat <rate>` instead asks the endpoint to take another sample rate and reports the answer. |

`autounattend.xml` does the rest: bypasses the TPM, Secure Boot and RAM checks, partitions and
installs, creates the local administrator `atmos` (password `atmos`, auto-logon), skips OOBE,
and at first logon turns test signing on, turns memory integrity off, installs VMware Tools
from its CD, writes the marker and reboots.

## A run

```powershell
cd apps\windows\driver-vm
.\New-DriverTestVm.ps1 -WindowsIso D:\ISOs\Win11_25H2_English_x64_v2.iso # 15-30 min unattended
.\Wait-DriverTestVm.ps1                                                  # snapshot when ready
.\Test-Driver.ps1                                                        # install and report
```

After a driver change: rebuild the package and run `Test-Driver.ps1` again; it reverts to
the clean snapshot, copies the package and the scripts from the working tree into the guest,
and installs from there, so the driver CD the guest was created with is not used after the
first run (`New-DriverTestVm.ps1 -Recreate` remakes it, which nothing needs). The install
needs nothing beyond Windows: `install.ps1` creates the device through SetupAPI
(`NullSinkDevice.ps1`) rather than the WDK's `devcon`.

Verified 2026-09-04 on Windows 11 Pro 25H2 (build 26200, installed from
`Win11_25H2_English_x64_v2.iso`) in the guest, against the ACX driver: the package stages,
`install.ps1` creates the root-enumerated "Desktop Atmos" device through SetupAPI, its
"Speakers (Desktop Atmos)" endpoint appears, the service runs, the endpoint takes the default
role through the same policy-config call the demo makes, and rendering into it (WAV playback,
speech synthesis) leaves the guest up with no bugcheck. `Verify-Driver.ps1` then ran the same
install under Driver Verifier's standard checks, DDI compliance and code-integrity checking,
and the KMDF framework verifier: its first run caught a paged-code-under-spin-lock bug as two
`0xD1` bugchecks (the fix is in the driver); after it, special pool accounted for 132 of 132
allocations, the loads and unloads balance, and the exercise (default role, playback, the idle
power-down and return, a refused format change, restarts idle and under a stream, three
streams at once, removal and driver unload under a live stream, a reinstall from scratch) ran
with no bugcheck and no minidump. The PortCls driver's record of 2026-09-03 (111 of 111
allocations, the KASAN proof with its `0x50` and `0x1F2` signatures) is on the plan page;
the KASAN pass is repeated against the ACX driver and recorded on
[docs/platforms/windows-driver-acx.md](../../../docs/platforms/windows-driver-acx.md).

Things learned the hard way, all encoded in the scripts: a hand-written VMX needs the PCIe
root-port bridges; do not set `bios.bootOrder` (it overrides the NVRAM entry Setup registers
to continue from the disk); Microsoft's media needs the no-prompt EFI boot image; Workstation
26's Tools ISO carries `setup.exe`, not `setup64.exe`; the VNC server types by the US layout;
`vmrun` reports a running Tools as "installed"; `vmrun`'s guest programs are elevated but
`Import-Certificate` is still refused where `certutil` is not; and `runScriptInGuest` needs an
empty interpreter passed as `'""'` from PowerShell.

The guest's credentials are deliberately trivial and the guest is NAT-only; do not put anything
on it you care about.
