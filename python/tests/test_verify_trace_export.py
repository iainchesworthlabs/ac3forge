"""ac3.verify - the research trace export (roadmap AP12).

Proves the data is genuinely reachable from Python in the form the roadmap asks for: CSV parsed
by the stdlib csv module, JSON Lines parsed line by line with the stdlib json module - neither
test depends on pandas/pyarrow, which are not a declared dependency of this package (see
ac3/verify/trace_export.hpp's own note on why Parquet is left to Python's own ecosystem rather
than grown here), but the output is exactly the shape pandas.read_csv/read_json(lines=True)
expects: one row per (frame, substream, block, stream, kind, index, value).
"""

import csv
import io
import json

import ac3forge as ac3
import numpy as np
import pytest


def _tone(freq_hz, n, sample_rate=48000, amplitude=0.35, phase=0.0):
    t = np.arange(n, dtype=np.float64) / sample_rate
    return (amplitude * np.sin(2 * np.pi * freq_hz * t + phase)).astype(np.float32)


def test_trace_csv_header():
    assert ac3.verify.trace_csv_header() == "frame,substream,block,stream,kind,index,value\n"


def test_ac3_trace_round_trips_through_csv():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192)
    encoder = ac3.FrameEncoder(config)
    trace = ac3.verify.FrameTrace()
    decoder = ac3.FrameDecoder(ac3.DecoderConfig(trace=trace))

    total = ac3.SAMPLES_PER_FRAME
    channels = [_tone(440.0, total), _tone(660.0, total, phase=0.3)]
    frame_bytes = encoder.encode_frame(channels)
    decoded = decoder.decode_frame(frame_bytes)
    assert decoded.acmod == ac3.Acmod.k2_0

    csv_text = ac3.verify.trace_csv_header() + ac3.verify.trace_to_csv(trace, 0)
    rows = list(csv.DictReader(io.StringIO(csv_text)))
    assert rows, "a real decode with a trace attached must produce rows"

    kinds = {row["kind"] for row in rows}
    assert kinds == {"exponent", "bap", "mask", "snr_offset"}

    # Every stream's mask curve is exactly Table 7.13's 50 bands, whatever the
    # exponent/bap bin count for that stream happens to be - the two index
    # spaces trace_export.hpp's own comment says never claim to line up.
    mask_rows = [row for row in rows if row["kind"] == "mask" and row["block"] == "0"]
    streams = {row["stream"] for row in mask_rows}
    for stream in streams:
        this_stream = [row for row in mask_rows if row["stream"] == stream]
        assert len(this_stream) == 50
        assert {int(row["index"]) for row in this_stream} == set(range(50))

    # snr_offset is one row per stream per block, not one per bin.
    snr_rows = [row for row in rows if row["kind"] == "snr_offset"]
    assert len(snr_rows) == len({(row["block"], row["stream"]) for row in rows})

    assert all(row["frame"] == "0" for row in rows)
    assert all(row["substream"] == "0" for row in rows)  # AC-3 has no substream layer


def test_ac3_trace_round_trips_through_json_lines():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192)
    encoder = ac3.FrameEncoder(config)
    trace = ac3.verify.FrameTrace()
    decoder = ac3.FrameDecoder(ac3.DecoderConfig(trace=trace))

    channels = [_tone(440.0, ac3.SAMPLES_PER_FRAME), _tone(660.0, ac3.SAMPLES_PER_FRAME)]
    frame_bytes = encoder.encode_frame(channels)
    decoder.decode_frame(frame_bytes)

    lines = ac3.verify.trace_to_json_lines(trace, 5).splitlines()
    assert lines
    records = [json.loads(line) for line in lines]
    assert all(record["frame"] == 5 for record in records)
    assert {"frame", "substream", "block", "stream", "kind", "index", "value"} <= records[0].keys()


def test_eac3_trace_has_substream_zero_for_a_single_independent_substream():
    encoder = ac3.eac3.FrameEncoder(ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0))
    trace = ac3.verify.Eac3AccessUnitTrace()
    decoder = ac3.Eac3Decoder(ac3.DecoderConfig(eac3_trace=trace))

    channels = [_tone(440.0, ac3.SAMPLES_PER_FRAME), _tone(660.0, ac3.SAMPLES_PER_FRAME)]
    frame_bytes = encoder.encode_frame(channels)
    decoded = decoder.decode_substream(frame_bytes)
    assert decoded is not None

    csv_text = ac3.verify.trace_csv_header() + ac3.verify.trace_to_csv(trace, 0)
    rows = list(csv.DictReader(io.StringIO(csv_text)))
    assert rows
    assert all(row["substream"] == "0" for row in rows)


def test_decoder_config_still_rejects_an_unknown_keyword():
    # trace/eac3_trace are popped out of kwargs by hand before the rest goes
    # through the generic binder (see bindings.cpp) - this proves that binder
    # still rejects an ordinary typo rather than silently accepting it.
    with pytest.raises(TypeError):
        ac3.DecoderConfig(nonsense=1)


def test_decoder_config_defaults_to_no_trace():
    # The facility is genuinely opt-in: a decoder built without trace=/
    # eac3_trace= behaves exactly as it always has.
    decoder = ac3.FrameDecoder(ac3.DecoderConfig())
    channels = [_tone(440.0, ac3.SAMPLES_PER_FRAME), _tone(660.0, ac3.SAMPLES_PER_FRAME)]
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192))
    decoded = decoder.decode_frame(encoder.encode_frame(channels))
    assert decoded.acmod == ac3.Acmod.k2_0
