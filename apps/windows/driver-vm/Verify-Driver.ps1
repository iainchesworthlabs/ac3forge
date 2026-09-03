# The dynamic tier of the driver's verification, run in the test guest
# (README.md; ..\driver\README.md "Analysis"): the driver under Driver
# Verifier's standard checks plus the KMDF-specific ones, with the WDF
# verifier turned on for it, exercised, and then asked whether it survived.
# Optionally the same with a KASAN-instrumented build of the package, on the
# guest's KASAN-enabled kernel.
#
#   .\Verify-Driver.ps1                 revert, arm the verifiers, install, exercise, report
#   .\Verify-Driver.ps1 -Kasan          the KASAN package (..\driver\Package\x64\Release\package-kasan) and kernel
#   .\Verify-Driver.ps1 -ReportOnly     just the report
#
# Arming Driver Verifier and the KASAN kernel each take a reboot; the script
# waits for Tools to come back. Every run starts from the "clean-install"
# snapshot, so the verifier settings never outlive the run.
[CmdletBinding()]
param(
    [string]$VmDir = 'D:\Virtual Machines\Atmos Driver Test',
    [string]$Name = 'Atmos Driver Test',
    [string]$Workstation = 'C:\Program Files\VMware\VMware Workstation',
    [switch]$Kasan,
    [switch]$ReportOnly
)
$ErrorActionPreference = 'Stop'
$vmrun = Join-Path $Workstation 'vmrun.exe'
$vmx = Join-Path $VmDir "$Name.vmx"
$guest = @('-T', 'ws', '-gu', 'atmos', '-gp', 'atmos')
$driverName = 'Ac3ForgeNullSink'

function Wait-Tools([int]$seconds = 300) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ((& $vmrun -T ws checkToolsState $vmx 2>$null) -match 'running|installed') { return }
        Start-Sleep -Seconds 5
    }
    throw 'VMware Tools not running in the guest'
}

function Invoke-Guest([string]$script, [string]$tag) {
    # Same mechanics as Test-Driver.ps1: an elevated batch logon, files under
    # the user's profile, runScriptInGuest with an empty interpreter.
    $local = Join-Path $env:TEMP "atmos-verify-$tag.ps1"
    $out = Join-Path $env:TEMP "atmos-verify-$tag.txt"
    $guestScript = "C:\Users\atmos\atmos-verify-$tag.ps1"
    $guestOut = "C:\Users\atmos\atmos-verify-$tag.txt"
    Set-Content -Path $local -Value $script -Encoding UTF8
    Remove-Item $out -ErrorAction SilentlyContinue
    & $vmrun @guest copyFileFromHostToGuest $vmx $local $guestScript | Out-Null
    & $vmrun @guest runScriptInGuest $vmx '' "cmd /c powershell -NoProfile -ExecutionPolicy Bypass -File $guestScript > $guestOut 2>&1" | Out-Null
    & $vmrun @guest copyFileFromGuestToHost $vmx $guestOut $out | Out-Null
    Get-Content $out -ErrorAction SilentlyContinue
}

function Restart-Guest {
    # A guest-initiated restart, then Tools again. Driver Verifier and the
    # KASAN kernel switch both apply at boot.
    Invoke-Guest 'shutdown /r /t 3 /f | Out-Null; "restarting"' 'restart' | Out-Null
    Start-Sleep -Seconds 20
    Wait-Tools 600
    Start-Sleep -Seconds 10
}

if (-not $ReportOnly) {
    Write-Host 'reverting to "clean-install"'
    & $vmrun -T ws revertToSnapshot $vmx 'clean-install'
    & $vmrun -T ws start $vmx nogui
    Wait-Tools

    # The package: the normal one or the KASAN-instrumented one, found by its
    # INF (the solution build lands it under x64\ or Package\ depending on how
    # it was driven). -Kasan wants the package under a *-kasan Release dir.
    $installScript = (Resolve-Path (Join-Path $PSScriptRoot '..\driver\install.ps1')).Path
    $driverRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\driver')).Path
    $wantKasan = $Kasan.IsPresent
    $inf = Get-ChildItem $driverRoot -Recurse -Filter 'Ac3ForgeNullSink.inf' |
        Where-Object { $_.FullName -match 'Release[^\\]*\\package\\' -and (($_.FullName -match 'kasan') -eq $wantKasan) } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $inf) { throw "no $(if ($wantKasan) { 'KASAN ' })package (Ac3ForgeNullSink.inf) in $driverRoot; build it first" }
    $packageDir = $inf.Directory.FullName
    Write-Host "package: $packageDir"
    & $vmrun @guest copyFileFromHostToGuest $vmx $installScript 'C:\Users\atmos\install.ps1' | Out-Null
    & $vmrun @guest createDirectoryInGuest $vmx 'C:\Users\atmos\package' 2>$null | Out-Null
    foreach ($f in Get-ChildItem $packageDir -File) {
        & $vmrun @guest copyFileFromHostToGuest $vmx $f.FullName "C:\Users\atmos\package\$($f.Name)" | Out-Null
    }
    $cert = Get-ChildItem (Split-Path $packageDir) -Filter '*.cer' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $cert) { $cert = Get-ChildItem $driverRoot -Recurse -Filter 'package.cer' | Select-Object -First 1 }
    if ($cert) { & $vmrun @guest copyFileFromHostToGuest $vmx $cert.FullName "C:\Users\atmos\$($cert.Name)" | Out-Null }

    Write-Host ('arming Driver Verifier (standard + KMDF checks) and the WDF verifier' + $(if ($Kasan) { ', and the KASAN kernel' } else { '' }))
    # A literal here-string (no host-side interpolation - that broke the guest
    # script): Driver Verifier's standard checks plus DDI compliance (the KMDF
    # bits) for this driver only, and the KMDF framework verifier (what
    # wdfverifier.exe sets under the service key). 0x1209BB is /standard plus
    # DDI compliance; verifier masks anything else.
    Invoke-Guest @'
verifier /flags 0x1209BB /driver Ac3ForgeNullSink.sys | Out-Null
"verifier: " + ((verifier /querysettings | Select-String -Pattern 'Verified Drivers|Special Pool|Force IRQL|DDI') | ForEach-Object { $_.Line.Trim() }) -join ' | '
$wdf = 'HKLM:\SYSTEM\CurrentControlSet\Services\Ac3ForgeNullSink\Parameters\Wdf'
New-Item -Path $wdf -Force | Out-Null
Set-ItemProperty -Path $wdf -Name VerifierOn -Value 1 -Type DWord
Set-ItemProperty -Path $wdf -Name TrackHandles -Value '*' -Type MultiString
Set-ItemProperty -Path $wdf -Name VerboseOn -Value 1 -Type DWord
Set-ItemProperty -Path $wdf -Name DbgBreakOnError -Value 0 -Type DWord
"wdf verifier armed: " + (Get-ItemProperty $wdf).VerifierOn
'@ 'arm' | ForEach-Object { "  $_" }
    if ($Kasan) {
        Invoke-Guest @'
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Kernel' -Name KasanEnabled -Value 1 -Type DWord
"kasan: kernel switch set"
'@ 'kasan' | ForEach-Object { "  $_" }
    }
    Restart-Guest

    Write-Host 'installing the package under the verifiers'
    Invoke-Guest @'
$drive = (Get-Volume | Where-Object FileSystemLabel -eq 'ATMOSDRV' | Select-Object -First 1).DriveLetter
"verifier active: " + ((verifier /query | Select-String -Pattern 'Ac3ForgeNullSink' -SimpleMatch) -join ' ')
& C:\Users\atmos\install.ps1 -PackageDir C:\Users\atmos\package -Devcon "$($drive):\devcon.exe"
'@ 'install' | ForEach-Object { "  $_" }
    Start-Sleep -Seconds 10
    Wait-Tools

    Write-Host 'exercising: default role, renders at several formats, restarts of the device, remove and reinstall'
    # Nothing here aborts the rest: each step is guarded, so one hiccup does
    # not cut the exercise short (the endpoint builds a moment after the
    # device, so the first thing is to wait for it).
    Invoke-Guest @'
$ErrorActionPreference = 'Continue'
$drive = (Get-Volume | Where-Object FileSystemLabel -eq 'ATMOSDRV' | Select-Object -First 1).DriveLetter
$deadline = (Get-Date).AddSeconds(30)
do {
    $ep = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object FriendlyName -match 'Desktop Atmos'
    if ($ep) { break }
    Start-Sleep 2
} while ((Get-Date) -lt $deadline)
"endpoint present: " + [bool]$ep + " status: " + ($ep.Status -join ',')
try { & "$($drive):\Set-DefaultToNullSink.ps1" | Out-Null; "default set" } catch { "set-default: $_" }
$player = New-Object System.Media.SoundPlayer
$n = 0
foreach ($wav in (Get-ChildItem C:\Windows\Media -Filter '*.wav' | Select-Object -First 12)) {
    $player.SoundLocation = $wav.FullName
    try { $player.PlaySync(); $n++ } catch {}
}
"played $n system sounds"
try { Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; 1..3 | ForEach-Object { $s.Speak("verification pass $_") }; "spoke 3 times" } catch { "speech: $_" }
$dev = Get-PnpDevice -Class MEDIA | Where-Object FriendlyName -eq 'Desktop Atmos'
$restarts = 0
1..3 | ForEach-Object {
    try { $dev | Disable-PnpDevice -Confirm:$false -ErrorAction Stop; Start-Sleep 1; $dev | Enable-PnpDevice -Confirm:$false -ErrorAction Stop; Start-Sleep 2; $restarts++ } catch { "restart: $_" }
}
"restarted the device $restarts times"
try {
    $job = Start-Job { Add-Type -AssemblyName System.Speech; (New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak("a long sentence to keep the stream open while the device is restarted underneath it") }
    Start-Sleep 1
    $dev | Disable-PnpDevice -Confirm:$false; Start-Sleep 1; $dev | Enable-PnpDevice -Confirm:$false
    Wait-Job $job -Timeout 30 | Out-Null; Remove-Job $job -Force
    "restarted the device under a live stream"
} catch { "live restart: $_" }
try { & C:\Users\atmos\install.ps1 -PackageDir C:\Users\atmos\package -Devcon "$($drive):\devcon.exe" | Out-Null; "reinstalled on top" } catch { "reinstall: $_" }
'@ 'exercise' | ForEach-Object { "  $_" }
    Start-Sleep -Seconds 8
    Wait-Tools
}

Write-Host 'report'
Invoke-Guest @'
"--- verifier ---"
verifier /query | Select-String -Pattern 'Ac3ForgeNullSink|Loads|Unloads|Allocations|Pool|Verified' | ForEach-Object { $_.Line.Trim() }
"--- wdf verifier ---"
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\Ac3ForgeNullSink\Parameters\Wdf' -ErrorAction SilentlyContinue | Select-Object VerifierOn, VerboseOn, TrackHandles | Format-List | Out-String).Trim()
"--- kasan ---"
"kernel switch: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Kernel' -Name KasanEnabled -ErrorAction SilentlyContinue).KasanEnabled
"--- driver ---"
Get-Service Ac3ForgeNullSink -ErrorAction SilentlyContinue | Select-Object Name, Status | Format-Table -AutoSize | Out-String
Get-PnpDevice -Class MEDIA | Select-Object Status, FriendlyName | Format-Table -AutoSize | Out-String
"--- bugchecks and minidumps ---"
Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001; ProviderName='Microsoft-Windows-WER-SystemErrorReporting'} -ErrorAction SilentlyContinue | Select-Object TimeCreated, Message | Format-List | Out-String
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-Kernel-Power'; Id=41} -ErrorAction SilentlyContinue | Select-Object TimeCreated | Format-Table | Out-String
Get-ChildItem C:\Windows\Minidump -ErrorAction SilentlyContinue | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String
"--- framework log tail ---"
Get-WinEvent -LogName System -MaxEvents 400 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'Ac3ForgeNullSink|Wdf|verifier' } | Select-Object -First 15 TimeCreated, Id, Message | Format-List | Out-String
'@ 'report'
