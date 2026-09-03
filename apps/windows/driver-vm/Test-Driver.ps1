# Installs the driver package in the test guest and reports what happened:
# whether the guest is in the state a test-signed driver needs, whether the
# device and its endpoint appeared, whether the driver is loaded, and
# whether the guest bugchecked along the way. Reverts to "clean-install"
# first unless told not to, so every run starts from the same place.
#
#   .\Test-Driver.ps1                 revert, install, report
#   .\Test-Driver.ps1 -NoRevert       install on top of whatever is there
#   .\Test-Driver.ps1 -ReportOnly     just the report
[CmdletBinding()]
param(
    [string]$VmDir = 'D:\Virtual Machines\Atmos Driver Test',
    [string]$Name = 'Atmos Driver Test',
    [string]$Workstation = 'C:\Program Files\VMware\VMware Workstation',
    [switch]$NoRevert,
    [switch]$ReportOnly
)
$ErrorActionPreference = 'Stop'
$vmrun = Join-Path $Workstation 'vmrun.exe'
$vmx = Join-Path $VmDir "$Name.vmx"
$guest = @('-T', 'ws', '-gu', 'atmos', '-gp', 'atmos')

function Wait-Tools([int]$seconds = 300) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ((& $vmrun -T ws checkToolsState $vmx 2>$null) -match 'running') { return }
        Start-Sleep -Seconds 5
    }
    throw 'VMware Tools not running in the guest'
}

function Invoke-Guest([string]$script, [string]$tag) {
    # Runs a PowerShell snippet in the guest elevated (atmos is an
    # administrator and UAC is not in the way for vmrun's session) and
    # brings its output back as a file.
    $local = Join-Path $env:TEMP "atmos-guest-$tag.ps1"
    $out = Join-Path $env:TEMP "atmos-guest-$tag.txt"
    Set-Content -Path $local -Value $script -Encoding UTF8
    & $vmrun @guest copyFileFromHostToGuest $vmx $local "C:\atmos-$tag.ps1" | Out-Null
    & $vmrun @guest runProgramInGuest $vmx -activeWindow -interactive 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', "& C:\atmos-$tag.ps1 *>&1 | Out-File -Encoding UTF8 C:\atmos-$tag.txt" | Out-Null
    & $vmrun @guest copyFileFromGuestToHost $vmx "C:\atmos-$tag.txt" $out | Out-Null
    Get-Content $out -ErrorAction SilentlyContinue
}

if (-not $ReportOnly -and -not $NoRevert) {
    Write-Host 'reverting to "clean-install"'
    & $vmrun -T ws revertToSnapshot $vmx 'clean-install'
    & $vmrun -T ws start $vmx nogui
    Wait-Tools
}

if (-not $ReportOnly) {
    Write-Host 'installing the driver package from the ATMOSDRV CD'
    Invoke-Guest @'
$drive = (Get-Volume | Where-Object FileSystemLabel -eq 'ATMOSDRV' | Select-Object -First 1).DriveLetter
if (-not $drive) { throw 'ATMOSDRV CD not found in the guest' }
"testsigning: " + ((bcdedit /enum '{current}' | Select-String testsigning) -join '')
"hvci: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' -ErrorAction SilentlyContinue).Enabled
& "$($drive):\install.ps1" -PackageDir "$($drive):\package" -Devcon "$($drive):\devcon.exe"
'@ 'install' | ForEach-Object { "  $_" }
    Start-Sleep -Seconds 15
    Wait-Tools
}

Write-Host 'report'
Invoke-Guest @'
"--- MEDIA devices ---"
Get-PnpDevice -Class MEDIA | Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String
"--- audio endpoints ---"
Get-PnpDevice -Class AudioEndpoint | Select-Object Status, FriendlyName | Format-Table -AutoSize | Out-String
"--- driver ---"
driverquery /v | Select-String -Pattern 'Ac3ForgeNullSink' | ForEach-Object { $_.Line }
Get-Service Ac3ForgeNullSink -ErrorAction SilentlyContinue | Select-Object Name, Status | Format-Table -AutoSize | Out-String
"--- bugchecks since the clean install ---"
Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001; ProviderName='Microsoft-Windows-WER-SystemErrorReporting'} -ErrorAction SilentlyContinue | Select-Object TimeCreated, Message | Format-List | Out-String
Get-ChildItem C:\Windows\Minidump -ErrorAction SilentlyContinue | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String
"--- setupapi tail ---"
Get-Content C:\Windows\INF\setupapi.dev.log -Tail 40 -ErrorAction SilentlyContinue
'@ 'report'
