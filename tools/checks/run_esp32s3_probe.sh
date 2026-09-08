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
#   - The probe's own verdict. Every fixture in apps/baremetal/fixture.hpp
#     decoded, every channel's level checked, result=pass. A failure here
#     means the decode is wrong on Xtensa.
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
# and the linked app currently uses 134,676 of it. That leaves 207,084 by the
# linker's estimate, though the allocator reports 280,792 free at runtime,
# against a 179,064-byte peak heap. The ceiling is on what the IMAGE uses, because that
# is what squeezes the heap: every byte of static data here is a byte the
# decode cannot allocate.
: "${AC3FORGE_ESP32S3_MAX_DIRAM_BYTES:=170000}"
: "${AC3FORGE_ESP32S3_MAX_HEAP_BYTES:=200000}"
: "${AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME:=100}"
# Enhanced coupling costs more per frame than the other fixtures for reasons
# that are in §E3.5 rather than in a regression - see run_baremetal_probe.sh's
# own copy of this ceiling for the detail.
: "${AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME_ECPL:=140}"
# Bytes still live when the probe finishes, after every decoder it made has been
# destroyed: the library's process-lifetime scratch, which nothing releases
# while the task that decoded is still running. Measured 34,232 here, the same
# number the arm-none-eabi leg reports - the allocations are the two thread_local
# scratch buffers in eac3_tools.cpp, so neither target's toolchain changes them.
# It matters more here than there: these are bytes of the 341,760 internal SRAM
# that the decode holds for as long as the task lives.
: "${AC3FORGE_ESP32S3_MAX_RETAINED_BYTES:=40000}"
# The decode runs on the main task, whose stack sdkconfig.defaults sets to
# 32,768 bytes after an overflow that surfaced as a LoadProhibited panic on the
# OTHER core - i.e. the failure mode here is not a clean error, it is corruption
# somewhere unrelated. Measured high-water leaves 14,000 free, so the decode uses
# about 18,800. This floor is what turns "we picked 32 KB and hoped" into a
# number that has to keep holding.
: "${AC3FORGE_ESP32S3_MIN_STACK_FREE_BYTES:=8192}"

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

retained=$(sed -n 's/.*heap\.retained_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$retained" ]]; then
    echo "error: the probe reported no heap.retained_bytes line" >&2
    exit 1
fi
echo "retained after teardown: $retained bytes (ceiling $AC3FORGE_ESP32S3_MAX_RETAINED_BYTES)"
if (( retained > AC3FORGE_ESP32S3_MAX_RETAINED_BYTES )); then
    echo "::error title=ESP32-S3 footprint regression::$retained bytes are still live after every decoder was destroyed, ceiling is $AC3FORGE_ESP32S3_MAX_RETAINED_BYTES - see the heap.retained_bucket lines for which buffer" >&2
    exit 1
fi

# Every fixture's steady-state churn, held to one ceiling: they are the same
# requirement and a regression in any of them is the same kind of news.
#
# The fixture names come from the probe's own output rather than from a list
# kept here, so adding one (apps/baremetal/probe.cpp's kEac3Fixtures and
# tools/generators/gen_baremetal_fixture.py's STREAMS) does not also mean
# remembering to widen a gate in two runner scripts. A hardcoded list still
# PASSES when a fixture is added and left off it, and the fixture nobody
# remembered is exactly the one whose churn nobody has seen.
# grep -o rather than a sed capture: the probe puts several key=value pairs on
# one line, and a leading `.*` in a substitution is greedy enough to swallow the
# fixture name and leave the capture empty.
CHURN=$(grep -o '[a-z0-9_]*\.steady_allocs_per_frame=[0-9]*' "$OUTPUT" | sed 's/\.steady_allocs_per_frame=/ /')
if [[ -z "$CHURN" ]]; then
    echo "error: the probe reported no <fixture>.steady_allocs_per_frame line" >&2
    exit 1
fi
while read -r codec per_frame; do
    case "$codec" in
        *ecpl*) ceiling=$AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME_ECPL ;;
        *) ceiling=$AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME ;;
    esac
    echo "churn: ${codec} = ${per_frame} allocations/frame (ceiling ${ceiling})"
    if (( per_frame > ceiling )); then
        echo "::error title=ESP32-S3 footprint regression::${codec} steady-state allocations are $per_frame per frame, ceiling is $ceiling" >&2
        exit 1
    fi
done <<< "$CHURN"

# --- what the ALLOCATOR has, as opposed to what the linker estimated -------
# `idf.py size` prints a DIRAM "remain" figure and it is a static estimate: it
# was 207,084 against the 280,792 the allocator actually reports, 73,708 bytes
# pessimistic. Quote the runtime numbers, not that one.
#
# The largest contiguous block is the one that decides whether a big allocation
# SUCCEEDS, and it is not a refinement of the total. Measured here it falls from
# 217,088 before the decode to 116,736 after, while the total only falls 35,544 -
# so a single 147,504-byte oba::joc::ReconstructionState would already be
# unallocatable after any other decode, on contiguity alone and whatever the
# budget says. The probe cannot see this: its own hooks count bytes, not runs.
for line in internal_free_bytes internal_largest_block_bytes internal_word_only_bytes; do
    grep -o "esp32s3.${line}\[[a-z]*\]=[0-9]*" "$OUTPUT" | sed "s/^/  /" || true
done

stack_free=$(sed -n 's/.*esp32s3\.main_task_stack_free_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$stack_free" ]]; then
    echo "error: the probe reported no esp32s3.main_task_stack_free_bytes line" >&2
    exit 1
fi
echo "main task stack free at high-water: $stack_free bytes (floor $AC3FORGE_ESP32S3_MIN_STACK_FREE_BYTES)"
if (( stack_free < AC3FORGE_ESP32S3_MIN_STACK_FREE_BYTES )); then
    echo "::error title=ESP32-S3 stack headroom::the decode left only $stack_free bytes of main-task stack, floor is $AC3FORGE_ESP32S3_MIN_STACK_FREE_BYTES - raise CONFIG_ESP_MAIN_TASK_STACK_SIZE rather than lowering this" >&2
    exit 1
fi

echo "ESP32-S3 decoder probe: pass"
