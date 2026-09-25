"""The race of the encoder's presentations against DEE's substreams (planning/ac4.md, phase E6).

DEE writes one presentation of one substream, so its side of the race is phase D7's test
multiplexer over the gold set's legs of music and effects, dialogue and associated audio, each a
DEE encode of its own source at its own rate (phases G0 and G1); the encoder's side encodes the same
sources into the same presentations, each substream at its leg's rate. `ac3tests "[.race]"`
(tests/ac4enc/test_ac4enc_presentations_race.cpp) writes both streams of each race; this decodes
every presentation of both with `ac3cli decode presentation-id=`, and scores each against its
sources' mix, the plain sum the presentations' default mixing values give: music and effects with
dialogue (presentation 1), with associated audio as well (2), and each substream alone (10 to 12).
The scores are those of tools/checks/score_ac4_encode.py's race: the gain-fitted SNR of each
channel, the log-spectral distance and, where visqol-python is installed, ViSQOL's MOS-LQO, and
each substream's rate as the stream's substream_index_table() gives it.

--librempeg PATH decodes both streams with librempeg's ffmpeg as well, run in WSL, which decodes a
presentation's first substream alone: its output is scored against the music and effects source.

It reads DEE's gold set, so it runs locally, never in CI; nothing it measures is checked.

Usage:
    python tools/checks/race_ac4_presentations.py --cli ac3cli.exe --tests ac3tests.exe
        --gold D:/ac3bld/ac4-gold [--librempeg /mnt/d/ac3bld/librempeg/install/bin/ffmpeg]
        [--work DIR]
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "ci"))
sys.path.insert(0, str(REPO / "tools" / "checks"))
sys.path.insert(0, str(REPO / "tools" / "references"))
import ac4_parse  # noqa: E402
import quality_race  # noqa: E402
import score_ac4_decode as decoding  # noqa: E402

SIDES = ("dee", "encoder")
NAMES = {"1": "M&E + dialogue", "2": "M&E + dialogue + associated", "10": "M&E alone",
         "11": "dialogue alone", "12": "associated alone"}


def run(command, env=None):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True,
                            check=False, env=env)
    if result.returncode != 0:
        raise SystemExit(f"{' '.join(str(c) for c in command)} failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    return result.stdout


def wsl_path(path):
    resolved = Path(path).resolve()
    return f"/mnt/{resolved.drive[0].lower()}{resolved.as_posix()[2:]}"


def decode(cli, stream, out_wav, presentation_id):
    run([cli, "decode", stream, out_wav, f"presentation-id={presentation_id}"])
    return decoding.read_wav(out_wav)[0]


def decode_librempeg(librempeg, stream, out_wav):
    if sys.platform == "win32":
        prefix = ["wsl", "-d", "Ubuntu-26.04", "--", librempeg]
        stream_arg, out_arg = wsl_path(stream), wsl_path(out_wav)
    else:
        prefix, stream_arg, out_arg = [librempeg], str(stream), str(out_wav)
    run([*prefix, "-hide_banner", "-loglevel", "error", "-y", "-i", stream_arg, "-acodec",
         "pcm_f32le", out_arg])
    return decoding.read_wav(out_wav)[0]


def substream_rates(stream):
    """Each audio substream's rate in kbps, by its substream_index, over the stream."""
    totals = {}
    frames = 0
    for _, _, raw, _ in ac4_parse.iter_sync_frames(Path(stream).read_bytes()):
        toc, _ = ac4_parse.parse_raw_frame(raw)
        frames += 1
        for group in toc["substream_groups"]:
            for sub in group["substreams"]:
                index = sub["info"]["substream_index"]
                totals[index] = totals.get(index, 0) + toc["substream_sizes"][index]
    seconds = frames * 2048 / 48000.0
    return [8.0 * totals[i] / seconds / 1000.0 for i in sorted(totals)]


def scores(reference, decoded):
    """(the channels' gain-fitted SNRs, the log-spectral distance, MOS or None)."""
    _, channels, ref, out = decoding.score(reference, decoded)
    lsd, _ = quality_race.spectral_scores(ref, out)
    mos = quality_race.perceptual_score(ref, out, decoding.RATE)
    return [snr for _, snr in channels], float(lsd), mos


def text(snrs, lsd, mos):
    mos_text = "  -  " if mos is None else f"{mos:.2f}"
    return f"SNR {' / '.join(f'{s:5.1f}' for s in snrs)} dB, LSD {lsd:5.2f} dB, MOS {mos_text}"


def race(args, work, name):
    config = json.loads((work / f"{name}.json").read_text(encoding="utf-8"))
    sources = [decoding.read_wav(args.gold / "sources" / f"{s}.wav")[0] for s in config["sources"]]
    rates = {side: substream_rates(work / f"{name}.{side}.ac4") for side in SIDES}
    print(f"\n{name}: substreams at {config['rates']} kbps; DEE's audio substreams "
          f"{', '.join(f'{r:.1f}' for r in rates['dee'])}, the encoder's "
          f"{', '.join(f'{r:.1f}' for r in rates['encoder'])} kbps")
    for pid, members in config["presentations"].items():
        count = min(len(sources[m]) for m in members)
        reference = sum(sources[m][:count] for m in members)
        line = f"  {pid:>2} {NAMES[pid]:<28}"
        for side in SIDES:
            stream = work / f"{name}.{side}.ac4"
            decoded = decode(args.cli, stream, work / f"{name}.{side}.{pid}.wav", pid)
            line += f"  {side}: {text(*scores(reference, decoded))}"
        print(line, flush=True)
    if args.librempeg:
        count = len(sources[0])
        for side in SIDES:
            decoded = decode_librempeg(args.librempeg, work / f"{name}.{side}.ac4",
                                       work / f"{name}.{side}.librempeg.wav")
            print(f"  librempeg on {side}'s stream, against the music and effects alone: "
                  f"{text(*scores(sources[0][:count], decoded))}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--tests", required=True, type=Path,
                        help="ac3tests, which writes the streams")
    parser.add_argument("--gold", required=True, type=Path)
    parser.add_argument("--librempeg", help="librempeg's ffmpeg, a WSL path on Windows")
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, AC4_GOLD=str(args.gold), AC4_E6_RACE=str(work))
        run([args.tests, "[.race]"], env=env)
        for config in sorted(work.glob("*.json")):
            race(args, work, config.stem)
    return 0


if __name__ == "__main__":
    np.seterr(all="ignore")
    sys.exit(main())
