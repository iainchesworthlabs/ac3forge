#!/usr/bin/env bash
# Build and run the minimum-footprint decoder probe (roadmap PF7), then gate on
# what it reports.
#
#   tools/checks/run_baremetal_probe.sh                  # arm-none-eabi under QEMU
#   tools/checks/run_baremetal_probe.sh --host           # natively, no emulator
#
# Two things are checked, and they fail for different reasons:
#
#   1. The probe's own verdict. It decodes both fixtures, compares every
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
if [[ "${1:-}" == "--host" ]]; then
    HOST=1
fi

# --- ceilings --------------------------------------------------------------
# Bytes. text+data+bss of the linked probe on the bare-metal target, and the
# probe's own peak heap on either. See docs/performance-trend.md's footprint
# table for the measured values these leave headroom over.
#
# AC3FORGE_MAX_IMAGE_BYTES was re-based from 400,000 to 465,000 after PF7's own
# feature branch (roadmap PF6/PF7, PR #351) picked up several mid-flight merges
# from `develop` - most significantly DC10's QMF-domain JOC reconstruction,
# which the decode path now genuinely needs (src/dsp/qmf.cpp and
# src/verify/eac3_mirror.cpp, both correctly added to src/forge/minimal.cmake's
# source list) - between when 354,060/400,000 were first measured and when the
# PR actually merged. The image had already reached 412,516 bytes at that
# point; nobody re-measured before merging. See docs/performance-trend.md's
# footprint table for the current breakdown.
: "${AC3FORGE_MAX_IMAGE_BYTES:=465000}"
: "${AC3FORGE_MAX_HEAP_BYTES:=300000}"
# Allocations per frame in the steady state, whichever codec is worse. The
# requirement PF7 states is ZERO and this is not it - see docs/building.md's
# gap note. The ceiling exists so the distance from zero cannot quietly grow
# while that gap is open: today's numbers are 45 (AC-3) and 87 (E-AC-3).
: "${AC3FORGE_MAX_STEADY_ALLOCS_PER_FRAME:=100}"

if [[ "$HOST" == "1" ]]; then
    PRESET=config-linux-gcc-minimal
    BUILD_PRESET=build-linux-gcc-minimal
else
    PRESET=config-arm-none-eabi-minimal
    BUILD_PRESET=build-arm-none-eabi-minimal
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
    echo "::error title=Minimum-footprint decoder probe failed::the probe did not report result=pass" >&2
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

# Both codecs' steady-state churn, held to one ceiling: they are the same
# requirement and a regression in either is the same kind of news.
for codec in ac3 eac3; do
    per_frame=$(sed -n "s/.*${codec}\.steady_allocs_per_frame=\([0-9]*\).*/\1/p" "$OUTPUT" | head -1)
    if [[ -z "$per_frame" ]]; then
        echo "error: the probe reported no ${codec}.steady_allocs_per_frame line" >&2
        exit 1
    fi
    if (( per_frame > AC3FORGE_MAX_STEADY_ALLOCS_PER_FRAME )); then
        echo "::error title=Footprint regression::${codec} steady-state allocations are $per_frame per frame, ceiling is $AC3FORGE_MAX_STEADY_ALLOCS_PER_FRAME" >&2
        exit 1
    fi
done

if [[ -n "${AC3FORGE_FOOTPRINT_SUMMARY:-}" ]]; then
    cp "$OUTPUT" "$AC3FORGE_FOOTPRINT_SUMMARY"
fi

echo "minimum-footprint decoder probe: pass"
