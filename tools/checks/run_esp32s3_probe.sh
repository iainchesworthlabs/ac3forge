#!/usr/bin/env bash
# Build and run a minimum-footprint probe (roadmap PF7) for ESP32-S3, under
# QEMU, then gate on what it reports. The sibling of run_baremetal_probe.sh,
# which does the same for arm-none-eabi, and it takes the same --decoder /
# --encoder switch for the same reason.
#
#   . $IDF_PATH/export.sh
#   tools/checks/run_esp32s3_probe.sh                  # decode (the default)
#   tools/checks/run_esp32s3_probe.sh --encoder        # encode
#   tools/checks/run_esp32s3_probe.sh --stage-timers   # plus a per-stage breakdown
#
# WHAT THIS GATES, and what it deliberately does not:
#
#   - The probe's own verdict. Decoding: every fixture in
#     apps/baremetal/fixture.hpp decoded, every channel's level checked.
#     Encoding: six frames of synthesised 5.1 through each of the two encoders,
#     byte count and FNV-1a hash checked against apps/baremetal/encode_fixture.hpp.
#     Either way, result=pass - a failure means the codec is wrong on Xtensa.
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

# Which direction. The two profiles are mutually exclusive - measured on this
# part, no two of decode / AC-3 encode / E-AC-3 encode fit in internal SRAM at
# once - so this selects a build rather than adding a fixture to one.
DIRECTION=decoder
# --stage-timers builds the library with AC3FORGE_STAGE_TIMERS so the probe
# prints a per-stage breakdown of each fixture's decode time beside the
# per-frame figure. Under QEMU that breakdown has the same standing as the
# per-frame number - shape only, never evidence - but the build and the lines
# it prints are what a board run uses, and this leg is where they are proven
# to build and run at all. Passed to CMake explicitly in BOTH states: an
# option set on one run stays in the cache for the next, and a "plain" run
# that silently inherited the timers would print numbers nobody asked for.
STAGE_TIMERS=OFF
for arg in "$@"; do
    case "$arg" in
        --encoder) DIRECTION=encoder ;;
        --decoder) DIRECTION=decoder ;;
        --stage-timers) STAGE_TIMERS=ON ;;
        *) echo "usage: run_esp32s3_probe.sh [--encoder|--decoder] [--stage-timers]" >&2; exit 2 ;;
    esac
done

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
# and the linked app currently uses 134,804 of it. That leaves 206,956 by the
# linker's estimate, though the allocator reports 280,792 free at runtime,
# against a 236,391-byte peak heap. The ceiling is on what the IMAGE uses,
# because that is what squeezes the heap: every byte of static data here is a
# byte the decode cannot allocate.
#
# Not 179,064: that is what the peak was BEFORE the probe reconstructed Atmos
# objects, and docs/performance-trend.md quotes it as the start of the sequence
# that ends at today's number rather than as today's number.
: "${AC3FORGE_ESP32S3_MAX_DIRAM_BYTES:=170000}"
# 245,000, raised from 200,000 when the probe started reconstructing Atmos
# objects rather than only decoding their bed. oba::joc::reconstruct now runs on
# this target, which it could not before: 449,826 bytes of peak as found,
# 233,546 after Domain::kMdctBand, a float32 ReconstructionState, per-object
# scratches sized to the stream, and handing back the enhanced-coupling scratch
# between decodes. docs/platforms/esp32.md has what each was worth.
#
# 245,000 sits below the 280,792 bytes the allocator reports free, not at it: a
# ceiling at the hardware limit fails at the same moment the part does, which is
# too late to be a warning. This leaves 35,792 bytes in which CI goes red while
# a board would still be running, and 11,454 of margin over the measurement.
#
# The margin matters more here than the arithmetic suggests. This part's heap is
# REGIONED - 280,792 free but a largest block of 217,088 - so a total-free figure
# is not an allocation budget, and a peak that packs into a flat newlib heap on
# the arm-none-eabi leg can still fail here. It did: at 267,754, before the
# scratch was released, this leg died on a 6,144-byte request.
: "${AC3FORGE_ESP32S3_MAX_HEAP_BYTES:=245000}"
# One ceiling for all nine fixtures. Enhanced coupling had its own of 140
# until the 60 allocations per frame behind that exemption turned out to be two
# std::vector<double> in the reconstruction loop rather than anything §E3.5
# asks for; it now measures 12, level with plain eac3. See
# run_baremetal_probe.sh's own copy of this ceiling for the fixture-by-fixture
# numbers - this leg reports every one of them identically, which is the check
# that section is really there for.
: "${AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME:=100}"
# Bytes still live when the probe finishes, after every decoder it made has been
# destroyed. 12 - one __cxa_thread_atexit registration record, for the pointer
# to enhanced coupling's spectrum scratch, the one thread_local the library
# still declares. (24 while its per-bin angle buffer was a second one; that is
# a stack array now.)
#
# It was 34,232 until the probe started calling ac3::eac3::release_ecpl_scratch()
# between fixtures. That difference was enhanced coupling's 32,768-byte spectrum
# scratch (23,552 in its float form) and the 1,440-byte bin-angle vector, which
# were thread_local and so resident for the life of a task that never exits. Not a leak - bounded,
# paid once, and the point of caching them - but enough to decide whether
# something else fits: object reconstruction did not, on an ESP32-S3, whenever
# it ran after an enhanced-coupling decode.
#
# 1,024 against a measured 12 is deliberately tight. There is nothing here that
# grows a little; either the scratch is being handed back or it is not, and the
# difference is five figures. A ceiling with room for half of it would report
# nothing useful.
: "${AC3FORGE_ESP32S3_MAX_RETAINED_BYTES:=1024}"
# The decode runs on the main task, whose stack sdkconfig.defaults sets to
# 32,768 bytes after an overflow that surfaced as a LoadProhibited panic on the
# OTHER core - i.e. the failure mode here is not a clean error, it is corruption
# somewhere unrelated. Measured high-water leaves 11,280 free in the decode
# direction and 23,040 in the encode one, so the decode uses about 21,500 of the
# 32,768. This floor is what turns "we picked 32 KB and hoped" into a number
# that has to keep holding - and the decode margin is the one to watch: it was
# 14,000 before object reconstruction ran on this target.
: "${AC3FORGE_ESP32S3_MIN_STACK_FREE_BYTES:=8192}"

# --- and the encode direction's own, where they differ ---------------------
# Only the ones that move. Retained bytes and the stack floor mean the same
# thing in both directions and are left alone.
#
# PLAIN ASSIGNMENT, not `: "${VAR:=default}"`. The block above has already set
# every one of these, so a := here would be a no-op and the encode direction
# would silently run under the decode ceilings - which are the wrong ones in
# both directions, being lower on heap and higher on churn. Each still takes an
# override, through its own _ENCODE variable: one knob per direction, rather
# than one knob whose meaning depends on an argument.
#
# run_baremetal_probe.sh had exactly this bug and it is fixed in the same
# commit as this file: its encode heap ceiling of 250,000 never applied,
# because line 65 had already set the variable to 300,000.
if [[ "$DIRECTION" == "encoder" ]]; then
    # 110,900 measured, against the decode image's 134,676 - the encode half is
    # SMALLER in internal SRAM despite eac3_frame.cpp being the single biggest
    # source in the profile at 5,715 lines. Most of that file is flash-resident
    # code; what sits in DIRAM is the decode side's tables and its per-channel
    # state. 125,000 leaves the same ~11% the other ceilings here leave.
    AC3FORGE_ESP32S3_MAX_DIRAM_BYTES=${AC3FORGE_ESP32S3_MAX_DIRAM_BYTES_ENCODE:-125000}
    # 218,560 measured on this target - AC-3 alone reaches 162,602 and E-AC-3
    # takes it the rest of the way. Identical to the arm-none-eabi leg's figure
    # to the byte, which is what a deterministic input through the same
    # arithmetic should give.
    #
    # The same regioning caveat applies as above: this part's heap is not one
    # pool. Here it happens to be comfortable - 241,664 bytes in the largest
    # block against a 218,560 peak - but that is a fact about this profile, not
    # a property of the part, and the decode direction had to be reshaped
    # precisely because it was not true there.
    AC3FORGE_ESP32S3_MAX_HEAP_BYTES=${AC3FORGE_ESP32S3_MAX_HEAP_BYTES_ENCODE:-240000}
    # 260 rather than 100, for a reason that is in the API rather than in a
    # regression: both encoders return std::vector<std::byte> from
    # encode_frame, with no encode_frame_into to match the decoder's
    # decode_frame_into, so an allocation per frame is unavoidable from
    # outside. E-AC-3 measures 249 per frame and AC-3 78. Holding this to the
    # decoder's number would gate a difference nothing in this profile can
    # currently close. run_baremetal_probe.sh carries the same ceiling.
    #
    AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME=${AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME_ENCODE:-260}
fi

OUTPUT="$(mktemp)"
trap 'rm -f "$OUTPUT"' EXIT

# fullclean between directions, not for tidiness: AC3FORGE_ESP_PROFILE reaches
# the library as CMake cache variables (AC3FORGE_MINIMAL_DECODER /
# AC3FORGE_MINIMAL_ENCODER, FORCEd by esp-idf/ac3forge/CMakeLists.txt), and a
# warm build directory has already resolved them. Reconfiguring over the top
# silently keeps the previous direction's archive - which links, runs, and
# reports the wrong profile's numbers under this one's ceilings.
#
# Only when the direction has actually changed: a rebuild of the same direction
# is the common case in CI and on a laptop, and a fullclean every time would
# cost several minutes to prove nothing.
STAMP="build/.ac3forge-direction"
if [[ -d build && "$(cat "$STAMP" 2>/dev/null || echo)" != "$DIRECTION" ]]; then
    echo "note: build directory holds a different profile - cleaning" >&2
    idf.py fullclean
fi

idf.py set-target esp32s3
idf.py -DAC3FORGE_ESP_PROFILE="$DIRECTION" -DAC3FORGE_STAGE_TIMERS="$STAGE_TIMERS" build
mkdir -p build && printf '%s' "$DIRECTION" > "$STAMP"

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
        echo "::error title=ESP32-S3 footprint regression::the image uses $DIRAM bytes of internal SRAM, ceiling is $AC3FORGE_ESP32S3_MAX_DIRAM_BYTES - every byte here is one the ${DIRECTION} cannot allocate (see docs/platforms/esp32.md)" >&2
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
    echo "::error title=ESP32-S3 ${DIRECTION} probe failed::the probe did not report result=pass" >&2
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
    ceiling=$AC3FORGE_ESP32S3_MAX_STEADY_ALLOCS_PER_FRAME
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
    echo "::error title=ESP32-S3 stack headroom::the ${DIRECTION} left only $stack_free bytes of main-task stack, floor is $AC3FORGE_ESP32S3_MIN_STACK_FREE_BYTES - raise CONFIG_ESP_MAIN_TASK_STACK_SIZE rather than lowering this" >&2
    exit 1
fi

echo "ESP32-S3 ${DIRECTION} probe: pass"
