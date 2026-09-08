#!/usr/bin/env bash
# Build and run the minimum-footprint decoder probe (roadmap PF7), then gate on
# what it reports.
#
#   tools/checks/run_baremetal_probe.sh                  # arm-none-eabi under QEMU
#   tools/checks/run_baremetal_probe.sh --host           # natively, no emulator
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
# Set AC3FORGE_FOOTPRINT_SUMMARY to a path to also write the probe's key=value
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
for arg in "$@"; do
    case "$arg" in
        --host) HOST=1 ;;
        --encoder) DIRECTION=encoder ;;
        --decoder) DIRECTION=decoder ;;
        *) echo "usage: run_baremetal_probe.sh [--host] [--encoder|--decoder]" >&2; exit 2 ;;
    esac
done

# --- ceilings --------------------------------------------------------------
# Bytes. text+data+bss of the linked probe on the bare-metal target, and the
# probe's own peak heap on either. See docs/performance-trend.md's footprint
# table for the measured values these leave headroom over.
#
# AC3FORGE_MAX_IMAGE_BYTES was re-based from 400,000 to 465,000 after PF7's own
# feature branch (roadmap PF6/PF7, PR #351) picked up several mid-flight merges
# from `develop` - most significantly DC10's QMF-domain JOC reconstruction,
# which the decode path now needs (src/forge/src/dsp/qmf.cpp and
# src/forge/src/verify/eac3_mirror.cpp, both correctly added to
# src/forge/minimal.cmake's source list) - between when 354,060/400,000 were
# first measured and when the PR actually merged. The image had already
# reached 412,516 bytes at that point; nobody re-measured before merging.
# See docs/performance-trend.md's footprint table for the current breakdown.
: "${AC3FORGE_MAX_IMAGE_BYTES:=465000}"
# Measured against main at e982712b: image 418,244 of 465,000 (11% headroom)
# and peak heap 270,886 of 300,000 (11%). The heap figure moved up from 243,470
# - and its headroom from 23% to 11% - when AP3's pimpl sweep put both decoders'
# state on the heap instead of in the caller's frame: a relocation out of
# automatic storage rather than new consumption. The ceiling is deliberately
# left where it is. This peak is deterministic for these fixed fixtures, so the
# margin only ever has to absorb a deliberate change, never run-to-run noise.
# See docs/performance-trend.md's footprint table.
: "${AC3FORGE_MAX_HEAP_BYTES:=300000}"
# Allocations per frame in the steady state, whichever fixture is worst. The
# requirement PF7 states is ZERO and this is not it - see docs/building.md's
# gap note. The ceiling exists so the distance from zero cannot quietly grow
# while that gap is open. Today's numbers, all eight fixtures:
#
#   16 ac3_mono   19 ac3_stereo   43 eac3_stereo   47 ac3
#   61 eac3_atmos_bed   66 eac3_ecpl   79 eac3_atmos_objects   86 eac3
#
# ONE ceiling, where there used to be a second one of 140 for enhanced coupling
# alone. That exemption was real while it lasted: §E3.5 reconstructs each
# coupled channel through three 512-point inverse transforms and a DFT per
# block, carries a 22-sub-band geometry against standard coupling's 18, and
# measured 126 per frame - so holding it to 100 would have meant either not
# covering §E3.5 or lifting the ceiling for every other fixture to a number
# none of them was near.
#
# It is gone because the 126 was not the tool's geometry after all. Sixty of it
# were two std::vector<double> constructed per coupled channel per block in
# eac3_decoder.cpp's reconstruction loop - hoisted into decoder scratch, ecpl
# now measures 66 and sits below eac3's own 86. A ceiling of 140 over a
# measurement of 66 would be dead slack, and the exemption would go on implying
# that enhanced coupling is inherently the expensive one.
: "${AC3FORGE_MAX_STEADY_ALLOCS_PER_FRAME:=100}"
# Bytes still live when the probe finishes, after every decoder it made has been
# destroyed. 24 - two __cxa_thread_atexit registration records, one per
# thread_local the library declares.
#
# It was 34,232 until the probe started calling ac3::eac3::release_ecpl_scratch()
# between fixtures. That difference is enhanced coupling's 32,768-byte spectrum
# scratch and its 1,440-byte bin-angle vector, which are thread_local and so
# were resident for the life of a task that never exits. Not a leak - bounded,
# paid once, and the point of caching them - but enough to decide whether
# something else fits: object reconstruction did not, on an ESP32-S3, whenever
# it ran after an enhanced-coupling decode.
#
# 1,024 against a measured 24 is deliberately tight. There is nothing here that
# grows a little; either the scratch is being handed back or it is not, and the
# difference is five figures. A ceiling with room for half of it would report
# nothing useful.
: "${AC3FORGE_MAX_RETAINED_BYTES:=1024}"

if [[ "$HOST" == "1" ]]; then
    PRESET=config-linux-gcc-minimal
    BUILD_PRESET=build-linux-gcc-minimal
else
    PRESET=config-arm-none-eabi-minimal
    BUILD_PRESET=build-arm-none-eabi-minimal
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
    AC3FORGE_MAX_HEAP_BYTES=${AC3FORGE_MAX_HEAP_BYTES_ENCODE:-250000}
fi

cmake --preset "$PRESET"
cmake --build --preset "$BUILD_PRESET"

BIN="build/$PRESET/bin/ac3probe"
if [[ ! -f "$BIN" ]]; then
    echo "error: $BIN was not produced" >&2
    exit 1
fi

OUTPUT=$(mktemp)
trap 'rm -f "$OUTPUT"' EXIT

if [[ "$HOST" == "1" ]]; then
    echo "== running ac3probe natively =="
    "$BIN" | tee "$OUTPUT"
else
    echo "== running ac3probe on qemu-system-arm (mps2-an385, cortex-m3) =="
    # -semihosting is what gives the probe stdout and an exit code at all; the
    # newlib rdimon specs the toolchain file links against are its other half.
    # A timeout because a probe that faults early would otherwise hang the leg
    # rather than fail it.
    timeout 300 qemu-system-arm \
        -M mps2-an385 -cpu cortex-m3 \
        -monitor none -nographic -semihosting \
        -kernel "$BIN" | tee "$OUTPUT"

    echo
    echo "== image size =="
    arm-none-eabi-size "$BIN"
    IMAGE=$(arm-none-eabi-size "$BIN" | awk 'NR==2 {print $4}')
    echo "image.total_bytes=$IMAGE" | tee -a "$OUTPUT"
    if (( IMAGE > AC3FORGE_MAX_IMAGE_BYTES )); then
        echo "::error title=Footprint regression::linked image is $IMAGE bytes, ceiling is $AC3FORGE_MAX_IMAGE_BYTES (see docs/performance-trend.md's footprint table)" >&2
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
if (( heap > AC3FORGE_MAX_HEAP_BYTES )); then
    echo "::error title=Footprint regression::peak heap is $heap bytes, ceiling is $AC3FORGE_MAX_HEAP_BYTES (see docs/performance-trend.md's footprint table)" >&2
    exit 1
fi

retained=$(sed -n 's/.*heap\.retained_bytes=\([0-9]*\).*/\1/p' "$OUTPUT" | head -1)
if [[ -z "$retained" ]]; then
    echo "error: the probe reported no heap.retained_bytes line" >&2
    exit 1
fi
echo "retained after teardown: $retained bytes (ceiling $AC3FORGE_MAX_RETAINED_BYTES)"
if (( retained > AC3FORGE_MAX_RETAINED_BYTES )); then
    echo "::error title=Footprint regression::$retained bytes are still live after every decoder was destroyed, ceiling is $AC3FORGE_MAX_RETAINED_BYTES - see the heap.retained_bucket lines for which buffer" >&2
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
if [[ "$DIRECTION" == "encoder" ]]; then
    # 260 rather than 100. E-AC-3 encode measures 249 allocations per frame and
    # AC-3 78, against the decoders' 43-126 - and the reason is in the API, not
    # the implementation: both encoders return std::vector<std::byte> from
    # encode_frame, with no encode_frame_into to match decode_frame_into. That
    # is PF7's zero-heap gap seen from the encode side, and it is wider here.
    # Holding this to the decoder's number would gate a difference nothing in
    # this profile can currently close.
    AC3FORGE_MAX_STEADY_ALLOCS_PER_FRAME=${AC3FORGE_MAX_STEADY_ALLOCS_PER_FRAME_ENCODE:-260}
fi
if [[ -z "$CHURN" ]]; then
    echo "error: the probe reported no <fixture>.steady_allocs_per_frame line" >&2
    exit 1
fi
while read -r codec per_frame; do
    ceiling=$AC3FORGE_MAX_STEADY_ALLOCS_PER_FRAME
    echo "churn: ${codec} = ${per_frame} allocations/frame (ceiling ${ceiling})"
    if (( per_frame > ceiling )); then
        echo "::error title=Footprint regression::${codec} steady-state allocations are $per_frame per frame, ceiling is $ceiling" >&2
        exit 1
    fi
done <<< "$CHURN"

if [[ -n "${AC3FORGE_FOOTPRINT_SUMMARY:-}" ]]; then
    cp "$OUTPUT" "$AC3FORGE_FOOTPRINT_SUMMARY"
fi

echo "minimum-footprint ${DIRECTION} probe: pass"
