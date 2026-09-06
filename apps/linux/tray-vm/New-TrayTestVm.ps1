# Creates and starts the Crucible tray-crash guest in VMware Workstation: a
# Debian 13 (trixie) desktop that provisions itself from a cloud-init seed,
# with a StatusNotifier host on its panel, Qt 6.8, the Crucible build
# dependencies, Qt's own debug symbols, gdb and valgrind. See README.md.
#
#   .\New-TrayTestVm.ps1
#
# Trixie because it carries Qt 6.8.2, which is the Qt the Raspberry Pi the
# crash was measured on carries, and because Debian publishes matching
# -dbgsym packages for it - which is the thing a 2 GB Pi could not give
# (docs/crucible/promotion.md, "The tray, and why Linux does not get one").
#
# Unlike the Windows guest next door this one runs no installer. The
# published cloud image is a partition table and a root filesystem, so the
# work here is to turn 3 GB of raw bytes into a VMware disk, grow it, and
# hand cloud-init a seed CD that does the rest on first boot.
[CmdletBinding()]
param(
    [string]$VmDir = 'D:\Virtual Machines\Crucible Tray Test',
    [string]$Name = 'Crucible Tray Test',
    [string]$IsoDir = 'D:\ISOs',
    [int]$MemoryMB = 16384,
    [int]$Cpus = 8,
    [int]$DiskGB = 60,
    [string]$Workstation = 'C:\Program Files\VMware\VMware Workstation',
    # Workstation's built-in VNC server on the guest console (loopback only,
    # no password). 5951 is the Windows driver guest's; this is the next one.
    [int]$VncPort = 5952,
    [string]$ImageUrl = 'https://cloud.debian.org/images/cloud/trixie/latest/debian-13-generic-amd64.tar.xz',
    [switch]$Recreate
)
$ErrorActionPreference = 'Stop'
$vmrun = Join-Path $Workstation 'vmrun.exe'
$vdisk = Join-Path $Workstation 'vmware-vdiskmanager.exe'
foreach ($f in @($vmrun, $vdisk)) { if (-not (Test-Path $f)) { throw "missing: $f" } }

$vmx = Join-Path $VmDir "$Name.vmx"
if ((Test-Path $vmx) -and -not $Recreate) { throw "$vmx exists; pass -Recreate to replace it (this deletes the guest)" }

# --- the published image ----------------------------------------------------
# Downloaded once into $IsoDir and kept, the way the Windows guest keeps its
# install media, and verified against Debian's own SHA512SUMS. The image is
# 3 GB of mostly zeroes, so it ships as a 305 MB tar.xz that Windows' own
# bsdtar unpacks (it is built with liblzma; no 7-zip needed).
$archive = Join-Path $IsoDir ([IO.Path]::GetFileName($ImageUrl))
if (-not (Test-Path $archive)) {
    New-Item -ItemType Directory -Path $IsoDir -Force | Out-Null
    Write-Host "downloading $ImageUrl"
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $ImageUrl -OutFile $archive -UseBasicParsing
    $sums = (Invoke-WebRequest -Uri ($ImageUrl -replace '/[^/]+$', '/SHA512SUMS') -UseBasicParsing).Content
    if ($sums -is [byte[]]) { $sums = [Text.Encoding]::ASCII.GetString($sums) }
    $leaf = [regex]::Escape([IO.Path]::GetFileName($ImageUrl))
    $want = (($sums -split "`n" | Where-Object { $_ -match "$leaf\s*$" }) -split '\s+' | Select-Object -First 1)
    $got = (Get-FileHash $archive -Algorithm SHA512).Hash.ToLower()
    if (-not $want) { Remove-Item $archive; throw "no SHA512 published for $leaf" }
    if ($want -ne $got) { Remove-Item $archive; throw "SHA512 mismatch for $leaf" }
    Write-Host "verified $leaf against Debian's SHA512SUMS"
}

if (Test-Path $VmDir) {
    & $vmrun stop $vmx hard 2>$null | Out-Null
    Remove-Item $VmDir -Recurse -Force
}
New-Item -ItemType Directory -Path $VmDir | Out-Null

# --- raw bytes to a VMware disk ---------------------------------------------
# vmware-vdiskmanager cannot read a raw image and neither can Workstation.
# What both read is a monolithicFlat descriptor: a dozen lines of text naming
# an extent file, which is exactly what disk.raw already is. So write the
# descriptor beside the raw, convert that into a real growable disk, and grow
# it - the image is a 3 GB root filesystem, and this guest needs Qt's debug
# symbols, a build tree and room for core dumps.
Write-Host "unpacking $([IO.Path]::GetFileName($archive))"
$raw = Join-Path $VmDir 'seed-flat.vmdk'
& tar -xf $archive -C $VmDir
if ($LASTEXITCODE -ne 0) { throw "tar failed on $archive" }
$unpacked = Get-ChildItem $VmDir -Filter 'disk.raw' -Recurse | Select-Object -First 1
if (-not $unpacked) { throw "no disk.raw inside $archive" }
Move-Item $unpacked.FullName $raw
Get-ChildItem $VmDir -Directory | Remove-Item -Recurse -Force

$sectors = [long]((Get-Item $raw).Length / 512)
@"
# Disk DescriptorFile
version=1
encoding="UTF-8"
CID=fffffffe
parentCID=ffffffff
createType="monolithicFlat"

# Extent description
RW $sectors FLAT "seed-flat.vmdk" 0

# The Disk Data Base
#DDB
ddb.adapterType = "lsilogic"
ddb.geometry.cylinders = "$([math]::Floor($sectors / (255 * 63)))"
ddb.geometry.heads = "255"
ddb.geometry.sectors = "63"
ddb.virtualHWVersion = "21"
"@ | Set-Content -Path (Join-Path $VmDir 'seed.vmdk') -Encoding ASCII

$vmdk = Join-Path $VmDir "$Name.vmdk"
Write-Host "converting and growing to $DiskGB GB"
& $vdisk -r (Join-Path $VmDir 'seed.vmdk') -t 0 $vmdk | Out-Null
if (-not (Test-Path $vmdk)) { throw 'vmware-vdiskmanager produced no disk' }
& $vdisk -x "${DiskGB}GB" $vmdk | Out-Null
Remove-Item (Join-Path $VmDir 'seed.vmdk'), $raw -Force

# --- the cloud-init seed CD -------------------------------------------------
# A CD labelled CIDATA carrying user-data and meta-data, and the ssh key that
# goes with it. New-SeedIso.ps1 is a script of its own so that an edit to
# guest/provision.sh is a re-seed and a reboot rather than a new guest.
& (Join-Path $PSScriptRoot 'New-SeedIso.ps1') -VmDir $VmDir -Name $Name

# --- the VM -----------------------------------------------------------------
# EFI with Secure Boot off: the cloud image ships a signed shim, but leaving
# Secure Boot on adds a way for this to fail that has nothing to do with the
# tray. The PCIe root ports are what give e1000e, NVMe and xHCI their slots
# (the Windows guest's script learned that the hard way). Sound is present so
# PipeWire has a real card to build a graph around rather than only the null
# sink Crucible makes. mks.enable3d is not decoration: without it the guest's
# vmwgfx reports "No 3D enabled", mesa cannot make a DRI2 screen, and labwc
# exits at "unable to create renderer" - no compositor, no panel, no
# StatusNotifier host, no experiment.
@"
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "21"
displayName = "$Name"
guestOS = "debian12-64"
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
sata0:0.fileName = "seed.iso"
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
mks.enable3d = "TRUE"
svga.graphicsMemoryKB = "262144"
tools.syncTime = "TRUE"
tools.upgrade.policy = "manual"
powerType.powerOff = "soft"
powerType.reset = "soft"
powerType.suspend = "soft"
RemoteDisplay.vnc.enabled = "TRUE"
RemoteDisplay.vnc.port = "$VncPort"
"@ | Set-Content -Path $vmx -Encoding UTF8

Write-Host "starting $Name; cloud-init installs the desktop, Qt and the debug symbols on first boot"
& $vmrun start $vmx nogui
Write-Host 'run Wait-TrayTestVm.ps1 next; it waits for provisioning and snapshots the result'
