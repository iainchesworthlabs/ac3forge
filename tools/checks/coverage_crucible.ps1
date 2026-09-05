# Line and branch coverage for AC3Forge Crucible (apps/crucible), the
# Windows counterpart of coverage_report.sh: runs the "crucible" (Catch2)
# and "crucible-ui" (Qt Quick Test) ctest labels of a config-windows-llvm-coverage
# build under LLVM_PROFILE_FILE, merges the profiles, and prints llvm-cov's
# per-file report over apps/crucible. Also writes an HTML report next to the
# build for browsing the uncovered lines.
#
# -Labels is a ctest label regex (ctest -L), and so is the coverage preset's
# filter in CMakePresets.json (test-windows-llvm-coverage). Both default to
# '^crucible(-ui)?$', naming the two labels this build carries: the Catch2
# engine cases and the Qt Quick suites (apps/crucible/ui/tests/CMakeLists.txt
# sets the second). Spelling both out keeps the Qt Quick half in the figure
# however the regex is read - a bare 'crucible' only reaches 'crucible-ui'
# because ctest matches a label anywhere in the string, and an anchored
# reading of the same filter would drop five suites out of the number without
# saying so.
#
#   cmake --preset config-windows-llvm-coverage
#   cmake --build --preset build-windows-llvm-coverage
#   .\tools\checks\coverage_crucible.ps1 -BuildDir <build dir>
#
# Gates: none yet. The first measured figures go into
# docs/crucible/promotion.md, beside the rest of the promotion's phase
# record; docs/platforms/windows-demo.md keeps the history the product grew
# from. Floors follow once the figures are stable, the way
# coverage_report.sh gates the library.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [string]$Llvm = 'C:\Program Files\LLVM\bin',
    [string]$Labels = '^crucible(-ui)?$'
)
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path $BuildDir).Path
$profdata = Join-Path $Llvm 'llvm-profdata.exe'
$cov = Join-Path $Llvm 'llvm-cov.exe'
foreach ($tool in @($profdata, $cov)) { if (-not (Test-Path $tool)) { throw "missing $tool" } }

$profiles = Join-Path $BuildDir 'coverage-profiles'
if (Test-Path $profiles) { Remove-Item $profiles -Recurse -Force }
New-Item -ItemType Directory -Path $profiles | Out-Null

# One .profraw per process: every ctest entry is its own process and the
# Qt Quick suites are one process each.
$env:LLVM_PROFILE_FILE = Join-Path $profiles '%p-%m.profraw'
Push-Location $BuildDir
try {
    & ctest -L $Labels --output-on-failure
    $testExit = $LASTEXITCODE
} finally {
    Pop-Location
    Remove-Item Env:\LLVM_PROFILE_FILE
}
if ($testExit -ne 0) { Write-Warning "ctest exited $testExit; the report below covers what ran" }

$raw = Get-ChildItem $profiles -Filter '*.profraw'
if (-not $raw) { throw "no .profraw written under ${profiles}: is this an AC3FORGE_ENABLE_COVERAGE build?" }
$merged = Join-Path $profiles 'crucible.profdata'
& $profdata merge -sparse -o $merged @($raw.FullName)
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed ($LASTEXITCODE)" }

# The instrumented binaries: the Catch2 runner, the window's test binary,
# and the window itself (its CrucibleController is compiled into the test
# binary; the executable is listed so its own main.cpp shows as uncovered
# rather than missing).
$binaries = @('bin\ac3tests.exe', 'bin\ac3crucible_qmltests.exe', 'bin\ac3crucible.exe') |
    ForEach-Object { Join-Path $BuildDir $_ } | Where-Object { Test-Path $_ }
if (-not $binaries) { throw "no instrumented binaries under $BuildDir\bin" }
$objects = @()
foreach ($b in $binaries[1..($binaries.Count - 1)]) { $objects += @('-object', $b) }

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$sources = Join-Path $root 'apps\crucible'
Write-Host "`nline / branch coverage over apps/crucible ($Labels):`n"
& $cov report $binaries[0] @objects "-instr-profile=$merged" "-ignore-filename-regex=.*(tests|shared|_autogen|\.qt|driver)[\\/].*" $sources
if ($LASTEXITCODE -ne 0) { throw "llvm-cov report failed ($LASTEXITCODE)" }

$html = Join-Path $BuildDir 'coverage-html'
& $cov show $binaries[0] @objects "-instr-profile=$merged" -format=html "-output-dir=$html" -show-branches=count "-ignore-filename-regex=.*(tests|shared|_autogen|\.qt|driver)[\\/].*" $sources | Out-Null
Write-Host "`nHTML report: $html\index.html"
