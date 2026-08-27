"""Export a real decode's per-block bap/exponent/mask/SNR-offset trace as CSV (roadmap AP12).

docs/library/python-api.md excerpts the functions below directly - this file is what the build
actually runs, not a snippet copied by hand. See ac3/verify/trace_export.hpp for the row schema
and why there is no Parquet writer here - pandas.read_csv(...).to_parquet() is the intended route,
not exercised in this script so it stays runnable without a pandas dependency.

Run with: python examples/python/trace_export.py
"""

from __future__ import annotations

import math

import ac3forge as ac3
import numpy as np

FRAME_COUNT = 3


def build_channel(tone_hz: float, frame: int, sample_rate: int = 48000) -> np.ndarray:
    n = np.arange(ac3.SAMPLES_PER_FRAME, dtype=np.float64)
    t = (frame * ac3.SAMPLES_PER_FRAME + n) / sample_rate
    return (0.4 * np.sin(2.0 * math.pi * tone_hz * t)).astype(np.float32)


def main() -> None:
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0))
    trace = ac3.verify.FrameTrace()
    decoder = ac3.FrameDecoder(ac3.DecoderConfig(trace=trace))

    csv_text = ac3.verify.trace_csv_header()
    for frame in range(FRAME_COUNT):
        channels = [build_channel(440.0, frame), build_channel(660.0, frame)]
        frame_bytes = encoder.encode_frame(channels)
        decoder.decode_frame(frame_bytes)
        # A fresh FrameDecoder.decode_frame call refills `trace` from scratch
        # (see DecoderConfig::trace's own doc comment), so it is read back out
        # once per frame rather than accumulated across the loop.
        csv_text += ac3.verify.trace_to_csv(trace, frame_index=frame)

    rows = csv_text.count("\n") - 1  # minus the header line
    print(f"{rows} trace rows across {FRAME_COUNT} frames")
    print(csv_text.splitlines()[0])
    print(csv_text.splitlines()[1])


if __name__ == "__main__":
    main()
