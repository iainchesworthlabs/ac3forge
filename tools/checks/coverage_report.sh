#!/usr/bin/env bash
#
# Coverage report + per-component statement/branch gate.
#
# One gcov extraction pass over an AC3FORGE_ENABLE_COVERAGE build (the
# config-linux-gcc-coverage preset - see CMakePresets.json), then one cheap
# gate pass per component off the shared JSON trace. Line and branch coverage
# are gated PER COMPONENT rather than as one blended number: src/forge is an
# order of magnitude larger than any container writer, so a blend would let a
# real regression in src/mpegts or src/capi hide inside ordinary drift in
# src/forge - and "which module is thin" is exactly the question a
# per-component table exists to answer.
#
# apps/cli is gated here too (coverage floors), not just src/. It is about 6,500
# lines across seven command modules, it is the executable the codec matrix,
# the gold-reference gate and the encoder-space fuzzer all drive, and it had
# no floor at all - while the two CLI bugs this project has actually shipped
# (the stdout/stderr leak and the Windows argv mangling) were both in exactly
# that kind of silently untested front-end path. Its per-command breakdown is
# printed below the gate so a thin command shows up as thin rather than
# averaging away inside the aggregate.
#
# apps/gui is NOT gated and is deliberately out of scope. Its C++ needs a Qt
# kit on the coverage leg, and no Linux CI leg installs one today
# (.github/workflows/_build.yml installs Qt only on the `gui: true` matrix
# entries, which are plain builds, not instrumented ones). Adding it means
# either putting Qt on the coverage job or standing up a second instrumented
# leg - a separate decision with its own runner-time cost, not something to
# smuggle in behind a threshold table. apps/gui's interactive surfaces are
# covered by its own Qt Quick tests, and its Qt-free pieces are exercised by
# ac3tests on every leg even though no row below gates them: RecordingSink
# (since moved to apps/common) and, from 2026-09-06,
# apps/gui/gui_diagnostics.cpp, whose whole reason for being Qt-free is that
# the no-secrets rule it holds is checked on legs that build no window.
#
# apps/crucible is out of scope here for the same reason and gated anyway,
# somewhere else: tools/checks/coverage_crucible.ps1 holds its line and branch
# floors and runs on the Windows clang-cl leg, where a Qt kit already is
# (.github/workflows/_build.yml, "Crucible coverage floor"). Most of that tree
# only executes under Qt - the window, its controller, and the Qt Quick suites
# that are the only thing driving its platform seams - so a figure taken in
# this job would cover the platform-free engine core and nothing else.
#
# src/sendspin and apps/hearth (Hearth, planning/hearth-reference-player.md) ARE gated here,
# unlike apps/crucible above: config-linux-gcc-coverage is the one coverage preset that turns
# AC3FORGE_BUILD_HEARTH on (CMakePresets.json), and both ac3::sendspin and ac3hearth_engine link
# ac3::coverage themselves for exactly the reason apps/cli's own link does - see their
# CMakeLists.txt. apps/hearth/testsink joins the apps/hearth row (its sources link into ac3tests
# too); apps/hearth/testserver does not, since nothing on this leg ever runs that executable, and
# apps/hearth/ui is Qt - same reason apps/gui is out of scope above, no Qt kit on this leg.
#
# Run by .github/workflows/ci.yml's coverage job after `ctest`; runnable
# locally the same way, from the repository root (see docs/building.md):
#
#   cmake --preset config-linux-gcc-coverage
#   cmake --build --preset build-linux-gcc-coverage -- -k 0
#   ctest --preset test-linux-gcc-coverage -LE Performance
#   ./tools/checks/coverage_report.sh -g gcov-16
#
# Thresholds sit a few points under each component's measured baseline (the
# table below records the measurement each floor was set against) so ordinary
# in-flight churn does not trip the gate while a real regression still fails
# the job. Raise them as the suite grows rather than leaving the headroom in
# place indefinitely - see ci.yml's coverage job comment for the calibration
# history and why hosted-runner numbers are the calibration authority.
#
# Usage:  ./tools/checks/coverage_report.sh [-b <build-dir>] [-g <gcov-executable>]
# Exit:   0 = every gate met, 1 = at least one gate missed or a component had
#         no coverage data at all. Every component is reported before the
#         failure exit, so the log always shows the whole table rather than
#         just the first miss.

set -euo pipefail

build_dir="build/config-linux-gcc-coverage"
gcov_exe="gcov"
while getopts "b:g:" opt; do
    case "$opt" in
        b) build_dir="$OPTARG" ;;
        g) gcov_exe="$OPTARG" ;;
        *) echo "Usage: $0 [-b <build-dir>] [-g <gcov-executable>]" >&2; exit 2 ;;
    esac
done

if [[ ! -f CMakePresets.json ]]; then
    echo "::error::coverage: run this from the repository root (CMakePresets.json not found)" >&2
    exit 2
fi

# Component floors, one row per component: <path> <line%> <branch%>. A path,
# not a bare name, since coverage floors added apps/ alongside src/.
#
# Calibrated 2026-08-20 (src/*) and 2026-08-24 (apps/cli, re-measured after
# merging container readers (mkv/mp4/ts)'s container-reader/probe work) against WSL2 runs on
# the CI toolchain pins (gcov 15.2.0, gcovr 8.6), measured per component as:
#
#   forge 93.2/86.0 audio 34.2/22.8   signing 89.2/68.9  matroska 92.9/87.7
#   mp4 94.9/92.5   mpegts 94.1/90.7  capi 87.8/79.2      ac3adm 87.9/82.4
#   admbridge 91.8/85.6               apps/cli 54.0/46.5
#
# Each floor sits ~4-8 points under its measurement: a couple of points for
# the known WSL-reads-higher-than-hosted effect (see ci.yml's coverage job
# comment), the rest as ordinary in-flight-churn headroom. Re-check against
# the first hosted run and tighten if the margin proves generous.
#
# src/audio's floor is low because its MEASUREMENT is low, deliberately not
# rounded up to look respectable: no test opens an audio device, so the ALSA
# capture/monitor/passthrough device paths (the bulk of src/audio's lines)
# never execute headless - only the device-naming/format logic does. The
# floor holds the line while that is true; raising it is a matter of writing
# the missing tests, not of editing this table. src/capi sat in the same
# paragraph (48.4/27.1: test_capi.cpp barely touched the E-AC-3 half,
# src/capi/src/eac3.cpp measured 31% line) until that half's tests were
# written; its remaining gap is src/capi/src/internal.hpp's guard() catch
# clauses (allocation failure is not fakeable from a test) and the
# defensively unreachable enum fallthroughs beside them.
#
# apps/cli's floor is the same kind of honest-low number, and its margin is
# the widest here for a reason its own breakdown below makes visible: two of
# its command modules (audio_io, live_audio) only execute at all to the
# extent the runner has a capture or render endpoint, and that differs
# between a developer's WSL (which has an ALSA `default`) and a headless CI
# container (which has nothing). Roughly 15% of apps/cli's lines sit behind
# that difference, so the floor is set to survive the no-device case rather
# than the measurement that produced it.
#
# src/sendspin and apps/hearth are NOT calibrated the same way as the rows above: nothing has
# measured them against the CI toolchain pin yet (Hearth only just started building on this leg -
# see CMakePresets.json's config-linux-gcc-coverage). Their floors are deliberately low - proof
# the components produce coverage data at all (the no-data check a few lines down is the real
# protection until a first real run exists), not a claim about what fraction of either is
# exercised. Tighten both against the first hosted-runner coverage run that includes them, the
# same "measure, then set a few points under" process every other row already went through.
components="
src/forge      88 78
src/audio      25 15
src/signing    82 55
src/matroska   88 85
src/mp4        90 85
src/mpegts     88 85
src/capi       82 72
src/ac3adm     82 75
src/admbridge  85 78
src/sendspin   10 5
apps/cli       40 34
apps/hearth    10 5
"

json="$build_dir/coverage.json"
html="$build_dir/coverage.html"

# The one expensive pass: run gcov over every object file and keep the result
# as a JSON trace the per-component gates below re-read, so N gates don't
# mean N re-extractions. Also writes the human-readable HTML report ci.yml
# uploads as its artifact (both outputs live in $build_dir so -b moves them
# together with the objects they describe; --html-self-contained so the
# uploaded pages carry their own CSS/JS instead of needing sidecar files the
# artifact glob would have to chase), and prints the whole-library summary.
#
# --gcov-ignore-parse-errors=suspicious_hits.warn: mdct.cpp's
# ForwardCosTable-driven hot loop (src/forge/src/core/mdct.cpp) trips a documented gcov
# bug (gcc.gnu.org/bugzilla#68080, a false "suspicious hit value" on a tight
# accumulation loop) that otherwise aborts gcovr outright rather than just
# under/over-reporting that one line's count - gcovr's own error message
# names this exact flag as the fix. Warn, not skip, so a genuinely new
# suspicious-hit line elsewhere still shows up in the log instead of
# vanishing silently.
#
# --gcov-ignore-parse-errors=negative_hits.warn: the same gcov bug
# (bugzilla#68080), a different symptom - bitalloc_memo.hpp:42's memo-validity
# check (`return valid && sample_rate == rate && csnroffst == csnr && ...`) is
# the same shape of tight, heavily-short-circuited boolean chain as mdct.cpp's
# loop, and gcov produces a negative rather than suspicious hit count for it.
# gcovr's own error message names this exact flag too; each ignored category
# is its own flag (gcovr's --gcov-ignore-parse-errors takes one value per
# occurrence, not a combined list) so a genuinely new problem in either
# category still shows up rather than both going quiet under one blanket
# `all`.
gcovr --root . \
    --filter 'src/(forge|audio|signing|matroska|mp4|mpegts|capi|ac3adm|admbridge|sendspin)/.*' \
    --filter 'apps/cli/.*' \
    --filter 'apps/hearth/(engine|testsink)/.*' \
    --gcov-executable "$gcov_exe" \
    --exclude-throw-branches --exclude-unreachable-branches \
    --gcov-ignore-errors=no_working_dir_found \
    --gcov-ignore-parse-errors=suspicious_hits.warn \
    --gcov-ignore-parse-errors=negative_hits.warn \
    --object-directory "$build_dir" \
    --json "$json" --html-details "$html" --html-self-contained --print-summary

fail=0
while read -r comp line_min branch_min; do
    [[ -n "$comp" ]] || continue

    # A component with zero files in the trace is a broken measurement (built
    # without instrumentation, or not built at all - e.g. a coverage preset
    # that lost AC3FORGE_BUILD_ADM=ON or AC3FORGE_BUILD_CLI=ON), not a
    # 0%-covered component. Fail loudly rather than letting a silent no-data
    # "pass" or a misleading 0% stand in for the real answer. This is exactly
    # what caught apps/cli linking an instrumented library without being
    # instrumented itself - see cmake/Coverage.cmake's own note.
    if ! grep -q "$comp/" "$json"; then
        echo "::error::coverage: no data for $comp - was it built with AC3FORGE_ENABLE_COVERAGE on?"
        fail=1
        continue
    fi

    echo
    echo "== $comp (gate: line >= $line_min%, branch >= $branch_min%) =="
    if ! gcovr --root . --add-tracefile "$json" --filter "$comp/.*" \
        --print-summary \
        --fail-under-line "$line_min" --fail-under-branch "$branch_min"; then
        echo "::error::coverage gate missed for $comp (need line >= $line_min%, branch >= $branch_min%)"
        fail=1
    fi
done <<EOF
$components
EOF

# apps/cli's per-command breakdown. Reported, never gated: one floor on the
# aggregate is what stops a regression, and a floor per command module would
# be ten more numbers to re-calibrate every time a command moves between
# files. What this exists for is visibility - the aggregate alone would let
# a command sitting at 0% hide behind six that are not, which is precisely
# the state apps/cli was in when this gate was written (containers, audio_io
# and live_audio were all at 0.0% line while the aggregate read 44.9%).
echo
echo "== apps/cli per command (reported, not gated) =="
printf '%-26s %8s %8s\n' "module" "line" "branch"
for src in apps/cli/*.cpp apps/cli/commands/*.cpp; do
    [[ -e "$src" ]] || continue
    # --print-summary writes its two lines after the per-file table, so the
    # whole report is captured and those two picked out of it. Redirecting the
    # table away with --txt /dev/null takes the summary with it.
    summary="$(gcovr --root . --add-tracefile "$json" --filter "${src}" \
        --print-summary 2>/dev/null || true)"
    line_pct="$(echo "$summary" | awk '/^lines:/ {print $2}')"
    branch_pct="$(echo "$summary" | awk '/^branches:/ {print $2}')"
    printf '%-26s %8s %8s\n' "${src#apps/cli/}" "${line_pct:-n/a}" "${branch_pct:-n/a}"
done

exit "$fail"
