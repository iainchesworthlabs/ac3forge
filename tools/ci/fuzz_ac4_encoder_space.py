"""Property/fuzz harness over the AC-4 encoder's input space.

The question tools/ci/fuzz_encoder_space.py asks of AC-3's `encode` and
tools/ci/fuzz_eac3_encoder_space.py of `eac3-encode`, asked of `ac4-encode`:
driven across its configuration space by adversarial but valid audio, does the
encoder ever write a stream a reader disagrees with, or refuse, crash or
misframe something it should have encoded? planning/ac4.md, the encoder's
ladder, item 7.

The PCM comes from the AC-3 harness's generator, imported rather than copied:
its per-block plan, the `cliff` profile and the correlation modes serve every
codec. The configuration space is this encoder's: mono or stereo, 48 or
44.1 kHz, a constant rate from 8 kbps up, the codec mode the rate picks or
either one forced, the experimental A-SPX tools, dialnorm, and raw or MP4
output.

Each case is held to:

  traces   the encoder's own trace of what it wrote (`ac4-encode ...
           syntax-trace=`), the decoder's trace of what it read (`decode ...
           syntax-trace=`) and tools/references/ac4_syntax.py's trace of the
           stream are the same, record for record, and the Python parser reads
           every substream to its end with its invariants holding - the
           ladder's item 1;
  framing  the stream's sync frames, walked here from the sync word and
           frame_size alone, tile the file exactly, each with its CRC (Part 2
           Annex G, computed here), and FFmpeg's raw AC-4 demuxer finds as many
           packets as the encoder wrote frames, each the size of its raw frame;
           the MP4 output's track is one AC-4 stream FFmpeg's mov demuxer reads
           with that many samples - item 3's FFmpeg half;
  decode   `ac3cli decode` reads it to PCM: every frame, at the input's
           channel count and sample rate.

A configuration outside the encoder's range - a rate below 8 or above 3000
kbps - must be refused with the encoder's own message, and is drawn on
purpose now and then. --check-envelope re-measures that range.

Every case is a pure function of one 64-bit case seed, printed with any
failure; --replay reruns it, and REGRESSION_SEEDS holds the seeds that ever
failed, replayed by --regressions.

Usage (repo root, after building):
  python tools/ci/fuzz_ac4_encoder_space.py --cli build/dev/bin/ac3cli.exe --cases 50
  python tools/ci/fuzz_ac4_encoder_space.py --seconds 120        # bounded, for CI
  python tools/ci/fuzz_ac4_encoder_space.py --check-envelope     # the accepted rates
  python tools/ci/fuzz_ac4_encoder_space.py --replay 1234567890  # one exact case
"""

import argparse
import concurrent.futures
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(REPO / "tools" / "references"))

import ac4_syntax  # noqa: E402  (the paths above have to come first)
import fuzz_encoder_space as ac3space  # noqa: E402

FRAME = 2048  # samples per frame at frame_rate_index 13
BLOCK = ac3space.BLOCK  # the generator's block, 256 samples
SAMPLE_RATES = [48000, 44100]
# The rates drawn, weighted low, where the frame's side information is under
# the most pressure; any whole rate in between is drawn too.
RATES = [8, 12, 16, 24, 32, 48, 64, 96, 128, 144, 192, 256, 320, 384, 448, 512, 640, 768, 1024]
LOWEST_KBPS = 8
HIGHEST_KBPS = 3000
# The encoder's delay (a frame and a half) and the decoder's at frame_rate_index 13, which the
# encoder's last frame covers: d_pcm (Part 1 Table 188), the QMF banks' 577 samples and six QMF
# slots.
LAG = 3072 + 352 + 577 + 6 * 64

# Refusals a case may end in, by the text ac3cli prints for each.
REFUSALS = {
    "rate out of range": "the configuration is not one this encoder writes",
    # dialnorm=auto on input BS.1770's gates leave nothing of, which the
    # generator's sparse profiles draw: the encoder commands all refuse it so.
    "nothing to measure": "no audio above the -70 LKFS absolute gate",
}

# Case seeds that ever failed, with why; --regressions replays them.
REGRESSION_SEEDS = {}


@dataclass
class Case:
    seed: int
    channels: int
    sample_rate: int
    bitrate: int
    blocks: int
    pcm16: bool
    audio_profile: str
    correlation: str
    mp4: bool
    options: list = field(default_factory=list)

    @property
    def in_range(self):
        return LOWEST_KBPS <= self.bitrate <= HIGHEST_KBPS


@dataclass
class Result:
    case: Case
    status: str  # "ok" | "refused" | "fail"
    stage: str = ""
    detail: str = ""


def draw_case(seed):
    rng = random.Random(seed)
    roll = rng.random()
    if roll < 0.04:
        bitrate = rng.choice([1, 4, 7, 3001, 4000])  # outside the range: must be refused
    elif roll < 0.25:
        bitrate = rng.randint(LOWEST_KBPS, 1536)
    else:
        weights = [1.0 / (1.0 + 0.3 * i) for i in range(len(RATES))]
        bitrate = rng.choices(RATES, weights=weights, k=1)[0]
    options = []
    roll = rng.random()
    if roll < 0.2:
        options.append("dialnorm=auto")
    elif roll < 0.5:
        options.append(f"dialnorm={rng.randint(1, 31)}")
    # The rate picks the codec mode (ASPX below 96 kbps a channel); now and
    # then either is forced, and the experimental A-SPX tools asked for.
    roll = rng.random()
    if roll < 0.15:
        options.append("codec-mode=simple")
    elif roll < 0.3:
        options.append("codec-mode=aspx")
    if rng.random() < 0.25:
        tools = rng.choice(
            [
                "aspx-balance",
                "aspx-varvar",
                "aspx-interleave",
                "aspx-balance,aspx-varvar,aspx-interleave",
            ]
        )
        options.append(f"experimental={tools}")
    sample_rate = rng.choice(SAMPLE_RATES)
    # Two to ten frames of input: several frames, never one, so that block
    # switching and the stereo choice change between frames. dialnorm=auto
    # needs more: BS.1770's gating blocks are 400 ms long.
    shortest = 2 * FRAME // BLOCK
    if "dialnorm=auto" in options:
        shortest = -(-sample_rate * 3 // 5 // BLOCK)
    return Case(
        seed=seed,
        channels=rng.choice([1, 2, 2]),
        sample_rate=sample_rate,
        bitrate=bitrate,
        blocks=rng.randint(shortest, max(shortest, 10 * FRAME // BLOCK)),
        pcm16=rng.random() < 0.5,
        audio_profile=rng.choice([*ac3space.AUDIO_PROFILES, "mixed"]),
        correlation=rng.choice(["independent", "identical", "pairs", "inverted"]),
        mp4=rng.random() < 0.3,
        options=options,
    )


def describe(case):
    return (
        f"seed {case.seed}: {case.channels} ch, {case.sample_rate} Hz, {case.bitrate} kbps, "
        f"{case.blocks * BLOCK} samples ({case.audio_profile}, {case.correlation}, "
        f"{'pcm16' if case.pcm16 else 'float'})"
        f"{' ' + ' '.join(case.options) if case.options else ''}"
        f"{', MP4 too' if case.mp4 else ''}"
    )


def _run(argv):
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, check=False)


# --- independent checks ------------------------------------------------------------------------


def crc16(data):
    """Part 2 Annex G.4.2's crc_word: CRC-16, polynomial 0x8005, initial 0, MSB first."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def sync_frames(data):
    """The raw frames of a stream of ac4_syncframe()s, or an error naming where the walk stopped.

    Only the sync word and frame_size are read, and each 0xAC41 frame's crc_word is checked over
    frame_size and the raw frame (Part 2 Annex G.3.1 and G.4.2)."""
    frames = []
    pos = 0
    while pos < len(data):
        if pos + 4 > len(data):
            return None, f"{len(data) - pos} bytes left at {pos}, too few for a sync frame header"
        sync = int.from_bytes(data[pos : pos + 2], "big")
        if sync not in (0xAC40, 0xAC41):
            return None, f"sync word 0x{sync:04X} at byte {pos}"
        size = int.from_bytes(data[pos + 2 : pos + 4], "big")
        header = 4
        if size == 0xFFFF:
            size = int.from_bytes(data[pos + 4 : pos + 7], "big")
            header = 7
        end = pos + header + size + (2 if sync == 0xAC41 else 0)
        if end > len(data):
            return None, f"the frame at byte {pos} runs {end - len(data)} bytes past the end"
        raw = data[pos + header : pos + header + size]
        if sync == 0xAC41:
            stored = int.from_bytes(data[end - 2 : end], "big")
            computed = crc16(data[pos + 2 : pos + header] + raw)
            if stored != computed:
                return (
                    None,
                    f"crc_word 0x{stored:04X} at frame {len(frames)}, computed 0x{computed:04X}",
                )
        frames.append(raw)
        pos = end
    return frames, ""


def read_trace(path):
    """(frame, substream, offset, width, value) per line of a syntax-trace= file."""
    records = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        records.append(tuple(int(x) for x in parts[:5]))
    return records


def python_trace(data):
    diagnostics = []
    records = []
    for frame, substream, _kind, recs in ac4_syntax.walk_stream(data, True, diagnostics):
        records.extend((frame, substream, pos, width, value) for pos, width, value, _ in recs)
    return records, diagnostics


def first_difference(a, b):
    for i, (x, y) in enumerate(zip(a, b, strict=False)):
        if x != y:
            return f"record {i}: {x} against {y}"
    if len(a) != len(b):
        return f"{len(a)} records against {len(b)}"
    return ""


def ffprobe_json(ffprobe, args, path):
    result = _run([ffprobe, "-v", "error", *args, "-of", "json", path])
    if result.returncode != 0:
        return None, result.stderr.strip()
    return json.loads(result.stdout), ""


# --- one case ----------------------------------------------------------------------------------


def run_case(cli, ffprobe, case, workdir):
    tmp = Path(tempfile.mkdtemp(prefix=f"case{case.seed}_", dir=workdir))
    try:
        return _run_case(cli, ffprobe, case, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _run_case(cli, ffprobe, case, tmp):
    rng = random.Random(case.seed ^ 0x5EED)
    pcm = ac3space.generate_pcm(
        rng, case.channels, case.blocks, case.sample_rate, case.audio_profile, case.correlation
    )
    wav = tmp / "in.wav"
    ac3space.write_wav(wav, pcm, case.sample_rate, case.pcm16)
    stream = tmp / "out.ac4"
    encoded = _run(
        [
            cli,
            "ac4-encode",
            wav,
            stream,
            case.bitrate,
            *case.options,
            f"syntax-trace={tmp / 'enc.tsv'}",
        ]
    )
    if not case.in_range:
        if encoded.returncode != 0 and REFUSALS["rate out of range"] in encoded.stderr:
            return Result(case, "refused", "encode", "rate out of range")
        return Result(
            case,
            "fail",
            "encode",
            f"a rate out of range was not refused (exit {encoded.returncode}): "
            f"{encoded.stderr.strip()}",
        )
    if encoded.returncode != 0:
        if "dialnorm=auto" in case.options and REFUSALS["nothing to measure"] in encoded.stderr:
            return Result(case, "refused", "encode", "nothing to measure")
        return Result(
            case, "fail", "encode", f"exit {encoded.returncode}: {encoded.stderr.strip()}"
        )
    match = re.search(r"encoded (\d+) AC-4 frames", encoded.stdout)
    if match is None:
        return Result(case, "fail", "encode", f"no frame count in: {encoded.stdout.strip()}")
    count = int(match.group(1))
    expected_frames = -(-(case.blocks * BLOCK + LAG) // FRAME)
    if count != expected_frames:
        return Result(case, "fail", "encode", f"{count} frames, expected {expected_frames}")

    data = stream.read_bytes()
    raw_frames, why = sync_frames(data)
    if raw_frames is None:
        return Result(case, "fail", "framing", why)
    if len(raw_frames) != count:
        return Result(
            case, "fail", "framing", f"{len(raw_frames)} sync frames, the encoder said {count}"
        )

    # The three traces.
    decoded = _run([cli, "decode", stream, tmp / "out.wav", f"syntax-trace={tmp / 'dec.tsv'}"])
    if decoded.returncode != 0:
        return Result(
            case, "fail", "decode", f"exit {decoded.returncode}: {decoded.stderr.strip()}"
        )
    written = read_trace(tmp / "enc.tsv")
    read = read_trace(tmp / "dec.tsv")
    parsed, diagnostics = python_trace(data)
    if diagnostics:
        return Result(case, "fail", "traces", f"ac4_syntax.py: {diagnostics[0]}")
    for label, other in (("the decoder's", read), ("ac4_syntax.py's", parsed)):
        difference = first_difference(written, other)
        if difference:
            return Result(
                case, "fail", "traces", f"the encoder's trace and {label} differ at {difference}"
            )

    # The decode.
    samples, rate = read_wav_shape(tmp / "out.wav")
    if rate != case.sample_rate or samples[0] != case.channels or samples[1] != count * FRAME:
        return Result(
            case,
            "fail",
            "decode",
            f"decoded {samples[0]} channels of {samples[1]} samples at {rate} Hz, expected "
            f"{case.channels} of {count * FRAME} at {case.sample_rate}",
        )

    # FFmpeg's framing.
    if ffprobe is not None:
        probed, why = ffprobe_json(ffprobe, ["-f", "ac4", "-show_entries", "packet=size"], stream)
        if probed is None:
            return Result(case, "fail", "ffprobe", why)
        sizes = [int(p["size"]) for p in probed.get("packets", [])]
        if sizes != [len(f) for f in raw_frames]:
            return Result(
                case,
                "fail",
                "ffprobe",
                f"{len(sizes)} packets, the first sizes {sizes[:4]}, against {count} frames of "
                f"{[len(f) for f in raw_frames[:4]]}",
            )
        if case.mp4:
            mp4 = tmp / "out.mp4"
            muxed = _run([cli, "ac4-encode", wav, mp4, case.bitrate, *case.options])
            if muxed.returncode != 0:
                return Result(
                    case, "fail", "mp4", f"exit {muxed.returncode}: {muxed.stderr.strip()}"
                )
            probed, why = ffprobe_json(
                ffprobe,
                [
                    "-count_packets",
                    "-show_entries",
                    "stream=codec_name,codec_tag_string,sample_rate,nb_read_packets",
                ],
                mp4,
            )
            if probed is None:
                return Result(case, "fail", "mp4", why)
            streams = probed.get("streams", [])
            wanted = {
                "codec_name": "ac4",
                "codec_tag_string": "ac-4",
                "sample_rate": str(case.sample_rate),
                "nb_read_packets": str(count),
            }
            if len(streams) != 1 or any(streams[0].get(k) != v for k, v in wanted.items()):
                return Result(
                    case,
                    "fail",
                    "mp4",
                    f"ffprobe read {streams}, expected one stream with {wanted}",
                )
    return Result(case, "ok")


def read_wav_shape(path):
    """((channels, frames), sample rate) of a WAV, from its fmt and data chunks."""
    blob = Path(path).read_bytes()
    pos = 12
    channels = rate = block = 0
    frames = 0
    while pos + 8 <= len(blob):
        chunk, size = blob[pos : pos + 4], int.from_bytes(blob[pos + 4 : pos + 8], "little")
        if chunk == b"fmt ":
            channels = int.from_bytes(blob[pos + 10 : pos + 12], "little")
            rate = int.from_bytes(blob[pos + 12 : pos + 16], "little")
            block = int.from_bytes(blob[pos + 20 : pos + 22], "little")
        elif chunk == b"data" and block:
            frames = size // block
        pos += 8 + size + (size & 1)
    return (channels, frames), rate


# --- the envelope ------------------------------------------------------------------------------


def check_envelope(cli):
    """The rates the encoder takes, at both sample rates and channel counts: the lowest accepted and
    the one below it refused, the highest accepted and the one above it refused."""
    failures = 0
    with tempfile.TemporaryDirectory(prefix="ac4envelope_") as tmp:
        for channels in (1, 2):
            for rate in SAMPLE_RATES:
                wav = Path(tmp) / f"{channels}-{rate}.wav"
                ac3space.write_wav(wav, [[0.0] * (4 * FRAME) for _ in range(channels)], rate, False)
                for kbps, accepted in (
                    (LOWEST_KBPS - 1, False),
                    (LOWEST_KBPS, True),
                    (HIGHEST_KBPS, True),
                    (HIGHEST_KBPS + 1, False),
                ):
                    result = _run([cli, "ac4-encode", wav, Path(tmp) / "out.ac4", kbps, "quiet"])
                    ok = (result.returncode == 0) == accepted
                    expected = "accepted" if accepted else "refused"
                    if not accepted:
                        ok = ok and REFUSALS["rate out of range"] in result.stderr
                    print(
                        f"  {channels} ch {rate} Hz {kbps:5d} kbps: "
                        f"{'accepted' if result.returncode == 0 else 'refused'}"
                        f"{'' if ok else '  <- expected ' + expected}"
                    )
                    failures += not ok
    return 1 if failures else 0


# --- main --------------------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--cli",
        default=os.environ.get("AC3CLI", "build/dev/bin/ac3cli.exe"),
        help="path to ac3cli (or set AC3CLI)",
    )
    parser.add_argument("--ffprobe", default="ffprobe", help="path to ffprobe")
    parser.add_argument("--no-ffmpeg", action="store_true", help="skip FFmpeg's framing checks")
    parser.add_argument(
        "--seed", type=int, default=None, help="master seed; drawn and printed if omitted"
    )
    parser.add_argument("--cases", type=int, default=None, help="run exactly this many cases")
    parser.add_argument(
        "--seconds", type=float, default=None, help="run until this many seconds have passed"
    )
    parser.add_argument(
        "--replay", type=int, default=None, help="run the one case with this case seed"
    )
    parser.add_argument(
        "--regressions", action="store_true", help="replay every REGRESSION_SEEDS case"
    )
    parser.add_argument(
        "--check-envelope", action="store_true", help="re-measure the accepted rates"
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--max-failures", type=int, default=10)
    args = parser.parse_args()

    cli = args.cli if Path(args.cli).is_absolute() else str((REPO / args.cli).resolve())
    if not Path(cli).exists():
        raise SystemExit(f"ac3cli not found at {cli} - build first, or pass --cli")
    if args.check_envelope:
        print("acceptance envelope - the rates ac4-encode takes")
        sys.exit(check_envelope(cli))

    ffprobe = None
    if not args.no_ffmpeg:
        ffprobe = shutil.which(args.ffprobe)
        if ffprobe is None:
            raise SystemExit(
                f"ffprobe not found ('{args.ffprobe}'); pass --no-ffmpeg to skip FFmpeg's checks"
            )

    def one(seed):
        case = draw_case(seed)
        print(describe(case))
        with tempfile.TemporaryDirectory(prefix="ac4space_") as workdir:
            result = run_case(cli, ffprobe, case, workdir)
        print(
            f"  {result.status}" + (f" ({result.stage}): {result.detail}" if result.detail else "")
        )
        return result.status != "fail"

    if args.regressions:
        if not REGRESSION_SEEDS:
            print("no regression seeds recorded")
        failed = sum(not one(seed) for seed in REGRESSION_SEEDS)
        sys.exit(1 if failed else 0)
    if args.replay is not None:
        sys.exit(0 if one(args.replay) else 1)

    if args.cases is None and args.seconds is None:
        args.cases = 50
    master = args.seed if args.seed is not None else random.SystemRandom().randrange(2**63)
    print(f"AC-4 encoder-space fuzz: cli={cli} ffprobe={ffprobe or 'off'}")
    print(
        f"master seed {master}"
        + (f", {args.cases} cases" if args.cases is not None else f", {args.seconds:g}s budget")
        + f", {args.jobs} jobs"
    )
    print("(every failure prints its case seed; --replay <seed> reruns it)\n")

    started = time.monotonic()
    counts = {"ok": 0, "refused": 0, "fail": 0}
    failures = []
    index = 0

    def budget_left():
        if args.cases is not None:
            return index < args.cases
        return (time.monotonic() - started) < args.seconds

    with (
        tempfile.TemporaryDirectory(prefix="ac4space_") as workdir,
        concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool,
    ):
        pending = set()
        while (budget_left() or pending) and len(failures) < args.max_failures:
            while budget_left() and len(pending) < args.jobs * 2:
                case = draw_case(ac3space.case_seed(master, index))
                index += 1
                pending.add(pool.submit(run_case, cli, ffprobe, case, workdir))
            if not pending:
                break
            done, pending = concurrent.futures.wait(
                pending, return_when=concurrent.futures.FIRST_COMPLETED
            )
            for future in done:
                result = future.result()
                counts[result.status] += 1
                if result.status == "fail":
                    failures.append(result)
                    print(f"FAIL [{result.stage}] {describe(result.case)}")
                    print(f"  {result.detail}")
                    print(
                        "  replay: python tools/ci/fuzz_ac4_encoder_space.py "
                        f"--replay {result.case.seed}\n"
                    )

    total = sum(counts.values())
    print(
        f"{total} cases in {time.monotonic() - started:.1f}s: {counts['ok']} encoded, read "
        f"and decoded cleanly, {counts['refused']} refused (a rate out of range, or no "
        f"loudness for dialnorm=auto), {counts['fail']} failed"
    )
    if counts["fail"]:
        sys.exit(1)
    if total == 0 or counts["ok"] < total * 0.5:
        print("too few cases encoded to call this a pass")
        sys.exit(1)


if __name__ == "__main__":
    main()
