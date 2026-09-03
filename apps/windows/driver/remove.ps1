# Removes the Ac3ForgeNullSink device and its staged driver package. Run as
# administrator. Pass -Devcon at a devcon.exe if it is not on PATH.
[CmdletBinding()]
param([string]$Devcon = 'devcon.exe')
$ErrorActionPreference = 'Continue'

Write-Host 'removing the device'
& $Devcon remove 'ROOT\Ac3ForgeNullSink'

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
