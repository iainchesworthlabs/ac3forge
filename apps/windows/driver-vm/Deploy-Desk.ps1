# Puts the built window (ac3desk with the Qt runtime windeployqt laid beside
# it) into the driver-test guest, installs the VC++ runtime there, and starts
# the window on the guest's desktop, so the demo can be seen end to end
# against the driver: Test-Driver.ps1 first, then this.
#
#   .\Deploy-Desk.ps1 -BuildDir D:\build            deploy and start
#   .\Deploy-Desk.ps1 -BuildDir D:\build -Shot out.png -Page output
#                                                   also capture a page inside the guest
#
# The guest auto-logs in as "atmos" (autounattend.xml), so runProgramInGuest
# -interactive lands on its desktop; the console is on VNC port 5951
# (guest_console.py shot out.png) and in Workstation's own window. The
# guest scripts run the same way Test-Driver's do (a file, an empty
# interpreter); see that script for why.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [string]$VmDir = 'D:\Virtual Machines\Atmos Driver Test',
    [string]$Name = 'Atmos Driver Test',
    [string]$Workstation = 'C:\Program Files\VMware\VMware Workstation',
    # The VC++ redistributable to install in the guest; found under the
    # Visual Studio installs when not given.
    [string]$Redist = '',
    [string]$Shot = '',
    [string]$Page = 'output'
)
$ErrorActionPreference = 'Stop'
$vmrun = Join-Path $Workstation 'vmrun.exe'
$vmx = Join-Path $VmDir "$Name.vmx"
$guest = @('-T', 'ws', '-gu', 'atmos', '-gp', 'atmos')
$work = Join-Path ([IO.Path]::GetTempPath()) 'ac3desk-deploy'
New-Item -ItemType Directory -Force $work | Out-Null

function Invoke-Guest([string]$script, [string]$tag) {
    $local = Join-Path $work "guest_$tag.ps1"
    $out = Join-Path $work "guest_$tag.txt"
    Set-Content -Path $local -Value $script -Encoding UTF8
    $guestScript = "C:\Users\atmos\$tag.ps1"
    $guestOut = "C:\Users\atmos\$tag.txt"
    & $vmrun @guest copyFileFromHostToGuest $vmx $local $guestScript | Out-Null
    & $vmrun @guest runScriptInGuest $vmx '' "cmd /c powershell -NoProfile -ExecutionPolicy Bypass -File $guestScript > $guestOut 2>&1" | Out-Null
    & $vmrun @guest copyFileFromGuestToHost $vmx $guestOut $out | Out-Null
    Get-Content $out -ErrorAction SilentlyContinue
}

if (-not $Redist) {
    foreach ($root in @('C:\Program Files\Microsoft Visual Studio', 'C:\Program Files (x86)\Microsoft Visual Studio')) {
        if (Test-Path $root) {
            $found = Get-ChildItem $root -Recurse -Filter 'vc_redist.x64.exe' -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending | Select-Object -First 1
            if ($found) { $Redist = $found.FullName; break }
        }
    }
    if (-not $Redist) { throw 'no vc_redist.x64.exe under a Visual Studio install; pass -Redist' }
}

# The window and what windeployqt put beside it; not the tests and tools
# that share the bin directory.
$bin = Join-Path $BuildDir 'bin'
if (-not (Test-Path (Join-Path $bin 'ac3desk.exe'))) { throw "no ac3desk.exe in $bin; build ac3desk first" }
$stage = Join-Path $work 'ac3desk'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $bin 'ac3desk.exe') $stage
Copy-Item (Join-Path $bin '*.dll') $stage
foreach ($d in @('generic', 'iconengines', 'imageformats', 'networkinformation', 'platforms', 'qml', 'qmltooling', 'styles', 'tls', 'translations')) {
    if (Test-Path (Join-Path $bin $d)) { Copy-Item (Join-Path $bin $d) (Join-Path $stage $d) -Recurse -Force }
}
$zip = Join-Path $work 'ac3desk.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host ("staged {0} MB, zipped {1} MB" -f [math]::Round((Get-ChildItem $stage -Recurse | Measure-Object -Property Length -Sum).Sum / 1MB), [math]::Round((Get-Item $zip).Length / 1MB))

Write-Host 'copying the window and the VC++ runtime to the guest'
& $vmrun @guest copyFileFromHostToGuest $vmx $zip 'C:\Users\atmos\ac3desk.zip' | Out-Null
& $vmrun @guest copyFileFromHostToGuest $vmx $Redist 'C:\Users\atmos\vc_redist.x64.exe' | Out-Null

Write-Host 'unpacking and installing the runtime'
Invoke-Guest @'
Get-Process ac3desk -ErrorAction SilentlyContinue | Stop-Process -Force
if (Test-Path C:\Users\atmos\ac3desk) { Remove-Item C:\Users\atmos\ac3desk -Recurse -Force }
Expand-Archive C:\Users\atmos\ac3desk.zip -DestinationPath C:\Users\atmos\ac3desk
"files: " + (Get-ChildItem C:\Users\atmos\ac3desk -Recurse -File | Measure-Object).Count
$p = Start-Process -FilePath C:\Users\atmos\vc_redist.x64.exe -ArgumentList '/install','/quiet','/norestart' -Wait -PassThru
"vc_redist exit: " + $p.ExitCode
"endpoints: " + ((Get-PnpDevice -Class AudioEndpoint | Where-Object Status -eq OK | Select-Object -ExpandProperty FriendlyName) -join ' | ')
'@ 'deploy' | ForEach-Object { "  $_" }

if ($Shot) {
    Write-Host "capturing --page $Page in the guest"
    & $vmrun @guest runProgramInGuest $vmx -interactive -activeWindow 'C:\Users\atmos\ac3desk\ac3desk.exe' '--shot' 'C:\Users\atmos\shot.png' '--page' $Page | Out-Null
    & $vmrun @guest copyFileFromGuestToHost $vmx 'C:\Users\atmos\shot.png' $Shot | Out-Null
    if (Test-Path $Shot) { "shot: $Shot" } else { 'shot: none' }
}

Write-Host 'starting the window on the guest desktop'
& $vmrun @guest runProgramInGuest $vmx -interactive -activeWindow -noWait 'C:\Users\atmos\ac3desk\ac3desk.exe' | Out-Null
Start-Sleep -Seconds 6
Invoke-Guest @'
Get-Process ac3desk -ErrorAction SilentlyContinue | Select-Object Id, StartTime | Format-Table -AutoSize | Out-String
'@ 'running' | ForEach-Object { "  $_" }
