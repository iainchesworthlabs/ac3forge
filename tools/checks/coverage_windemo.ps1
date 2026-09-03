# Line and branch coverage for the Desktop Atmos Demo (apps/windows), the
# Windows counterpart of coverage_report.sh: runs the "windemo" (Catch2)
# and "desk" (Qt Quick Test) ctest labels of a config-windows-llvm-coverage
# build under LLVM_PROFILE_FILE, merges the profiles, and prints llvm-cov's
# per-file report over apps/windows. Also writes an HTML report next to the
# build for browsing the uncovered lines.
#
#   cmake --preset config-windows-llvm-coverage
#   cmake --build --preset build-windows-llvm-coverage
#   .\tools\checks\coverage_windemo.ps1 -BuildDir <build dir>
#
# Gates: none yet. The first measured figures go into
# docs/platforms/windows-demo.md; floors follow once they are stable, the
# way coverage_report.sh gates the library.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [string]$Llvm = 'C:\Program Files\LLVM\bin',
    [string]$Labels = 'windemo|desk'
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
$merged = Join-Path $profiles 'windemo.profdata'
& $profdata merge -sparse -o $merged @($raw.FullName)
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed ($LASTEXITCODE)" }

# The instrumented binaries: the Catch2 runner, the window's test binary,
# and the window itself (its DeskController is compiled into the test
# binary; the executable is listed so its own main.cpp shows as uncovered
# rather than missing).
$binaries = @('bin\ac3tests.exe', 'bin\ac3desk_qmltests.exe', 'bin\ac3desk.exe') |
    ForEach-Object { Join-Path $BuildDir $_ } | Where-Object { Test-Path $_ }
if (-not $binaries) { throw "no instrumented binaries under $BuildDir\bin" }
$objects = @()
foreach ($b in $binaries[1..($binaries.Count - 1)]) { $objects += @('-object', $b) }

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$sources = Join-Path $root 'apps\windows'
Write-Host "`nline / branch coverage over apps/windows ($Labels):`n"
& $cov report $binaries[0] @objects "-instr-profile=$merged" "-ignore-filename-regex=.*(tests|shared|_autogen|\.qt|driver)[\\/].*" $sources
if ($LASTEXITCODE -ne 0) { throw "llvm-cov report failed ($LASTEXITCODE)" }

$html = Join-Path $BuildDir 'coverage-html'
& $cov show $binaries[0] @objects "-instr-profile=$merged" -format=html "-output-dir=$html" -show-branches=count "-ignore-filename-regex=.*(tests|shared|_autogen|\.qt|driver)[\\/].*" $sources | Out-Null
Write-Host "`nHTML report: $html\index.html"
