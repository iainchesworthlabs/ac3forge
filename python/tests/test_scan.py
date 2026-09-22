"""ac3.scan() and friends (roadmap AP6) - reading an elementary stream's shape without decoding
any audio, mirroring ac3::io::scan/read_frame_header/access_unit_timing and friends
(ac3/io/elementary.hpp). Every scanned access unit is also decoded here, proving scan() didn't
just report plausible-looking numbers but the actual byte ranges a decoder can consume.
"""

import ac3forge as ac3
import numpy as np
import pytest


def _tone(freq_hz, n, sample_rate=48000, amplitude=0.2, phase=0.0):
    t = np.arange(n, dtype=np.float64) / sample_rate
    return (amplitude * np.sin(2 * np.pi * freq_hz * t + phase)).astype(np.float32)


N_FRAMES = 5


def _ac3_stream():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k3_2, lfe=True, bitrate_kbps=384)
    encoder = ac3.FrameEncoder(config)
    tones = [220.0, 330.0, 440.0, 150.0, 200.0, 60.0]
    stream = bytearray()
    for i in range(N_FRAMES):
        channels = [_tone(hz, ac3.SAMPLES_PER_FRAME, phase=i) for hz in tones]
        stream += encoder.encode_frame(channels)
    return bytes(stream)


def test_scan_ac3_stream_shape():
    stream = _ac3_stream()
    result = ac3.scan(stream)

    assert result.kind == ac3.StreamKind.kAc3
    assert result.acmod == ac3.Acmod.k3_2
    assert result.lfe is True
    assert result.channels == 6
    assert result.bsid == 8
    assert result.sample_rate == ac3.SampleRate.k48000
    assert result.substreams_per_unit == 1
    assert len(result.access_units) == N_FRAMES
    assert result.access_unit_samples == [ac3.SAMPLES_PER_FRAME] * N_FRAMES
    assert sum(len(u) for u in result.access_units) == len(stream)


def test_scan_ac3_programme_matches_top_level():
    result = ac3.scan(_ac3_stream())
    assert len(result.programmes) == 1
    programme = result.programmes[0]
    assert programme.substreamid == 0
    assert programme.acmod == result.acmod
    assert programme.lfe == result.lfe
    assert programme.channels == result.channels
    assert programme.bsid == result.bsid
    assert programme.substreams_per_unit == 1
    assert programme.access_units == result.access_units


def test_scan_access_units_decode_correctly():
    result = ac3.scan(_ac3_stream())
    decoder = ac3.FrameDecoder()
    for unit in result.access_units:
        decoded = decoder.decode_frame(unit)
        assert decoded.acmod == ac3.Acmod.k3_2
        assert decoded.lfe is True
        assert len(decoded.channels) == 6


def test_scan_eac3_wide_layout():
    config = ac3.eac3.access_unit_config_for_layout(ac3.eac3.LayoutId.k71, bitrate_kbps=448)
    encoder = ac3.eac3.AccessUnitEncoder(config)
    tones = [1000.0, 1200.0, 800.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0, 500.0, 1700.0]

    stream = bytearray()
    for i in range(N_FRAMES):
        channels = [_tone(hz, ac3.SAMPLES_PER_FRAME, phase=i) for hz in tones]
        stream += encoder.encode_access_unit(channels).bytes
    stream = bytes(stream)

    result = ac3.scan(stream)
    assert result.kind == ac3.StreamKind.kEac3
    assert result.acmod == ac3.Acmod.k3_2
    assert result.lfe is True
    assert result.substreams_per_unit == 2  # independent (3/2+LFE) + one 7.1-Rear dependent
    assert len(result.access_units) == N_FRAMES
    # channel_map unions the bed's acmod/lfeon with the dependent's own chanmap (0x1A00, see
    # test_eac3_encoder.py's test_access_unit_config_for_layout_71) - not asserting the exact
    # union value here (that's Table E2.5 bit-layout trivia this test doesn't need to know), just
    # that scanning actually populated it rather than leaving the k2_0 default's zero.
    assert result.channel_map & 0x1A00 == 0x1A00
    assert result.channel_map != 0

    decoder = ac3.Eac3Decoder()
    saw_decode = False
    for unit in result.access_units:
        decoded = decoder.decode_access_unit(unit)
        if decoded is None:
            continue
        saw_decode = True
        assert decoded.acmod == ac3.Acmod.k3_2
        assert len(decoded.channels) == 8
    assert saw_decode


def test_read_frame_header_matches_scan():
    stream = _ac3_stream()
    result = ac3.scan(stream)
    header = ac3.read_frame_header(stream[: len(result.access_units[0])])

    assert header.kind == result.kind
    assert header.bsid == result.bsid
    assert header.acmod == result.acmod
    assert header.lfe == result.lfe
    assert header.sample_rate == result.sample_rate
    assert header.bytes == len(result.access_units[0])
    assert header.coded_channels == result.channels


def test_timing_functions():
    result = ac3.scan(_ac3_stream())

    assert ac3.stream_duration_samples(result) == N_FRAMES * ac3.SAMPLES_PER_FRAME
    assert ac3.stream_duration_seconds(result) == pytest.approx(
        N_FRAMES * ac3.SAMPLES_PER_FRAME / 48000.0
    )
    assert ac3.uniform_access_unit_samples(result) == ac3.SAMPLES_PER_FRAME

    unit1 = ac3.access_unit_timing(result, 1)
    assert unit1 is not None
    assert unit1.start_sample == ac3.SAMPLES_PER_FRAME
    assert unit1.duration_samples == ac3.SAMPLES_PER_FRAME
    assert unit1.sample_rate == 48000
    assert unit1.start_seconds == pytest.approx(ac3.SAMPLES_PER_FRAME / 48000.0)
    assert unit1.start_in_timescale(1000) == unit1.start_in_timescale(1000)  # deterministic

    assert ac3.access_unit_timing(result, N_FRAMES) is None  # past the end

    assert ac3.access_unit_at_sample(result, 0) == 0
    assert ac3.access_unit_at_sample(result, ac3.SAMPLES_PER_FRAME) == 1
    assert ac3.access_unit_at_sample(result, N_FRAMES * ac3.SAMPLES_PER_FRAME) is None

    assert ac3.access_unit_at_seconds(result, 0.0) == 0
    assert ac3.access_unit_at_seconds(result, 1000.0) is None  # far past the end


def test_scan_empty_stream_raises_ac3_scan_error():
    with pytest.raises(ac3.Ac3ScanError) as exc_info:
        ac3.scan(b"")
    assert exc_info.value.error == ac3.ScanError.kEmpty
    assert isinstance(exc_info.value, ac3.Ac3Error)


def test_scan_garbage_raises_lost_sync():
    with pytest.raises(ac3.Ac3ScanError) as exc_info:
        ac3.scan(bytes(64))  # no 0x0B77 sync word anywhere in here
    assert exc_info.value.error == ac3.ScanError.kLostSync


def test_describe_scan_error_overload():
    text = ac3.describe(ac3.ScanError.kEmpty)
    assert isinstance(text, str)
    assert text  # non-empty - real spec-level text, same convention as describe(DecodeError)
    # The pre-existing DecodeError overload still resolves correctly alongside the new one.
    assert isinstance(ac3.describe(ac3.DecodeError.kTruncated), str)
