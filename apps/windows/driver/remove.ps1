# Removes the Ac3ForgeNullSink device and its staged driver package. Run as
# administrator. Needs nothing beyond Windows (NullSinkDevice.ps1).
[CmdletBinding()]
param()
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'NullSinkDevice.ps1')

Write-Host 'removing the device'
if (Get-NullSinkDevices) { Remove-NullSinkDevice } else { Write-Host '  (not present)' }

# Find the staged package by its original INF name and delete it.
$staged = & pnputil /enum-drivers | Out-String
$blocks = $staged -split "`r?`n`r?`n"
foreach ($block in $blocks) {
    if ($block -match 'Ac3ForgeNullSink\.inf' -and $block -match 'Published Name:\s*(oem\d+\.inf)') {
        $oem = $Matches[1]
        Write-Host "deleting staged package $oem"
        & pnputil /delete-driver $oem /uninstall /force
    }
}
Write-Host 'done'
