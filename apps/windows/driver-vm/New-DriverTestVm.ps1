# Creates and starts the Desktop Atmos driver test guest in VMware Workstation:
# a throwaway Windows 11 VM that installs itself unattended (autounattend.xml)
# with test signing on and memory integrity off, so the test-signed
# Ac3ForgeNullSink package can be loaded, crashed and rolled back without
# touching the host. See README.md.
#
#   .\New-DriverTestVm.ps1 -WindowsIso D:\ISOs\Win11_25H2_English_x64_v2.iso
#
# Attaches four virtual CDs: the Windows ISO, an ISO carrying
# autounattend.xml, an ISO carrying the built driver package plus the WDK's
# devcon.exe, and VMware Workstation's own Tools ISO (so first logon can
# install Tools, which is what lets vmrun run commands in the guest).
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WindowsIso,
    [string]$VmDir = 'D:\Virtual Machines\Atmos Driver Test',
    [string]$Name = 'Atmos Driver Test',
    [int]$MemoryMB = 8192,
    [int]$Cpus = 4,
    [int]$DiskGB = 64,
    [string]$PackageDir = (Join-Path $PSScriptRoot '..\driver\x64\Release\package'),
    [string]$Devcon = 'F:\Program Files\Windows Kits\10\Tools\10.0.28000.0\x64\devcon.exe',
    [string]$Workstation = 'C:\Program Files\VMware\VMware Workstation',
    # Workstation's built-in VNC server on the guest console (loopback use
    # only; no password). guest_console.py uses it to take screenshots and
    # press keys before Tools is running.
    [int]$VncPort = 5951,
    [switch]$Recreate
)
$ErrorActionPreference = 'Stop'
$vmrun = Join-Path $Workstation 'vmrun.exe'
$vdisk = Join-Path $Workstation 'vmware-vdiskmanager.exe'
$toolsIso = Join-Path $Workstation 'windows.iso'
foreach ($f in @($vmrun, $vdisk, $toolsIso, $WindowsIso)) { if (-not (Test-Path $f)) { throw "missing: $f" } }
$PackageDir = (Resolve-Path $PackageDir).Path
$inf = Join-Path $PackageDir 'Ac3ForgeNullSink.inf'
if (-not (Test-Path $inf)) { throw "no driver package at $PackageDir (build apps/windows/driver first)" }

# Stock Microsoft media stops at "Press any key to boot from CD or DVD" under
# EFI and nobody is there to press it; boot a no-prompt re-pack instead.
if ($WindowsIso -notmatch '-noprompt\.iso$') {
    $WindowsIso = & (Join-Path $PSScriptRoot 'New-NoPromptIso.ps1') -WindowsIso $WindowsIso | Select-Object -Last 1
}
$WindowsIso = (Resolve-Path $WindowsIso).Path

$vmx = Join-Path $VmDir "$Name.vmx"
if ((Test-Path $vmx) -and -not $Recreate) { throw "$vmx exists; pass -Recreate to replace it (this deletes the guest)" }
if (Test-Path $VmDir) {
    & $vmrun stop $vmx hard 2>$null | Out-Null
    Remove-Item $VmDir -Recurse -Force
}
New-Item -ItemType Directory -Path $VmDir | Out-Null

# --- the two data CDs -------------------------------------------------------
$stage = Join-Path $VmDir 'stage'
New-Item -ItemType Directory -Path "$stage\unattend", "$stage\driver\package" | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'autounattend.xml') "$stage\unattend\autounattend.xml"
Copy-Item "$PackageDir\*" "$stage\driver\package\" -Recurse
$cert = Get-ChildItem (Split-Path $PackageDir) -Filter '*.cer' | Select-Object -First 1
if ($cert) { Copy-Item $cert.FullName "$stage\driver\" }
Copy-Item (Join-Path $PSScriptRoot '..\driver\install.ps1'), (Join-Path $PSScriptRoot '..\driver\remove.ps1') "$stage\driver\"
Copy-Item (Join-Path $PSScriptRoot 'guest\*.ps1') "$stage\driver\"
if (Test-Path $Devcon) { Copy-Item $Devcon "$stage\driver\devcon.exe" } else { Write-Warning "devcon not found at $Devcon; install.ps1 in the guest will need it on PATH" }
& (Join-Path $PSScriptRoot 'Build-Iso.ps1') -Source "$stage\unattend" -Out (Join-Path $VmDir 'autounattend.iso') -Label 'UNATTEND'
& (Join-Path $PSScriptRoot 'Build-Iso.ps1') -Source "$stage\driver" -Out (Join-Path $VmDir 'driver.iso') -Label 'ATMOSDRV'
Remove-Item $stage -Recurse -Force

# --- the disk and the VM ------------------------------------------------------
$vmdk = Join-Path $VmDir "$Name.vmdk"
& $vdisk -c -s "${DiskGB}GB" -a nvme -t 1 $vmdk | Out-Null

# Secure Boot deliberately off: a test-signed driver does not load under it.
# No vTPM either (Workstation would want the guest encrypted for one); the
# answer file bypasses the TPM check. Sound present so the guest has a
# normal endpoint to compare against. The PCIe root ports (pciBridge4-7)
# are what give e1000e, NVMe and xHCI their slots; without them vmware-vmx
# logs "No PCIe slot available" and crashes at power-on.
# No bios.bootOrder: that overrides the NVRAM boot entries, and Setup
# registers one to carry on from the disk after its first reboot; the
# firmware's own order tries the (empty) disk first and falls through to
# the CD, which is exactly the sequence an unattended install wants.
@"
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "21"
displayName = "$Name"
guestOS = "windows11-64"
firmware = "efi"
uefi.secureBoot.enabled = "FALSE"
memsize = "$MemoryMB"
numvcpus = "$Cpus"
cpuid.coresPerSocket = "$Cpus"
vhv.enable = "FALSE"
mem.hotadd = "TRUE"
hpet0.present = "TRUE"
pciBridge0.present = "TRUE"
pciBridge4.present = "TRUE"
pciBridge4.virtualDev = "pcieRootPort"
pciBridge4.functions = "8"
pciBridge5.present = "TRUE"
pciBridge5.virtualDev = "pcieRootPort"
pciBridge5.functions = "8"
pciBridge6.present = "TRUE"
pciBridge6.virtualDev = "pcieRootPort"
pciBridge6.functions = "8"
pciBridge7.present = "TRUE"
pciBridge7.virtualDev = "pcieRootPort"
pciBridge7.functions = "8"
nvme0.present = "TRUE"
nvme0:0.present = "TRUE"
nvme0:0.fileName = "$Name.vmdk"
sata0.present = "TRUE"
sata0:0.present = "TRUE"
sata0:0.deviceType = "cdrom-image"
sata0:0.fileName = "$WindowsIso"
sata0:1.present = "TRUE"
sata0:1.deviceType = "cdrom-image"
sata0:1.fileName = "autounattend.iso"
sata0:2.present = "TRUE"
sata0:2.deviceType = "cdrom-image"
sata0:2.fileName = "driver.iso"
sata0:3.present = "TRUE"
sata0:3.deviceType = "cdrom-image"
sata0:3.fileName = "$toolsIso"
ethernet0.present = "TRUE"
ethernet0.connectionType = "nat"
ethernet0.virtualDev = "e1000e"
ethernet0.addressType = "generated"
sound.present = "TRUE"
sound.virtualDev = "hdaudio"
sound.autodetect = "TRUE"
usb.present = "TRUE"
usb_xhci.present = "TRUE"
svga.autodetect = "TRUE"
svga.vramSize = "134217728"
tools.syncTime = "TRUE"
tools.upgrade.policy = "manual"
powerType.powerOff = "soft"
powerType.reset = "soft"
powerType.suspend = "soft"
RemoteDisplay.vnc.enabled = "TRUE"
RemoteDisplay.vnc.port = "$VncPort"
"@ | Set-Content -Path $vmx -Encoding UTF8

Write-Host "starting $Name; Windows Setup runs unattended (allow 15 to 30 minutes)"
& $vmrun start $vmx nogui
Write-Host "when the guest has rebooted after first logon, run Wait-DriverTestVm.ps1 then Test-Driver.ps1"
