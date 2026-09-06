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
# The gate is the same shape coverage_report.sh gives the library: a table of
# floors, one row per component with a line percentage and a branch
# percentage, every figure reported before the script exits so the log shows
# the whole picture rather than only the first miss. One row, because
# apps/crucible IS the component - it sits in the family beside src/forge and
# apps/cli, not as a tree of components (docs/family/recasting.md). The
# per-file breakdown printed under it is reported and never gated, the way
# apps/cli's per-command breakdown is: one floor on the aggregate is what
# stops a regression, and a floor per file would be twenty-five numbers to
# re-calibrate every time a class moves.
#
# A failing ctest also fails this script. It did not, and a green run that
# reported the coverage of a suite which had gone red would have been worth
# nothing - the whole point of a floor.
#
# The measured figures and what is thin, with reasons, are in
# docs/crucible/promotion.md beside the rest of the promotion's phase record;
# docs/platforms/windows-demo.md keeps the history the product grew from.
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
    # --no-tests=error for the reason .github/workflows/_build.yml's Linux
    # Crucible pass carries it: a label with nothing behind it once passed a
    # step like this having run nothing. It matters more here now that this
    # script's exit code gates - a mistyped -Labels would otherwise reach the
    # profile merge below and be reported as a build that was never
    # instrumented, which is the wrong thing to go and look at.
    & ctest -L $Labels --no-tests=error --output-on-failure
    $testExit = $LASTEXITCODE
} finally {
    Pop-Location
    Remove-Item Env:\LLVM_PROFILE_FILE
}
# Recorded rather than thrown: the report below still describes what ran, and
# it is more use in the log than an early exit would be. It is folded into
# this script's own exit code at the end - a red suite cannot leave a green
# coverage step behind it.
$fail = 0
if ($testExit -ne 0) {
    Write-Host "::error::ctest exited $testExit; the coverage figures below cover only what ran"
    $fail = 1
}

$raw = Get-ChildItem $profiles -Filter '*.profraw'
if (-not $raw) { throw "no .profraw written under ${profiles}: is this an AC3FORGE_ENABLE_COVERAGE build, and did any test matching '$Labels' run?" }
$merged = Join-Path $profiles 'crucible.profdata'
& $profdata merge -sparse -o $merged @($raw.FullName)
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed ($LASTEXITCODE)" }

# The instrumented binaries: the Catch2 runner, the window's test binary,
# and the window itself (its CrucibleController is compiled into the test
# binary; the executable is listed so its own main.cpp shows as uncovered
# rather than missing).
#
# The first is llvm-cov's positional binary and the rest are -object; both
# spellings below are written to survive a build that produced fewer than
# three, which is the shape a broken configure leaves and which this script
# now has to fail clearly rather than confusingly. @() around the filter,
# because a Where-Object that keeps exactly one item hands back the string
# itself and not a one-element array; Select-Object -Skip 1, because
# `$binaries[1..($binaries.Count - 1)]` reads as a REVERSED range when the
# count is one - 1..0 - and hands back element 0, passing the positional
# binary to llvm-cov a second time as an -object.
$binaries = @(@('bin\ac3tests.exe', 'bin\ac3crucible_qmltests.exe', 'bin\ac3crucible.exe') |
    ForEach-Object { Join-Path $BuildDir $_ } | Where-Object { Test-Path $_ })
if (-not $binaries) { throw "no instrumented binaries under $BuildDir\bin" }
$objects = @()
foreach ($b in ($binaries | Select-Object -Skip 1)) { $objects += @('-object', $b) }

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$sources = Join-Path $root 'apps\crucible'
# The filter every pass below shares, so the reported table, the gated number
# and the HTML cannot describe different sets of files. tests/ and shared/ are
# the suites themselves and the QML copied in from apps/gui; _autogen and .qt
# are Qt's generated moc/qmltyperegistrar output; driver/ is the kernel driver,
# which no user-mode test may execute.
$ignore = '-ignore-filename-regex=.*(tests|shared|_autogen|\.qt|driver)[\\/].*'
$common = @($binaries[0]) + $objects + @("-instr-profile=$merged", $ignore)

Write-Host "`nline / branch coverage over apps/crucible ($Labels):`n"
& $cov report @common $sources
if ($LASTEXITCODE -ne 0) { throw "llvm-cov report failed ($LASTEXITCODE)" }

$html = Join-Path $BuildDir 'coverage-html'
& $cov show @common -format=html "-output-dir=$html" -show-branches=count $sources | Out-Null
Write-Host "`nHTML report: $html\index.html"

# ---------------------------------------------------------------------------
# The gate.
#
# The numbers come from llvm-cov's own JSON export rather than from scraping
# the table printed above: `llvm-cov report`'s column layout is meant for a
# person to read and has changed between LLVM releases, while the export
# carries a version and a stable shape. One export, not one per row - it
# carries `totals` for the whole filter and a `files` entry per source file,
# which is both the gated figure and the breakdown under it.
#
# Component floors, one row per component: <path> <line%> <branch%>, the
# shape tools/checks/coverage_report.sh uses for src/* and apps/cli.
#
# Calibrated 2026-09-06 against the first CI run of this gate, on the
# windows-llvm leg it runs on: 68.5% of lines (1,401 of 4,442 missed) and
# 55.1% of branches, over the `crucible` and `crucible-ui` labels.
#
# The number measured on the development workstation the day before was 76.3%
# and 62.2%, and the eight points between them are not drift. They are the
# Windows platform seams, which cover far more on a machine with a real audio
# environment than on a headless runner: session_monitor.cpp read 85.0% of
# lines here and 34.0% there, default_device.cpp 45.8% and 24.7%,
# library_devices.cpp 64.3% and 11.9%. Those files enumerate endpoints, walk
# audio sessions and ask the shell about windows; a runner with no sound
# device and no desktop takes the early return in each. The workstation figure
# was the more flattering of two honest measurements, and the gate has to hold
# on the machine that runs it.
#
# So the floors are set from the CI reading, about a point and a half under
# it. That margin is deliberately close: this exists to hold today's state,
# not to demand tests nobody has written. Raise it as the suite grows rather
# than leaving headroom in place indefinitely - the same instruction
# coverage_report.sh's own table carries.
#
# What can still move this number is the kit. Qt Quick 3D and Qt SVG are both
# optional (apps/crucible/CMakeLists.txt), and a build without Quick3D leaves
# Room3DView.qml and its controller path out of the denominator. Both were
# present on the run this was calibrated against; if the margin ever proves
# too tight, check that before lowering anything.
$componentFloors = @(
    [pscustomobject]@{ Path = 'apps/crucible'; Line = 67.0; Branch = 53.5 }
)

$exportText = (& $cov export @common '-summary-only' $sources) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "llvm-cov export failed ($LASTEXITCODE)" }
$export = $exportText | ConvertFrom-Json
$measured = $export.data[0]

foreach ($floor in $componentFloors) {
    Write-Host ""
    Write-Host "== $($floor.Path) (gate: line >= $($floor.Line)%, branch >= $($floor.Branch)%) =="
    # No files in the export is a broken measurement - built without
    # instrumentation, or the labels selected nothing - not a 0%-covered
    # component. Fail loudly rather than letting a silent no-data pass or a
    # misleading 0% stand in for the answer, the same rule coverage_report.sh
    # applies when a component is missing from its gcov trace. ctest exiting
    # 0 having matched no tests at all is not hypothetical here: it is how an
    # earlier version of the Linux CI step passed without running anything
    # (.github/workflows/_build.yml says so at the Crucible pass).
    if ($null -eq $measured -or $measured.files.Count -eq 0) {
        Write-Host "::error::coverage: no data for $($floor.Path) - was this an AC3FORGE_ENABLE_COVERAGE build, and did the '$Labels' labels match anything?"
        $fail = 1
        continue
    }
    $line = [double]$measured.totals.lines.percent
    $branch = [double]$measured.totals.branches.percent
    Write-Host ("lines: {0:N1}%   branches: {1:N1}%   functions: {2:N1}%   ({3} files)" -f `
        $line, $branch, [double]$measured.totals.functions.percent, $measured.files.Count)
    if ($line -lt $floor.Line -or $branch -lt $floor.Branch) {
        Write-Host ("::error::coverage gate missed for {0} (need line >= {1}%, branch >= {2}%; got {3:N1}% / {4:N1}%)" -f `
            $floor.Path, $floor.Line, $floor.Branch, $line, $branch)
        $fail = 1
    }
}

# Reported, never gated, and here for one reason: the aggregate alone would
# let a file sitting at 0% hide behind twenty that are not. That is not
# hypothetical either - ui/main.cpp reads 0% because the Qt Quick harness has
# an entry point of its own and never runs the application's.
Write-Host "`n== apps/crucible per file (reported, not gated) =="
Write-Host ('{0,-46} {1,9} {2,9}' -f 'file', 'line', 'branch')
foreach ($file in @($measured.files | Sort-Object filename)) {
    $name = $file.filename
    if ($name.StartsWith($sources, [System.StringComparison]::OrdinalIgnoreCase)) {
        $name = $name.Substring($sources.Length).TrimStart('\', '/')
    }
    Write-Host ('{0,-46} {1,9:N1} {2,9:N1}' -f $name.Replace('\', '/'),
        [double]$file.summary.lines.percent, [double]$file.summary.branches.percent)
}

exit $fail
