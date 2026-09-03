# Waits until the test guest has finished its unattended install and first
# logon (VMware Tools running, the first-logon marker present), then takes
# the "clean-install" snapshot that every later test rolls back to.
[CmdletBinding()]
param(
    [string]$VmDir = 'D:\Virtual Machines\Atmos Driver Test',
    [string]$Name = 'Atmos Driver Test',
    [int]$TimeoutMinutes = 45,
    [string]$Workstation = 'C:\Program Files\VMware\VMware Workstation'
)
$ErrorActionPreference = 'Stop'
$vmrun = Join-Path $Workstation 'vmrun.exe'
$vmx = Join-Path $VmDir "$Name.vmx"
$guest = @('-T', 'ws', '-gu', 'atmos', '-gp', 'atmos')
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)

Write-Host "waiting for Tools and the first-logon marker in $Name ..."
while ((Get-Date) -lt $deadline) {
    $tools = & $vmrun -T ws checkToolsState $vmx 2>$null
    if ($tools -match 'running') {
        $marker = & $vmrun @guest fileExistsInGuest $vmx 'C:\atmos-first-logon-done.txt' 2>$null
        if ($marker -match 'exists') {
            # First logon ends with a reboot; give the guest a moment to be
            # past it, then confirm Tools again.
            Start-Sleep -Seconds 60
            if ((& $vmrun -T ws checkToolsState $vmx 2>$null) -match 'running') {
                Write-Host 'guest ready; taking snapshot "clean-install"'
                & $vmrun -T ws snapshot $vmx 'clean-install'
                exit 0
            }
        }
    }
    Start-Sleep -Seconds 20
}
throw "guest not ready within $TimeoutMinutes minutes"
