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
| `New-DriverTestVm.ps1 -WindowsIso <iso>` | Creates the VM under `D:\Virtual Machines\Atmos Driver Test` (EFI, Secure Boot off, no TPM, 4 vCPU, 8 GB, 64 GB NVMe, NAT, HD Audio, console on VNC port 5951), attaches the no-prompt copy of the Windows ISO, an ISO carrying `autounattend.xml`, an ISO carrying the built driver package plus `devcon.exe` and the guest scripts, and VMware's Tools ISO, then starts it. |
| `New-NoPromptIso.ps1 -WindowsIso <iso>` | Re-packs Microsoft's ISO as `<stem>-noprompt.iso` beside it, booting through the media's own `efisys_noprompt.bin` instead of `efisys.bin`, because the stock EFI boot stops at "Press any key to boot from CD or DVD" and nobody is there to press it. `New-DriverTestVm.ps1` calls it and reuses the result. A few minutes the first time. |
| `Wait-DriverTestVm.ps1` | Polls until Tools is running and the first-logon marker exists, then takes the `clean-install` snapshot. |
| `Test-Driver.ps1` | Reverts to `clean-install`, runs `install.ps1` from the driver CD inside the guest, and reports: test signing and HVCI state, MEDIA devices and audio endpoints, whether the driver service is loaded, any bugchecks and minidumps, and the tail of `setupapi.dev.log`. `-NoRevert` skips the revert, `-ReportOnly` just reports. |
| `Build-Iso.ps1` | Writes an ISO from a directory with Windows' own IMAPI2 (ISO 9660 + Joliet, or UDF with an EFI boot image); the other scripts use it. |
| `guest_console.py` | Screenshots (`shot out.png`), key presses (`key space`, `combo Super_L+r`) and typing (`type text`) on the guest console over Workstation's VNC server, for the stretch before Tools is running when `vmrun` cannot see the guest. Needs Python with Pillow. The VNC server maps keys by the US layout, so the guest is installed with a US keyboard and the helper adds Shift for capitals and symbols itself. |
| `guest/Set-DefaultToNullSink.ps1` | Runs inside the guest, on the driver CD: makes "Speakers (Desktop Atmos)" the default output the way the demo does, so the endpoint is exercised as a default without the demo installed there. |

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

After a driver change: rebuild the package, then `New-DriverTestVm.ps1 -Recreate` is the
heavy way; the light way is to rebuild `driver.iso` with `Build-Iso.ps1` into the VM directory
and run `Test-Driver.ps1` again, which reverts to the clean snapshot first. A change to
`install.ps1` alone needs neither: the test copies the working tree's copy into the guest and
runs that against the package on the CD.

Verified 2026-09-03 on Windows 11 Pro 25H2 (build 26100) in the guest: the package stages,
the root-enumerated "Desktop Atmos" device and its "Speakers (Desktop Atmos)" endpoint
appear, the service runs, the endpoint takes the default role through the same
policy-config call the demo makes, and rendering into it (WAV playback, speech synthesis)
leaves the guest up with no bugcheck.

Things learned the hard way, all encoded in the scripts: a hand-written VMX needs the PCIe
root-port bridges; do not set `bios.bootOrder` (it overrides the NVRAM entry Setup registers
to continue from the disk); Microsoft's media needs the no-prompt EFI boot image; Workstation
26's Tools ISO carries `setup.exe`, not `setup64.exe`; the VNC server types by the US layout;
`vmrun` reports a running Tools as "installed"; `vmrun`'s guest programs are elevated but
`Import-Certificate` is still refused where `certutil` is not; and `runScriptInGuest` needs an
empty interpreter passed as `'""'` from PowerShell.

The guest's credentials are deliberately trivial and the guest is NAT-only; do not put anything
on it you care about.
