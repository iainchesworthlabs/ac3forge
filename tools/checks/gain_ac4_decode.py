"""Check the gains of ac3cli's AC-4 output processing on DEE's and the encoder's streams against
Part 1's formulas.

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

  dialogue enhancement
           on the encoder's streams (--encoder), dialogue-enhancement=3, 6 and 12: each channel
           the stream marks as carrying dialogue alone, whose parameters are 1 in every band, is
           the coded output raised by the gain or the stream's cap where that is lower (clause
           5.7.8, 1 + g p with g = 10^(G / 20) - 1), with the Mid method both of a pair carrying
           the same tone; the other channels are the coded output. To 0.01 dB, and what the gain
           leaves is 80 dB under the output.

All skip the first three frames, where the stream's values have not yet reached the QMF domain,
and stop three frames before the first frame whose values differ from the first frame's: DEE's
immersive stereo at 24 and 25 fps sends a dialnorm of -24 dBFS in its last frame. DRC's curves and
dialogue enhancement's gains are held on known input by tests/ac4dec/test_ac4dec_drc.cpp and
test_ac4dec_de.cpp; this script reads the gains from the stream, as those tests cannot.

The committed legs (tests/golden/external-baseline/) are checked by default. --gold DIR checks
phase G0's local gold set in DIR (DIR/streams/<leg>/dee.ac4, DIR/gold-manifest.json), which never
runs in CI. --encoder checks streams `ac3cli ac4-encode` writes from tones here, a leg for each
metadata option the output processing reads (ENCODER_LEGS), at several frame rates (planning/
ac4.md, phase E5).

--engine RENDER decodes through Hearth's engine instead (planning/ac4.md, phase I2): RENDER is
ac3hearth-render (apps/hearth/render), which plays a stream through the player, session and stream
decoder the window uses, and each of ac3cli's options above becomes the Decoder page's setting for
it (engine_settings()). The coded output is the engine's too, on a layout of the stream's own
channels in ac3cli's order; the stream's values still come from ac3cli's syntax trace, and the
formulas and tolerances are the same.

Usage:
    python tools/checks/gain_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli
    python tools/checks/gain_ac4_decode.py --cli ac3cli.exe --gold D:/ac3bld/ac4-gold
    python tools/checks/gain_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli --encoder
    python tools/checks/gain_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli \
        --engine build/config-linux-llvm/bin/ac3hearth-render
"""

import argparse
import json
import struct
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
DE_GAINS = (3.0, 6.0, 12.0)
DE_TOLERANCE_DB = 0.01
DE_RESIDUAL_DB = -80.0
# The encoder's legs: name, channels, rate, ac4-encode's options, and the channels the stream marks
# as carrying dialogue alone (dialogue-channels=), as output channel indices, or "mid" where the
# Mid of L and R is raised.
ENCODER_LEGS = (
    ("enc-2.0-192-dialnorm31", 2, 192, ("dialnorm=31",), None),
    ("enc-2.0-128-29.97-dialnorm24.5", 2, 128, ("frame-rate=29.97", "dialnorm=24.5"), None),
    ("enc-1.0-64-120-dialnorm17.25", 1, 64, ("frame-rate=120", "dialnorm=17.25"), None),
    ("enc-5.1-384-loro", 6, 384, ("dialnorm=27", "lorocmixlev=-1.5", "lorosurmixlev=-4.5",
                                  "lfemix=-4.5", "dmixmod=loro", "loro-correction=-2"), None),
    ("enc-5.1-384-ltrt", 6, 384, ("ltrtcmixlev=-3", "ltrtsurmixlev=-6", "dmixmod=ltrt",
                                  "ltrt-correction=1.5", "lfemix=+2.5"), None),
    ("enc-5.1-128-25-pl2", 6, 128, ("frame-rate=25", "dmixmod=pl2", "lorocmixlev=0",
                                    "lorosurmixlev=off"), None),
    ("enc-5.0-192-none", 5, 192, ("dmixmod=none", "cmixlev=+3", "surmixlev=0"), None),
    ("enc-1.0-96-de-c", 1, 96, ("dialogue-channels=c", "dialogue-max-gain=12"), (0,)),
    ("enc-2.0-192-de-lr", 2, 192, ("dialogue-channels=l,r", "dialogue-max-gain=9"), (0, 1)),
    ("enc-2.0-192-de-mid", 2, 192, ("dialogue-channels=l,r", "dialogue-method=mid",
                                    "dialogue-max-gain=6"), "mid"),
    ("enc-5.1-384-50-de-c", 6, 384, ("frame-rate=50", "dialogue-channels=c",
                                     "dialogue-max-gain=9"), (C,)),
)
# Each channel's tone, under Table 173's last dialogue enhancement band (subband 41, 15.4 kHz) and
# the LFE's under 140 Hz, 20 dB under full scale, for four seconds.
TONES_HZ = (440.0, 620.0, 800.0, 90.0, 1030.0, 1270.0)
ENCODER_SECONDS = 4
# --engine: the layout of a stream's own channels, in the order ac3cli decode writes them.
ENGINE_LAYOUTS = {1: "1.0", 2: "2.0", 5: "L,R,C,Ls,Rs", 6: "L,R,C,LFE,Ls,Rs"}


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
        if name not in ("dialnorm_bits", "de_max_gain") and name not in DOWNMIX_FIELDS:
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


def write_wav_f32(path, samples, rate):
    """An IEEE float WAV of `samples`, shape (n, channels)."""
    data = np.asarray(samples, dtype="<f4").tobytes()
    channels = samples.shape[1]
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 3, channels, rate, rate * 4 * channels,
                                    4 * channels, 32)
    Path(path).write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


def legs_encoder(cli, work):
    """ENCODER_LEGS encoded from tones by `ac3cli ac4-encode`: (name, stream, dialogue)."""
    rate = 48000
    t = np.arange(ENCODER_SECONDS * rate) / rate
    legs = []
    for name, channels, kbps, options, dialogue in ENCODER_LEGS:
        hz = TONES_HZ if channels > 2 else (TONES_HZ[0], TONES_HZ[1])[:channels]
        if channels == 5:
            hz = (TONES_HZ[L], TONES_HZ[R], TONES_HZ[C], TONES_HZ[LS], TONES_HZ[RS])
        if dialogue == "mid":
            hz = (hz[0], hz[0])
        wav = work / f"{name}-in.wav"
        write_wav_f32(wav, np.stack([0.1 * np.sin(2.0 * np.pi * f * t) for f in hz], axis=1), rate)
        stream = work / f"{name}.ac4"
        command = [str(cli), "ac4-encode", str(wav), str(stream), str(kbps), *options, "quiet"]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise SystemExit(f"{name}: ac3cli ac4-encode failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
        legs.append((name, stream, dialogue))
    return legs


def decode(cli, stream, out_wav, *options):
    command = [str(cli), "decode", str(stream), str(out_wav), *options]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{stream}: ac3cli decode {' '.join(options)} failed "
                         f"({result.returncode}):\n{result.stdout}{result.stderr}")
    samples, _ = read_wav(out_wav)
    return samples


def engine_settings(options, layout):
    """ac3cli decode's `options` as ac3hearth-render's settings, the Decoder page's AC-4 controls
    (apps/hearth/engine/decoder_settings.hpp), on `layout` unless an option folds it."""
    settings = []
    for option in options:
        key, _, value = option.partition("=")
        if key == "output-level":
            settings += ["normalise=on", option]
        elif key == "drcmode":
            settings.append(f"drc={value}")
        elif key == "dialogue-enhancement":
            settings.append(option)
        elif key == "channels" and value == "2":
            # A stereo fold by the stream's preferred method.
            layout = "2.0"
            settings.append("preferred-downmix=on")
        elif key == "downmix" and value in ("loro", "ltrt"):
            layout = "2.0"
            settings.append(option)
        elif key == "downmix" and value == "mono":
            # A one-speaker layout, which folds to L + R of the stream's preferred downmix.
            layout = "1.0"
        else:
            raise SystemExit(f"--engine: ac3cli's {option} has no setting here")
    return [f"layout={layout}", *settings]


def render(engine, stream, out_wav, layout, *options):
    """decode()'s output through Hearth's engine: ac3hearth-render with engine_settings()."""
    command = [str(engine), str(stream), str(out_wav), *engine_settings(options, layout)]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{stream}: ac3hearth-render {' '.join(command[3:])} failed "
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
    parser.add_argument("--encoder", action="store_true",
                        help="check streams ac4-encode writes here (ENCODER_LEGS)")
    parser.add_argument("--engine", type=Path,
                        help="decode through Hearth's engine with this ac3hearth-render")
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    parser.add_argument("--only", nargs="+", metavar="LEG", help="check only these legs")
    args = parser.parse_args()

    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        if args.encoder:
            legs = legs_encoder(args.cli, work)
        else:
            legs = [(name, stream, None)
                    for name, stream in (legs_gold(args.gold) if args.gold else legs_committed())]
        if args.only:
            legs = [leg for leg in legs if leg[0] in args.only]
        if not legs:
            raise SystemExit("no leg to check")
        for name, stream, dialogue in legs:
            trace = work / f"{name}.trace"
            coded = decode(args.cli, stream, work / f"{name}.wav", f"syntax-trace={trace}")
            if args.engine:
                layout = ENGINE_LAYOUTS.get(coded.shape[1])
                if layout is None:
                    failures.append(f"{name}: no layout for its {coded.shape[1]} channels")
                    continue

                def output(path, *options, stream=stream, layout=layout):
                    return render(args.engine, stream, path, layout, *options)

                coded = output(work / f"{name}-engine.wav")
            else:

                def output(path, *options, stream=stream):
                    return decode(args.cli, stream, path, *options)

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
                out = output(work / f"{name}-level.wav", f"output-level={lout:g}", "drcmode=off")
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
            if dialogue is not None:
                cap = 3.0 * (values.get("de_max_gain", 0) + 1)
                cells = []
                for gain in DE_GAINS:
                    out = output(work / f"{name}-de.wav", f"dialogue-enhancement={gain:g}")
                    worst = 0.0
                    for c in range(coded.shape[1]):
                        a, b = coded[skip:end, c], out[skip:end, c]
                        got_db = db(float(np.sum(a * b) / np.sum(a * a)))
                        raised = c < 2 if dialogue == "mid" else c in dialogue
                        expected_db = min(gain, cap) if raised else 0.0
                        left = residual_db(from_db(got_db) * a, b)
                        worst = max(worst, abs(got_db - expected_db))
                        if abs(got_db - expected_db) > DE_TOLERANCE_DB:
                            failures.append(f"{name}: dialogue-enhancement={gain:g} gives channel "
                                            f"{c} {got_db:+.3f} dB, expected {expected_db:+.3f}")
                        if left > DE_RESIDUAL_DB:
                            failures.append(f"{name}: dialogue-enhancement={gain:g} leaves "
                                            f"{left:.1f} dB beside channel {c}'s gain")
                    cells.append(f"{gain:g}: within {worst:.4f} dB")
                print(f"{'':<40} dialogue enhancement (cap {cap:g} dB) {'  '.join(cells)}",
                      flush=True)
            if coded.shape[1] == 5:
                # 5.0: the matrix's LFE column meets silence.
                coded = np.insert(coded, LFE, 0.0, axis=1)
            elif coded.shape[1] != 6:
                continue
            downmix = values.copy()
            cells = []
            for target, options in (("stereo", ("channels=2",)), ("loro", ("downmix=loro",)),
                                    ("ltrt", ("downmix=ltrt",)), ("mono", ("downmix=mono",))):
                out = output(work / f"{name}-{target}.wav", *options)
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
