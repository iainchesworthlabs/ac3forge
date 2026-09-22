# Writes the guest's cloud-init seed CD from seed/ and guest/.
#
#   .\New-SeedIso.ps1                   rebuild seed.iso beside the VM
#   .\New-SeedIso.ps1 -Reboot           and restart the guest into it
#
# New-TrayTestVm.ps1 calls this when it makes the VM; it is separate so an
# edit to guest/provision.sh is a re-seed and a reboot rather than a new
# guest. cloud-init only runs its per-instance modules once, so seed/meta-data
# carries an instance-id that this bumps whenever the seed changes - without
# that, a rebooted guest reads the new seed and does nothing with it.
#
# ISO 9660 + Joliet, not UDF: cloud-init's NoCloud datasource intersects
# "devices labelled cidata" with "devices whose type is vfat or iso9660", so
# a UDF disc with a perfect CIDATA label is invisible to it and the guest
# boots with no user, no ssh key and no provisioning.
[CmdletBinding()]
param(
    [string]$VmDir = 'D:\Virtual Machines\Crucible Tray Test',
    [string]$Name = 'Crucible Tray Test',
    [string]$Workstation = 'C:\Program Files\VMware\VMware Workstation',
    [switch]$Reboot
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $VmDir)) { throw "no guest at $VmDir; run New-TrayTestVm.ps1" }
$vmrun = Join-Path $Workstation 'vmrun.exe'
$vmx = Join-Path $VmDir "$Name.vmx"

# A running guest holds the CD open, so the disc cannot be replaced under it.
if ((Test-Path $vmx) -and (& $vmrun list) -contains $vmx) {
    if (-not $Reboot) { throw "the guest is running and holding seed.iso; pass -Reboot to stop it, re-seed and start it again" }
    Write-Host 'stopping the guest'
    & $vmrun stop $vmx hard 2>$null | Out-Null
}

# A key per guest, kept beside it, so the host drives the guest over ssh
# without a password prompt - Windows has no sshpass and the trial loop is
# dozens of commands. Written from bash-free PowerShell, which cannot pass an
# empty argument to ssh-keygen: -N '""' arrives as a two-character passphrase
# and the key is then unusable without it, so the passphrase is stripped
# afterwards instead.
$key = Join-Path $VmDir 'id_traytest'
if (-not (Test-Path "$key.pub")) {
    & ssh-keygen -q -t ed25519 -N '""' -C 'crucible-tray-vm' -f $key
    if (-not (Test-Path "$key.pub")) { throw 'ssh-keygen produced no key' }
    & ssh-keygen -q -p -P '""' -N '""' -f $key | Out-Null
    if ((Get-Content $key -TotalCount 2)[1] -notmatch '^b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQ') {
        throw "ssh-keygen left a passphrase on $key"
    }
}

$stage = Join-Path $VmDir 'seed-stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

$template = Get-Content (Join-Path $PSScriptRoot 'seed\user-data.template') -Raw
$template = $template.Replace('__SSH_PUBKEY__', ((Get-Content "$key.pub" -Raw).Trim()))
foreach ($script in Get-ChildItem (Join-Path $PSScriptRoot 'guest') -Filter '*.sh') {
    # .gitattributes keeps these LF, but a tree that arrived some other way
    # would send `#!/bin/bash\r` into the guest as a missing interpreter.
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(((Get-Content $script.FullName -Raw) -replace "`r`n", "`n")))
    $lines = for ($i = 0; $i -lt $b64.Length; $i += 76) { '      ' + $b64.Substring($i, [Math]::Min(76, $b64.Length - $i)) }
    $token = '__SCRIPT_' + [IO.Path]::GetFileNameWithoutExtension($script.Name).ToUpper().Replace('-', '_') + '__'
    $template = $template.Replace($token, ($lines -join "`n").TrimStart())
}
if ($template -match '__SCRIPT_\w+__') { throw "user-data.template has an unfilled placeholder: $($Matches[0])" }

# cloud-init runs `runcmd` and the user/ssh modules once per instance-id, so
# a re-seeded guest needs a new one or the new scripts are read and ignored.
$meta = (Get-Content (Join-Path $PSScriptRoot 'seed\meta-data') -Raw)
$meta = $meta -replace 'instance-id:.*', "instance-id: crucible-tray-vm-$([DateTime]::UtcNow.ToString('yyyyMMddHHmmss'))"

# LF only: the guest reads shell scripts back out of this YAML.
[IO.File]::WriteAllText((Join-Path $stage 'user-data'), ($template -replace "`r`n", "`n"))
[IO.File]::WriteAllText((Join-Path $stage 'meta-data'), ($meta -replace "`r`n", "`n"))
# Windows' own IMAPI2 ISO writer, shared with the Windows driver guest next
# door; nothing in it is Windows-driver specific.
$iso = Join-Path $VmDir 'seed.iso'
& (Join-Path $PSScriptRoot '..\..\windows\driver-vm\Build-Iso.ps1') -Source $stage -Out $iso -Label 'CIDATA'
Remove-Item $stage -Recurse -Force

if ($Reboot -and (Test-Path $vmx)) {
    Write-Host 'starting the guest on the new seed'
    & $vmrun start $vmx nogui
}
