# The static tier of the driver's verification (README.md, "Analysis"):
#
#   1. Code Analysis with the WDK's driver rule set (the successor to PREfast
#      for Drivers): a rebuild with RunCodeAnalysis on, which writes one
#      *.nativecodeanalysis.xml per source file. Any defect fails the run.
#   2. CodeQL with Microsoft's windows-drivers pack: a database built from the
#      same rebuild, analysed with the mustfix and recommended suites into
#      SARIF next to the driver project. Any result fails the run.
#   3. The Driver Verification Log (dvl.exe, the kit's "dvl" target), which
#      bundles both into <driver>.DVL.XML, the artefact the HLK's Static Tools
#      Logo test reads.
#
# Static Driver Verifier is not part of this: the kit says it is no longer
# shipped or compatible with VS2022+, and CodeQL is what replaced it.
#
#   .\Analyze-Driver.ps1 [-BuildEnv F:\BuildEnv\SetupBuildEnv.cmd] [-CodeQL D:\tools\codeql\codeql.exe] [-SkipCodeQL] [-SkipDvl]
#
# Needs: the EWDK mounted (the BuildEnv script), the CodeQL CLI, and the
# microsoft/windows-drivers pack (`codeql pack download microsoft/windows-drivers`).
# The rule sets are copied to a path without spaces because MSBuild's
# property does not survive one through cmd.
[CmdletBinding()]
param(
    [string]$BuildEnv = 'F:\BuildEnv\SetupBuildEnv.cmd',
    [string]$CodeQL = 'D:\tools\codeql\codeql.exe',
    [string]$RuleSet = 'DriverRecommendedRules.ruleset',
    [string]$Database = 'D:\aa-wt-builds\codeql-db\Ac3ForgeNullSink',
    [switch]$SkipCodeQL,
    [switch]$SkipDvl
)
$ErrorActionPreference = 'Stop'
$driver = $PSScriptRoot
$solution = Join-Path $driver 'Ac3ForgeNullSink.sln'
if (-not (Test-Path $BuildEnv)) { throw "EWDK build environment not found: $BuildEnv (mount the EWDK ISO)" }
$kits = Join-Path (Split-Path (Split-Path $BuildEnv)) 'Program Files\Windows Kits\10'
$rulesFrom = Join-Path $kits 'CodeAnalysis'
$rulesTo = 'D:\tools\driverca'
New-Item -ItemType Directory -Force $rulesTo | Out-Null
Copy-Item (Join-Path $rulesFrom '*.ruleset') $rulesTo -Force
$failed = @()

function Invoke-EwdkBuild([string[]]$properties, [string]$target) {
    $args = @("/p:Configuration=Release", "/p:Platform=x64", "/t:$target", "/m", "/v:m", "/nologo") + $properties
    $line = "call `"$BuildEnv`" && msbuild `"$solution`" " + ($args -join ' ')
    & cmd /c $line 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "msbuild $target failed ($LASTEXITCODE)" }
}

# --- 1. Code Analysis ---------------------------------------------------------
Write-Host "== Code Analysis ($RuleSet)"
Get-ChildItem $driver -Recurse -Filter '*.nativecodeanalysis.xml' | Remove-Item -Force
Invoke-EwdkBuild @("/p:RunCodeAnalysis=true", "/p:CodeAnalysisRuleSet=$rulesTo\$RuleSet", "/p:CodeAnalysisTreatWarningsAsErrors=false") 'Rebuild' |
    Where-Object { $_ -match 'warning [A-Z]+\d+|error [A-Z]+\d+' } | ForEach-Object { Write-Host "   $_" }
$reports = Get-ChildItem (Join-Path $driver 'Source') -Recurse -Filter '*.nativecodeanalysis.xml'
$defects = 0
foreach ($r in $reports) { $defects += ([regex]::Matches((Get-Content $r.FullName -Raw), '<DEFECT>')).Count }
Write-Host ("   {0} reports, {1} defects" -f $reports.Count, $defects)
if ($reports.Count -eq 0) { $failed += 'Code Analysis produced no reports' }
if ($defects -gt 0) { $failed += "Code Analysis: $defects defects" }

# --- 2. CodeQL ------------------------------------------------------------------
if (-not $SkipCodeQL) {
    Write-Host "== CodeQL (microsoft/windows-drivers)"
    if (-not (Test-Path $CodeQL)) { throw "CodeQL CLI not found: $CodeQL" }
    if (Test-Path $Database) { Remove-Item $Database -Recurse -Force }
    New-Item -ItemType Directory -Force (Split-Path $Database) | Out-Null
    $build = "cmd /c call `"$BuildEnv`" && msbuild `"$solution`" /p:Configuration=Release /p:Platform=x64 /t:Rebuild /m /v:m /nologo"
    & $CodeQL database create $Database --language=cpp --source-root=$driver "--command=$build" 2>&1 |
        Where-Object { $_ -match 'error|Finalizing|Successfully' } | ForEach-Object { Write-Host "   $_" }
    if ($LASTEXITCODE -ne 0) { throw "codeql database create failed ($LASTEXITCODE)" }
    foreach ($suite in 'mustfix', 'recommended') {
        $sarif = Join-Path $driver "Ac3ForgeNullSink.codeql.$suite.sarif"
        & $CodeQL database analyze $Database "microsoft/windows-drivers:windows-driver-suites/$suite.qls" --format=sarifv2.1.0 --output=$sarif --download 2>&1 |
            Where-Object { $_ -match 'error|Interpreting|results' } | ForEach-Object { Write-Host "   $_" }
        if ($LASTEXITCODE -ne 0) { throw "codeql database analyze ($suite) failed ($LASTEXITCODE)" }
        $results = 0
        foreach ($run in (Get-Content $sarif -Raw | ConvertFrom-Json).runs) { $results += @($run.results).Count }
        Write-Host ("   {0}: {1} results -> {2}" -f $suite, $results, (Split-Path $sarif -Leaf))
        if ($results -gt 0) { $failed += "CodeQL $suite`: $results results" }
    }
}

# --- 3. DVL ---------------------------------------------------------------------
if (-not $SkipDvl) {
    Write-Host "== Driver Verification Log"
    # dvl.exe reads the CA reports and the CodeQL SARIF beside the driver
    # project; the kit's target runs it in the project directory.
    $main = Join-Path $driver 'Source\Main'
    foreach ($s in Get-ChildItem $driver -Filter '*.sarif') { Copy-Item $s.FullName $main -Force }
    $line = "call `"$BuildEnv`" && msbuild `"$main\Main.vcxproj`" /p:Configuration=Release /p:Platform=x64 /t:dvl /v:m /nologo"
    & cmd /c $line 2>&1 | Where-Object { $_ -match 'error|warning|DVL|dvl' } | ForEach-Object { Write-Host "   $_" }
    $dvl = Get-ChildItem $main -Filter '*.DVL.XML' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dvl) { Write-Host "   $($dvl.FullName)" } else { $failed += 'no DVL produced' }
}

if ($failed) {
    Write-Host "FAILED:"; $failed | ForEach-Object { Write-Host "   $_" }
    exit 1
}
Write-Host "static tier clean"
