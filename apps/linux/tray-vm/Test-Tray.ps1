# The measurement. Builds ac3crucible in the guest and launches it in the
# guest's own desktop session, counting how many launches survived - which is
# the only measurement that means anything for a fault that appears on nine
# or ten launches out of ten.
#
#   .\Test-Tray.ps1                     the window as it ships, ten launches
#   .\Test-Tray.ps1 -NestSubmenu        and again with a submenu in the tray's
#                                       menu, which is the Qt bug: expect it
#                                       to die on nine or ten of them
#   .\Test-Tray.ps1 -NestSubmenu -Gdb   each launch under gdb, with the
#                                       backtrace and registers on death
#   .\Test-Tray.ps1 -Valgrind           one launch under memcheck
#
# A single launch is evidence of nothing here. Both arms run by default with
# -NestSubmenu for the same reason: the number that matters is the difference
# between them on this machine, not either one alone.
[CmdletBinding()]
param(
    [string]$VmDir = 'D:\Virtual Machines\Crucible Tray Test',
    [string]$Name = 'Crucible Tray Test',
    [int]$Runs = 10,
    # The reproducer for the Qt bug the flat tray menu works around.
    [switch]$NestSubmenu,
    # Force the tray on even where the seam says the session has none.
    [switch]$ForceTray,
    [switch]$NoBuild,
    [switch]$Asan,
    [switch]$Gdb,
    [switch]$Valgrind,
    [string]$Address
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\TrayVmCommon.ps1"
$vm = Get-TrayVm -VmDir $VmDir -Name $Name
if (-not $Address) { $Address = Get-TrayVmAddress $vm }

$arms = @(@{ Label = 'shipped'; Flags = @() })
if ($NestSubmenu) {
    $arms += @{ Label = 'nested-submenu'; Flags = @('--nest-submenu') }
}
foreach ($arm in $arms) {
    $flags = @($arm.Flags)
    if ($ForceTray) { $flags += '--tray' }
    if ($Asan) { $flags += '--asan' }
    # Named here rather than derived on both sides, so the two scripts cannot
    # disagree about where the build went. $HOME rather than ~, because bash
    # does not tilde-expand one in the middle of --build-dir=...
    $dir = '$HOME/src/build/arm-' + $arm.Label + $(if ($ForceTray) { '-tray' }) + $(if ($Asan) { '-asan' })
    $flags += "--build-dir=$dir"

    Write-Host ''
    Write-Host "=== $($arm.Label) ===" -ForegroundColor Cyan
    if (-not $NoBuild) {
        # Filtered, because Debian's Qt prints a screenful of "the qml plugin
        # ... will not be linked" at finalize time and there are two lines in
        # there worth reading. PIPESTATUS so a failed build is still a failed
        # build; the concatenation keeps PowerShell out of the ${...}.
        $build = "crucible-build $($flags -join ' ') 2>&1 | grep -E 'edit applied|built:|rror'" +
                 '; exit ${PIPESTATUS[0]}'
        Invoke-TrayGuest $vm $build -Address $Address
    }

    $trial = @("crucible-trials --build $dir -n $Runs --label $($arm.Label)")
    if ($Gdb) { $trial += '--gdb' }
    if ($Valgrind) { $trial += '--valgrind' }
    # Not piped to Out-Null: the survival count is the whole output.
    Invoke-TrayGuest $vm ($trial -join ' ') -Address $Address -AllowFailure
}
