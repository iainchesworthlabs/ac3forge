#!/usr/bin/env pwsh
#
# Platform-isolation guard.
#
# ac3forge branches on the operating system in CMake, never in the preprocessor:
# src/audio/CMakeLists.txt picks one src/audio/src/backend/<backend>/ directory
# (alsa/android/macos/pipewire/posix/windows) for the target OS, so exactly one
# audio_backend.cpp/capture.cpp/monitor.cpp/passthrough.cpp set is ever
# compiled. That only stays true if nobody reaches for an #ifdef, and an #ifdef
# is the path of least resistance the moment a second platform misbehaves --
# hence this check.
#
# The rule here is stricter than "no OS macros": NO preprocessor conditional of
# any kind is allowed in the trees below. The codebase has none today, so the
# check costs nothing to keep at zero, and zero is a far easier line to hold
# than "only the justified ones". Header-configuration defines that a platform
# header genuinely requires (WIN32_LEAN_AND_MEAN, NOMINMAX) belong in
# target_compile_definitions -- see the WIN32 block in src/audio/CMakeLists.txt
# for the worked example.
#
# The scan covered src/ and apps/ only until 2026-09-23, and tests/ had quietly
# become where every violation lived: nineteen copies of a `#ifdef _WIN32`
# getpid() branch, eight of a cmd.exe quoting one, eleven AVX2 cases whose
# bodies a non-x86_64 leg never even parsed, and an MSVC-only pair of ABI size
# assertions. Each is now the same directory-selected shape the rest of the
# tree uses -- tests/platform/<os>/, tests/core/avx2/{present,absent}/,
# tests/render/abi/{msvc,unknown}/ -- and python/ likewise
# (python/src/ac3forge_ext/{signing,containers}/{present,absent}/), so every
# tree here starts at zero rather than being grandfathered in with a waiver list.
#
# NOT scanned, deliberately: esp-idf/. That tree is an ESP-IDF component built
# by idf.py, not by this repository's CMake, and its `#if CONFIG_*` guards are
# Kconfig symbols -- the documented IDF idiom, and in the CONFIG_SPIRAM case
# load-bearing in a way a directory split would not reproduce: on a target with
# no PSRAM bus, esp_psram_get_size() is never exposed to the linker at all
# (see esp-idf/ac3forge/src/control.cpp's own comment). Holding this rule over
# somebody else's build system, with no toolchain here to verify against, would
# be a change made blind.
#
# Include guards are not affected: the codebase uses #pragma once.
#
# One other narrow exception, added for src/capi/include/ac3forge_c/ac3forge.h
# (C API): `#ifdef __cplusplus` / `extern "C" {` / `#endif` is the
# standard idiom that lets one header be included from both a C and a C++
# translation unit, which a C-callable public header genuinely needs -
# `extern "C"` is not even legal syntax outside `#ifdef __cplusplus`, since a
# .c file's compiler does not know the token. This is a LANGUAGE-DIALECT
# marker, not a platform or feature branch - the thing this check exists to
# forbid - so a bare `#ifdef __cplusplus ... #endif` pair (no #else/#elif
# inside it) is tracked separately below and excluded from violations; every
# other conditional, including one that merely mentions __cplusplus in an
# #if/#elif expression alongside something else, is still flagged.
#
# Usage:  ./tools/checks/check_platform_macros.ps1 [-Root <repo-root>]
# Exit:   0 = clean, 1 = violation(s) found, 2 = bad invocation.

param(
    [string]$Root = $PWD
)

$ErrorActionPreference = 'Stop'

# Any conditional-compilation directive. Deliberately broad: #if 0 to comment a
# block out, or a feature-flag #ifdef, are just as unwelcome as a platform one.
# `#define` is NOT matched -- constants and macros are ordinary C++ -- and
# neither is #include or #pragma.
$directivePattern = '^\s*#\s*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b'
$cplusplusGuardPattern = '^\s*#\s*ifdef\s+__cplusplus\b'

$srcRoot = Join-Path $Root 'src'
if (-not (Test-Path $srcRoot)) {
    Write-Error "No src/ directory under '$Root'. Pass -Root <repo-root>."
    exit 2
}

# apps/ (the runnable-application tree - ac3cli, ac3gui, the Android and WASM
# demos) carries the same rule and is scanned alongside src/ once it exists.
# Optional rather than required: a repo state mid-way through the src/->apps/
# consolidation (or a checkout of an older tag, before apps/ existed at all)
# still has a valid src/ to scan even with no apps/ yet.
$scanRoots = @($srcRoot)

# Every other first-party C++ tree, each optional in the same way and for the
# same reason apps/ is: a checkout mid-way through a reorganisation, or of an
# older tag from before one of these existed, still has a valid src/ to scan.
# fuzz/, examples/ and tools/ were already clean when they were added here on
# 2026-09-23 and cost nothing to hold; tests/ and python/ were cleaned to join
# them.
foreach ($name in @('apps', 'tests', 'fuzz', 'examples', 'tools', 'python')) {
    $candidate = Join-Path $Root $name
    if (Test-Path $candidate) {
        $scanRoots += $candidate
    }
}

# '*.mm' was added on 2026-09-06 with the first Objective-C++ in the tree:
# src/audio/src/backend/macos/process_tap.mm, the seam for Core Audio's
# process tap, and apps/crucible's foreground.mm and app_icon_provider.mm.
# Those files say in their own headers that this check holds the no-#ifdef
# rule over them, and that was not true while the extension list stopped at
# the C and C++ ones. A .mm is where an #ifdef would be most tempting, since
# it is the one language here that only ever compiles on one operating
# system, and it has its own conditionals to reach for - @available answers
# the runtime version question, but __IPHONE_OS_VERSION_MIN_REQUIRED and its
# neighbours are right there. `#import` is not a conditional and does not
# match the directive pattern.
$files = Get-ChildItem -Path $scanRoots -Recurse -File -Include '*.h', '*.hpp', '*.cpp', '*.cc', '*.cxx', '*.inl', '*.mm'

# apps/windows/driver/ is Microsoft's Simple Audio Sample under its own MS-PL
# licence (see its README): a separate kernel-mode work that shares no code
# with the rest of the tree, kept as close to the sample as possible so its
# cuts read as a diff. It is written the way Windows drivers are written,
# include guards and all, and the rule this check holds is about ac3forge's
# own code selecting platforms in CMake - so the sample is left out.
$driverRoot = Join-Path (Join-Path $Root 'apps') 'windows\driver'
$files = @($files | Where-Object { -not $_.FullName.StartsWith($driverRoot, [System.StringComparison]::OrdinalIgnoreCase) })

# Build output is not source, and a build configured INSIDE the tree puts some
# of it under apps/: apps/baremetal/platform/esp32s3 is built in place by
# idf.py (docs/platforms/bare-metal/esp32-s3.md), and CMake's generated ac3/export.hpp is a
# conditional-compilation header by its very nature. One such build produced
# 658 "violations", every one of them generated and none of them anybody's
# code. src/quarantine/ was the same story waiting to happen on the src/ side.
#
# Ask git which files are source rather than pattern-matching directory names
# here: .gitignore already carries that answer and stays the single place it is
# written down, so a new ignored directory costs no second edit in this file.
#
#   --cached           files git tracks
#   --others           plus files it does not yet track
#   --exclude-standard minus everything .gitignore excludes
#
# which is exactly "in the repository, or on its way in". `--others` is the
# half that matters for a local pre-commit run: a source file that is new and
# not yet committed is precisely the case worth catching before it lands, and
# a tracked-only listing would not see it.
#
# -z makes the output NUL-separated and UNQUOTED. Without it git C-quotes any
# path holding a space or a non-ASCII byte, quotation marks and all, and the
# comparison below would then miss that path and scan the file anyway.
$sourcePaths = $null
# PowerShell 7.3+ turns a non-zero native exit code into a terminating error
# while $ErrorActionPreference is 'Stop', so that translation is switched off
# around the call and restored afterwards.
$previousNativePreference = $PSNativeCommandUseErrorActionPreference
try {
    $PSNativeCommandUseErrorActionPreference = $false
    $listed = git -C $Root ls-files -z --cached --others --exclude-standard -- $scanRoots 2>$null
    if ($LASTEXITCODE -eq 0) {
        $sourcePaths = [System.Collections.Generic.HashSet[string]]::new(
            [string[]]@((@($listed) -join '') -split "`0" | Where-Object { $_ }),
            [System.StringComparer]::OrdinalIgnoreCase)
    }
} catch {
    # No git on PATH, or not a work tree (an exported tarball, a vendored
    # copy). Scanning everything is the safe direction to fail in: noisier,
    # never quieter.
    $sourcePaths = $null
} finally {
    $PSNativeCommandUseErrorActionPreference = $previousNativePreference
}

$scannedBeforeFilter = $files.Count
if ($null -ne $sourcePaths -and $sourcePaths.Count -gt 0) {
    $files = @($files | Where-Object {
        $sourcePaths.Contains([System.IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\', '/'))
    })
}
$excludedCount = $scannedBeforeFilter - $files.Count

$violations = @()
foreach ($file in $files) {
    # Per-file stack tracking ONLY whether each currently-open conditional is
    # exactly a plain `#ifdef __cplusplus` guard, so its matching `#endif` can
    # be recognised too - a bare regex match on `#endif` alone cannot tell
    # which opening directive it closes.
    $guardStack = New-Object System.Collections.Generic.Stack[bool]
    foreach ($m in (Select-String -Path $file.FullName -Pattern $directivePattern -AllMatches -CaseSensitive)) {
        $line = $m.Line.Trim()
        $isOpen = $line -match '^\s*#\s*(if|ifdef|ifndef)\b'
        $isEndif = $line -match '^\s*#\s*endif\b'

        if ($isOpen) {
            $isCplusplusGuard = $line -match $cplusplusGuardPattern
            $guardStack.Push($isCplusplusGuard)
            if ($isCplusplusGuard) {
                continue
            }
        } elseif ($isEndif -and $guardStack.Count -gt 0) {
            if ($guardStack.Pop()) {
                continue
            }
        }

        $violations += [pscustomobject]@{
            Path = [System.IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
            Line = $m.LineNumber
            Text = $line
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host ''
    Write-Host 'Platform-isolation violation: preprocessor conditional in a scanned tree.' -ForegroundColor Red
    Write-Host 'Per-OS code is selected by CMake (see the WIN32 block in src/audio/CMakeLists.txt),'
    Write-Host 'so it belongs in its own translation unit, not behind an #ifdef.'
    Write-Host ''
    foreach ($v in $violations) {
        Write-Host ('  {0}:{1}: {2}' -f $v.Path, $v.Line, $v.Text)
        # GitHub Actions annotation; prints harmlessly when run locally.
        Write-Host ('::error file={0},line={1}::Preprocessor conditional - select the variant in CMake instead' -f $v.Path, $v.Line)
    }
    Write-Host ''
    Write-Host ('{0} violation(s) found.' -f $violations.Count) -ForegroundColor Red
    exit 1
}

$summary = "OK: no preprocessor conditionals in src/, apps/, tests/, fuzz/, examples/, tools/ or python/ ($($files.Count) files scanned"
if ($excludedCount -gt 0) {
    # Printed rather than left implicit: this filter turning the check
    # green for the wrong reason - by excluding real source - is the one
    # failure mode that would not announce itself, so what it removed is
    # always on the record.
    $summary += ", $excludedCount skipped as build output or git-ignored"
}
Write-Host "$summary)." -ForegroundColor Green
exit 0
