"""Score ac3cli's AC-4 encoding: the encoder's streams, decoded, against their sources.

planning/ac4.md, the encoder's ladder, items 4 and 5, as phases E1 and E2 need them: SIMPLE and
ASPX, mono and stereo, at frame_rate_index 13. Each leg encodes a source with `ac3cli
ac4-encode`, which picks the codec mode from the rate, decodes the stream with `ac3cli decode`,
aligns the output with the source by cross-correlation, fits a least-squares gain per channel,
and checks, as score_ac4_decode.py scores DEE's streams:

  lag      the output lags the source by LAG samples: the encoder's delay, a frame and a half,
           and the decoder's, 1 313 at frame_rate_index 13 (score_ac4_decode.py);
  gain     every channel within 0.2 dB of unity, where its SNR is 20 dB or more: below that the
           least-squares gain of a coarse quantiser's output is no measure of level. In ASPX the
           gain is fitted below the crossover, where the output is the source's waveform;
  SNR      every channel's signal-to-noise ratio at or above its floor, the first measurement
           less 1 dB (FLOORS): over the whole band in SIMPLE, below the crossover in ASPX, which
           the stream's own aspx_config() gives;
  tiles    in ASPX, above the crossover, the mean absolute difference between the A-SPX tiles'
           energy and the source's at or below its ceiling, the first measurement plus 0.5 dB;
  LSD      tools/ci/quality_race.py's log-spectral distance at or below its ceiling, the first
           measurement plus 0.5 dB;
  MOS      ViSQOL's MOS-LQO (quality_race.perceptual_score) at or above its floor, the first
           measurement less 0.1, where visqol-python is installed;
  routing  on a tone leg, each channel's tone at least 40 dB above the other channel's in it.

The sources are 5 s long: the committed programme fixtures' 2.0 cuts and one tone per channel,
rebuilt by tools/generators/gen_ac4_baseline.py (which reads the FLAC fixtures with ffmpeg, so
ffmpeg must be on PATH); their mono downmixes; and synthetic signals made here: a sweep, noise,
castanet-like bursts after silence, and a source panned between the channels, which the
encoder's stereo prediction codes.

--gold DIR runs the race instead, locally: for each of phase G0's 2.0 legs, ASPX from 48 to 144
kbps and SIMPLE from 192 to 768, DEE's stream (DIR/streams/<leg>/dee.ac4) and this encoder's
stream of the same source (DIR/sources/<source>.wav) at the same rate are both decoded and scored
as above. The encoder's scores are checked against RACE, pinned the same way, and its gaps to
DEE's are printed.

--librempeg PATH also decodes every stream with librempeg's ffmpeg, run in WSL (the path is the
WSL one), and reports its scores against the source and how far its output is from the
decoder's. Nothing it measures is checked: planning/ac4.md, decision 13.

--measure prints what every leg measures and checks nothing, for pinning a new leg.

Usage:
    python tools/checks/score_ac4_encode.py --cli build/config-linux-llvm/bin/ac3cli
    python tools/checks/score_ac4_encode.py --cli ac3cli.exe --gold D:/ac3bld/ac4-gold
        [--librempeg /mnt/d/ac3bld/librempeg/install/bin/ffmpeg]
"""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "generators"))
sys.path.insert(0, str(REPO / "tools" / "ci"))
sys.path.insert(0, str(REPO / "tools" / "checks"))
import gen_ac4_baseline as baseline  # noqa: E402
import quality_race  # noqa: E402
import score_ac4_decode as decoding  # noqa: E402

SECONDS = 5.0
# The encoder's delay, 3 072 samples, and the decoder's, 1 313, at frame_rate_index 13.
LAG = 3072 + decoding.DECODER_DELAY
GAIN_TOLERANCE_DB = 0.2
GAIN_MIN_SNR_DB = 20.0
ROUTING_MARGIN_DB = 40.0
SNR_MARGIN_DB = 1.0
LSD_MARGIN_DB = 0.5
TILE_MARGIN_DB = 0.5
MOS_MARGIN = 0.1

# name: (source, sample rate, kbps). The source's name ends in its channel count, 20 or 10. The
# encoder picks the codec mode: ASPX below 96 kbps a channel, SIMPLE from there.
LEGS = {
    "20-music-48": ("music_20", 48000, 48),
    "20-music-64": ("music_20", 48000, 64),
    "20-music-96": ("music_20", 48000, 96),
    "20-music-128": ("music_20", 48000, 128),
    "20-music-192": ("music_20", 48000, 192),
    "20-music-256": ("music_20", 48000, 256),
    "20-speech-48": ("speech_20", 48000, 48),
    "20-speech-128": ("speech_20", 48000, 128),
    "20-speech-192": ("speech_20", 48000, 192),
    "20-tones-64": ("tones_20", 48000, 64),
    "20-tones-192": ("tones_20", 48000, 192),
    "20-sweep-96": ("sweep_20", 48000, 96),
    "20-sweep-192": ("sweep_20", 48000, 192),
    "20-noise-192": ("noise_20", 48000, 192),
    "20-castanets-64": ("castanets_20", 48000, 64),
    "20-castanets-192": ("castanets_20", 48000, 192),
    "20-panned-128": ("panned_20", 48000, 128),
    "10-music-64": ("music_10", 48000, 64),
    "10-speech-24": ("speech_10", 48000, 24),
    "10-speech-48": ("speech_10", 48000, 48),
    "20-sweep-64-44k": ("sweep_20", 44100, 64),
    "20-tones-192-44k": ("tones_20", 44100, 192),
    "20-sweep-192-44k": ("sweep_20", 44100, 192),
}

# Per leg: (SNR floor per channel in dB, LSD ceiling in dB, A-SPX tile ceiling in dB or None for
# SIMPLE, MOS floor), the first measurement less 1 dB, plus 0.5 dB, plus 0.5 dB and less 0.1.
# Measured 2026-09-25 with the encoder of phase E2 and the decoder of phase D3, whose QMF banks
# every decode passes through: they reconstruct to about 78 dB, which bounds the sweeps' SNR.
FLOORS = {
    "20-music-48": ((13.9, 14.5), 2.10, 3.48, 4.39),
    "20-music-64": ((17.4, 18.0), 1.57, 2.24, 4.50),
    "20-music-96": ((22.0, 22.4), 1.31, None, 4.57),
    "20-music-128": ((32.0, 32.7), 1.04, None, 4.60),
    "20-music-192": ((42.5, 43.3), 1.60, None, 4.60),
    "20-music-256": ((49.7, 50.4), 1.53, None, 4.60),
    "20-speech-48": ((22.5, 22.5), 1.44, 2.79, 3.99),
    "20-speech-128": ((43.4, 43.4), 0.75, 2.56, 4.43),
    "20-speech-192": ((53.4, 53.4), 0.58, None, 4.62),
    "20-tones-64": ((54.8, 57.8), 12.62, None, 4.63),
    "20-tones-192": ((81.4, 81.8), 9.98, None, 4.63),
    "20-sweep-96": ((28.4, 28.6), 15.89, 19.30, 4.63),
    "20-sweep-192": ((75.7, 75.6), 12.77, None, 4.63),
    "20-noise-192": ((12.0, 12.0), 1.02, None, 3.72),
    "20-castanets-64": ((-4.7, -4.8), 3.01, 2.01, 4.55),
    "20-castanets-192": ((6.8, 6.8), 1.96, None, 4.62),
    "20-panned-128": ((42.3, 38.5), 1.00, None, 4.61),
    "10-music-64": ((26.0,), 1.14, None, 4.59),
    "10-speech-24": ((9.7,), 2.56, 2.91, 3.93),
    "10-speech-48": ((22.5,), 1.43, 2.46, 4.34),
    "20-sweep-64-44k": ((19.8, 19.6), 15.95, 13.16, 4.63),
    "20-tones-192-44k": ((81.4, 79.1), 8.95, None, 4.63),
    "20-sweep-192-44k": ((75.9, 75.9), 13.16, None, 4.63),
}

# The race, per G0 leg: the encoder's scores, pinned as FLOORS are. What it measured against DEE's
# streams of the same legs, both decoded by the decoder. In the ASPX legs, ViSQOL is within 0.02
# of DEE's or above it from 64 kbps up (music +0.05 at 64 and 96, speech +0.07 at 64), and 0.06
# under it on music at 48 kbps. Below the crossover the encoder's SNR is 1.4 to 3.6 dB under DEE's
# at 48 and 64 kbps and on music at 96, whose bits go where ViSQOL marks the noise, and 0.5 to 8.2
# dB over it on speech at 96 and from 128. The A-SPX tiles land within 0.25 dB of DEE's distance
# from the source's energy, or closer (music at 64 kbps: 1.9 dB against 5.4). In the SIMPLE legs,
# from 192 kbps, the encoder is E1's: 5.6 dB over DEE's SNR on music at 192 kbps, 14.9 on speech
# and 32 on the tones, its log-spectral distance lower on each, and ViSQOL within 0.01 of DEE's
# 4.70 to 4.73.
# From 256 kbps DEE's audio stops changing and the encoder's goes on improving, to 74 dB on music
# at 768 kbps, where the QMF banks' reconstruction bounds it.
RACE = {
    "20-music-48": ((12.4, 12.4), 2.10, 3.12, 4.37),
    "20-music-64": ((15.1, 15.2), 1.62, 2.35, 4.55),
    "20-music-96": ((20.8, 20.8), 1.37, 0.57, 4.61),
    "20-music-128": ((29.4, 29.5), 1.09, 0.69, 4.62),
    "20-music-144": ((32.4, 32.4), 1.04, 0.69, 4.62),
    "20-music-192": ((39.2, 39.2), 1.54, None, 4.62),
    "20-music-256": ((46.3, 46.4), 1.44, None, 4.62),
    "20-music-288": ((49.4, 49.5), 1.39, None, 4.62),
    "20-music-320": ((52.3, 52.3), 1.33, None, 4.63),
    "20-music-384": ((57.3, 57.4), 1.01, None, 4.63),
    "20-music-448": ((61.5, 61.5), 0.69, None, 4.63),
    "20-music-512": ((65.0, 65.0), 0.59, None, 4.63),
    "20-music-768": ((73.6, 73.4), 0.59, None, 4.63),
    "20-speech-48": ((22.3, 22.3), 1.46, 2.90, 4.15),
    "20-speech-64": ((25.8, 25.8), 1.25, 3.55, 4.37),
    "20-speech-96": ((34.1, 34.1), 0.92, 2.92, 4.55),
    "20-speech-128": ((43.2, 43.2), 0.78, 2.89, 4.55),
    "20-speech-144": ((46.5, 46.5), 0.75, 2.88, 4.55),
    "20-speech-192": ((53.3, 53.3), 0.59, None, 4.62),
    "20-speech-256": ((61.5, 61.5), 0.54, None, 4.63),
    "20-speech-288": ((65.1, 65.1), 0.53, None, 4.63),
    "20-speech-320": ((68.2, 68.2), 0.52, None, 4.63),
    "20-speech-384": ((72.6, 72.6), 0.51, None, 4.63),
    "20-speech-448": ((74.6, 74.6), 0.51, None, 4.63),
    "20-speech-512": ((75.3, 75.3), 0.51, None, 4.63),
    "20-speech-768": ((75.5, 75.5), 0.51, None, 4.63),
    "20-tones-48": ((54.7, 57.6), 11.13, None, 4.63),
    "20-tones-64": ((54.8, 57.8), 11.25, None, 4.63),
    "20-tones-96": ((54.8, 57.9), 11.82, None, 4.63),
    "20-tones-128": ((81.0, 81.7), 8.87, None, 4.63),
    "20-tones-144": ((81.1, 81.9), 8.76, None, 4.63),
    "20-tones-192": ((81.2, 82.0), 8.80, None, 4.63),
    "20-tones-256": ((81.3, 82.2), 8.60, None, 4.63),
    "20-tones-288": ((81.3, 82.2), 8.53, None, 4.63),
    "20-tones-320": ((81.3, 82.2), 8.49, None, 4.63),
    "20-tones-384": ((81.3, 82.2), 8.48, None, 4.63),
    "20-tones-448": ((81.3, 82.2), 8.48, None, 4.63),
    "20-tones-512": ((81.3, 82.2), 8.48, None, 4.63),
    "20-tones-768": ((81.3, 82.2), 8.48, None, 4.63),
}
# The G0 legs the race runs, by rate: ASPX where DEE writes it, SIMPLE from 192 kbps.
RACE_RATES = (48, 64, 96, 128, 144, 192, 256, 288, 320, 384, 448, 512, 768)


# --- Sources ----------------------------------------------------------------------------------


def write_wav_f32(path, columns, rate):
    """An IEEE float WAV of the given channels (a list of equal-length arrays)."""
    data = np.stack([np.asarray(c, dtype="<f4") for c in columns], axis=1).tobytes()
    channels = len(columns)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack(
        "<IHHIIHH", 16, 3, channels, rate, rate * 4 * channels, 4 * channels, 32
    )
    Path(path).write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


def synthetic(name, rate, programme):
    """The synthetic sources, deterministic; `programme` holds the rebuilt 2.0 fixtures."""
    n = int(SECONDS * rate)
    t = np.arange(n) / rate
    if name == "tones_20":
        return [baseline.TONE_AMPLITUDE * np.sin(2 * np.pi * hz * t) for hz in baseline.TONE_HZ[:2]]
    if name == "sweep_20":
        # Logarithmic sweeps from 40 Hz to 18 kHz, up in L and down in R, at -12 dBFS.
        low, high = 40.0, 18000.0
        k = np.log(high / low) / SECONDS
        up = 0.25 * np.sin(2 * np.pi * low * (np.exp(k * t) - 1) / k)
        down = 0.25 * np.sin(2 * np.pi * high * (1 - np.exp(-k * t)) / k)
        return [up, down]
    rng = np.random.default_rng(20260925)
    if name == "noise_20":
        # Pink noise at -20 dBFS RMS, half of it shared by both channels.
        def pink():
            spectrum = np.fft.rfft(rng.standard_normal(n))
            spectrum /= np.sqrt(np.maximum(np.fft.rfftfreq(n, 1 / rate), 20.0))
            x = np.fft.irfft(spectrum, n)
            return x / np.sqrt(np.mean(x * x))

        shared = pink()
        return [0.1 * (np.sqrt(0.5) * shared + np.sqrt(0.5) * pink()) for _ in range(2)]
    if name == "castanets_20":
        # A second of silence, then a noise burst every 250 ms with a 4 ms decay, peaking near
        # -6 dBFS, louder in L.
        burst = np.zeros(n)
        envelope = np.exp(-np.arange(int(0.05 * rate)) / (0.004 * rate))
        for start in range(int(1.0 * rate), n - len(envelope), int(0.25 * rate)):
            burst[start : start + len(envelope)] += envelope * rng.standard_normal(len(envelope))
        burst *= 0.5 / np.max(np.abs(burst))
        return [burst, 0.6 * burst]
    if name == "panned_20":
        mono = programme["music_20"].mean(axis=1)
        return [0.8 * mono, 0.3 * mono]
    if name in ("music_10", "speech_10"):
        return [programme[f"{name[:-3]}_20"].mean(axis=1)]
    raise SystemExit(f"unknown source {name}")


def build_sources(work):
    """Every leg's source WAV, by (source, rate)."""
    rebuilt = baseline.build_sources(work / "fixtures", SECONDS, ["music_20", "speech_20"])
    programme = {name: decoding.read_wav(path)[0] for name, path in rebuilt.items()}
    paths = {}
    for source, rate, _ in LEGS.values():
        if (source, rate) in paths:
            continue
        path = work / f"{source}-{rate}.wav"
        if source in programme and rate == 48000:
            columns = list(programme[source].T)
        else:
            columns = synthetic(source, rate, programme)
        write_wav_f32(path, columns, rate)
        paths[(source, rate)] = path
    return paths


# --- Encoding, decoding and scoring -----------------------------------------------------------


def run(command):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(
            f"{' '.join(str(c) for c in command)} failed ({result.returncode}):\n"
            f"{result.stdout}{result.stderr}"
        )


def wsl_path(path):
    text = str(Path(path).resolve()).replace("\\", "/")
    return f"/mnt/{text[0].lower()}{text[2:]}"


def decode_librempeg(args, stream, out_wav):
    """librempeg's decode of `stream`; on Windows run in WSL, where it is built."""
    if sys.platform == "win32":
        prefix = ["wsl", "-d", args.wsl_distro, "--", args.librempeg]
        stream_arg, out_arg = wsl_path(stream), wsl_path(out_wav)
    else:
        prefix = [args.librempeg]
        stream_arg, out_arg = str(stream), str(out_wav)
    run(
        [
            *prefix,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            stream_arg,
            "-acodec",
            "pcm_f32le",
            out_arg,
        ]
    )
    return decoding.read_wav(out_wav)


def decode_with_groups(cli, stream, out_wav):
    """ac3cli's decode of `stream`, and the low-resolution A-SPX subband groups its first
    aspx_config() gives (score_ac4_decode.py's aspx_groups), or None for a SIMPLE stream."""
    trace = Path(str(out_wav) + ".trace")
    decoded, rate = decoding.decode(cli, stream, out_wav, trace)
    values = decoding.trace_values(trace)
    return decoded, rate, None if values is None else decoding.aspx_groups(values)


@dataclass
class Measured:
    lag: int
    channels: list  # per channel (gain dB, SNR dB)
    lsd: float
    tiles: float | None  # ASPX: the tiles' mean absolute difference in dB
    mos: float | None
    aligned: np.ndarray


def measure(source, decoded, rate, groups):
    """The scores above, of `decoded` against `source`: SNR over the whole band where `groups` is
    None (SIMPLE), and below the crossover, with the A-SPX tiles above it, otherwise."""
    decoding.RATE = rate  # tone_power's, band_snr's and tile_error's rate
    lag, reference, aligned = decoding.align(source, decoded)
    channels = []
    tiles = []
    for c in range(reference.shape[1]):
        r, o = reference[:, c], aligned[:, c]
        gain = float(np.dot(r, o) / np.dot(r, r))
        if groups is None:
            error = o - gain * r
            snr = 10.0 * np.log10(np.dot(gain * r, gain * r) / np.dot(error, error))
        else:
            top_hz = (groups[0] - 1) * decoding.subband_hz()
            gain = decoding.band_gain(r, o, top_hz)
            snr = decoding.band_snr(r, o, gain, top_hz)
            tiles += decoding.tile_error(r, o, groups)
        channels.append((20.0 * np.log10(abs(gain)), float(snr)))
    lsd, _ = quality_race.spectral_scores(reference, aligned)
    mos = quality_race.perceptual_score(reference, aligned, rate)
    tile = float(np.mean(np.abs(tiles))) if tiles else None
    return Measured(lag, channels, float(lsd), tile, mos, aligned)


def routing_failures(name, aligned, rate):
    failures = []
    decoding.RATE = rate
    for c in range(aligned.shape[1]):
        own = decoding.tone_power(aligned[:, c], baseline.TONE_HZ[c])
        for other in range(aligned.shape[1]):
            if other != c:
                leak = decoding.tone_power(aligned[:, c], baseline.TONE_HZ[other])
                margin = 10.0 * np.log10(own / max(leak, 1e-30))
                if margin < ROUTING_MARGIN_DB:
                    failures.append(
                        f"{name} ch{c}: its tone only {margin:.1f} dB above ch{other}'s"
                    )
    return failures


def row(label, m):
    cells = "  ".join(
        f"ch{c} {gain:+.3f} dB {snr:6.2f} dB" for c, (gain, snr) in enumerate(m.channels)
    )
    tiles = "" if m.tiles is None else f"  tiles {m.tiles:4.2f} dB"
    mos_text = "-" if m.mos is None else f"{m.mos:.2f}"
    return f"{label:<26} lag {m.lag:5d}  {cells}  LSD {m.lsd:5.2f} dB{tiles}  MOS {mos_text}"


def pinned_failures(name, pins, m):
    failures = []
    if m.lag != LAG:
        failures.append(f"{name}: lag {m.lag}, expected {LAG}")
    for c, (gain, snr) in enumerate(m.channels):
        if snr >= GAIN_MIN_SNR_DB and abs(gain) > GAIN_TOLERANCE_DB:
            failures.append(f"{name} ch{c}: gain {gain:+.3f} dB, beyond +-{GAIN_TOLERANCE_DB} dB")
    if pins is None:
        return [*failures, f"{name}: nothing pinned"]
    snr_floors, lsd_ceiling, tile_ceiling, mos_floor = pins
    for c, (_, snr) in enumerate(m.channels):
        if snr < snr_floors[c]:
            failures.append(f"{name} ch{c}: SNR {snr:.2f} dB below its floor {snr_floors[c]}")
    if m.lsd > lsd_ceiling:
        failures.append(f"{name}: LSD {m.lsd:.2f} dB above its ceiling {lsd_ceiling}")
    if (tile_ceiling is None) != (m.tiles is None):
        failures.append(f"{name}: tiles {m.tiles}, pinned {tile_ceiling}: the codec mode changed")
    elif m.tiles is not None and m.tiles > tile_ceiling:
        failures.append(f"{name}: A-SPX tiles {m.tiles:.2f} dB above the ceiling {tile_ceiling}")
    if m.mos is not None and m.mos < mos_floor:
        failures.append(f"{name}: MOS {m.mos:.2f} below its floor {mos_floor}")
    return failures


def pin_text(name, m):
    snrs = ", ".join(f"{snr - SNR_MARGIN_DB:.1f}" for _, snr in m.channels)
    snrs += "," if len(m.channels) == 1 else ""
    tiles = "None" if m.tiles is None else f"{m.tiles + TILE_MARGIN_DB:.2f}"
    mos_text = "0.0" if m.mos is None else f"{m.mos - MOS_MARGIN:.2f}"
    return f'    "{name}": (({snrs}), {m.lsd + LSD_MARGIN_DB:.2f}, {tiles}, {mos_text}),'


def encode(cli, source_path, kbps, out):
    run([cli, "ac4-encode", source_path, out, kbps, "quiet"])


def committed_run(args, work):
    paths = build_sources(work)
    failures = []
    pins = []
    for name, (source, rate, kbps) in LEGS.items():
        stream = work / f"{name}.ac4"
        encode(args.cli, paths[(source, rate)], kbps, stream)
        original, _ = decoding.read_wav(paths[(source, rate)])
        decoded, decoded_rate, groups = decode_with_groups(args.cli, stream, work / f"{name}.wav")
        if decoded_rate != rate or decoded.shape[1] != original.shape[1]:
            failures.append(f"{name}: decoded {decoded.shape[1]} channels at {decoded_rate} Hz")
            continue
        m = measure(original, decoded, rate, groups)
        print(row(name, m), flush=True)
        pins.append(pin_text(name, m))
        if args.librempeg:
            other, _ = decode_librempeg(args, stream, work / f"{name}.librempeg.wav")
            print(row("  librempeg", measure(original, other, rate, groups)))
            print(row("  librempeg - decoder", measure(decoded, other, rate, groups)))
        if args.measure:
            continue
        failures += pinned_failures(name, FLOORS.get(name), m)
        if source.startswith("tones"):
            failures += routing_failures(name, m.aligned, rate)
    if args.measure:
        print("\nFLOORS = {\n" + "\n".join(pins) + "\n}")
    return failures, len(LEGS)


def gold_run(args, work):
    manifest = json.loads((args.gold / "gold-manifest.json").read_text(encoding="utf-8"))
    legs = [
        (name, leg)
        for name, leg in sorted(manifest["legs"].items())
        if name.startswith("20-")
        and leg.get("codec_mode") in ("SIMPLE", "ASPX")
        and leg.get("frame_rate_index") == 13
        and leg.get("bitrate_kbps") in RACE_RATES
    ]
    if not legs:
        raise SystemExit(f"no 2.0 leg at the race's rates in {args.gold}")
    failures = []
    pins = []
    for name, leg in legs:
        source_path = args.gold / "sources" / f"{leg['source']}.wav"
        original, rate = decoding.read_wav(source_path)
        dee_stream = args.gold / "streams" / name / "dee.ac4"
        ours_stream = work / f"{name}.ours.ac4"
        encode(args.cli, source_path, leg["bitrate_kbps"], ours_stream)
        scores = {}
        for label, stream in (("DEE", dee_stream), ("ours", ours_stream)):
            decoded, _, groups = decode_with_groups(args.cli, stream, work / f"{name}.{label}.wav")
            scores[label] = measure(original, decoded, rate, groups)
            print(row(f"{name} {label}", scores[label]), flush=True)
            if args.librempeg:
                other, _ = decode_librempeg(args, stream, work / f"{name}.{label}.librempeg.wav")
                print(row("  librempeg", measure(original, other, rate, groups)))
                print(row("  librempeg - decoder", measure(decoded, other, rate, groups)))
        ours, dee = scores["ours"], scores["DEE"]
        pairs = zip(ours.channels, dee.channels, strict=True)
        gaps = "  ".join(f"ch{c} {o[1] - d[1]:+.2f} dB" for c, (o, d) in enumerate(pairs))
        mos_gap = "" if ours.mos is None or dee.mos is None else f", MOS {ours.mos - dee.mos:+.3f}"
        print(
            f"{'':<26} ours less DEE's: SNR {gaps}, LSD {ours.lsd - dee.lsd:+.2f} dB{mos_gap}",
            flush=True,
        )
        pins.append(pin_text(name, ours))
        if not args.measure:
            failures += pinned_failures(name, RACE.get(name), ours)
            if leg["source"].startswith("tones"):
                failures += routing_failures(f"{name} ours", ours.aligned, rate)
    if args.measure:
        print("\nRACE = {\n" + "\n".join(pins) + "\n}")
    return failures, len(legs)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--cli", required=True, type=Path, help="the ac3cli to encode and decode with"
    )
    parser.add_argument(
        "--gold", type=Path, help="run the race against G0's gold set in this directory"
    )
    parser.add_argument(
        "--librempeg", help="librempeg's ffmpeg, to decode with as well (a WSL path on Windows)"
    )
    parser.add_argument(
        "--wsl-distro", default="Ubuntu-26.04", help="the WSL distribution librempeg is built in"
    )
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    parser.add_argument(
        "--measure", action="store_true", help="print the measurements, check nothing"
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        failures, count = gold_run(args, work) if args.gold else committed_run(args, work)
    if failures:
        print("\nFAILED:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    if not args.measure:
        print(f"\n{count} legs: lag, level, SNR, tiles, LSD and MOS floors all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
