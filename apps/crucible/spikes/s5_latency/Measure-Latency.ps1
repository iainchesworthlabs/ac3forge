# Runs spike S5 over the four configurations the plan page's latency table
# wants: normal and low-latency frames, each with the codec in the loop and
# bypassed. For each, starts the demo's runner (ac3crucible-run) on the given
# null sink, waits for it to settle, runs s5_latency against its pid, and
# collects the RESULT line.
#
#   .\Measure-Latency.ps1 -Runner <path\ac3crucible-run.exe> -Spike <path\s5_latency.exe> [-NullSink FxSound] [-Seconds 20]
#
# The runner must end up in a PCM mode (stereo or PCM surround) on a real
# endpoint for the tap on it to hear anything: on the workstation that is
# the default Realtek endpoint, stereo. Pin it with -Pin if the policy
# would choose otherwise.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Runner,
    [Parameter(Mandatory)][string]$Spike,
    [string]$NullSink = 'FxSound',
    [double]$Seconds = 20,
    [string]$Pin = 'stereo'
)
$ErrorActionPreference = 'Stop'
foreach ($f in @($Runner, $Spike)) { if (-not (Test-Path $f)) { throw "missing: $f" } }

function Measure-One([string]$label, [string[]]$runnerArgs, [bool]$bypass) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Runner).Path
    $psi.Arguments = ($runnerArgs -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $runner = [System.Diagnostics.Process]::Start($psi)
    try {
        Start-Sleep -Seconds 3
        if ($bypass) { $runner.StandardInput.WriteLine('bypass on') }
        $runner.StandardInput.WriteLine('status')
        Start-Sleep -Seconds 2
        $out = & $Spike $runner.Id $NullSink $Seconds 2>&1
        $result = ($out | Where-Object { $_ -match '^RESULT' } | Select-Object -First 1)
        $runner.StandardInput.WriteLine('quit')
        if (-not $runner.WaitForExit(5000)) { $runner.Kill() }
        $status = ($runner.StandardOutput.ReadToEnd() -split "`n" | Where-Object { $_ -match '^\[' } | Select-Object -Last 1)
        [pscustomobject]@{ configuration = $label; runner = $status.Trim(); result = ($result -replace '^RESULT ', '') }
    } finally {
        if (-not $runner.HasExited) { $runner.Kill() }
    }
}

$base = @('--null-sink', $NullSink, '--pin', $Pin)
$rows = @(
    (Measure-One 'normal, codec'      $base $false),
    (Measure-One 'normal, bypass'     $base $true),
    (Measure-One 'low-latency, codec'  ($base + '--low-latency') $false),
    (Measure-One 'low-latency, bypass' ($base + '--low-latency') $true)
)
$rows | Format-Table -AutoSize -Wrap
