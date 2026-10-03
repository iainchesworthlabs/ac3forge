#!/usr/bin/env bash
# Build and run the minimum-footprint decoder probe (minimum-footprint decoder profile), then gate on
# what it reports.
#
#   tools/checks/run_baremetal_probe.sh                  # arm-none-eabi under QEMU
#   tools/checks/run_baremetal_probe.sh --host           # natively, no emulator
#   tools/checks/run_baremetal_probe.sh --icount         # QEMU, clock = instruction count, gated
#   tools/checks/run_baremetal_probe.sh --encoder --icount   # the same for the encode probe
#   tools/checks/run_baremetal_probe.sh --ac4 --icount       # the AC-4 decoder's probe, in float
#
# Two things are checked, and they fail for different reasons:
#
#   1. The probe's own verdict. It decodes every fixture, compares every
#      channel's level against apps/baremetal/fixture.hpp, and prints
#      result=pass or result=fail (see apps/baremetal/probe.cpp). A failure
#      here means the decode is wrong on this target.
#
#   2. The footprint ceilings below. These are not aspirations - they are the
#      measured numbers with headroom, and a change that pushes past one is
#      meant to stop here and be explained in docs/performance-trend.md's
#      footprint table rather than land silently. Raise them WITH the table.
#
# Set ICLFORGE_FOOTPRINT_SUMMARY to a path to also write the probe's key=value
# output there, which is what the CI leg feeds to
# tools/checks/footprint_report.py.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

HOST=0
# Which direction. The two profiles are mutually exclusive (no two of decode /
# AC-3 encode / E-AC-3 encode fit in an ESP32-S3's internal SRAM at once), so
# this picks a preset rather than adding a fixture.
DIRECTION=decoder
# --ac4: the third profile, the AC-4 decoder (planning/ac4.md, D14a) in float, with its own
# probe (apps/baremetal/ac4_probe.cpp) and its own presets, since AC-4 shares nothing with
# iclforge::ac3 and an image carries one probe. --ac4 --scalar=fixed is the same probe on the
# decoder's fixed-point tier (D14d), with ceilings of its own.
# --stage-timers: build the library with ICLFORGE_STAGE_TIMERS, so the probe
# prints where each fixture's decode time goes stage by stage. On this leg
# that is shape only - QEMU's clock describes the host, and the host shape's
# describes a desktop - but it is the same build a board run uses, and this
# is where it is proven to build and run. Passed to CMake in both states, so
# a cached ON from an earlier run cannot leak into a plain one. With --ac4 the
# probe also names the stage open at each fixture's peak heap
# (<fixture>.peak_stage), which a build without the timers prints as "-".
STAGE_TIMERS=OFF
# --icount: the one timing figure on this leg that means anything. The probe
# is built with its clock on the mps2-an385's 25 MHz CMSDK timer
# (ICLFORGE_BAREMETAL_CLOCK=timer, apps/baremetal/platform/baremetal/
# clock_timer.cpp) and QEMU runs with -icount shift=0, under which the guest
# clock advances one nanosecond per executed instruction. Every microsecond
# the probe prints is then a thousand Thumb-2 instructions, deterministic on
# any host, and the per-fixture ceilings below gate it. With --stage-timers
# the per-stage lines count instructions per stage the same way. Either
# direction (--encoder --icount reads the encode probe's own us_per_frame
# lines against a second table), and QEMU only: it has no meaning natively.
ICOUNT=0
for arg in "$@"; do
    case "$arg" in
        --host) HOST=1 ;;
        --encoder) DIRECTION=encoder ;;
        --decoder) DIRECTION=decoder ;;
        --ac4) DIRECTION=ac4 ;;
        --stage-timers) STAGE_TIMERS=ON ;;
        --icount) ICOUNT=1 ;;
        --scalar=*) SCALAR="${arg#--scalar=}" ;;
        *) echo "usage: run_baremetal_probe.sh [--host] [--encoder|--decoder|--ac4] [--stage-timers] [--icount] [--scalar=float|fixed]" >&2; exit 2 ;;
    esac
done
if [[ "$ICOUNT" == "1" && "$HOST" == "1" ]]; then
    echo "error: --icount is a QEMU mode; it cannot be combined with --host" >&2
    exit 2
fi
# --- instruction ceilings (--icount) ---------------------------------------
# Thumb-2 instructions per frame on the soft-float Cortex-M3 leg, measured
# 2026-09-10 at the values docs/performance-trend.md's "Instructions per
# frame" table records, with the same headroom rule as every ceiling above:
# a change that pushes past one stops here and is explained in that table.
# Counts are deterministic, so a move of one per cent is visible in the run's
# own lines long before a ceiling is; the ceilings catch the silent ones.
# Override one for a run with ICLFORGE_MAX_INSTRUCTIONS_PER_FRAME_<fixture>.
declare -A ICOUNT_CEILING=(
    [ac3_mono]=2000000
    [ac3_stereo]=4500000
    [eac3_stereo]=6000000
    [eac3_atmos_bed]=11000000
    [ac3]=13000000
    [eac3]=16000000
    [eac3_atmos_objects]=35000000
    [eac3_ecpl]=36000000
    [eac3_714]=42000000
    [ac3_fold]=13500000
    [eac3_fold]=17000000
    [eac3_714_fold]=45000000
    [eac3_line]=17000000
    [eac3_atmos_render]=36000000
)
# The fixed-point tier's (--scalar=fixed --icount), measured 2026-09-10 on the
# same leg with the same headroom rule (planning/arithmetic-tiers.md). Integer
# arithmetic where the float tier's is software floating point, so most rows
# are a third to a half of the float table's. The two object rows are the
# exception, and not because of the tier: JOC's reconstruction runs in float
# in every build of this library (oba/joc.hpp's recon_scalar_t), so those rows
# are a float transform sandwich either way.
declare -A ICOUNT_CEILING_FIXED=(
    [ac3_mono]=1000000
    [ac3_stereo]=2000000
    [eac3_stereo]=2500000
    [eac3_atmos_bed]=4500000
    [ac3]=5000000
    [ac3_fold]=7000000
    [eac3]=6500000
    [eac3_fold]=10500000
    [eac3_714_fold]=21500000
    [eac3_line]=7000000
    [eac3_atmos_objects]=31500000
    [eac3_atmos_render]=32000000
    [eac3_ecpl]=13000000
    [eac3_714]=15500000
)
# The encode direction's, from --encoder --icount: Thumb-2 instructions per
# ENCODED frame, measured 2026-09-10 on the same leg with the same headroom
# rule. Both encoders are double throughout - the arithmetic is the
# bitstream's own, and the E-AC-3 encoder's tools are the decoder's double
# instantiations - so on a part with no FPU every operation is a software
# call, which is the whole of the gap between these and the decode rows.
declare -A ICOUNT_CEILING_ENCODE=(
    [ac3_stereo]=16000000
    [eac3_stereo]=30000000
    [eac3_tools]=31000000
    [ac3]=43000000
    [eac3]=78000000
    [eac3_ecpl]=104000000
)
# The AC-4 decoder's rows (--ac4), in float on the same soft-float leg, measured 2026-09-30 with
# GCC 14.2.1 under QEMU 10.2.1 (planning/ac4.md, D14a; docs/performance-trend.md's AC-4 table).
# Each is the measured figure and about a tenth over it: the counts, the allocation counts and the
# peaks are deterministic for these fixed streams, so a tenth only has to absorb a deliberate
# change, and a change past one stops here to be explained in that table.
#
# Thumb-2 instructions per frame (--ac4 --icount), the generic seam's portable vector types
# and all. Measured again 2026-10-03 with the decoder's memory work in (planning/ac4.md): 25.2 M,
# 35.1 M, 87.2 M, 100.7 M, 161.8 M and 29.3 M, from 54.8 M, 57.3 M, 116.7 M, 122.9 M, 206.3 M and
# 58.8 M. A figure is the average over a fixture's three or four frames, the first included, and
# the first no longer builds the inverse transform's tables in software floating point: they
# are in flash (dsp/transform_tables.hpp).
declare -A ICOUNT_CEILING_AC4=(
    [ac4_20_music]=28000000
    [ac4_20_acpl]=39000000
    [ac4_51_music]=96000000
    [ac4_51_acpl]=111000000
    [ac4_514_tones]=178000000
    [ac4_20_companding]=32500000
)
# The fixed-point tier's (--ac4 --scalar=fixed --icount), on the same leg with the same rule:
# 6,700,000, 10,529,000, 24,041,000, 28,787,000, 42,962,000 and 8,308,000 in the order below
# (2026-10-03; 38.2 M, 34.2 M, 55.6 M, 52.5 M, 90.4 M and 39.9 M at D14d, before the tables
# went to flash). Integer arithmetic where the float tier's is software floating point.
declare -A ICOUNT_CEILING_AC4_FIXED=(
    [ac4_20_music]=7500000
    [ac4_20_acpl]=11600000
    [ac4_51_music]=26500000
    [ac4_51_acpl]=31700000
    [ac4_514_tones]=47500000
    [ac4_20_companding]=9200000
)
# Steady-state allocations per frame. The decoder's syntax layer still builds its element
# vectors afresh each frame (planning/ac4.md, D14a's memory audit); these hold the distance
# from the frame's own zero from growing while that is open.
declare -A CHURN_CEILING_AC4=(
    [ac4_20_music]=58
    [ac4_20_acpl]=56
    [ac4_51_music]=168
    [ac4_51_acpl]=99
    [ac4_514_tones]=210
    [ac4_20_companding]=82
)
# Each fixture's peak heap in bytes, on either leg and at either tier: the host's 64-bit
# pointers put it a few per cent above the Cortex-M3's, and one figure covers both. Measured
# 2026-10-03 at the fixed tier (the larger) on the host: 295,225, 435,486, 722,007, 886,336,
# 1,526,819 and 337,507; on the Cortex-M3 286,365, 426,918, 704,311, 868,424, 1,502,903 and
# 329,147 (429,667, 626,368, 970,430, 1,172,502, 1,825,056 and 486,331 at D14d).
declare -A PEAK_CEILING_AC4=(
    [ac4_20_music]=325000
    [ac4_20_acpl]=480000
    [ac4_51_music]=795000
    [ac4_51_acpl]=975000
    [ac4_514_tones]=1680000
    [ac4_20_companding]=372000
)

# --- ceilings --------------------------------------------------------------
# Bytes. text+data+bss of the linked probe on the bare-metal target, and the
# probe's own peak heap on either. See docs/performance-trend.md's footprint
# table for the measured values these leave headroom over.
#
# ICLFORGE_MAX_IMAGE_BYTES was re-based from 400,000 to 465,000 after the profile's own
# feature branch (PR #351) picked up several mid-flight merges
# from `develop` - most significantly DC10's QMF-domain JOC reconstruction,
# which the decode path now needs (src/dsp/src/qmf.cpp and
# src/ac3/src/verify/eac3_mirror.cpp, both correctly added to
# src/ac3/minimal.cmake's source list) - between when 354,060/400,000 were
# first measured and when the PR actually merged. The image had already
# reached 412,516 bytes at that point; nobody re-measured before merging.
# See docs/performance-trend.md's footprint table for the current breakdown.
: "${ICLFORGE_MAX_IMAGE_BYTES:=465000}"
# Measured on main at be71f454, 2026-09-09, arm-none-eabi GCC 14.2.1 under QEMU
# 10.2.1: image 320,940 of 465,000 (31% headroom) and peak heap 236,391 of
# 300,000 (21%). Both fell after the float32 decode path and the thread_local
# move; the heap figure had earlier moved up from 243,470
# - and its headroom from 23% to 11% - when the pimpl sweep put both decoders'
# state on the heap instead of in the caller's frame: a relocation out of
# automatic storage rather than new consumption. The ceiling is deliberately
# left where it is. This peak is deterministic for these fixed fixtures, so the
# margin only ever has to absorb a deliberate change, never run-to-run noise.
# See docs/performance-trend.md's footprint table.
: "${ICLFORGE_MAX_HEAP_BYTES:=300000}"
# Allocations per frame in the steady state, whichever fixture is worst. The
# requirement the profile states is ZERO and this is not it - see docs/building.md's
# gap note. The ceiling exists so the distance from zero cannot quietly grow
# while that gap is open. Measured on main at be71f454, 2026-09-09, all eight
# fixtures, identical on this leg and on tools/checks/run_esp32s3_probe.sh's:
#
#   1 ac3_mono   1 ac3_stereo   3 ac3   10 eac3_stereo
#   12 eac3   12 eac3_ecpl   23 eac3_atmos_bed   41 eac3_atmos_objects
# (20 and 31 for the Atmos pair since the object description is moved, and 35 for
# the 7.1.4 fixture added 2026-09-10, whose three substreams each allocate.)
#
# A ceiling of 100 over a worst fixture of 41 is loose, deliberately: what it
# guards is the DISTANCE FROM ZERO not growing, and the fixtures that could
# grow it - the Atmos ones, whose remaining churn is the EMDF payload chain
# rather than anything per-block - are the ones furthest from the ceiling. Cut
# it to the measurement and every Atmos fixture change becomes a ceiling edit.
#
# ONE ceiling, where there used to be a second one of 140 for enhanced coupling
# alone. That exemption was real while it lasted: §E3.5 reconstructs each
# coupled channel through three 512-point inverse transforms and a DFT per
# block, carries a 22-sub-band geometry against standard coupling's 18, and
# measured 126 per frame - so holding it to 100 would have meant either not
# covering §E3.5 or lifting the ceiling for every other fixture to a number
# none of them was near.
#
# It is gone because the 126 was never the tool's geometry. Sixty of it were
# two std::vector<double> constructed per coupled channel per block in
# eac3_decoder.cpp's reconstruction loop; the rest went the same way when both
# decoders' frame-scope buffers moved onto the decoder. eac3_ecpl now measures
# 12, level with plain eac3 - which is the answer to the question the exemption
# was really asking, and it is no.
: "${ICLFORGE_MAX_STEADY_ALLOCS_PER_FRAME:=100}"
# Bytes still live when the probe finishes, after every decoder it made has been
# destroyed. 12 - one __cxa_thread_atexit registration record, for the pointer
# to enhanced coupling's spectrum scratch, the one thread_local the library
# still declares. (24 while its per-bin angle buffer was a second one; that is
# a stack array now.)
#
# It was 34,232 until the probe started calling iclforge::ac3::eac3::release_ecpl_scratch()
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
: "${ICLFORGE_MAX_RETAINED_BYTES:=1024}"

if [[ "$HOST" == "1" ]]; then
    PRESET=config-linux-gcc-minimal
    BUILD_PRESET=build-linux-gcc-minimal
else
    PRESET=config-arm-none-eabi-minimal
    BUILD_PRESET=build-arm-none-eabi-minimal
fi
if [[ "${SCALAR:-}" == "fixed" && "$DIRECTION" == "decoder" ]]; then
    for key in "${!ICOUNT_CEILING_FIXED[@]}"; do
        ICOUNT_CEILING[$key]=${ICOUNT_CEILING_FIXED[$key]}
    done
fi
if [[ "$DIRECTION" == "encoder" ]]; then
    PRESET="${PRESET}-encoder"
    BUILD_PRESET="${BUILD_PRESET}-encoder"
    # The encode probe reports no per-fixture churn lines and no image ceiling
    # of its own yet - it has no linked-in fixture, so its image is a different
    # kind of number. What it does report, and what is gated below, is the peak
    # and the retained bytes.
    #
    # PLAIN ASSIGNMENT, not `: "${VAR:=250000}"`. This read as the latter until
    # tools/checks/run_esp32s3_probe.sh grew the same block and the pattern was
    # looked at twice: the ceilings section above has ALREADY set this variable
    # to 300,000, so a := here did nothing at all and every encode run has been
    # gating against the decode ceiling. Nothing failed as a result - the
    # measured peak is 218,560 - but the number in this file was not the number
    # being enforced, which is the part worth not repeating.
    ICLFORGE_MAX_HEAP_BYTES=${ICLFORGE_MAX_HEAP_BYTES_ENCODE:-250000}
fi
if [[ "$DIRECTION" == "ac4" ]]; then
    PRESET="${PRESET}-ac4"
    BUILD_PRESET="${BUILD_PRESET}-ac4"
    # Plain assignments over the E-AC-3 defaults above, for the reason the encode block's
    # comment gives. Measured 2026-10-02 (D14e): image 691,896 bytes (689,244 .text, 392 .data,
    # 2,260 .bss), 8,448 more than at D14a5 (683,448, which was 198,416 more than the tree before
    # it, 196,464 of them the sample rate converter's float tables, which the compiler builds
    # into the image; planning/ac4.md, D14a5); peak heap 1,800,312 (the 5.1.4 fixture); 0
    # retained; the stack a decode used, read by painting, 19,480 bytes on the Cortex-M3 and
    # 23,920 on the x86-64 host, whose frames are larger. Every ceiling a tenth or so over its
    # figure.
    #
    # Measured again 2026-10-03 with the decoder's memory work in: 750,276 bytes, 58,380 more,
    # the inverse transform's float tables for the five block lengths of a 2048-sample frame in
    # flash where they were built on the heap (dsp/transform_tables.hpp), and 2,400 bytes of
    # .bss the probe's own stage-timer tables hold now that it names the stage at a peak.
    ICLFORGE_MAX_IMAGE_BYTES=${ICLFORGE_MAX_IMAGE_BYTES_AC4:-825000}
    # The fixed-point tier's image: 727,656 bytes at D14d (725,004 .text), 801,812 on
    # 2026-10-03 with the transform tables (Q7.24, and the post-twiddles the tier has besides)
    # in flash. Its converter tables are Q1.30 integers built by the compiler, the 1001/960 one
    # 188,376 bytes and read in place, and the float tables are not linked. Peaks, churn, stack
    # and retained bytes share the float tier's ceilings.
    if [[ "${SCALAR:-}" == "fixed" ]]; then
        ICLFORGE_MAX_IMAGE_BYTES=${ICLFORGE_MAX_IMAGE_BYTES_AC4:-880000}
        for key in "${!ICOUNT_CEILING_AC4_FIXED[@]}"; do
            ICOUNT_CEILING_AC4[$key]=${ICOUNT_CEILING_AC4_FIXED[$key]}
        done
    fi
    ICLFORGE_MAX_HEAP_BYTES=${ICLFORGE_MAX_HEAP_BYTES_AC4:-1680000}
    ICLFORGE_MAX_STEADY_ALLOCS_PER_FRAME=${ICLFORGE_MAX_STEADY_ALLOCS_PER_FRAME_AC4:-210}
    ICLFORGE_MAX_RETAINED_BYTES=${ICLFORGE_MAX_RETAINED_BYTES_AC4:-1024}
    if [[ "$HOST" == "1" ]]; then
        ICLFORGE_MAX_STACK_BYTES=${ICLFORGE_MAX_STACK_BYTES_AC4:-28500}
    else
        ICLFORGE_MAX_STACK_BYTES=${ICLFORGE_MAX_STACK_BYTES_AC4:-21500}
    fi
fi
# Its own preset and build directory, so a plain run and an --icount run
# never share a CMake cache (the clock is a cache variable).
QEMU_ICOUNT=()
if [[ "$ICOUNT" == "1" ]]; then
    PRESET="${PRESET}-icount"
    BUILD_PRESET="${BUILD_PRESET}-icount"
    # Quoted: the comma is QEMU's own option syntax, one argument, not an
    # array separator (shellcheck SC2054 cannot tell the two apart).
    QEMU_ICOUNT=(-icount "shift=0,sleep=off")
fi

# --scalar=<float|fixed>: the decode arithmetic (planning/arithmetic-tiers.md).
# The profile's default is float; `fixed` is the tier for a part with no FPU,
# and its probe is the same probe in its own build directory, so the two never
# share a cache. Its PCM hashes (<codec>.pcm_hash) are identical on every leg
# by construction - integer arithmetic - which is what a fixed run is for.
BUILD_DIR="build/$PRESET"
if [[ -n "${SCALAR:-}" ]]; then
    BUILD_DIR="build/$PRESET-$SCALAR"
    cmake --preset "$PRESET" -B "$BUILD_DIR" -DICLFORGE_STAGE_TIMERS="$STAGE_TIMERS" -DICLFORGE_DECODE_SCALAR="$SCALAR"
    cmake --build "$BUILD_DIR" --parallel
else
    cmake --preset "$PRESET" -DICLFORGE_STAGE_TIMERS="$STAGE_TIMERS"
    cmake --build --preset "$BUILD_PRESET"
fi

BIN="$BUILD_DIR/bin/iclforge-probe"
if [[ ! -f "$BIN" ]]; then
    echo "error: $BIN was not produced" >&2
    exit 1
fi

OUTPUT=$(mktemp)
trap 'rm -f "$OUTPUT"' EXIT

if [[ "$HOST" == "1" ]]; then
    echo "== running iclforge-probe natively =="
    "$BIN" | tee "$OUTPUT"
else
    echo "== running iclforge-probe on qemu-system-arm (mps2-an385, cortex-m3) =="
    # -semihosting is what gives the probe stdout and an exit code at all; the
    # newlib rdimon specs the toolchain file links against are its other half.
    # A timeout because a probe that faults early would otherwise hang the leg
    # rather than fail it; longer under -icount, which runs the guest at a
    # deterministic pace rather than as fast as the host allows.
    if [[ "$ICOUNT" == "1" ]]; then
        echo "   (-icount shift=0: every microsecond the probe prints is a thousand instructions)"
    fi
    timeout $(( ICOUNT == 1 ? 900 : 300 )) qemu-system-arm \
        -M mps2-an385 -cpu cortex-m3 \
        -monitor none -nographic -semihosting "${QEMU_ICOUNT[@]}" \
        -kernel "$BIN" | tee "$OUTPUT"

    echo
    echo "== image size =="
    arm-none-eabi-size "$BIN"
    IMAGE=$(arm-none-eabi-size "$BIN" | awk 'NR==2 {print $4}')
    echo "image.total_bytes=$IMAGE" | tee -a "$OUTPUT"
    if (( IMAGE > ICLFORGE_MAX_IMAGE_BYTES )); then
        echo "::error title=Footprint regression::linked image is $IMAGE bytes, ceiling is $ICLFORGE_MAX_IMAGE_BYTES (see docs/performance-trend.md's footprint table)" >&2
        exit 1
    fi
fi

if ! grep -q '^result=pass$' "$OUTPUT"; then
    echo "::error title=Minimum-footprint ${DIRECTION} probe failed::the probe did not report result=pass" >&2
    exit 1
fi

heap=$(sed -n 's/.*heap\.peak_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$heap" ]]; then
    echo "error: the probe reported no heap.peak_bytes line" >&2
    exit 1
fi
if (( heap > ICLFORGE_MAX_HEAP_BYTES )); then
    echo "::error title=Footprint regression::peak heap is $heap bytes, ceiling is $ICLFORGE_MAX_HEAP_BYTES (see docs/performance-trend.md's footprint table)" >&2
    exit 1
fi

retained=$(sed -n 's/.*heap\.retained_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$retained" ]]; then
    echo "error: the probe reported no heap.retained_bytes line" >&2
    exit 1
fi
echo "retained after teardown: $retained bytes (ceiling $ICLFORGE_MAX_RETAINED_BYTES)"
if (( retained > ICLFORGE_MAX_RETAINED_BYTES )); then
    echo "::error title=Footprint regression::$retained bytes are still live after every decoder was destroyed, ceiling is $ICLFORGE_MAX_RETAINED_BYTES - see the heap.retained_bucket lines for which buffer" >&2
    exit 1
fi

# The AC-4 probe's own rows: the stack a decode used (read by painting, see
# apps/baremetal/ac4_probe.cpp) and each fixture's peak heap against its own ceiling.
if [[ "$DIRECTION" == "ac4" ]]; then
    stack=$(sed -n 's/.*stack\.peak_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
    if [[ -z "$stack" ]]; then
        echo "error: the probe reported no stack.peak_bytes line" >&2
        exit 1
    fi
    echo "stack: ${stack} bytes (ceiling ${ICLFORGE_MAX_STACK_BYTES})"
    if (( stack > ICLFORGE_MAX_STACK_BYTES )); then
        echo "::error title=Footprint regression::a decode used $stack bytes of stack, ceiling is $ICLFORGE_MAX_STACK_BYTES" >&2
        exit 1
    fi
    # The fixtures' own lines: heap.peak_bytes and stack.peak_bytes end the same way.
    PEAKS=$(grep -o 'ac4_[a-z0-9_]*\.peak_bytes=[0-9]*' "$OUTPUT" | sed 's/\.peak_bytes=/ /')
    while read -r codec peak; do
        ceiling=${PEAK_CEILING_AC4[$codec]:-}
        if [[ -z "$ceiling" ]]; then
            echo "::error title=No peak ceiling::${codec} has no entry in run_baremetal_probe.sh's PEAK_CEILING_AC4 table - add one from a measured run" >&2
            exit 1
        fi
        echo "peak heap: ${codec} = ${peak} bytes (ceiling ${ceiling})"
        if (( peak > ceiling )); then
            echo "::error title=Footprint regression::${codec} peaks at $peak bytes of heap, ceiling is $ceiling" >&2
            exit 1
        fi
    done <<< "$PEAKS"
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
if [[ "$DIRECTION" == "encoder" ]]; then
    # 260 rather than 100. E-AC-3 encode measures 249 allocations per frame and
    # AC-3 78, against the decoders' 1-31 - and the reason is in the API, not
    # the implementation: both encoders return std::vector<std::byte> from
    # encode_frame, with no encode_frame_into to match decode_frame_into. That
    # is the profile's zero-heap gap seen from the encode side, and it is wider here.
    # Holding this to the decoder's number would gate a difference nothing in
    # this profile can currently close.
    ICLFORGE_MAX_STEADY_ALLOCS_PER_FRAME=${ICLFORGE_MAX_STEADY_ALLOCS_PER_FRAME_ENCODE:-260}
fi
if [[ -z "$CHURN" ]]; then
    echo "error: the probe reported no <fixture>.steady_allocs_per_frame line" >&2
    exit 1
fi
while read -r codec per_frame; do
    ceiling=$ICLFORGE_MAX_STEADY_ALLOCS_PER_FRAME
    # The AC-4 rows each have their own, from the table above.
    if [[ "$DIRECTION" == "ac4" ]]; then
        ceiling=${CHURN_CEILING_AC4[$codec]:-$ceiling}
    fi
    echo "churn: ${codec} = ${per_frame} allocations/frame (ceiling ${ceiling})"
    if (( per_frame > ceiling )); then
        echo "::error title=Footprint regression::${codec} steady-state allocations are $per_frame per frame, ceiling is $ceiling" >&2
        exit 1
    fi
done <<< "$CHURN"

# --icount: every fixture's instructions per frame, from the probe's own
# us_per_frame under the instruction-counting clock, held to the per-fixture
# ceilings above. The fixture names come from the probe's output, as for the
# churn gate; a fixture with no ceiling in the table is an error rather than
# a pass, for the same reason a forgotten fixture must not pass the churn gate.
if [[ "$ICOUNT" == "1" ]]; then
    # The stage lines are left out: `<codec>.stage[<zone>].us_per_frame=` ends
    # in the same key, and the `]` before it would leave the pattern below an
    # empty fixture name and the zone's figure in its place.
    INSTR=$(grep -v '\.stage\[' "$OUTPUT" | grep -o '[a-z0-9_]*\.us_per_frame=[0-9]*' |
        sed 's/\.us_per_frame=/ /')
    if [[ -z "$INSTR" ]]; then
        echo "error: the probe reported no <fixture>.us_per_frame line" >&2
        exit 1
    fi
    while read -r codec us; do
        instr=$(( us * 1000 ))
        override="ICLFORGE_MAX_INSTRUCTIONS_PER_FRAME_${codec}"
        if [[ "$DIRECTION" == "encoder" ]]; then
            table_ceiling=${ICOUNT_CEILING_ENCODE[$codec]:-}
            table_name=ICOUNT_CEILING_ENCODE
        elif [[ "$DIRECTION" == "ac4" ]]; then
            table_ceiling=${ICOUNT_CEILING_AC4[$codec]:-}
            table_name=ICOUNT_CEILING_AC4
        else
            table_ceiling=${ICOUNT_CEILING[$codec]:-}
            table_name=ICOUNT_CEILING
        fi
        ceiling=${!override:-$table_ceiling}
        echo "${codec}.instructions_per_frame=${instr}" | tee -a "$OUTPUT"
        if [[ -z "$ceiling" ]]; then
            echo "::error title=No instruction ceiling::${codec} has no entry in run_baremetal_probe.sh's ${table_name} table - add one from a measured run" >&2
            exit 1
        fi
        echo "instructions: ${codec} = ${instr} per frame (ceiling ${ceiling})"
        if (( instr > ceiling )); then
            echo "::error title=Instruction-count regression::${codec} executes ${instr} instructions per frame, ceiling is ${ceiling} (see docs/performance-trend.md's instructions-per-frame table)" >&2
            exit 1
        fi
    done <<< "$INSTR"
fi

if [[ -n "${ICLFORGE_FOOTPRINT_SUMMARY:-}" ]]; then
    cp "$OUTPUT" "$ICLFORGE_FOOTPRINT_SUMMARY"
fi

echo "minimum-footprint ${DIRECTION} probe: pass"
