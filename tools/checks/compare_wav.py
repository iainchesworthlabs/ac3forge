"""Delay-compensated SNR between two WAVs - stdlib only, no numpy.

Used by tools/checks/verify_gold_reference.sh as the L4-lite half of the gold-
reference gate: FFmpeg's decode of ac3cli's own encoder output is the
independent reference, ac3cli's own decode of the same file is what is being
checked, and this asserts they agree to within a threshold rather than just
"neither one crashed."

Deliberately stdlib-only (wave/struct/math): GitHub-hosted runners ship
Python 3 preinstalled on Windows/Linux/macOS, so this adds zero new CI
provisioning the way pulling in numpy would.

Usage:  python compare_wav.py <reference.wav> <actual.wav> [--min-snr-db N] [--max-lag-samples N]
Exit 0 if every channel's SNR >= its threshold, exit 1 (with the offending
channel and its SNR) otherwise.

--min-snr-db-per-channel is the same gate stated per channel instead of once
for all of them, and it exists because one floor across six channels is not
one gate - it is one gate on the worst channel and dead slack on the rest.
On the 5.1 fixtures here the surrounds sit 20-60 dB below the front channels
by construction (the encoder spends fewer bits there, and §7.3.4 dither in the
zero-bit bins puts two independently correct decoders further apart still), so
a floor low enough for Ls/Rs to pass leaves L/R/C/LFE 30-70 dB of room to
collapse in silently. Every floor here was derived from that channel's own
measured minimum across every CI leg and every recorded commit - see
tools/checks/verify_gold_reference.sh, which carries the vectors and the
derivation.

--json-out additionally writes a structured result (per-channel SNR, lag,
per-channel thresholds and headroom, pass/fail) for a caller that wants to
persist the numbers rather than just gate on them - see docs/quality-trend.md.
--codec-label and --bitrate-kbps are pure passthrough metadata for that file;
this script does not care what codec produced its inputs.

--max-diff-dbfs is the other way of asking the same question, for material
where the SNR form cannot answer it. SNR is a RATIO, so on a passage that is
itself near the noise floor - which several of the FFmpeg FATE excerpts
tools/checks/verify_fate_interop.py fetches are, at -93 to -100 dBFS - a
completely inaudible disagreement still scores a couple of dB and no floor
can tell that apart from a real defect. The absolute form asserts on the
difference signal directly instead: every channel's RMS difference must sit
below the given dBFS. Both may be given, and both are then enforced.
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

# Not the stdlib `wave` module: it hard-rejects WAVE_FORMAT_IEEE_FLOAT
# (format tag 3, "unknown format: 3") with no opt-out, and ac3cli's own
# decode writes exactly that (see write_wav_f32 in src/forge/src/io/wav.cpp) -
# confirmed locally, this is not a hypothetical. A small manual RIFF/WAVE
# walk (mirroring that same C++ reader) handles PCM16 and float32 uniformly,
# plus WAVE_FORMAT_EXTENSIBLE, without needing two code paths.


def read_channels(path: Path) -> tuple[list[list[float]], int]:
    data = path.read_bytes()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a RIFF/WAVE file")

    fmt_tag = channels = bits = 0
    rate = 0
    payload = b""
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        chunk_size = struct.unpack_from("<I", data, pos + 4)[0]
        body_at = pos + 8
        if chunk_id == b"fmt ":
            fmt_tag, channels = struct.unpack_from("<HH", data, body_at)
            rate = struct.unpack_from("<I", data, body_at + 4)[0]
            bits = struct.unpack_from("<H", data, body_at + 14)[0]
            if fmt_tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: real tag is in the SubFormat GUID
                fmt_tag = struct.unpack_from("<H", data, body_at + 24)[0]
        elif chunk_id == b"data":
            payload = data[body_at:body_at + chunk_size]
        pos = body_at + chunk_size + (chunk_size & 1)  # chunks are word-aligned

    if channels == 0 or rate == 0 or not payload:
        raise SystemExit(f"{path}: missing fmt/data chunk")

    if fmt_tag == 1 and bits == 16:
        sample_fmt, scale = "h", 32768.0
    elif fmt_tag == 3 and bits == 32:
        sample_fmt, scale = "f", 1.0
    else:
        raise SystemExit(f"{path}: unsupported format tag {fmt_tag}/{bits}-bit")

    sample_size = struct.calcsize(sample_fmt)
    frame_bytes = channels * sample_size
    frames = len(payload) // frame_bytes
    interleaved = struct.unpack(f"<{frames * channels}{sample_fmt}",
                                 payload[:frames * frame_bytes])
    out = [[0.0] * frames for _ in range(channels)]
    for i in range(frames):
        base = i * channels
        for c in range(channels):
            out[c][i] = interleaved[base + c] / scale
    return out, rate


# strict=True at every zip in this file, not the length-tolerant default: this
# script is the gold-reference gate's comparator, and a silent truncation to
# the shorter sequence is exactly how a comparison oracle reports a pass it
# never checked. Every call site here already guarantees equal lengths -
# best_lag skips a short probe, align() trims both to one common n, and the
# channel-count mismatch is rejected above - so strict= can say so out loud.
def dot(a: list[float], b: list[float]) -> float:
    return sum(x * y for x, y in zip(a, b, strict=True))


def best_lag(ref_mono: list[float], act_mono: list[float], max_lag: int, probe_len: int) -> int:
    """Cross-correlate a short prefix to find how many samples `act` leads or
    lags `ref` by - decoder priming/lookahead can shift the two by a handful
    of samples even when decoding the identical bitstream, and a search here
    is cheap insurance against penalizing that as a fidelity loss."""
    n = min(probe_len, len(ref_mono), len(act_mono))
    ref_probe = ref_mono[:n]
    best = (0, -1.0)
    for lag in range(-max_lag, max_lag + 1):
        act_probe = act_mono[lag:lag + n] if lag >= 0 else ([0.0] * -lag + act_mono)[:n]
        if len(act_probe) < n:
            continue
        score = dot(ref_probe, act_probe)
        if score > best[1]:
            best = (lag, score)
    return best[0]


def align(a: list[float], b: list[float], lag: int) -> tuple[list[float], list[float]]:
    """Shift `b` by `lag` samples relative to `a`, then trim both to the
    overlapping region."""
    if lag >= 0:
        b = b[lag:]
    else:
        a = a[-lag:]
    n = min(len(a), len(b))
    return a[:n], b[:n]


def diff_rms_dbfs(reference: list[float], actual: list[float]) -> float:
    """RMS of the difference signal, in dBFS - an absolute error, unlike
    snr_db's ratio. -inf (a bit-exact match) is reported as-is and clamped
    only on the way into JSON, same as snr_db's +inf."""
    if not reference:
        return -math.inf
    power = sum((r - a) ** 2 for r, a in zip(reference, actual, strict=True)) / len(reference)
    if power <= 1e-30:
        return -math.inf
    return 10.0 * math.log10(power)


def snr_db(reference: list[float], actual: list[float]) -> float:
    signal_power = sum(v * v for v in reference)
    noise_power = sum((r - a) ** 2 for r, a in zip(reference, actual, strict=True))
    if noise_power <= 1e-20:
        return math.inf
    if signal_power <= 1e-20:
        return -math.inf
    return 10.0 * math.log10(signal_power / noise_power)


# WAV channel order ac3::io::ac3_layout_for(6) expects - see
# tools/generators/gen_gold_reference_wav.py, and docs/quality-trend.md's own
# copy of this list for the rendered table. Naming the channels in this
# script's output rather than only numbering them is what lets a CI log say
# which channel moved without the reader holding the layout in their head.
CHANNEL_LABELS = {
    1: ["M"],
    2: ["L", "R"],
    6: ["L", "R", "C", "LFE", "Ls", "Rs"],
}


def channel_labels(channels: int) -> list[str]:
    """Falls back to a plain index label for any layout not named above,
    rather than guessing at a mapping - same rule docs/quality-trend.md's
    channelLabel() follows for the same reason."""
    return CHANNEL_LABELS.get(channels, [f"ch{i}" for i in range(channels)])


def resolve_thresholds(per_channel: str | None, scalar: float, channels: int) -> list[float]:
    """The per-channel floor vector this run gates on.

    Absent --min-snr-db-per-channel, that is the scalar floor repeated - which
    is exactly what every caller got before per-channel floors existed, so an
    unconverted call site behaves identically rather than quietly gaining a
    different gate.

    A length mismatch is fatal rather than padded or truncated. A vector
    written for 5.1 and handed a stereo file would otherwise gate two channels
    and silently ignore four floors, which is the same class of
    comparison-oracle-that-checks-less-than-it-claims that `strict=True` on
    every zip in this file exists to prevent.
    """
    if per_channel is None:
        return [scalar] * channels
    try:
        values = [float(v) for v in per_channel.split(",")]
    except ValueError as exc:
        raise SystemExit(f"--min-snr-db-per-channel: not a comma-separated list of "
                          f"numbers: {per_channel!r}") from exc
    if len(values) != channels:
        raise SystemExit(f"--min-snr-db-per-channel has {len(values)} floor(s) but the "
                          f"files carry {channels} channel(s): {per_channel!r}")
    return values


def json_safe_db(value: float) -> float:
    """Clamp +-inf (a bit-exact match, or a degenerate all-zero reference) to a
    finite sentinel. json.dumps happily emits the non-standard `Infinity`
    token, but JSON.parse in the browser rejects it outright, and this file's
    JSON output is meant to be fetched and parsed client-side (see
    docs/quality-trend.md) - so it must stay valid JSON, not just valid
    Python-flavoured JSON."""
    if value == math.inf:
        return 200.0
    if value == -math.inf:
        return -200.0
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("actual", type=Path)
    parser.add_argument("--min-snr-db", type=float, default=20.0)
    parser.add_argument("--min-snr-db-per-channel", default=None,
                         help="Comma-separated per-channel SNR floors, in the file's own "
                              "channel order, e.g. '51,57,52,76,16,16' for L R C LFE Ls Rs. "
                              "Overrides --min-snr-db, which stays the fallback for callers "
                              "that have no per-channel vector. See the module docstring.")
    parser.add_argument("--max-diff-dbfs", type=float, default=None,
                         help="Also require every channel's RMS difference to sit below "
                              "this absolute level, in dBFS. See the module docstring for "
                              "when this answers a question --min-snr-db cannot.")
    parser.add_argument("--max-lag-samples", type=int, default=512)
    parser.add_argument("--probe-samples", type=int, default=20000)
    parser.add_argument("--json-out", type=Path, default=None,
                         help="Also write a structured JSON result here.")
    parser.add_argument("--codec-label", default="",
                         help="Passthrough metadata for --json-out (e.g. 'ac3').")
    parser.add_argument("--bitrate-kbps", type=int, default=0,
                         help="Passthrough metadata for --json-out.")
    args = parser.parse_args()

    ref_channels, ref_rate = read_channels(args.reference)
    act_channels, act_rate = read_channels(args.actual)

    if ref_rate != act_rate:
        print(f"FAIL: sample rate mismatch ({args.reference}={ref_rate} "
              f"vs {args.actual}={act_rate})")
        return 1
    if len(ref_channels) != len(act_channels):
        print(f"FAIL: channel count mismatch ({args.reference}={len(ref_channels)} "
              f"vs {args.actual}={len(act_channels)})")
        return 1

    ref_mono = [sum(frame) for frame in zip(*ref_channels, strict=True)]
    act_mono = [sum(frame) for frame in zip(*act_channels, strict=True)]
    lag = best_lag(ref_mono, act_mono, args.max_lag_samples, args.probe_samples)

    thresholds = resolve_thresholds(args.min_snr_db_per_channel, args.min_snr_db,
                                     len(ref_channels))
    labels = channel_labels(len(ref_channels))

    worst = math.inf
    worst_diff = -math.inf
    failed = False
    channels_db = []
    diffs_dbfs = []
    headroom_db = []
    for idx, (ref_ch, act_ch) in enumerate(zip(ref_channels, act_channels, strict=True)):
        r, a = align(ref_ch, act_ch, lag)
        result = snr_db(r, a)
        diff = diff_rms_dbfs(r, a)
        floor = thresholds[idx]
        channels_db.append(result)
        diffs_dbfs.append(diff)
        headroom_db.append(result - floor)
        worst = min(worst, result)
        worst_diff = max(worst_diff, diff)
        ok = result >= floor
        if args.max_diff_dbfs is not None:
            ok = ok and diff <= args.max_diff_dbfs
        status = "ok" if ok else "FAIL"
        if not ok:
            failed = True
        suffix = "" if args.max_diff_dbfs is None else f", diff {diff:.2f} dBFS"
        print(f"channel {idx} ({labels[idx]}): {result:.2f} dB{suffix} "
              f"[floor {floor:g} dB, {result - floor:+.2f} dB] [{status}]")

    # The channel closest to its OWN floor, which is the thing a reader wants
    # and is no longer the same channel as the worst-scoring one: with per-
    # channel floors a 58 dB centre channel 6 dB above a 52 dB floor is tighter
    # than a 22 dB surround 6 dB above a 16 dB floor, even though the surround
    # is the lower number. Reported alongside the worst channel rather than
    # instead of it - they answer different questions, and collapsing them is
    # what the single-floor form did.
    tightest = min(range(len(headroom_db)), key=lambda i: headroom_db[i])
    worst_idx = channels_db.index(worst)

    print(f"lag: {lag} samples, worst channel: {labels[worst_idx]} {worst:.2f} dB "
          f"(floor {thresholds[worst_idx]:g} dB)")
    print(f"tightest margin: {labels[tightest]} {headroom_db[tightest]:+.2f} dB over its "
          f"{thresholds[tightest]:g} dB floor")
    if args.max_diff_dbfs is not None:
        print(f"loudest channel difference: {worst_diff:.2f} dBFS, "
              f"threshold: {args.max_diff_dbfs} dBFS")

    if args.json_out is not None:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps({
            "codec": args.codec_label,
            "bitrate_kbps": args.bitrate_kbps,
            "sample_rate": ref_rate,
            "lag_samples": lag,
            # Kept, and kept scalar: every consumer written before per-channel
            # floors existed reads this, and docs/performance-quality.md
            # computes a headroom from it. In per-channel mode it is the floor
            # the WORST-scoring channel was judged against, so `worst_db -
            # threshold_db` still means what it always meant rather than
            # silently becoming a comparison against an unrelated channel's
            # gate.
            "threshold_db": thresholds[worst_idx],
            "channels_db": [json_safe_db(v) for v in channels_db],
            "worst_db": json_safe_db(worst),
            # The new, complete form. thresholds_db is the vector actually
            # gated on; headroom_db is channels_db - thresholds_db, carried
            # rather than left for each consumer to recompute and get subtly
            # different. tightest_channel indexes the smallest headroom, which
            # is the number a status card should lead with.
            "channel_labels": labels,
            "thresholds_db": thresholds,
            "headroom_db": [json_safe_db(v) for v in headroom_db],
            "tightest_channel": tightest,
            "tightest_headroom_db": json_safe_db(headroom_db[tightest]),
            "diffs_dbfs": [json_safe_db(v) for v in diffs_dbfs],
            "worst_diff_dbfs": json_safe_db(worst_diff),
            "pass": not failed,
        }, indent=2))

    if failed:
        if args.max_diff_dbfs is None:
            print("FAIL: at least one channel is below its own SNR floor")
        else:
            print("FAIL: at least one channel is below its own SNR floor or above the "
                  "difference threshold")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
