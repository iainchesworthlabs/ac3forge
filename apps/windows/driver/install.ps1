# Installs the test-signed Ac3ForgeNullSink package on this machine and creates
# its root-enumerated device. Run as administrator, with test signing on (see
# README.md). Pass -PackageDir to point at a build output other than the
# default Release x64 one. Needs nothing beyond Windows: the package is
# staged with pnputil and the device is created through SetupAPI
# (NullSinkDevice.ps1), the sequence the WDK's devcon performs.
[CmdletBinding()]
param(
    [string]$PackageDir = (Join-Path $PSScriptRoot 'x64\Release\package')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'NullSinkDevice.ps1')

$inf = Join-Path $PackageDir 'Ac3ForgeNullSink.inf'
if (-not (Test-Path $inf)) { throw "no package at $PackageDir (build the solution first)" }

$cert = Get-ChildItem (Split-Path $PackageDir) -Filter '*.cer' | Select-Object -First 1
if ($cert) {
    Write-Host "trusting test certificate $($cert.Name) (local machine, Root + TrustedPublisher)"
    # certutil rather than Import-Certificate: the cmdlet is refused
    # (E_ACCESSDENIED) under the elevated batch logon vmrun gives the test
    # guest, certutil is not, and both do the same thing.
    foreach ($store in 'Root', 'TrustedPublisher') {
        & certutil -addstore -f $store $cert.FullName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "certutil -addstore $store failed ($LASTEXITCODE)" }
    }
}

Write-Host "staging $inf"
& pnputil /add-driver $inf /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) { throw "pnputil failed ($LASTEXITCODE)" }

if (Test-NullSinkDevice) {
    Write-Host 'device already present; restarting it'
    Restart-NullSinkDevice
} else {
    Write-Host 'creating the root-enumerated device'
    $instance = New-NullSinkDevice -Inf $inf
    Write-Host "created $instance"
}
Write-Host 'done: look for "Speakers (Desktop Atmos)" in Sound settings'
