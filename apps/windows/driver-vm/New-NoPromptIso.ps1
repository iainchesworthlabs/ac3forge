# Re-packs a Microsoft Windows install ISO so that its EFI boot does not
# stop at "Press any key to boot from CD or DVD". The stock media boots
# through efisys.bin, which asks; the same media carries efisys_noprompt.bin,
# which does not. Nothing else changes: the tree is copied as is into a UDF
# image (install.wim is over 4 GB, so ISO 9660 cannot hold it).
#
# Output defaults to <stem>-noprompt.iso beside the source. Takes a few
# minutes; the result is reused on later runs.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WindowsIso,
    [string]$Out
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $WindowsIso)) { throw "no such ISO: $WindowsIso" }
$WindowsIso = (Resolve-Path $WindowsIso).Path
if (-not $Out) {
    $Out = Join-Path (Split-Path $WindowsIso) ([System.IO.Path]::GetFileNameWithoutExtension($WindowsIso) + '-noprompt.iso')
}
if (Test-Path $Out) { Write-Host "reusing $Out"; return $Out }

$mounted = Mount-DiskImage -ImagePath $WindowsIso -PassThru
try {
    $drive = ($mounted | Get-Volume).DriveLetter
    if (-not $drive) { throw "could not find a drive letter for the mounted ISO" }
    $root = "${drive}:\"
    $bootImage = Join-Path $root 'efi\microsoft\boot\efisys_noprompt.bin'
    if (-not (Test-Path $bootImage)) { throw "$WindowsIso has no efi\microsoft\boot\efisys_noprompt.bin; is it Microsoft install media?" }
    $label = (Get-Volume -DriveLetter $drive).FileSystemLabel
    if (-not $label) { $label = 'CCCOMA_X64FRE_EN-US_DV9' }
    Write-Host "re-packing $WindowsIso (label $label) with the no-prompt EFI boot image"
    & (Join-Path $PSScriptRoot 'Build-Iso.ps1') -Source $root -Out $Out -Label $label -BootImage $bootImage -Udf
} finally {
    Dismount-DiskImage -ImagePath $WindowsIso | Out-Null
}
return $Out
