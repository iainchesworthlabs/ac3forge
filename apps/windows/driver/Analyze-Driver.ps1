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

# The EWDK's environment, imported into this process once: CodeQL's tracer
# runs the build command in an environment of its own making, in which
# SetupBuildEnv.cmd cannot find vswhere or msbuild ("is not recognized"),
# so the environment is set up here and the tracer is handed a bare msbuild.
$ewdkEnv = & cmd /c "call `"$BuildEnv`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) { throw "could not set up the EWDK environment from $BuildEnv" }
foreach ($line in $ewdkEnv) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}
if (-not (Get-Command msbuild -ErrorAction SilentlyContinue)) { throw 'msbuild is not on PATH after the EWDK environment was imported' }

function Invoke-EwdkBuild([string[]]$properties, [string]$target) {
    $args = @("/p:Configuration=Release", "/p:Platform=x64", "/t:$target", "/m", "/v:m", "/nologo") + $properties
    & msbuild $solution @args 2>&1 | ForEach-Object { "$_" }
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
    $build = "msbuild `"$solution`" /p:Configuration=Release /p:Platform=x64 /t:Rebuild /m /v:m /nologo"
    & $CodeQL database create $Database --language=cpp --source-root=$driver "--command=$build" 2>&1 |
        Where-Object { $_ -match 'error|Finalizing|Successfully' } | ForEach-Object { Write-Host "   $_" }
    if ($LASTEXITCODE -ne 0) { throw "codeql database create failed ($LASTEXITCODE)" }
    # Justified exceptions: a CodeQL rule id plus a file substring the finding
    # is known-good in, with the reason. Both suites carry none for the ACX
    # driver (the PortCls driver's one waiver, init-not-cleared at its
    # PcAddAdapterDevice, went with the PortCls code). A waiver added here
    # needs its reason beside it.
    $known = @{
        'mustfix' = @()
        'recommended' = @()
    }
    foreach ($suite in 'mustfix', 'recommended') {
        $sarif = Join-Path $driver "Ac3ForgeNullSink.codeql.$suite.sarif"
        & $CodeQL database analyze $Database "microsoft/windows-drivers:windows-driver-suites/$suite.qls" --format=sarifv2.1.0 --output=$sarif --download 2>&1 |
            Where-Object { $_ -match 'error|Interpreting|results' } | ForEach-Object { Write-Host "   $_" }
        if ($LASTEXITCODE -ne 0) { throw "codeql database analyze ($suite) failed ($LASTEXITCODE)" }
        $exceptions = @($known[$suite])
        $blocking = 0
        $waived = 0
        foreach ($run in (Get-Content $sarif -Raw | ConvertFrom-Json).runs) {
            foreach ($res in @($run.results)) {
                $uri = $res.locations[0].physicalLocation.artifactLocation.uri
                $ex = $exceptions | Where-Object { $_ -and $res.ruleId -eq $_.rule -and $uri -match $_.file }
                if ($ex) { $waived++; Write-Host "   waived $($res.ruleId) in $uri ($($ex[0].why))" }
                else { $blocking++; Write-Host "   $($res.ruleId) in $uri" }
            }
        }
        Write-Host ("   {0}: {1} blocking, {2} waived -> {3}" -f $suite, $blocking, $waived, (Split-Path $sarif -Leaf))
        if ($blocking -gt 0) { $failed += "CodeQL $suite`: $blocking results" }
    }
}

# --- 3. DVL ---------------------------------------------------------------------
if (-not $SkipDvl) {
    Write-Host "== Driver Verification Log"
    # dvl.exe reads the CA reports and the CodeQL SARIF beside the driver
    # project; the kit's target runs it in the project directory.
    $main = Join-Path $driver 'Source\Main'
    foreach ($s in Get-ChildItem $driver -Filter '*.sarif') { Copy-Item $s.FullName $main -Force }
    # dvl.exe /create takes the current directory as the project directory.
    Push-Location $main
    try {
        & msbuild "$main\Main.vcxproj" /p:Configuration=Release /p:Platform=x64 /t:dvl /v:m /nologo 2>&1 | Where-Object { $_ -match 'error|warning|DVL|dvl' } | ForEach-Object { Write-Host "   $_" }
    } finally {
        Pop-Location
    }
    $dvl = Get-ChildItem $main -Filter '*.DVL.XML' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dvl) { Write-Host "   $($dvl.FullName)" } else { $failed += 'no DVL produced' }
}

if ($failed) {
    Write-Host "FAILED:"; $failed | ForEach-Object { Write-Host "   $_" }
    exit 1
}
Write-Host "static tier clean"
