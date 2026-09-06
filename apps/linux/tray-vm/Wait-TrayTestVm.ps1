# Waits for the guest New-TrayTestVm.ps1 started to finish provisioning
# itself, prints what it ended up with, and takes the `clean-install`
# snapshot everything else reverts to.
#
#   .\Wait-TrayTestVm.ps1
#
# First boot is a desktop, a Qt kit, Qt's debug symbols and a vcpkg
# bootstrap over a NAT link; twenty minutes is normal and the default
# timeout is generous.
[CmdletBinding()]
param(
    [string]$VmDir = 'D:\Virtual Machines\Crucible Tray Test',
    [string]$Name = 'Crucible Tray Test',
    [int]$TimeoutMin = 60,
    [string]$Snapshot = 'clean-install',
    [switch]$NoSnapshot
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\TrayVmCommon.ps1"
$vm = Get-TrayVm -VmDir $VmDir -Name $Name

Write-Host 'waiting for the guest to take an address (VMware Tools comes up with the desktop)'
$address = Get-TrayVmAddress $vm -TimeoutSec 900
Write-Host "guest is $address"

$deadline = (Get-Date).AddMinutes($TimeoutMin)
$last = ''
while ((Get-Date) -lt $deadline) {
    $marker = & ssh @(Get-TraySshArgs $vm) "$($vm.User)@$address" 'cat /var/lib/crucible-tray-vm/provisioned 2>/dev/null' 2>$null
    if ($LASTEXITCODE -eq 0 -and $marker) {
        if ($marker -match '^FAILED') { Write-Host ($marker -join "`n"); throw 'provisioning failed; /var/log/crucible-provision.log in the guest says where' }
        Write-Host ''
        Write-Host ($marker -join "`n")
        break
    }
    # Something to look at while it works: the tail of its own log.
    $tail = & ssh @(Get-TraySshArgs $vm) "$($vm.User)@$address" 'tail -1 /var/log/crucible-provision.log 2>/dev/null' 2>$null
    if ($tail -and $tail -ne $last) { Write-Host "  $tail"; $last = $tail }
    Start-Sleep -Seconds 20
}
if (-not $marker) { throw "provisioning did not finish within $TimeoutMin minutes" }

# The precondition the whole exercise rests on, asserted here rather than
# assumed: a desktop that owns org.kde.StatusNotifierWatcher. Reading a clean
# run on a session with no host as "it does not reproduce" is the first
# mistake this crash caused (docs/crucible/promotion.md).
Write-Host ''
Write-Host 'StatusNotifier host on the guest session:'
& ssh @(Get-TraySshArgs $vm) "$($vm.User)@$address" 'set -a; . ~/.crucible-session-env 2>/dev/null; set +a; busctl --user list --no-pager --no-legend | grep -i statusnotifier || echo "  NONE - the panel has no tray plugin loaded"'

if (-not $NoSnapshot) {
    Write-Host "taking snapshot $Snapshot"
    & $vm.Vmrun snapshot $vm.Vmx $Snapshot | Out-Null
}
Write-Host ''
Write-Host 'ready. Sync-Source.ps1 pushes the working tree, Test-Tray.ps1 runs the two arms.'
