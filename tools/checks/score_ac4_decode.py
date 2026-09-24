"""Score ac3cli's AC-4 decoding of DEE's streams against the sources they were encoded from.

For each leg the decoder turns into PCM - SIMPLE mono or stereo at frame_rate_index 13 (see
src/ac4dec/include/ac4dec/decoder.hpp) - this decodes the stream with `ac3cli decode`, aligns
the output with the source by cross-correlation, fits a least-squares gain per channel, and
checks (planning/ac4.md, the decoder's ladder, item 3):

  lag      the output lags the source by LAG samples, the same on every leg: DEE's encoder
           delay plus this decoder's, whose frame alignment (Part 1 Table 188's d_pcm) is
           part of it. The QMF domain of phase D3 will add its own delay.
  gain     every channel within 0.2 dB of unity. The streams were made with loudness measured
           only, and the decoder applies no DRC or output level yet, so the prediction is the
           source's own level; src/ac4dec/ERRATA.md, "Full scale, and the overlap-add's factor
           of two", is what this settles.
  SNR      every channel's signal-to-noise ratio against the gain-scaled source at or above its
           floor: the first measurement less 1 dB, pinned in FLOORS below.
  routing  on a tone leg, each channel's own tone at least 40 dB above every other channel's
           tone in it.

The committed legs (tests/golden/external-baseline/) are scored by default; their sources are
rebuilt by tools/generators/gen_ac4_baseline.py from the committed FLAC fixtures, which needs
ffmpeg on PATH. --gold DIR scores phase G0's local gold set in DIR instead (DIR/streams/<leg>/
dee.ac4, DIR/sources/<source>.wav, DIR/gold-manifest.json), which never runs in CI.

--measure prints what every leg measures and checks nothing, for pinning a new leg's floors.

Usage:
    python tools/checks/score_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli
    python tools/checks/score_ac4_decode.py --cli ac3cli.exe --gold D:/ac3bld/ac4-gold
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "generators"))
import gen_ac4_baseline as baseline  # noqa: E402

BASELINE_DIR = REPO / "tests" / "golden" / "external-baseline"
RATE = 48000

# DEE's encoder and this decoder together, at frame_rate_index 13: 3 072 samples, a frame and a
# half, plus d_pcm's 352.
LAG = 3424
GAIN_TOLERANCE_DB = 0.2
ROUTING_MARGIN_DB = 40.0
# Samples left out of the SNR at each end of the aligned overlap: the decoder's first frame
# starts from silence, and the encoder's last frames are padding.
EDGE = 4096

# Per-channel SNR floors in dB, the first measurement less 1 dB, by leg: the committed legs by
# their directory under tests/golden/external-baseline/, the gold legs by their name in
# gold-manifest.json. Measured 2026-09-25 with the decoder of phase D2.
FLOORS = {
    "ac4-20-music-192": (33.8, 34.5),
    "ac4-20-tones-192": (48.9, 50.5),
    "20-music-192": (33.5, 33.6),
    "20-speech-192": (38.4, 38.4),
    "20-tones-192": (48.9, 50.3),
}
# DEE's 2.0 streams carry the same audio from 256 kbps up, the rest of each frame being fill,
# so every rate from 256 to 768 decodes to the same samples and takes the same floors.
for _rate in (256, 288, 320, 384, 448, 512, 768):
    FLOORS[f"20-music-{_rate}"] = (35.7, 35.7)
    FLOORS[f"20-speech-{_rate}"] = (38.4, 38.4)
    FLOORS[f"20-tones-{_rate}"] = (48.9, 50.3)


def read_wav(path):
    """(samples of shape (n, channels) as float64, rate) from a PCM or IEEE float WAV."""
    blob = Path(path).read_bytes()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV file")
    fmt = data = None
    pos = 12
    while pos + 8 <= len(blob):
        chunk, size = blob[pos:pos + 4], int.from_bytes(blob[pos + 4:pos + 8], "little")
        body = blob[pos + 8:pos + 8 + size]
        if chunk == b"fmt ":
            fmt = body
        elif chunk == b"data":
            data = body
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise SystemExit(f"{path}: no fmt or data chunk")
    tag = int.from_bytes(fmt[0:2], "little")
    channels = int.from_bytes(fmt[2:4], "little")
    rate = int.from_bytes(fmt[4:8], "little")
    bits = int.from_bytes(fmt[14:16], "little")
    if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: the subformat's first two bytes
        tag = int.from_bytes(fmt[24:26], "little")
    if tag == 3 and bits == 32:
        samples = np.frombuffer(data, dtype="<f4").astype(np.float64)
    elif tag == 1 and bits == 24:
        raw = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        value = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        samples = np.where(value >= 1 << 23, value - (1 << 24), value) / float(1 << 23)
    elif tag == 1 and bits == 16:
        samples = np.frombuffer(data, dtype="<i2") / 32768.0
    else:
        raise SystemExit(f"{path}: WAV format {tag} at {bits} bits is not read here")
    return samples.reshape(-1, channels), rate


def best_lag(reference, decoded, max_lag):
    """The shift d that best matches decoded[n + d] to reference[n], within +-max_lag."""
    size = 1 << int(np.ceil(np.log2(len(reference) + len(decoded))))
    spectrum = np.conj(np.fft.rfft(reference, size)) * np.fft.rfft(decoded, size)
    correlation = np.fft.irfft(spectrum, size)
    lags = np.concatenate([np.arange(0, max_lag + 1), np.arange(-max_lag, 0)])
    values = np.concatenate([correlation[:max_lag + 1], correlation[-max_lag:]])
    return int(lags[np.argmax(values)])


def tone_power(x, hz):
    """The power of x at hz, normalised so a sine of amplitude A reads A^2 / 4."""
    n = np.arange(len(x))
    projection = np.dot(x, np.exp(-2j * np.pi * hz * n / RATE)) / len(x)
    return float(np.abs(projection) ** 2)


def score(source, decoded):
    """lag, and per channel (gain in dB, SNR in dB), of decoded against source."""
    lag = best_lag(source.sum(axis=1), decoded.sum(axis=1), 16384)
    if lag >= 0:
        count = min(len(source), len(decoded) - lag)
        ref, out = source[:count], decoded[lag:lag + count]
    else:
        count = min(len(source) + lag, len(decoded))
        ref, out = source[-lag:-lag + count], decoded[:count]
    ref, out = ref[EDGE:-EDGE], out[EDGE:-EDGE]
    channels = []
    for c in range(source.shape[1]):
        gain = float(np.dot(ref[:, c], out[:, c]) / np.dot(ref[:, c], ref[:, c]))
        error = out[:, c] - gain * ref[:, c]
        snr = 10.0 * np.log10(np.dot(gain * ref[:, c], gain * ref[:, c]) / np.dot(error, error))
        channels.append((20.0 * np.log10(abs(gain)), float(snr)))
    return lag, channels, out


def decode(cli, stream, out_wav):
    result = subprocess.run([str(cli), "decode", str(stream), str(out_wav)], capture_output=True,
                            text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{stream}: ac3cli decode failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    return read_wav(out_wav)


def legs_committed(work):
    """(name, stream, source WAV path, source name) for every committed leg decode reads.

    The manifest records what each stream turned out to be and a digest of the source it was
    made from; gen_ac4_baseline.py's LEGS names the source, which is rebuilt here and must
    hash to that digest."""
    manifest = json.loads((BASELINE_DIR / "ac4-manifest.json").read_text(encoding="utf-8"))
    source_of = {leg["name"]: leg["source"] for leg in baseline.LEGS}
    chosen = {name: leg for name, leg in manifest["legs"].items()
              if leg.get("codec_mode") == "SIMPLE" and leg.get("frame_rate_index") == 13
              and leg.get("output_channel_layout") in ("stereo", "mono")}
    sources = sorted({source_of[name] for name in chosen})
    paths = baseline.build_sources(work / "sources", baseline.COMMITTED_SECONDS, sources)
    legs = []
    for name, leg in sorted(chosen.items()):
        path = paths[source_of[name]]
        if baseline.sha256(path) != leg["source_sha256"]:
            raise SystemExit(f"{name}: the rebuilt source {path.name} does not hash to the "
                             "manifest's source_sha256")
        legs.append((name, BASELINE_DIR / name / "dee.ac4", path, source_of[name]))
    return legs


def legs_gold(gold):
    manifest = json.loads((gold / "gold-manifest.json").read_text(encoding="utf-8"))
    return [(name, gold / "streams" / name / "dee.ac4", gold / "sources" / f"{leg['source']}.wav",
             leg["source"])
            for name, leg in sorted(manifest["legs"].items())
            if leg.get("codec_mode") == "SIMPLE" and leg.get("frame_rate_index") == 13
            and leg.get("output_channel_layout") in ("stereo", "mono")]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path, help="the ac3cli to decode with")
    parser.add_argument("--gold", type=Path, help="score G0's local gold set in this directory")
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    parser.add_argument("--measure", action="store_true",
                        help="print every leg's measurements and check nothing")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        legs = legs_gold(args.gold) if args.gold else legs_committed(work)
        if not legs:
            raise SystemExit("no leg to score")
        failures = []
        for name, stream, source_path, source_name in legs:
            source, source_rate = read_wav(source_path)
            decoded, rate = decode(args.cli, stream, work / f"{name}.wav")
            if rate != RATE or source_rate != RATE:
                failures.append(f"{name}: decoded at {rate} Hz, source at {source_rate} Hz")
                continue
            if decoded.shape[1] != source.shape[1]:
                failures.append(f"{name}: {decoded.shape[1]} channels decoded, the source has "
                                f"{source.shape[1]}")
                continue
            lag, channels, aligned = score(source, decoded)
            cells = "  ".join(f"ch{c} {gain:+.3f} dB {snr:.2f} dB" for c, (gain, snr) in
                              enumerate(channels))
            print(f"{name:<24} lag {lag:5d}  {cells}")
            if args.measure:
                continue
            if lag != LAG:
                failures.append(f"{name}: lag {lag}, expected {LAG}")
            floors = FLOORS.get(name)
            if floors is None:
                failures.append(f"{name}: no SNR floors pinned in FLOORS")
            for c, (gain, snr) in enumerate(channels):
                if abs(gain) > GAIN_TOLERANCE_DB:
                    failures.append(f"{name} ch{c}: gain {gain:+.3f} dB, beyond "
                                    f"+-{GAIN_TOLERANCE_DB} dB of unity")
                if floors is not None and snr < floors[c]:
                    failures.append(f"{name} ch{c}: SNR {snr:.2f} dB below its floor {floors[c]}")
            if source_name.startswith("tones"):
                for c in range(aligned.shape[1]):
                    own = tone_power(aligned[:, c], baseline.TONE_HZ[c])
                    for other in range(aligned.shape[1]):
                        if other == c:
                            continue
                        leak = tone_power(aligned[:, c], baseline.TONE_HZ[other])
                        margin = 10.0 * np.log10(own / max(leak, 1e-30))
                        if margin < ROUTING_MARGIN_DB:
                            failures.append(f"{name} ch{c}: its tone only {margin:.1f} dB above "
                                            f"ch{other}'s")
        if failures:
            print("\nFAILED:")
            for failure in failures:
                print(f"  {failure}")
            return 1
        if not args.measure:
            print(f"\n{len(legs)} legs: lag, level, SNR floors and routing all hold")
        return 0


if __name__ == "__main__":
    sys.exit(main())
