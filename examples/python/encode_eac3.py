"""Encode a 7.1 layout to E-AC-3 and decode it straight back.

Mirrors examples/encode_eac3.cpp in spirit but through the Python bindings (roadmap AP6), and uses
ac3.eac3.access_unit_config_for_layout() so the dependent substream's chanmap never has to be
hand-built - the whole point of the named-layout convenience.
docs/library/python-api.md excerpts the functions below directly - this file is what the build
actually runs, not a snippet copied by hand.

Run with: python examples/python/encode_eac3.py
"""

from __future__ import annotations

import math

import ac3forge as ac3
import numpy as np

# L, C, R, Ls, Rs, LFE (the 3/2+LFE bed, 6 channels), then the dependent's own acmod 2/2 (4
# channels, coded as k71Rear's Ls/Rs/Lrs/Rrs quad) - 10 in total, encoder.channel_count below.
TONES_HZ = [1000.0, 1200.0, 800.0, 600.0, 1400.0, 60.0, 600.0, 1400.0, 2000.0, 1300.0]
SAMPLE_RATE = 48000
FRAME_COUNT = 31  # ~48000 / 1536 samples, one second


def build_channel(tone_hz: float, frame: int) -> np.ndarray:
    """One frame (ac3.SAMPLES_PER_FRAME samples) of a sine tone, continuous across frames."""
    n = np.arange(ac3.SAMPLES_PER_FRAME, dtype=np.float64)
    t = (frame * ac3.SAMPLES_PER_FRAME + n) / SAMPLE_RATE
    return (0.3 * np.sin(2.0 * math.pi * tone_hz * t)).astype(np.float32)


def encode_stream() -> bytes:
    config = ac3.eac3.access_unit_config_for_layout(ac3.eac3.LayoutId.k71, bitrate_kbps=448)
    encoder = ac3.eac3.AccessUnitEncoder(config)
    stream = bytearray()
    for frame in range(FRAME_COUNT):
        channels = [build_channel(tone, frame) for tone in TONES_HZ]
        stream += encoder.encode_access_unit(channels).bytes
    return bytes(stream)


def decode_stream(stream: bytes) -> None:
    decoder = ac3.Eac3Decoder()
    for i, unit_bytes in enumerate(ac3.split_access_units(stream)):
        decoded = decoder.decode_access_unit(unit_bytes)
        if decoded is None:
            continue  # transient pre-noise hold-back (§3.7); not exercised by this config
        if i == 0:
            print(f"substreams: {decoded.substream_count}, channels: {decoded.channel_labels}")
            print(f"sample rate: {ac3.sample_rate_hz(decoded.sample_rate)} Hz")


def main() -> None:
    stream = encode_stream()
    print(f"{len(stream)} bytes of E-AC-3 (7.1, {FRAME_COUNT} frames)")
    decode_stream(stream)


if __name__ == "__main__":
    main()
