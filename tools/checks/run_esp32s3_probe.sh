#!/usr/bin/env bash
# Build and run the minimum-footprint decoder probe (roadmap PF7) for ESP32-S3,
# under QEMU, then gate on what it reports. The sibling of
# run_baremetal_probe.sh, which does the same for arm-none-eabi.
#
#   . $IDF_PATH/export.sh
#   tools/checks/run_esp32s3_probe.sh
#
# WHAT THIS GATES, and what it deliberately does not:
#
#   - The probe's own verdict. Both fixtures decoded, every channel's level
#     against apps/baremetal/fixture.hpp, result=pass. A failure here means the
#     decode is wrong on Xtensa.
#   - Internal SRAM. The ESP32-S3 has 341,760 bytes of DIRAM and this profile
#     has to fit its static data AND its peak heap inside it. That is the
#     constraint the port actually ran into - it failed with
#     `out_of_memory bytes=86016` until the decode path moved to float32 - so
#     the headroom between the two is what is worth holding.
#   - Allocation churn, the same PF7 gap the ARM leg gates.
#
# NOT speed. QEMU is not a cycle-accurate emulator and reports a CPU clock that
# disagrees with its own boot log; the probe's us_per_frame lines are printed
# for shape and are not evidence of anything. The real-time answer needs
# hardware - see docs/platforms/esp32.md.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT="$REPO/apps/baremetal/platform/esp32s3"
cd "$PROJECT"

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "error: IDF_PATH is not set - source \$IDF_PATH/export.sh first" >&2
    exit 1
fi

# --- ceilings --------------------------------------------------------------
# Bytes. Measured values with headroom, not aspirations; a change that pushes
# past one should stop here and be explained rather than land silently.
#
# DIRAM is the interesting one. The ESP32-S3's internal SRAM is 341,760 bytes
# and the linked app currently uses 134,676 of it, leaving 207,084 against a
# 171,558-byte peak heap. The ceiling is on what the IMAGE uses, because that
# is what squeezes the heap: every byte of static data here is a byte the
# decode cannot allocate.
: "${AC3FORGE_ESP32S3_MAX_DIRAM_BYTES:=170000}"
: "${AC3FORGE_ESP32S3_MAX_HEAP_BYTES:=200000}"
: "${AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME:=100}"

OUTPUT="$(mktemp)"
trap 'rm -f "$OUTPUT"' EXIT

idf.py set-target esp32s3
idf.py build

echo
echo "== internal SRAM =="
idf.py size

# Parsed out of the table above rather than from a JSON mode: `idf.py size
# --format json` is not available on every IDF that can build this (it is not on
# v6.1), and a gate that silently stops running when the tool changes shape is
# worse than one that says so. The DIRAM row's first number is the bytes used;
# tr strips the box-drawing characters and the percentage's decimal point,
# leaving fields awk can take. If that row ever moves or vanishes this yields
# empty, and the note below fires instead of a bogus pass.
DIRAM=$(idf.py size 2>/dev/null | grep -m1 'DIRAM' | tr -cd '0-9 \n' | awk '{print $1}' || true)
DIRAM="${DIRAM:-0}"
if [[ "$DIRAM" == "0" ]]; then
    echo "note: could not find a DIRAM row in idf.py size's output;" \
         "the SRAM ceiling is not being enforced on this run" >&2
else
    echo "esp32s3.diram_bytes=$DIRAM" | tee -a "$OUTPUT"
    if (( DIRAM > AC3FORGE_ESP32S3_MAX_DIRAM_BYTES )); then
        echo "::error title=ESP32-S3 footprint regression::the image uses $DIRAM bytes of internal SRAM, ceiling is $AC3FORGE_ESP32S3_MAX_DIRAM_BYTES - every byte here is one the decode cannot allocate (see docs/platforms/esp32.md)" >&2
        exit 1
    fi
fi

echo
echo "== running ac3probe on qemu-system-xtensa (esp32s3) =="
# Unlike the arm-none-eabi leg, there is no semihosting exit: an ESP-IDF
# application returns from app_main into a FreeRTOS task that is then deleted,
# and the system goes on idling forever. So QEMU is killed on a timeout and the
# verdict is read from what it printed - which means a timeout here is the
# EXPECTED outcome, not a failure, and the gate below is on the captured output
# rather than on an exit code.
timeout 300 idf.py qemu 2>&1 | tee -a "$OUTPUT" || true

# A relative path is taken against the REPO ROOT rather than the project
# directory this script cd'd into, because that is what a caller writing one in
# a workflow file will mean. Absolute paths are used as given.
#
# The copy is not allowed to fail quietly. Getting this wrong once already cost
# an artifact that uploaded the linker map and silently dropped the summary -
# the one file that says what the probe measured - because the path the script
# wrote to and the path the upload step searched were not the same one.
if [[ -n "${AC3FORGE_ESP32S3_SUMMARY:-}" ]]; then
    case "$AC3FORGE_ESP32S3_SUMMARY" in
        /*) summary_dest="$AC3FORGE_ESP32S3_SUMMARY" ;;
        *) summary_dest="$REPO/$AC3FORGE_ESP32S3_SUMMARY" ;;
    esac
    cp "$OUTPUT" "$summary_dest"
fi

if ! grep -q '^result=pass' "$OUTPUT"; then
    echo "::error title=ESP32-S3 decoder probe failed::the probe did not report result=pass" >&2
    grep -E 'result=|reason=|Guru Meditation|assert failed' "$OUTPUT" >&2 || true
    exit 1
fi

heap=$(sed -n 's/.*heap\.peak_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$heap" ]]; then
    echo "error: the probe reported no heap.peak_bytes line" >&2
    exit 1
fi
if (( heap > AC3FORGE_ESP32S3_MAX_HEAP_BYTES )); then
    echo "::error title=ESP32-S3 footprint regression::peak heap is $heap bytes, ceiling is $AC3FORGE_ESP32S3_MAX_HEAP_BYTES" >&2
    exit 1
fi

for codec in ac3 eac3; do
    per_frame=$(sed -n "s/.*${codec}\.steady_allocs_per_frame=\([0-9]*\).*/\1/p" "$OUTPUT" | head -1)
    if [[ -z "$per_frame" ]]; then
        echo "error: the probe reported no ${codec}.steady_allocs_per_frame line" >&2
        exit 1
    fi
    if (( per_frame > AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME )); then
        echo "::error title=ESP32-S3 footprint regression::${codec} steady-state allocations are $per_frame per frame, ceiling is $AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME" >&2
        exit 1
    fi
done

echo "ESP32-S3 decoder probe: pass"
