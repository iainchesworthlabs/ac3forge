# The dynamic tier of the driver's verification, run in the test guest
# (README.md; ..\driver\README.md "Analysis"): the driver under Driver
# Verifier's standard checks plus the KMDF-specific ones, with the WDF
# verifier turned on for it, exercised, and then asked whether it survived.
# Optionally the same with a KASAN-instrumented build of the package, on the
# guest's KASAN-enabled kernel.
#
#   .\Verify-Driver.ps1                 revert, arm the verifiers, install, exercise, report
#   .\Verify-Driver.ps1 -Kasan          the newest package under a ..\driver\x64\Release*kasan*\ dir, on the KASAN kernel
#   .\Verify-Driver.ps1 -NoDdi          drop Driver Verifier's DDI compliance checking (on by default: the driver is pure WDF)
#   .\Verify-Driver.ps1 -NoExercise     install under the verifiers and report, no exercise
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
    # Drop Driver Verifier's DDI compliance checking (0x20000). On by
    # default: the driver is a pure WDF (ACX) driver, which is what the check
    # targets. The PortCls driver this replaced could not run under it (it
    # failed the device start, CM_PROB_FAILED_START, no bugcheck), which is
    # one of the reasons it was replaced.
    [switch]$NoDdi,
    # Install under the verifiers and report, without the device-restart
    # cycling and reinstall - to tell a start failure caused by the verifier
    # from one caused by the exercise itself.
    [switch]$NoExercise,
    # Skip Driver Verifier (keep the WDF verifier and, with -Kasan, the KASAN
    # kernel). A KASAN proof needs this: special pool catches an overrun on
    # its guard page before the sanitizer sees it.
    [switch]$NoVerifier,
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
    # With a timeout: if the guest bugchecks while the script runs, vmrun
    # never returns. Kill it, let the guest come back, and carry on - the
    # report will show the bugcheck, which is the answer.
    $p = Start-Process -FilePath $vmrun -ArgumentList ($guest + @('runScriptInGuest', "`"$vmx`"", "`"`"", "`"cmd /c powershell -NoProfile -ExecutionPolicy Bypass -File $guestScript > $guestOut 2>&1`"")) -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit($GuestStepTimeoutMs)) {
        $p | Stop-Process -Force -ErrorAction SilentlyContinue
        Write-Host "  (guest step '$tag' did not return in $($GuestStepTimeoutMs / 1000) s: the guest may have bugchecked; waiting for it)"
        Start-Sleep -Seconds 20
        Wait-Tools 600
        Start-Sleep -Seconds 10
    }
    & $vmrun @guest copyFileFromGuestToHost $vmx $guestOut $out 2>$null | Out-Null
    Get-Content $out -ErrorAction SilentlyContinue
}
$GuestStepTimeoutMs = 240000

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
    $driverRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\driver')).Path
    $wantKasan = $Kasan.IsPresent
    $inf = Get-ChildItem $driverRoot -Recurse -Filter 'Ac3ForgeNullSink.inf' |
        Where-Object { $_.FullName -match 'Release[^\\]*\\package\\' -and (($_.FullName -match 'kasan') -eq $wantKasan) } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $inf) { throw "no $(if ($wantKasan) { 'KASAN ' })package (Ac3ForgeNullSink.inf) in $driverRoot; build it first" }
    $packageDir = $inf.Directory.FullName
    Write-Host "package: $packageDir"
    # The scripts come from the working tree, not the CD the guest was made
    # with, so a change to them is what is tested; nothing on the CD is used.
    foreach ($script in 'install.ps1', 'remove.ps1', 'NullSinkDevice.ps1') {
        & $vmrun @guest copyFileFromHostToGuest $vmx (Join-Path $driverRoot $script) "C:\Users\atmos\$script" | Out-Null
    }
    & $vmrun @guest copyFileFromHostToGuest $vmx (Join-Path $PSScriptRoot 'guest\Set-DefaultToNullSink.ps1') 'C:\Users\atmos\Set-DefaultToNullSink.ps1' | Out-Null
    & $vmrun @guest createDirectoryInGuest $vmx 'C:\Users\atmos\package' 2>$null | Out-Null
    foreach ($f in Get-ChildItem $packageDir -File) {
        & $vmrun @guest copyFileFromHostToGuest $vmx $f.FullName "C:\Users\atmos\package\$($f.Name)" | Out-Null
    }
    $cert = Get-ChildItem (Split-Path $packageDir) -Filter '*.cer' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $cert) { $cert = Get-ChildItem $driverRoot -Recurse -Filter 'package.cer' | Select-Object -First 1 }
    if ($cert) { & $vmrun @guest copyFileFromHostToGuest $vmx $cert.FullName "C:\Users\atmos\$($cert.Name)" | Out-Null }

    Write-Host ($(if ($NoVerifier) { 'arming the WDF verifier' } else { 'arming Driver Verifier and the WDF verifier' }) + $(if ($NoDdi) { ' (without DDI compliance)' } else { ' (with DDI compliance and code integrity)' }) + $(if ($Kasan) { ', and the KASAN kernel' } else { '' }))
    # 0x9BB is the memory/IRQL/pool/IO/DMA/security/misc checks; 0x20000 is
    # DDI compliance (-NoDdi drops it); 0x02000000 is code integrity
    # checking, which flags executable pool and the other things HVCI
    # refuses, so the compliance an attestation-signed driver needs on a
    # default Windows 11 install is demonstrated here, where memory
    # integrity itself is off for test signing. A literal here-string with
    # the flags picked host-side, so the guest script has no interpolation
    # of its own.
    $flags = if ($NoDdi) { '0x20009BB' } else { '0x20209BB' }
    $armLine = if ($NoVerifier) { '"driver verifier: not armed (-NoVerifier)"' } else { "verifier /flags $flags /driver Ac3ForgeNullSink.sys | Out-Null" }
    Invoke-Guest (@"
$armLine
"@ + @'

"verifier: " + ((verifier /querysettings | Select-String -Pattern 'Verified Drivers|Special Pool|Force IRQL|DDI|Code integrity') | ForEach-Object { $_.Line.Trim() }) -join ' | '
$wdf = 'HKLM:\SYSTEM\CurrentControlSet\Services\Ac3ForgeNullSink\Parameters\Wdf'
New-Item -Path $wdf -Force | Out-Null
Set-ItemProperty -Path $wdf -Name VerifierOn -Value 1 -Type DWord
Set-ItemProperty -Path $wdf -Name TrackHandles -Value '*' -Type MultiString
Set-ItemProperty -Path $wdf -Name VerboseOn -Value 1 -Type DWord
Set-ItemProperty -Path $wdf -Name DbgBreakOnError -Value 0 -Type DWord
"wdf verifier armed: " + (Get-ItemProperty $wdf).VerifierOn
'@) 'arm' | ForEach-Object { "  $_" }
    if ($Kasan) {
        Invoke-Guest @'
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Kernel' -Name KasanEnabled -Value 1 -Type DWord
"kasan: kernel switch set"
'@ 'kasan' | ForEach-Object { "  $_" }
    }
    Restart-Guest

    Write-Host 'installing the package under the verifiers'
    Invoke-Guest @'
"verifier active: " + ((verifier /query | Select-String -Pattern 'Ac3ForgeNullSink' -SimpleMatch) -join ' ')
& C:\Users\atmos\install.ps1 -PackageDir C:\Users\atmos\package
'@ 'install' | ForEach-Object { "  $_" }
    Start-Sleep -Seconds 10
    Wait-Tools

    if ($NoExercise) {
        Invoke-Guest @'
$deadline = (Get-Date).AddSeconds(40)
do {
    $ep = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object FriendlyName -match 'Desktop Atmos'
    $dev = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object FriendlyName -match 'Desktop Atmos'
    if ($ep) { break }
    Start-Sleep 2
} while ((Get-Date) -lt $deadline)
"device: " + ($dev.Status -join ',') + " problem: " + ($dev.Problem -join ',')
"endpoint present: " + [bool]$ep + " status: " + ($ep.Status -join ',')
'@ 'noexercise' | ForEach-Object { "  $_" }
        Start-Sleep -Seconds 3
        Wait-Tools
        return
    }

    Write-Host 'exercising: default role, playback, idle power-down and back, a format change, device restarts idle and under a stream, concurrent streams, removal (driver unload) under a stream, reinstall'
    # Nothing here aborts the rest: each step is guarded, so one hiccup does
    # not cut the exercise short (the endpoint builds a moment after the
    # device, so the first thing is to wait for it).
    Invoke-Guest @'
$ErrorActionPreference = 'Continue'
$deadline = (Get-Date).AddSeconds(30)
do {
    $ep = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object FriendlyName -match 'Desktop Atmos'
    if ($ep) { break }
    Start-Sleep 2
} while ((Get-Date) -lt $deadline)
"endpoint present: " + [bool]$ep + " status: " + ($ep.Status -join ',')
try { & C:\Users\atmos\Set-DefaultToNullSink.ps1 | Out-Null; "default set" } catch { "set-default: $_" }
$player = New-Object System.Media.SoundPlayer
$n = 0
foreach ($wav in (Get-ChildItem C:\Windows\Media -Filter '*.wav' | Select-Object -First 12)) {
    $player.SoundLocation = $wav.FullName
    try { $player.PlaySync(); $n++ } catch {}
}
"played $n system sounds"
try { Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; 1..3 | ForEach-Object { $s.Speak("verification pass $_") }; "spoke 3 times" } catch { "speech: $_" }
# The driver's S0 idle policy sends the device to D3 after five seconds
# with no stream open and brings it back for the next one: the power
# callbacks (D0Exit, D0Entry, circuit power down and up) under a real
# transition, which a guest sleep would also give but VMware cannot wake
# from under script control.
try { Start-Sleep -Seconds 9; $s.Speak("back from idle"); "idle power-down and back" } catch { "idle: $_" }
# A format change on the endpoint: the driver offers one format and its
# set-data-format callback refuses others; the engine's answer is reported,
# not judged (a refusal is the expected outcome, a hang or bugcheck is not).
try { "format change: " + (& C:\Users\atmos\Set-DefaultToNullSink.ps1 -TryFormat 44100 | Select-Object -Last 1) } catch { "format change: $_" }
try { $s.Speak("still rendering after the format change"); "rendered after the format change" } catch { "after format change: $_" }
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
# Three streams at once: the miniport's stream table and the position
# timer with more than one client.
try {
    $jobs = 1..3 | ForEach-Object { Start-Job -ArgumentList $_ { param($n) Add-Type -AssemblyName System.Speech; (New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak("concurrent stream number $n speaking over the others") } }
    $jobs | Wait-Job -Timeout 40 | Out-Null; $jobs | Remove-Job -Force
    "three concurrent streams"
} catch { "concurrent: $_" }
# Removal under a live stream: the device node goes away while a client
# holds a pin (surprise removal / remove with open streams), and remove.ps1
# then deletes the package, which unloads the driver; then the package is
# installed again from scratch.
try {
    $job = Start-Job { Add-Type -AssemblyName System.Speech; (New-Object System.Speech.Synthesis.SpeechSynthesizer).Speak("this stream is open while the device is removed from underneath it entirely") }
    Start-Sleep 1
    & C:\Users\atmos\remove.ps1 | Out-Null
    Wait-Job $job -Timeout 30 | Out-Null; Remove-Job $job -Force
    "removed the device and unloaded the driver under a live stream; loaded now: " + [bool](Get-Service Ac3ForgeNullSink -ErrorAction SilentlyContinue | Where-Object Status -eq Running)
} catch { "removal: $_" }
try { & C:\Users\atmos\install.ps1 -PackageDir C:\Users\atmos\package | Out-Null; "reinstalled from scratch" } catch { "reinstall: $_" }
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
