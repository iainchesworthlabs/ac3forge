#!/usr/bin/env bash
# fuzz/generate-seeds.sh - grow a fuzzing corpus from ac3forge's own valid
# output, by running ac3cli across the layout/codec/tool matrix this project
# already supports. Cheaper and more representative than hand-written corpus
# files: every seed here is a real, self-consistent stream this project can
# actually produce, so a fuzzer's mutations start from "almost valid" rather
# than from nothing.
#
# Needs a built ac3cli. Any working configuration will do: this generates
# plain valid streams, not instrumented ones, so it needs neither Clang nor
# sanitizers. The Windows MSVC leg is what gets used in practice, for no
# better reason than that it is the one already built on the development
# host - every other leg builds clean too.
#
#   AC3CLI_BIN=build/config-windows-msvc-debug/bin/ac3cli.exe fuzz/generate-seeds.sh
#
# Usage: fuzz/generate-seeds.sh [output-dir]   (default: fuzz/seeds)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
AC3CLI="${AC3CLI_BIN:-}"
OUT="${1:-$REPO_ROOT/fuzz/seeds}"

if [ -z "$AC3CLI" ] || [ ! -f "$AC3CLI" ]; then
    echo "error: set AC3CLI_BIN to a built ac3cli (see this script's header)" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$OUT/fuzz_scan" "$OUT/fuzz_ac3_decode" "$OUT/fuzz_eac3_decode" "$OUT/fuzz_wav_read" \
         "$OUT/fuzz_signing_verify" "$OUT/fuzz_iec61937_unwrap"

run() { "$AC3CLI" "$@" >/dev/null; }

# add_seed <comma-separated dest names under $OUT> <file>
add_seed() {
    local dests="$1" file="$2" base names
    base="$(basename "$file")"
    IFS=',' read -ra names <<< "$dests"
    for name in "${names[@]}"; do
        cp "$file" "$OUT/$name/$base"
    done
}

echo "==> AC-3: silence, sine and orbit across every layout AC-3 can carry"
for layout in mono stereo 51 51c; do
    f="$WORK/ac3-sine-$layout.ac3"
    run sine "$f" 1 192 1000 50 "$layout"
    add_seed "fuzz_scan,fuzz_ac3_decode" "$f"
done
run silence "$WORK/ac3-silence.ac3" 1 192
add_seed "fuzz_scan,fuzz_ac3_decode" "$WORK/ac3-silence.ac3"
run orbit "$WORK/ac3-orbit.ac3" 1 448 2
add_seed "fuzz_scan,fuzz_ac3_decode" "$WORK/ac3-orbit.ac3"

echo "==> AC-3 -> WAV roundtrip, for real-audio encode input and WAV-reader seeds"
run decode "$WORK/ac3-sine-51.ac3" "$WORK/roundtrip-51.wav"
add_seed "fuzz_wav_read" "$WORK/roundtrip-51.wav"
run decode "$WORK/ac3-sine-stereo.ac3" "$WORK/roundtrip-stereo.wav"
add_seed "fuzz_wav_read" "$WORK/roundtrip-stereo.wav"

echo "==> AC-3: real-audio encode (silence/tones alone give false passes - see"
echo "    codec-validation-needs-real-audio)"
f="$WORK/ac3-encode-51.ac3"
run encode "$WORK/roundtrip-51.wav" "$f" 448 51
add_seed "fuzz_scan,fuzz_ac3_decode" "$f"

# 512 kbit/s stereo: 2048-byte syncframes, the top of AC-3's per-frame size
# range for 48 kHz, with most of each frame spent on §5.3.3 skip-field
# padding and 16-bit (bap 15) mantissas - a size-and-layout corner none of
# the 192/448 kbit/s seeds above reach. This shape surfaced the FFmpeg
# probe-window misdetection recorded in tools/ci/fuzz_encoder_space.py (the
# note above MIN_STREAM_BYTES, case seed 1124127684685913171): the stream is
# fully valid, so the decoder-side fuzzers should mutate from it too.
f="$WORK/ac3-encode-stereo-512.ac3"
run encode "$WORK/roundtrip-stereo.wav" "$f" 512 stereo
add_seed "fuzz_scan,fuzz_ac3_decode" "$f"

echo "==> E-AC-3: silence and sine across every layout"
for layout in mono stereo 51 71 512 514 714; do
    f="$WORK/eac3-sine-$layout.ec3"
    run eac3-sine "$f" 1 192 1000 50 "$layout"
    add_seed "fuzz_scan,fuzz_eac3_decode" "$f"
    f="$WORK/eac3-silence-$layout.ec3"
    run eac3-silence "$f" 1 192 "$layout"
    add_seed "fuzz_scan,fuzz_eac3_decode" "$f"
done

echo "==> E-AC-3: real-audio encode across every Annex E tool combination"
for tools in none cpl spx aht all "cpl+spx" "cpl:4+spx:5" "aht:0" "spx+noatten" "spx+atten:12"; do
    safe="${tools//[:+]/_}"
    for layout in 51 714; do
        f="$WORK/eac3-encode-${safe}-${layout}.ec3"
        run eac3-encode "$WORK/roundtrip-51.wav" "$f" 448 "$tools" "$layout"
        add_seed "fuzz_scan,fuzz_eac3_decode" "$f"
    done
done

echo "==> Atmos: JOC + OAMD object container, and the bed51 fallback (both are"
echo "    plain E-AC-3 syntax as far as the decoder entry points are concerned -"
echo "    see graceful-51-fallback-either-or)"
run atmos "$WORK/atmos-objects.ec3" 1 448 4 3 objects
add_seed "fuzz_scan,fuzz_eac3_decode" "$WORK/atmos-objects.ec3"
run atmos "$WORK/atmos-bed51.ec3" 1 448 4 3 bed51
add_seed "fuzz_scan,fuzz_eac3_decode" "$WORK/atmos-bed51.ec3"
run atmos-encode "$WORK/roundtrip-51.wav" "$WORK/atmos-encode.ec3" 448 0
add_seed "fuzz_scan,fuzz_eac3_decode" "$WORK/atmos-encode.ec3"

echo "==> Third-party structure: the committed external-baseline bitstreams"
echo "    (Dolby Encoding Engine 6.5.4 and FFmpeg 8.0.1 - roadmap VX4)"
# Everything above this point is ac3forge's own output, so every seed shares
# ac3forge's own encoder choices: frame-hoisted exponent strategies, coupling
# either on for all six blocks or off, snroffststr 0, frmfgaincode 0, geometry
# sent once in block 0 and never resent. A mutation starting from one of those
# explores the neighbourhood of THIS encoder's syntax, and no amount of it
# reaches syntax the encoder never emits in the first place. The external
# baseline is real third-party structure - a Dolby encoder resending coupling
# geometry mid-frame, bringing channels into coupling part-way through, and
# transmitting the per-channel fast-gain and fine-SNR elements this one leaves
# out - which is exactly the shape that carried five real decoder defects
# found while wiring up tools/checks/verify_gold_reference.sh's interop
# checks. Seeding from these puts the fuzzers' mutations in that neighbourhood
# too.
EXTERNAL_BASELINE="$REPO_ROOT/tests/golden/external-baseline"
for leg in ac3-51-448 eac3-51-256 eac3-stereo-192; do
    case "$leg" in
        ac3-*) ext=ac3; targets="fuzz_scan,fuzz_ac3_decode" ;;
        *)     ext=ec3; targets="fuzz_scan,fuzz_eac3_decode" ;;
    esac
    for tool in dee ffmpeg; do
        src="$EXTERNAL_BASELINE/$leg/$tool.$ext"
        if [ ! -f "$src" ]; then
            echo "error: external-baseline fixture missing: $src" >&2
            exit 1
        fi
        # Renamed on the way in: every fixture is called dee.ac3/ffmpeg.ec3
        # inside its own leg directory, and the corpus directories are flat.
        cp "$src" "$WORK/external-${leg}-${tool}.${ext}"
        add_seed "$targets" "$WORK/external-${leg}-${tool}.${ext}"
    done
done

echo "==> WAV: an IEC 61937 burst-wrapped PCM16 WAV too - a different write path"
run spdif "$WORK/ac3-silence.ac3" "$WORK/spdif.wav"
add_seed "fuzz_wav_read" "$WORK/spdif.wav"

echo "==> Object signing: the same Atmos streams, signed and unsigned, each"
echo "    prefixed with the key fuzz_signing_verify's input format expects"
# fuzz_signing_verify reads a length byte, that many key bytes, then the
# stream (see the harness's own header). A seed therefore has to be built,
# not just copied - and it needs to cover both verification outcomes, so one
# copy is signed with the key the seed carries (kValid) and one is not
# (kMismatch on every frame that has a container to check). The key itself is
# arbitrary throwaway bytes generated here: nothing in this project ships,
# needs or derives a real Dolby key, and verification works against whatever
# key it is handed.
KEY_FILE="$WORK/seed-signing.key"
head -c 16 /dev/urandom > "$KEY_FILE"
# Truncated to the first few syncframes rather than carrying the whole
# second of audio: libFuzzer takes its -max_len from the largest seed, and
# verify_atmos_frame's message-A reconstruction is linear in frame size (it
# walks every bit of the frame once per call), so a 33 KB seed would set the
# mutation length for the whole run and cost exec/s for no extra syntax. Four
# kilobytes is three complete 448 kbit/s frames plus a partial fourth - which
# is itself worth having, since a stream ending mid-frame is exactly what the
# framing walk's `off + size > stream.size()` break exists for.
signing_seed() {
    local name="$1" stream="$2"
    # `out` on its own line, not folded into the `local` above: every word of
    # a `local` is expanded before the builtin assigns any of them, so a
    # "$name" there would still read the caller's (here add_seed's leftover
    # loop variable), not this function's parameter.
    local out="$OUT/fuzz_signing_verify/$name"
    printf '\020' > "$out"       # key length: 16
    cat "$KEY_FILE" >> "$out"
    head -c 4096 "$stream" >> "$out"
}
run atmos "$WORK/atmos-signed.ec3" 1 448 4 3 objects sign-objects "signing-key=$KEY_FILE"
run atmos-encode "$WORK/roundtrip-51.wav" "$WORK/atmos-encode-signed.ec3" 448 0 \
    sign-objects "signing-key=$KEY_FILE"
signing_seed "atmos-objects-signed.bin" "$WORK/atmos-signed.ec3"
signing_seed "atmos-encode-signed.bin" "$WORK/atmos-encode-signed.ec3"
signing_seed "atmos-objects-unsigned.bin" "$WORK/atmos-objects.ec3"
signing_seed "atmos-encode-unsigned.bin" "$WORK/atmos-encode.ec3"
# The bed51 fallback carries no object container at all, so every frame
# reports kNoContainer - the third of verify_atmos_frame's three outcomes,
# and the one a plain non-Atmos stream takes.
signing_seed "atmos-bed51.bin" "$WORK/atmos-bed51.ec3"

echo "==> Metadata payloads: the EMDF containers, and the OAMD and JOC payloads"
echo "    inside them, extracted from the Atmos streams above (roadmap VX3)"
python3 "$SCRIPT_DIR/metadata-seeds.py" extract "$OUT" \
    "$WORK/atmos-objects.ec3" "$WORK/atmos-encode.ec3" "$WORK/atmos-bed51.ec3"

echo "==> ADM: BW64/RF64 fixtures for fuzz_adm_parse - synthesised rather than"
echo "    encoded, since nothing ac3cli produces is an ADM file"
python3 "$SCRIPT_DIR/metadata-seeds.py" adm "$OUT"

echo "==> IEC 61937 carriers, for the burst de-framer (roadmap IO3)"
# Both data types and both burst periods: AC-3's 6144 bytes and E-AC-3's
# 24576. The WAV header stays on deliberately - unspdif walks the RIFF chunk
# list itself, and a fuzzer that only ever saw bare carrier bytes would never
# mutate the chunk sizes that walk trusts.
#
# Cut to the first eight bursts, unlike every other seed here, which is a
# whole file: a burst carrier is 6144 or 24576 bytes PER FRAME, so a second
# of E-AC-3 already comes to 1.5 MB, and libFuzzer deprioritizes large corpus
# entries anyway. Eight bursts exercises every part of the framing a hundred
# would. The cut lands on a burst boundary, so the last one is whole; the
# RIFF header left behind still declares the untruncated length, which is
# itself worth starting from - unspdif clamps a data chunk to what the file
# actually holds, and that clamp is exactly the kind of thing to mutate.
# 44 is the header write_wav_pcm16_raw emits.
run spdif "$WORK/ac3-sine-51.ac3" "$WORK/spdif-ac3-full.wav"
head -c $((44 + 8 * 6144)) "$WORK/spdif-ac3-full.wav" > "$WORK/spdif-ac3-51.wav"
add_seed "fuzz_iec61937_unwrap,fuzz_wav_read" "$WORK/spdif-ac3-51.wav"
run spdif "$WORK/eac3-sine-51.ec3" "$WORK/spdif-eac3-full.wav"
head -c $((44 + 8 * 24576)) "$WORK/spdif-eac3-full.wav" > "$WORK/spdif-eac3-51.wav"
add_seed "fuzz_iec61937_unwrap,fuzz_wav_read" "$WORK/spdif-eac3-51.wav"
# A plain PCM WAV too: "this is not a carrier" is a verdict the de-framer has
# to reach as reliably as it reaches the other one.
head -c 131072 "$WORK/roundtrip-stereo.wav" > "$WORK/carrier-not.wav"
add_seed "fuzz_iec61937_unwrap" "$WORK/carrier-not.wav"

echo "==> done:"
for d in "$OUT"/fuzz_*; do
    printf '    %-20s %s files\n' "$(basename "$d")" "$(find "$d" -type f | wc -l)"
done
