"""Check the gains of ac3cli's AC-4 output processing on DEE's streams against Part 1's formulas.

For each leg the decoder turns into PCM, at every frame rate, this decodes the stream as coded and
again with an output option, and holds the second decode to the first through the formula the option
applies, with the stream's own values read from `ac3cli decode ... syntax-trace=` (planning/ac4.md,
phase D6):

  level    output-level=-31, -24 and -17 with drcmode=off: every channel is the coded output times
           2^((Lout - dialnorm) / 6) (ETSI TS 103 190-1 clause 5.7.9.3.3), dialnorm being
           -dialnorm_bits / 4 dBFS, to 0.01 dB, and nothing else: what the gain leaves is 100 dB
           under the output.
  downmix  a 5.1 leg's channels=2 (the stream's preferred method), downmix=loro, ltrt and mono: the
           output is clause 6.2.17's matrix with the stream's values applied to the coded output,
           which is to say Table 218's Lo/Ro or Lt/Rt, the latter in its Pro Logic II form (+1.8 and
           -3.2 dB) where the stream prefers that, with Tables 149 and 149a's centre and surround
           gains, the LFE at 5.5 - lfe_mixgain dB when the stream sends lfe_mixgain, the stream's
           loudness correction of (15 - x) / 2 dB2, and for mono L + R. What the matrix leaves is 80
           dB under the output: the downmix works in the QMF domain before the synthesis bank, which
           is linear, so the two differ only in the output's rounding.

Both skip the first three frames, where the stream's values have not yet reached the QMF domain,
and stop three frames before the first frame whose values differ from the first frame's: DEE's
immersive stereo at 24 and 25 fps sends a dialnorm of -24 dBFS in its last frame. DRC's curves and
dialogue enhancement's gains are held on known input by tests/ac4dec/test_ac4dec_drc.cpp and
test_ac4dec_de.cpp; this script reads the gains from the stream, as those tests cannot.

The committed legs (tests/golden/external-baseline/) are checked by default. --gold DIR checks
phase G0's local gold set in DIR (DIR/streams/<leg>/dee.ac4, DIR/gold-manifest.json), which never
runs in CI.

Usage:
    python tools/checks/gain_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli
    python tools/checks/gain_ac4_decode.py --cli ac3cli.exe --gold D:/ac3bld/ac4-gold
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "checks"))
from score_ac4_decode import BASELINE_DIR, RESAMPLING, read_wav  # noqa: E402

OUTPUT_LEVELS = (-31.0, -24.0, -17.0)
LEVEL_TOLERANCE_DB = 0.01
LEVEL_RESIDUAL_DB = -100.0
DOWNMIX_RESIDUAL_DB = -80.0
SKIP_FRAMES = 3
CODEC_MODES = ("SIMPLE", "ASPX", "ASPX_ACPL_1", "ASPX_ACPL_2", "ASPX_ACPL_3")
LAYOUTS = ("stereo", "mono", "IMS", "5.1")
# The fields of the stream's downmix values, wherever the stream sends them: the presentation
# substream's custom_dmx_data() and loud_corr(), or basic_metadata().
DOWNMIX_FIELDS = ("loro_centre_mixgain", "loro_surround_mixgain", "b_ltrt_mixinfo",
                  "ltrt_centre_mixgain", "ltrt_surround_mixgain", "lfe_mixgain",
                  "preferred_dmx_method", "loro_dmx_loud_corr", "ltrt_dmx_loud_corr")
# The coded order of a 5.1 leg's output, and ac3cli's WAV order, which is the same.
L, R, C, LFE, LS, RS = range(6)


def db(x):
    return 20.0 * np.log10(x)


def from_db(value):
    return 10.0 ** (value / 20.0)


def centre_gain(code):
    """Table 149: +3, +1.5, 0, -1.5, -3, -4.5, -6 dB and silence; -3 dB where none is sent."""
    if code is None:
        return from_db(-3.0)
    return 0.0 if code == 7 else from_db((3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0)[code])


def surround_gain(code):
    """Table 149a: codes 0 and 1 reserved, read as none sent (-3 dB); then 0, -1.5, -3, -4.5 and
    -6 dB and silence."""
    if code is None or code < 2:
        return from_db(-3.0)
    return 0.0 if code == 7 else from_db((0.0, -1.5, -3.0, -4.5, -6.0)[code - 2])


def loudness_correction(code):
    """(15 - x) / 2 dB2, 31 reading as 0 dB; 1 where none is sent."""
    if code is None or code == 31:
        return 1.0
    return 2.0 ** ((15.0 - code) / 2.0 / 6.0)


def stereo_matrix(values, target):
    """Rows Lo and Ro (or C for mono) over L R C LFE Ls Rs, for `target` 'stereo', 'loro', 'ltrt'
    or 'mono' with the stream's `values` (DOWNMIX_FIELDS, absent where the stream sends none)."""
    preferred = values.get("preferred_dmx_method", 0)
    method = preferred if preferred in (2, 3) else 1
    if target == "loro":
        method = 1
    elif target == "ltrt":
        method = 3 if preferred == 3 else 2
    loro = method == 1
    gains = "ltrt" if not loro and values.get("b_ltrt_mixinfo", 0) == 1 else "loro"
    cmg = centre_gain(values.get(f"{gains}_centre_mixgain"))
    smg = surround_gain(values.get(f"{gains}_surround_mixgain"))
    lo = np.zeros(6)
    ro = np.zeros(6)
    lo[L] = ro[R] = 1.0
    lo[C] = ro[C] = cmg
    if loro:
        lo[LS] = ro[RS] = smg
    else:
        near = smg * from_db(1.8) if method == 3 else smg
        far = smg * from_db(-3.2) if method == 3 else smg
        lo[LS], lo[RS] = -near, -far
        ro[RS], ro[LS] = near, far
    if values.get("lfe_mixgain") is not None:
        lo[LFE] = ro[LFE] = from_db(5.5 - values["lfe_mixgain"])
    correction = loudness_correction(values.get("loro_dmx_loud_corr" if loro
                                                else "ltrt_dmx_loud_corr"))
    lo, ro = lo * correction, ro * correction
    return (lo + ro)[np.newaxis, :] if target == "mono" else np.stack([lo, ro])


def stream_values(trace):
    """The dialnorm and the downmix values of a syntax trace's first frames, the frame at which
    one of them first changes (None where none does), and the number of frames."""
    first = {}
    change = None
    frames = 0
    for line in Path(trace).read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 6:
            continue
        frame = int(fields[0])
        frames = max(frames, frame + 1)
        name, value = fields[5], int(fields[4])
        if name != "dialnorm_bits" and name not in DOWNMIX_FIELDS:
            continue
        if change is None and first.setdefault(name, value) != value:
            change = frame
    return first, change, frames


def decodable(leg):
    return (leg.get("codec_mode") in CODEC_MODES and leg.get("frame_rate_index") in RESAMPLING
            and leg.get("output_channel_layout") in LAYOUTS)


def legs_committed():
    manifest = json.loads((BASELINE_DIR / "ac4-manifest.json").read_text(encoding="utf-8"))
    return [(name, BASELINE_DIR / name / "dee.ac4")
            for name, leg in sorted(manifest["legs"].items()) if decodable(leg)]


def legs_gold(gold):
    manifest = json.loads((gold / "gold-manifest.json").read_text(encoding="utf-8"))
    return [(name, gold / "streams" / name / "dee.ac4")
            for name, leg in sorted(manifest["legs"].items()) if decodable(leg)]


def decode(cli, stream, out_wav, *options):
    command = [str(cli), "decode", str(stream), str(out_wav), *options]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{stream}: ac3cli decode {' '.join(options)} failed "
                         f"({result.returncode}):\n{result.stdout}{result.stderr}")
    samples, _ = read_wav(out_wav)
    return samples


def residual_db(expected, got):
    """How far under `expected` what separates it from `got` is, in dB; -inf where nothing does."""
    error = float(np.sum((got - expected) ** 2))
    if error == 0.0:
        return float("-inf")
    return 10.0 * np.log10(error / max(float(np.sum(expected**2)), 1e-300))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path, help="the ac3cli to decode with")
    parser.add_argument("--gold", type=Path, help="check G0's local gold set in this directory")
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    parser.add_argument("--only", nargs="+", metavar="LEG", help="check only these legs")
    args = parser.parse_args()

    legs = legs_gold(args.gold) if args.gold else legs_committed()
    if args.only:
        legs = [leg for leg in legs if leg[0] in args.only]
    if not legs:
        raise SystemExit("no leg to check")
    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        for name, stream in legs:
            trace = work / f"{name}.trace"
            coded = decode(args.cli, stream, work / f"{name}.wav", f"syntax-trace={trace}")
            values, change, frames = stream_values(trace)
            if "dialnorm_bits" not in values:
                failures.append(f"{name}: no dialnorm_bits in its syntax trace")
                continue
            # At least SKIP_FRAMES frames at every rate: none is longer than 2 048 samples. Where
            # the stream's values change (DEE's immersive stereo at 24 and 25 fps sends another
            # dialnorm in its last frame), the checks stop SKIP_FRAMES frames before it.
            skip = SKIP_FRAMES * 2048
            end = coded.shape[0]
            if change is not None:
                end = change * coded.shape[0] // frames - skip
            if end <= 2 * skip:
                failures.append(f"{name}: its values change at frame {change}, too soon to check")
                continue
            dialnorm = -values["dialnorm_bits"] / 4.0
            cells = [] if change is None else [f"(to frame {change})"]
            for lout in OUTPUT_LEVELS:
                out = decode(args.cli, stream, work / f"{name}-level.wav", f"output-level={lout:g}",
                             "drcmode=off")
                a, b = coded[skip:end], out[skip:end]
                gain_db = db(float(np.sum(a * b) / np.sum(a * a)))
                expected_db = db(2.0 ** ((lout - dialnorm) / 6.0))
                left = residual_db(from_db(gain_db) * a, b)
                cells.append(f"{lout:g}: {gain_db - expected_db:+.4f} dB, {left:.0f} dB")
                if abs(gain_db - expected_db) > LEVEL_TOLERANCE_DB:
                    failures.append(f"{name}: output-level={lout:g} gives {gain_db:+.3f} dB, "
                                    f"2^((Lout - dialnorm) / 6) {expected_db:+.3f} dB")
                if left > LEVEL_RESIDUAL_DB:
                    failures.append(f"{name}: output-level={lout:g} leaves {left:.1f} dB beside "
                                    "its gain")
            print(f"{name:<40} dialnorm {dialnorm:6.2f}  level {'  '.join(cells)}", flush=True)
            if coded.shape[1] != 6:
                continue
            downmix = values.copy()
            cells = []
            for target, options in (("stereo", ("channels=2",)), ("loro", ("downmix=loro",)),
                                    ("ltrt", ("downmix=ltrt",)), ("mono", ("downmix=mono",))):
                out = decode(args.cli, stream, work / f"{name}-{target}.wav", *options)
                matrix = stereo_matrix(downmix, target)
                if out.shape[1] != matrix.shape[0]:
                    failures.append(f"{name}: {target} gives {out.shape[1]} channels, not "
                                    f"{matrix.shape[0]}")
                    continue
                expected = coded @ matrix.T
                left = residual_db(expected[skip:end], out[skip:end])
                cells.append(f"{target} {left:.0f} dB")
                if left > DOWNMIX_RESIDUAL_DB:
                    failures.append(f"{name}: {target} leaves {left:.1f} dB beside clause 6.2.17's "
                                    "matrix with the stream's values")
            shown = {k: downmix[k] for k in DOWNMIX_FIELDS if k in downmix}
            print(f"{'':<40} downmix {'  '.join(cells)}  {shown}", flush=True)
    for failure in failures:
        print(f"FAIL {failure}")
    print(f"{len(legs)} legs, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
