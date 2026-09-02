#!/usr/bin/env bash
#
# The gold-reference correctness gate: proves ac3cli's own decoder agrees
# with an independent decoder (FFmpeg) on the same encoded bitstream, using a
# fixed, checked-in 5.1 WAV (tests/golden/audio/reference_51.wav - see
# tools/generators/gen_gold_reference_wav.py) as the input material. This is
# docs/RESEARCH.md's validation pyramid L3 ("FFmpeg oracle, every commit")
# plus a lightweight L4 (SNR vs. FFmpeg's own decode) - designed from the
# start, but never wired into any CI leg until now. tools/ci/run_codec_
# matrix.sh already exercises "does every layout/tool/metadata combination
# run without crashing"; this is the complementary "is the audio actually
# right" check that script deliberately does not attempt.
#
# Usage: verify_gold_reference.sh <path-to-ac3cli> [workdir]
# Requires ffmpeg and python3 (or python) on PATH. Exits non-zero on the
# first check that fails.
set -euo pipefail

CLI="${1:?usage: verify_gold_reference.sh <path-to-ac3cli> [workdir]}"
WORKDIR="${2:-$(mktemp -d)}"
mkdir -p "$WORKDIR"

# Resolved from this script's own location, not the caller's cwd, so it works
# the same whether invoked from the repo root (as CI does) or anywhere else.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GOLD_WAV="$REPO_ROOT/tests/golden/audio/reference_51.wav"
COMPARE="$REPO_ROOT/tools/checks/compare_wav.py"

# Same reasoning as docs/RESEARCH.md's L3 recipe: pin drc_scale to 0 on both
# sides so a dynamic-range-compression default mismatch between FFmpeg and
# ac3cli's own decoder (which also defaults drc_scale to 0 - see
# apps/cli/main.cpp's MetaOptions) can never masquerade as a fidelity loss.
#
# 55, not some more conservative-looking round number: this gate compares two
# decodes of the *same* bitstream (FFmpeg vs. ac3cli's own decoder), so absent
# a real bug it should sit near the floating-point noise floor forever, not
# vary the way a lossy-vs-original comparison (see tools/ci/quality_race.py's
# very different, much lower floors) legitimately does. Every real run
# recorded in quality-history (docs/quality-trend.md) to date has landed
# 61.8-67.9 dB, with the ~6 dB floor-to-floor spread being every arm64 and
# macOS leg landing ~6.02 dB (one AC-3 exponent step) below every x86 leg -
# NOT a libm-package difference (an earlier version of this comment blamed
# "macOS-vs-Linux/Windows libm", which the glibc/GCC arm64 Linux legs added
# later contradict outright), still unexplained after ruling out FMA
# contraction and architecture-specific libm sin/cos by direct measurement -
# see docs/building.md's "Floating-point contraction" and roadmap VX11. Not
# commit-to-commit noise either way, which has stayed inside 0.02-0.08 dB.
# 55 leaves the lowest-scoring legs' own ~61.8 dB floor about 7 dB of
# headroom (comfortably above that noise) while catching a regression more
# than an order of magnitude smaller than the previous 30 dB floor ever
# could.
MIN_SNR_DB="${MIN_SNR_DB:-55}"

# --- Per-channel floors -----------------------------------------------------
#
# Every floor below is a VECTOR, one entry per channel in WAV order
# (L R C LFE Ls Rs for the 5.1 fixtures, L R for the stereo ones), because one
# floor across six channels is not one gate: it is one gate on the worst
# channel and dead slack on the other five.
#
# The surrounds of a 5.1 fixture sit far below the front channels by
# construction - the encoder spends fewer bits there, and §7.3.4 leaves dither
# VALUES decoder-defined, so two independently correct decoders diverge in the
# zero-bit bins by design (this is the same freedom the dither=off note further
# down exists for, except a third-party fixture's dithflag is not ours to turn
# off). A single floor therefore has to clear the surrounds, and on
# ext_ac3_51_448_dee that meant 22 dB - against a centre channel measuring
# 58.11 dB. The centre could have collapsed by 36 dB and this gate would still
# have gone green, which is precisely the per-channel syntax misread the
# external-baseline block below was added to catch (see its own list: the
# firstcplcos[ch] bug is per channel).
#
# Derivation, uniformly: floor = floor(min_observed - 6.02), where
# min_observed is that channel's lowest value across EVERY leg and EVERY commit
# recorded in the quality-history branch (9 legs, 57-74 commits, depending on
# the check). 6.02 dB is one AC-3 exponent step - the single unexplained
# cross-platform effect this project has measured, where every arm64 and macOS
# leg lands ~6.02 dB below every x86 leg on some channels (see MIN_SNR_DB's own
# comment and roadmap VX11). A floor tighter than one step risks a new platform
# tripping it for a reason that is not a defect; a floor looser than one step
# is the slack this change exists to remove.
#
# One channel's floor went DOWN: ext_ac3_51_448_dee's Ls/Rs, 22 -> 16, because
# 22 was never derived for those channels - it was the single floor the whole
# fixture had to share, and it happened to sit 0.7 dB under them. Its four
# other channels gained 29-54 dB of gate in exchange, and the check as a whole
# went from catching only a total surround collapse to catching a 6 dB
# regression in any channel. That is the trade, stated out loud rather than
# hidden in a table.
#
# Regenerate after a deliberate, reviewed quality change - never to make a red
# gate green:
#   python3 tools/checks/derive_channel_floors.py --history <main.jsonl>
AC3_GOLD_FLOORS="75,75,68,76,55,60"
EAC3_GOLD_FLOORS="75,75,68,76,55,60"
EAC3_CPL_GOLD_FLOORS="75,75,68,76,55,60"

# Optional: when set, check_one also asks compare_wav.py to write a
# structured result to "$RESULTS_JSON_DIR/<label>.json" - consumed by CI's
# quality-trend job (see .github/workflows/_build.yml and
# tools/ci/append_quality_history.py). Unset for a plain local/manual run,
# which behaves exactly as before this existed.
RESULTS_JSON_DIR="${RESULTS_JSON_DIR:-}"

# Optional (roadmap VX10): "reference" makes every ac3cli encode and decode
# below take mode=reference, so the whole gate runs on the spec's own direct
# transform evaluations - the §8.2.3.2 forward MDCT and §7.9.4's step-3
# inverse - instead of the fast paths that have been the default since 0.9.0.
# Those direct forms are the oracle every fast path is validated against, and
# after the defaults flipped, nothing that touches a real stream exercised
# them any more: the transform-level unit tests did, and no end-to-end gate
# did. One CI leg runs this script a second time with this set (see
# .github/workflows/_build.yml), which is what keeps a change to either fast
# path from silently taking its own oracle with it.
#
# Empty (the default) passes no mode token at all, so an ordinary run is
# byte-for-byte the same commands it was before this existed. Set, it also
# suffixes every check's label, so the second run's per-check JSON results
# land beside the first run's rather than overwriting them.
TRANSFORM_MODE="${TRANSFORM_MODE:-}"
CLI_MODE_ARGS=()
LABEL_SUFFIX=""
if [ -n "$TRANSFORM_MODE" ]; then
    CLI_MODE_ARGS=("mode=$TRANSFORM_MODE")
    LABEL_SUFFIX="_$TRANSFORM_MODE"
fi

# Every ac3cli invocation goes through here so the mode token is applied in
# exactly one place. The length test rather than a bare "${CLI_MODE_ARGS[@]}"
# guards the macOS bash 3.2 `set -u` behaviour where expanding a zero-element
# array is an unbound-variable error - see compare_and_gate's own note, which
# explains why the same guard is NOT needed on its argument array.
run_cli() {
    if [ ${#CLI_MODE_ARGS[@]} -eq 0 ]; then
        "$CLI" "$@"
    else
        "$CLI" "$@" "${CLI_MODE_ARGS[@]}"
    fi
}

PYTHON=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done
if [ -z "$PYTHON" ]; then
    echo "::error::no python3/python on PATH" >&2
    exit 1
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "::error::ffmpeg not on PATH" >&2
    exit 1
fi
if [ ! -f "$GOLD_WAV" ]; then
    echo "::error::gold reference WAV missing: $GOLD_WAV" >&2
    exit 1
fi

count=0

# L3: FFmpeg strict-decode. -err_detect crccheck+bitstream+buffer+explode
# turns on checks FFmpeg does NOT run by default (crucially the AC-3 CRC,
# per docs/RESEARCH.md's own critical finding) - pass is exit 0 with empty
# stderr, not just "a WAV came out."
ffmpeg_strict_decode() {
    local in="$1" out="$2"
    local stderr_out
    stderr_out="$(ffmpeg -y -v error -err_detect crccheck+bitstream+buffer+explode \
        -drc_scale 0 -i "$in" -f wav "$out" 2>&1 >/dev/null)"
    if [ -n "$stderr_out" ]; then
        echo "::error::ffmpeg strict decode of $in reported errors:" >&2
        echo "$stderr_out" >&2
        exit 1
    fi
}

# The single place compare_wav.py is invoked, so the scalar-vs-vector choice
# and the --json-out wiring are decided once rather than in each caller.
# $1: reference WAV. $2: WAV under test. $3: label. $4: codec label.
# $5: bitrate. $6: scalar floor. $7: per-channel vector, or empty for scalar.
#
# compare_args always carries at least a floor argument, so expanding it is
# safe on macOS's bash 3.2 - whose `set -u` treats expanding a ZERO-element
# array as an unbound variable (the reason the previous form guarded on
# ${#json_args[@]}; there is nothing left to guard now that the array can
# never be empty).
compare_and_gate() {
    local reference="$1" actual="$2" label="$3" codec="$4" bitrate_kbps="$5"
    local min_snr_db="$6" per_channel="$7"
    local compare_args=()

    if [ -n "$per_channel" ]; then
        compare_args+=(--min-snr-db-per-channel "$per_channel")
    else
        compare_args+=(--min-snr-db "$min_snr_db")
    fi
    if [ -n "$RESULTS_JSON_DIR" ]; then
        mkdir -p "$RESULTS_JSON_DIR"
        compare_args+=(--json-out "$RESULTS_JSON_DIR/${label}.json"
                       --codec-label "$codec" --bitrate-kbps "$bitrate_kbps")
    fi
    "$PYTHON" "$COMPARE" "$reference" "$actual" "${compare_args[@]}"
}

# One (encode, decode-with-ac3cli, decode-with-ffmpeg, compare) round for a
# given codec. $1: human label. $2: the file ac3cli just produced. $3: codec
# label for --json-out. $4: nominal bitrate in kbps for --json-out. $5:
# min-snr-db override (default MIN_SNR_DB) - separate from the global default
# because that default is calibrated for streams THIS PROJECT's own encoder
# produced (see MIN_SNR_DB's own comment); a real third-party encoder's
# output has no such guarantee and needs its own, separately-justified floor.
# $6: per-channel floor vector, which supersedes $5 when given.
check_one() {
    local label="$1$LABEL_SUFFIX" encoded="$2" codec="$3" bitrate_kbps="$4"
    local min_snr_db="${5:-$MIN_SNR_DB}"
    # $6: optional per-channel floor vector (see "Per-channel floors" above).
    # Empty means "gate on the scalar", which is what every call site did
    # before vectors existed and what a check with no derived vector still
    # does - never a silently different gate.
    local per_channel="${6:-}"
    local ffmpeg_wav="$WORKDIR/${label}_ffmpeg.wav"
    local our_wav="$WORKDIR/${label}_ours.wav"

    count=$((count + 1))
    echo "[$count] $label: FFmpeg strict decode (L3)"
    ffmpeg_strict_decode "$encoded" "$ffmpeg_wav"

    count=$((count + 1))
    echo "[$count] $label: ac3cli decode"
    run_cli decode "$encoded" "$our_wav" >/dev/null

    count=$((count + 1))
    if [ -n "$per_channel" ]; then
        echo "[$count] $label: SNR vs. FFmpeg's decode (L4-lite, per channel >= ${per_channel})"
    else
        echo "[$count] $label: SNR vs. FFmpeg's decode (L4-lite, >= ${min_snr_db} dB)"
    fi
    compare_and_gate "$ffmpeg_wav" "$our_wav" "$label" "$codec" "$bitrate_kbps" \
        "$min_snr_db" "$per_channel"
}

# The other half of check_one, for a third-party bitstream FFmpeg cannot
# read. There is no FFmpeg decode to compare against and no strict decode to
# run, so the reference is the WAV the third-party encoder was handed - which
# this repository also has, since these fixtures were produced from its own
# committed source material (tools/generators/gen_external_baseline.py). That
# makes this a lossy-vs-original measurement rather than
# check_one's two-decodes-of-one-bitstream one, so its floor is set on the
# same basis tools/ci/quality_race.py's are (well below a measured number)
# rather than on check_one's near-noise-floor basis.
# $1: label. $2: the bitstream. $3: the source WAV it was encoded from.
# $4: codec label for --json-out. $5: nominal bitrate. $6: min SNR.
# $7: per-channel floor vector, which supersedes $6 when given.
check_against_source() {
    local label="$1$LABEL_SUFFIX" encoded="$2" source_wav="$3" codec="$4" bitrate_kbps="$5"
    local min_snr_db="$6"
    local per_channel="${7:-}"
    local our_wav="$WORKDIR/${label}_ours.wav"

    count=$((count + 1))
    echo "[$count] $label: ac3cli decode (no FFmpeg oracle - see this check's own comment)"
    run_cli decode "$encoded" "$our_wav" >/dev/null

    count=$((count + 1))
    if [ -n "$per_channel" ]; then
        echo "[$count] $label: SNR vs. the source WAV (per channel >= ${per_channel})"
    else
        echo "[$count] $label: SNR vs. the source WAV (>= ${min_snr_db} dB)"
    fi
    compare_and_gate "$source_wav" "$our_wav" "$label" "$codec" "$bitrate_kbps" \
        "$min_snr_db" "$per_channel"
}


# dither=off / nodither below: §7.3.4 leaves the actual dither VALUES
# decoder-defined ("any reasonably random sequence"), so two independent,
# spec-correct decoders given the same dithered stream diverge in the
# dithered bins by design, not by bug - once dithflag is really decided from
# content (EncoderConfig::dither / plan::Tools::dither), the gold material
# below hits that on most blocks and would fail this gate's tight floor for a
# reason that has nothing to do with a regression. This gate exists to
# compare two decodes of the SAME bitstream at floating-point-noise
# precision, so it asks every encode here for the deterministic, pre-EQ4
# dithflag=0 behaviour instead - the one thing this gate cannot tell apart
# from a real bug. Nothing about dither itself is exercised here;
# tools/ci/quality_race.py and the CLI's own dithflag tests own that.
count=$((count + 1))
echo "[$count] encode: AC-3 5.1 @ 448 kbps"
run_cli encode "$GOLD_WAV" "$WORKDIR/gold.ac3" 448 51 dither=off >/dev/null
check_one "ac3" "$WORKDIR/gold.ac3" "ac3" 448 "$MIN_SNR_DB" "$AC3_GOLD_FLOORS"

count=$((count + 1))
echo "[$count] encode: E-AC-3 5.1 @ 256 kbps (tools=none)"
# The plain-path baseline: no Annex E tool engages, so this is the closest
# thing to an apples-to-apples comparison between the two decoders and sets
# the tightest floor (see MIN_SNR_DB's own comment for the 61.8-67.9 dB range
# this has landed in historically). "nodither" alone is this generation's
# "no tools" - the literal "none" is a distinct special case parse_tools()
# does not let another token join.
run_cli eac3-encode "$GOLD_WAV" "$WORKDIR/gold.ec3" 256 nodither 51 >/dev/null
check_one "eac3" "$WORKDIR/gold.ec3" "eac3" 256 "$MIN_SNR_DB" "$EAC3_GOLD_FLOORS"

count=$((count + 1))
echo "[$count] encode: E-AC-3 5.1 @ 256 kbps (tools=cpl)"
# The in-repo decoder reads every Annex E tool now (see run_codec_matrix.sh's
# header comment - the "this decoder refuses any Annex E tool-enabled
# stream" limitation that used to live in this comment is gone, confirmed by
# hand 2026-08-12: `eac3-encode ... 256 cpl 51` followed by `decode` round-
# trips cleanly). That means this gate can finally exercise a tool-enabled
# encode/decode path for real, and coupling is the one worth adding here:
# measured worst-channel SNR against FFmpeg's own decode is 67.73 dB, close
# enough to the tools=none baseline above (67.90 dB on the same run) to
# reuse the same MIN_SNR_DB floor rather than needing a separately-justified
# one - unlike the cplbndstrce0 fixture below, whose lower floor is about
# comparing against a real third-party bitstream, not about coupling itself.
#
# spx and aht are deliberately NOT added here even though the decoder reads
# them too: measured worst-channel SNR against FFmpeg's own decode is only
# ~31 dB for spx, ~20 dB for aht, and ~28 dB for all (cpl+spx+aht together) -
# both tools are approximate/generative reconstruction (spx regenerates high
# frequencies from a copied-down band plus a noise blend; AHT's Huffman/
# transform path has its own reconstruction choices), so two independent,
# spec-correct decoders legitimately diverge far more than the plain or
# cpl-only paths do. A 55 dB floor would fail on that legitimate divergence,
# not a bug, and a floor low enough to accommodate it (~15-20 dB, like
# cplbndstrce0's) would be too loose to catch a real regression in this
# tool-enabled path specifically. run_codec_matrix.sh already covers spx/aht/
# all at the round-trips-without-crashing and FFmpeg-parses-it level; this
# gate's tight dB-based regression detection just isn't the right tool for
# them yet.
run_cli eac3-encode "$GOLD_WAV" "$WORKDIR/gold_cpl.ec3" 256 cpl+nodither 51 >/dev/null
check_one "eac3_cpl" "$WORKDIR/gold_cpl.ec3" "eac3" 256 "$MIN_SNR_DB" "$EAC3_CPL_GOLD_FLOORS"

# Third-party interop: cplbndstrce == 0 (Annex E's default coupling band
# structure, Table E2.12). This project's own encoder always transmits an
# explicit structure (see eac3_tools.hpp's kDefaultCplBandStructure comment),
# so nothing above ever exercises this path - only a real third-party
# encoder does, which is exactly what let a real decoder bug here go
# unnoticed until FFmpeg's own E-AC-3 output was tried against this decoder
# for the first time: it refused cplbndstrce == 0 outright (kUnsupported).
# gold.ec3 above (this project's own encoder) cannot stand in for that, so
# this checks a real FFmpeg-encoded fixture directly instead of an
# ac3cli-produced one:
#   tests/golden/audio/reference_51_eac3_448k_cplbndstrce0.ec3
#     ffmpeg -y -i tests/golden/audio/reference_51.wav -c:a eac3 -b:a 448k \
#         tests/golden/audio/reference_51_eac3_448k_cplbndstrce0.ec3
# Confirmed (ffmpeg 8.0.1) to set cplbndstrce == 0 with cplbegf == 12 in
# every block - cplbegf != 0 matters: an indexing bug that reads the default
# table relative to cplbegf instead of absolutely from it would still pass
# on a stream where cplbegf happens to be 0, so this fixture is deliberately
# NOT one of those.
# Measured (ffmpeg 8.0.1, this fixture) at 25.42 dB worst-channel agreement
# between ac3cli's decode and FFmpeg's own - lower than gold.ac3/gold.ec3's
# 55 dB floor above because those compare two decodes of a stream THIS
# PROJECT's own encoder produced, while this compares two decodes of a real
# third-party bitstream neither side controls. 15 dB stays well clear of
# that measured floor while still failing hard on the failure modes this
# fixture exists to catch: a full revert (kUnsupported, no WAV at all) and
# an indexing bug that reads the table relative to cplbegf instead of
# absolutely from it (confirmed by hand: with this fixture's cplbegf == 12,
# that specific bug picks the wrong merge decisions for every coupling band
# and the corrupted geometry fails outright with kInvalidStream downstream,
# rather than merely losing fidelity).
CPLBNDSTRCE0_MIN_SNR_DB=15
CPLBNDSTRCE0_FLOORS="45,60,51,76,16,16"
CPLBNDSTRCE0_EC3="$REPO_ROOT/tests/golden/audio/reference_51_eac3_448k_cplbndstrce0.ec3"
if [ ! -f "$CPLBNDSTRCE0_EC3" ]; then
    echo "::error::fixture missing: $CPLBNDSTRCE0_EC3" >&2
    exit 1
fi
check_one "eac3_cplbndstrce0" "$CPLBNDSTRCE0_EC3" "eac3" 448 \
    "$CPLBNDSTRCE0_MIN_SNR_DB" "$CPLBNDSTRCE0_FLOORS"

# --- Third-party decode interop (roadmap VX4) -------------------------------
# tests/golden/external-baseline/ holds six bitstreams from two real
# third-party encoders - Dolby Encoding Engine 6.5.4 and FFmpeg 8.0.1 - across
# three legs (see tools/generators/gen_external_baseline.py, which produced
# them, and tests/golden/external-baseline/manifest.json for the versions).
# They are the closest thing to conformance vectors this project can legally
# hold, and until now nothing in tests/ or src/ read them at all: their only
# consumer was tools/ci/quality_race.py, which decodes them with FFMPEG for
# the landscape spectrograms. So the streams were in the repository, and this
# project's own decoder had never been pointed at four of them.
#
# That was not a theoretical gap. Running this check for the first time found
# five separate Annex E decoder defects, all of them syntax this project's own
# encoder and FFmpeg's both happen never to produce and DEE's routinely does
# (each one is documented at its own site in
# src/forge/src/decoder/eac3_decoder.cpp):
#   - cplahtinu/chahtinu[ch]/lfeahtinu read unconditionally, when §E2.2.3
#     transmits each only where that stream's exponents are sent exactly once
#     in the frame;
#   - cplfgaincod and cplfsnroffst - the coupling channel's own fast gain and
#     fine SNR offset, which lead the per-channel lists in Table E1.4 - not
#     read at all;
#   - the cplbndstrce/spxbndstrce/ecplbndstrce default band-structure tables
#     applied in every block whose flag was clear, when §E2.3.3.7/.15/.18 use
#     the default only in the first block using that tool and the PREVIOUS
#     BLOCK's structure in every later one;
#   - firstcplcos[ch]/firstspxcos[ch]/firstcplleak treated as "blk == 0"
#     rather than the per-frame, per-channel state §E2.3.2.28-30 defines, so a
#     channel joining coupling part-way through a frame read a coordinate
#     exist bit that was never transmitted.
# Every one of them desynchronised the bit reader and made the whole frame
# unreadable, and none was reachable from anything this repository could
# encode for itself. That is the entire argument for this block.
#
# Floors are set the same way the cplbndstrce0 fixture's above was: from a
# measured number, with enough headroom that only a real defect trips them.
# They are much lower than MIN_SNR_DB because a real third-party encoder's
# choices - dither in bap == 0 bins above all, whose pseudo-random sequence is
# per-implementation - put two independently correct decoders far apart on the
# channels those choices land in, while the untouched channels stay in the
# 50-90 dB range.
#
# "the channels those choices land in" is the whole reason these floors are now
# per channel rather than one number per fixture. The dither divergence is
# concentrated in the surrounds; the front channels and the LFE stay in that
# 50-90 dB band. A single floor low enough for the former left the latter
# ungated by 30-70 dB - see "Per-channel floors" at the top of this file.
EXTERNAL_BASELINE_DIR="$REPO_ROOT/tests/golden/external-baseline"
if [ ! -d "$EXTERNAL_BASELINE_DIR" ]; then
    echo "::error::external-baseline fixtures missing: $EXTERNAL_BASELINE_DIR" >&2
    exit 1
fi

# label:relative-path:codec:bitrate:scalar-floor:per-channel-floors. Five of
# the six: FFmpeg strict-decodes each of these cleanly, so they get check_one's
# full both-decoders-then-diff treatment. The scalar floor is retained as the
# documented fallback and as the historical record of what this check used to
# gate on; the vector beside it is what actually gates now.
#
# Per channel, L R C LFE Ls Rs (L R for the stereo fixture), each derived as
# floor(min_observed - 6.02) - see "Per-channel floors" at the top of this
# file for the derivation and for why one pair of floors went down:
#
#                              was            now (per channel)
#   ac3-51-448/dee.ac3          22    51,57,52,76,16,16
#   ac3-51-448/ffmpeg.ac3       14    45,60,51,76,16,17
#   eac3-51-256/dee.ec3         10    37,42,44,76,12,12
#   eac3-51-256/ffmpeg.ec3      25    39,45,48,76,29,30
#   eac3-stereo-192/ffmpeg.ec3  25    30,31
#
# The LFE floor is 76 on every 5.1 fixture because that channel is a single
# low-frequency band both decoders reproduce almost exactly (82-88 dB measured,
# the ~6 dB spread being the exponent step). It was previously gated at 10-25
# dB, i.e. not gated at all.
for entry in \
    "ext_ac3_51_448_dee:ac3-51-448/dee.ac3:ac3:448:22:51,57,52,76,16,16" \
    "ext_ac3_51_448_ffmpeg:ac3-51-448/ffmpeg.ac3:ac3:448:14:45,60,51,76,16,17" \
    "ext_eac3_51_256_dee:eac3-51-256/dee.ec3:eac3:256:10:37,42,44,76,12,12" \
    "ext_eac3_51_256_ffmpeg:eac3-51-256/ffmpeg.ec3:eac3:256:25:39,45,48,76,29,30" \
    "ext_eac3_stereo_192_ffmpeg:eac3-stereo-192/ffmpeg.ec3:eac3:192:25:30,31" \
    ; do
    IFS=: read -r ext_label ext_path ext_codec ext_kbps ext_floor ext_channel_floors <<EOF
$entry
EOF
    if [ ! -f "$EXTERNAL_BASELINE_DIR/$ext_path" ]; then
        echo "::error::fixture missing: $EXTERNAL_BASELINE_DIR/$ext_path" >&2
        exit 1
    fi
    check_one "$ext_label" "$EXTERNAL_BASELINE_DIR/$ext_path" "$ext_codec" "$ext_kbps" \
        "$ext_floor" "$ext_channel_floors"
done

# The sixth. FFmpeg fails frame 0 of DEE's stereo E-AC-3 stream from cold -
# one frame of the 94, reporting "exponent 25 is out-of-range" and "error
# decoding the audio block" - and conceals it by repeating block 0 across
# blocks 1-4 rather than dropping it. Under this script's strict flags that
# one frame is a hard failure outright, and even without them the concealment
# is whole-file damage: FFmpeg's decode of this fixture scores 14.30 dB
# against the source WAV, where ac3cli's scores 33.72 dB on the same
# alignment. So there is no usable FFmpeg oracle here, the same situation
# run_codec_matrix.sh already handles by skipping the FFmpeg check rather
# than tolerating its failure. The reference used instead is the WAV DEE was
# handed, which this repository has: 33.72 dB measured, and comparable to the
# 33.1 dB ac3cli's decoder gets on FFmpeg's own encode of the same source at
# the same rate - two encoders' output landing within 0.6 dB of each other
# through one decoder is what says this decode is right and FFmpeg's is not.
# Floor 25 on the same measured-minus-8 basis as above, superseded in practice
# by the per-channel vector 27,28 (L R) passed below - derived, like every
# other vector here, as floor(min_observed - 6.02) from recorded history.
#
# manifest.json's 33.32 dB for this leg says "decoded_with": "ffmpeg" and is
# not in conflict with the 14.30 dB above: quality_race.py's score_fixed
# skips the first 0.2 s, which is exactly where the failing frame sits. See
# gen_external_baseline.py's module docstring, which records the whole
# analysis.
DEE_STEREO_EC3="$EXTERNAL_BASELINE_DIR/eac3-stereo-192/dee.ec3"
STEREO_WAV="$REPO_ROOT/tests/golden/audio/reference_stereo.wav"
for required in "$DEE_STEREO_EC3" "$STEREO_WAV"; do
    if [ ! -f "$required" ]; then
        echo "::error::fixture missing: $required" >&2
        exit 1
    fi
done
check_against_source "ext_eac3_stereo_192_dee" "$DEE_STEREO_EC3" "$STEREO_WAV" "eac3" 192 \
    25 "27,28"

# --- Cross-platform bitstream-hash gate (roadmap VX11) ----------------------
# Every check above compares two DECODES of the same bitstream, which cannot
# see a divergence in the bitstream itself - the ~6.02 dB gap the arm64/macOS
# legs measure against x86 on this same gate is exactly that kind of
# divergence. This pins it instead: SHA-256 of the three streams this
# project's own encoder just produced above (gold.ac3/gold.ec3/gold_cpl.ec3),
# checked against tests/golden/bitstream-hashes.json. See
# tools/checks/check_cross_platform_hash.py's own header for what a
# not-yet-pinned kernel/mode key does (reported, not failed) versus a real
# mismatch (failed, same as every other bit-exactness gate here).
"$PYTHON" "$REPO_ROOT/tools/checks/check_cross_platform_hash.py" \
    --cli "$CLI" --workdir "$WORKDIR" --label-suffix "$LABEL_SUFFIX"

echo "gold reference gate: $count checks passed in $WORKDIR"
