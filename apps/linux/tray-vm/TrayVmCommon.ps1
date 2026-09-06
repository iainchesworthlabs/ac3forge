# Shared by the other scripts in this directory: where the guest is, how to
# reach it, and how to run something in it. Dot-source it.
#
#   . "$PSScriptRoot\TrayVmCommon.ps1"
#   $vm = Get-TrayVm
#   Invoke-TrayGuest $vm 'busctl --user list | grep StatusNotifier'
#
# The guest is reached over ssh rather than through vmrun's guest programs:
# ssh gives real exit codes and streams output, and this guest's work is
# builds and launch loops rather than the single elevated calls the Windows
# driver guest makes.

function Get-TrayVm {
    [CmdletBinding()]
    param(
        [string]$VmDir = 'D:\Virtual Machines\Crucible Tray Test',
        [string]$Name = 'Crucible Tray Test',
        [string]$Workstation = 'C:\Program Files\VMware\VMware Workstation'
    )
    $vmx = Join-Path $VmDir "$Name.vmx"
    if (-not (Test-Path $vmx)) { throw "no guest at $vmx; run New-TrayTestVm.ps1" }
    [pscustomobject]@{
        Vmx   = $vmx
        VmDir = $VmDir
        Name  = $Name
        Vmrun = Join-Path $Workstation 'vmrun.exe'
        Key   = Join-Path $VmDir 'id_traytest'
        User  = 'crucible'
    }
}

# The guest's NAT address, from VMware's own DHCP lease file rather than from
# vmrun. vmrun getGuestIPAddress needs VMware Tools, and Tools is one of the
# things provisioning installs - so asking vmrun would mean not being able to
# reach the guest until the work that needs reaching it is already done. The
# lease is keyed on the MAC in the VMX and is there from the guest's first
# DHCP request. Not cached: a lease can move.
function Get-TrayVmAddress {
    param([Parameter(Mandatory)]$Vm, [int]$TimeoutSec = 300,
          [string]$Leases = 'C:\ProgramData\VMware\vmnetdhcp.leases')
    $mac = ((Select-String -Path $Vm.Vmx -Pattern '^ethernet0\.generatedAddress\s*=\s*"(.+)"').Matches.Groups[1].Value)
    if (-not $mac) { throw "no ethernet0.generatedAddress in $($Vm.Vmx)" }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ($true) {
        if (Test-Path $Leases) {
            # Last lease wins: the file is append-only and a renewal or a new
            # guest with the same MAC adds a fresh block rather than editing.
            $address = $null
            foreach ($line in Get-Content $Leases) {
                if ($line -match '^lease\s+(\d+\.\d+\.\d+\.\d+)') { $candidate = $Matches[1] }
                elseif ($line -match ('^\s*hardware ethernet\s+' + [regex]::Escape($mac) + ';')) { $address = $candidate }
            }
            if ($address) { return $address }
        }
        if ((Get-Date) -ge $deadline) { throw "no DHCP lease for $mac after $TimeoutSec s" }
        Start-Sleep -Seconds 5
    }
}

# StrictHostKeyChecking off and a throwaway known-hosts file: the guest is
# recreated often and its key changes with it.
function Get-TraySshArgs {
    param([Parameter(Mandatory)]$Vm)
    @('-i', $Vm.Key, '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=NUL',
      '-o', 'LogLevel=ERROR', '-o', 'ConnectTimeout=10')
}

function Invoke-TrayGuest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0)]$Vm,
        [Parameter(Mandatory, Position = 1)][string]$Command,
        [string]$Address,
        [switch]$AllowFailure
    )
    if (-not $Address) { $Address = Get-TrayVmAddress $Vm }
    # The command travels base64-encoded. Two shells and a PowerShell parser
    # stand between here and the guest, and a command with quotes in it -
    # every gdb invocation has several - loses an argument to one of them
    # otherwise. `bash -l` so /etc/profile.d (VCPKG_ROOT, DEBUGINFOD_URLS)
    # applies, reading the script from its own stdin.
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Command))
    & ssh @(Get-TraySshArgs $Vm) "$($Vm.User)@$Address" "echo $b64 | base64 -d | bash -l"
    # Nothing is returned, so the guest's own output is the only thing on the
    # pipeline and a caller can just let it print. The exit code is in
    # $LASTEXITCODE for a caller that passed -AllowFailure and cares.
    if ($LASTEXITCODE -ne 0 -and -not $AllowFailure) { throw "guest command failed (rc=$LASTEXITCODE): $Command" }
}
