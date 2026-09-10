#!/usr/bin/env bash
# Build and run the minimum-footprint decoder probe for ESP32-C3, under QEMU,
# then gate on what it reports. The third sibling of run_baremetal_probe.sh
# and run_esp32s3_probe.sh, and the fixed-point tier's own leg
# (planning/arithmetic-tiers.md, Phase D).
#
#   . $IDF_PATH/export.sh
#   tools/checks/run_esp32c3_probe.sh                 # the fixed tier
#   tools/checks/run_esp32c3_probe.sh --scalar=float  # the same part in float
#
# WHAT THIS GATES that the other two legs do not.
#
# The C3 has no floating-point unit, which is why the fixed-point tier exists,
# and it is a third ARCHITECTURE: RV32IMC beside x86-64 and Thumb-2. The tier's
# central claim is that its decode is bit-identical on every machine, integer
# arithmetic having no rounding mode, no fused multiply-add and no C library's
# last bit to differ by. Two architectures agreeing is a coincidence a third
# can break, so this leg holds the probe's <codec>.pcm_hash lines to the same
# pinned values (tests/golden/fixed-probe-pcm-hashes.json) the host and the
# Cortex-M3 leg are held to. That check is this leg's reason for existing; the
# footprint ceilings below are the ordinary ones every probe leg carries.
#
# NOT speed. QEMU is not cycle-accurate and reports a CPU clock that disagrees
# with its own boot log, so the probe's us_per_frame lines are shape only. The
# instruction counts that mean something come from the Cortex-M3 leg's -icount
# mode (docs/performance-trend.md); what a C3 does per frame in cycles needs a
# board.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT="$REPO/apps/baremetal/platform/esp32c3"
PINS="$REPO/tests/golden/fixed-probe-pcm-hashes.json"

# The decode arithmetic. `fixed` is the project's own default and the reason
# this target exists; `float` builds the same part with the S3's tier, which is
# the comparison docs/platforms/esp32.md's C3 row is made of. Passed to CMake
# explicitly in both cases, for the reason the S3 runner gives about
# AC3FORGE_STAGE_TIMERS: an option set on one run stays in the cache for the
# next, and a run that silently inherited the other tier would report the
# wrong thing under this one's hash pins.
SCALAR=fixed
STAGE_TIMERS=OFF
for arg in "$@"; do
    case "$arg" in
        --scalar=*) SCALAR="${arg#--scalar=}" ;;
        --stage-timers) STAGE_TIMERS=ON ;;
        *) echo "usage: run_esp32c3_probe.sh [--scalar=fixed|float] [--stage-timers]" >&2; exit 2 ;;
    esac
done

cd "$PROJECT"

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "error: IDF_PATH is not set - source \$IDF_PATH/export.sh first" >&2
    exit 1
fi

# --- ceilings --------------------------------------------------------------
# Bytes, measured with headroom, as on the other two legs.
#
# The C3's internal SRAM is 400 KB against the S3's 512, and its heap is
# regioned the same way: the probe reports 249,180 bytes free with a largest
# block of 114,688. A total is therefore not a budget here either, and this
# part proves it - the 7.1.4 fixture, whose peak is 238,094, failed on a
# 6,144-byte request with eleven kilobytes still showing free. That fixture is
# skipped by the project's own AC3FORGE_PROBE_HEAP_BUDGET_BYTES rather than
# gated here; what this ceiling holds is the peak of the eleven that do run.
: "${AC3FORGE_ESP32C3_MAX_HEAP_BYTES:=235000}"
: "${AC3FORGE_ESP32C3_MAX_RETAINED_BYTES:=1024}"
: "${AC3FORGE_ESP32C3_MAX_STEADY_ALLOCS_PER_FRAME:=100}"
: "${AC3FORGE_ESP32C3_MIN_STACK_FREE_BYTES:=8192}"

OUTPUT="$(mktemp)"
trap 'rm -f "$OUTPUT"' EXIT

# fullclean when the scalar changes, for the reason the S3 runner cleans
# between profiles: AC3FORGE_DECODE_SCALAR reaches the library as a cache
# variable that a warm build directory has already resolved into an include
# path, and reconfiguring over the top keeps the previous tier's archive -
# which links, runs, and reports the wrong arithmetic under this run's pins.
STAMP="build/.ac3forge-scalar"
if [[ -d build && "$(cat "$STAMP" 2>/dev/null || echo)" != "$SCALAR" ]]; then
    echo "note: build directory holds a different scalar - cleaning" >&2
    idf.py fullclean
fi

idf.py set-target esp32c3
idf.py -DAC3FORGE_DECODE_SCALAR="$SCALAR" -DAC3FORGE_STAGE_TIMERS="$STAGE_TIMERS" build
mkdir -p build && printf '%s' "$SCALAR" > "$STAMP"

echo
echo "== running ac3probe on qemu-system-riscv32 (esp32c3) =="
# No semihosting exit, as on the S3 leg: an ESP-IDF application returns from
# app_main into a FreeRTOS task that is then deleted, and QEMU keeps running.
# The timeout ending the run is the EXPECTED outcome and the gate below is on
# the captured output rather than on an exit code.
timeout 900 idf.py qemu 2>&1 | tee -a "$OUTPUT" || true

if ! grep -q '^result=pass' "$OUTPUT"; then
    echo "::error title=ESP32-C3 probe failed::the probe did not report result=pass" >&2
    grep -E 'result=|reason=|Guru Meditation|assert failed|status=fail' "$OUTPUT" >&2 || true
    exit 1
fi

# And nothing wrong after it: no panic output and no second boot, for the
# reason run_esp32s3_probe.sh gives beside its copy of this line.
python3 "$REPO/tools/checks/check_esp_console.py" --title "ESP32-C3 probe" "$OUTPUT"

# --- the tier's own check --------------------------------------------------
# Only for the fixed tier: the float tier's hashes are the compiler's business
# and vary between legs by design, so there is nothing to pin them to.
if [[ "$SCALAR" == "fixed" ]]; then
    echo
    echo "== PCM hashes against the pinned ones =="
    python3 "$REPO/tools/checks/check_probe_hashes.py" --expected "$PINS" "$OUTPUT"
fi

heap=$(sed -n 's/.*heap\.peak_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$heap" ]]; then
    echo "error: the probe reported no heap.peak_bytes line" >&2
    exit 1
fi
echo "peak heap: $heap bytes (ceiling $AC3FORGE_ESP32C3_MAX_HEAP_BYTES)"
if (( heap > AC3FORGE_ESP32C3_MAX_HEAP_BYTES )); then
    echo "::error title=ESP32-C3 footprint regression::peak heap is $heap bytes, ceiling is $AC3FORGE_ESP32C3_MAX_HEAP_BYTES" >&2
    exit 1
fi

retained=$(sed -n 's/.*heap\.retained_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$retained" ]]; then
    echo "error: the probe reported no heap.retained_bytes line" >&2
    exit 1
fi
echo "retained after teardown: $retained bytes (ceiling $AC3FORGE_ESP32C3_MAX_RETAINED_BYTES)"
if (( retained > AC3FORGE_ESP32C3_MAX_RETAINED_BYTES )); then
    echo "::error title=ESP32-C3 footprint regression::$retained bytes are still live after every decoder was destroyed, ceiling is $AC3FORGE_ESP32C3_MAX_RETAINED_BYTES" >&2
    exit 1
fi

# Every fixture's steady-state churn against one ceiling, with the fixture
# names taken from the probe's own output rather than a list kept here - see
# run_esp32s3_probe.sh's copy of this loop for why that matters.
CHURN=$(grep -o '[a-z0-9_]*\.steady_allocs_per_frame=[0-9]*' "$OUTPUT" | sed 's/\.steady_allocs_per_frame=/ /')
if [[ -z "$CHURN" ]]; then
    echo "error: the probe reported no <fixture>.steady_allocs_per_frame line" >&2
    exit 1
fi
while read -r codec per_frame; do
    echo "churn: ${codec} = ${per_frame} allocations/frame (ceiling ${AC3FORGE_ESP32C3_MAX_STEADY_ALLOCS_PER_FRAME})"
    if (( per_frame > AC3FORGE_ESP32C3_MAX_STEADY_ALLOCS_PER_FRAME )); then
        echo "::error title=ESP32-C3 footprint regression::${codec} steady-state allocations are $per_frame per frame, ceiling is $AC3FORGE_ESP32C3_MAX_STEADY_ALLOCS_PER_FRAME" >&2
        exit 1
    fi
done <<< "$CHURN"

stack=$(sed -n 's/.*esp32c3\.main_task_stack_free_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$stack" ]]; then
    echo "error: the probe reported no esp32c3.main_task_stack_free_bytes line" >&2
    exit 1
fi
echo "main task stack free: $stack bytes (floor $AC3FORGE_ESP32C3_MIN_STACK_FREE_BYTES)"
if (( stack < AC3FORGE_ESP32C3_MIN_STACK_FREE_BYTES )); then
    echo "::error title=ESP32-C3 stack headroom::the decode left $stack bytes of the main task's stack free, floor is $AC3FORGE_ESP32C3_MIN_STACK_FREE_BYTES" >&2
    exit 1
fi

echo
echo "ESP32-C3 probe: pass (scalar=$SCALAR)"
