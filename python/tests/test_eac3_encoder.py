"""ac3.eac3.FrameEncoder/AccessUnitEncoder (roadmap AP6) round trips through ac3.Eac3Decoder,
plus the named-layout convenience (ac3.eac3.access_unit_config_for_layout) and the error paths
KwargBinder/FrameError give every other encoder here.
"""

import ac3forge as ac3
import numpy as np
import pytest


def _tone(freq_hz, n, sample_rate=48000, amplitude=0.3, phase=0.0):
    t = np.arange(n, dtype=np.float64) / sample_rate
    return (amplitude * np.sin(2 * np.pi * freq_hz * t + phase)).astype(np.float32)


N_FRAMES = 3


def test_frame_encoder_roundtrip():
    config = ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0)
    encoder = ac3.eac3.FrameEncoder(config)
    decoder = ac3.Eac3Decoder()

    assert encoder.channel_count == 2
    assert encoder.samples_per_frame == ac3.SAMPLES_PER_FRAME

    rendered = []
    for i in range(N_FRAMES):
        channels = [
            _tone(1000.0, encoder.samples_per_frame, phase=i),
            _tone(800.0, encoder.samples_per_frame, phase=i),
        ]
        frame = encoder.encode_frame(channels)
        assert isinstance(frame, bytes)
        assert len(frame) > 0

        decoded = decoder.decode_substream(frame)
        if decoded is None:
            continue  # transient pre-noise hold-back (§3.7); not exercised by default config
        assert decoded.strmtyp == ac3.StreamType.kIndependent
        assert decoded.acmod == ac3.Acmod.k2_0
        assert len(decoded.channels) == 2
        rendered.append(decoded)

    assert rendered, "expected at least one decoded substream"


def test_frame_encoder_explicit_metadata():
    config = ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0)
    encoder = ac3.eac3.FrameEncoder(config)
    channels = [_tone(1000.0, ac3.SAMPLES_PER_FRAME), _tone(800.0, ac3.SAMPLES_PER_FRAME)]

    metadata = ac3.eac3.FrameMetadata(dynrng=[1, 2, 3, 4, 5, 6], compr=7)
    frame = encoder.encode_frame(channels, metadata=metadata)
    assert isinstance(frame, bytes)
    # KwargBinder round-trips the field back exactly.
    assert metadata.dynrng == [1, 2, 3, 4, 5, 6]
    assert metadata.compr == 7


def test_access_unit_config_for_layout_71():
    config = ac3.eac3.access_unit_config_for_layout(ac3.eac3.LayoutId.k71, bitrate_kbps=448)
    assert config.independent.acmod == ac3.Acmod.k3_2
    assert config.independent.lfe is True
    assert len(config.dependents) == 1
    dependent = config.dependents[0]
    assert dependent.acmod == ac3.Acmod.k2_2
    assert dependent.chanmap == 0x1A00  # Ls, Rs, Lrs, Rrs (Table E2.5) - k71Rear
    assert dependent.bitrate_kbps == 224  # half of the independent's, the documented default


def test_access_unit_encoder_71_roundtrip():
    config = ac3.eac3.access_unit_config_for_layout(ac3.eac3.LayoutId.k71, bitrate_kbps=448)
    encoder = ac3.eac3.AccessUnitEncoder(config)
    decoder = ac3.Eac3Decoder()
    assert encoder.channel_count == 10  # 3/2+LFE (6) + the dependent's 2/2 (4)

    tones = [1000.0, 1200.0, 800.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0, 500.0, 1700.0]
    saw_decode = False
    for i in range(N_FRAMES):
        channels = [_tone(hz, ac3.SAMPLES_PER_FRAME, phase=i) for hz in tones]
        unit = encoder.encode_access_unit(channels)
        assert isinstance(unit.bytes, bytes)
        assert unit.substream_count == 2
        assert sum(unit.substream_bytes) == len(unit.bytes)

        decoded = decoder.decode_access_unit(unit.bytes)
        if decoded is None:
            continue
        saw_decode = True
        assert decoded.acmod == ac3.Acmod.k3_2
        assert decoded.substream_count == 2
        assert len(decoded.channels) == 8  # rendered programme: bed 6 + 2 new height/rear pairs...

    assert saw_decode


def test_frame_encoder_reports_encode_error():
    config = ac3.eac3.FrameConfig(bitrate_kbps=0, acmod=ac3.Acmod.k2_0)  # no such Annex E rate
    encoder = ac3.eac3.FrameEncoder(config)
    channels = [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]

    with pytest.raises(ac3.Ac3EncodeError) as exc_info:
        encoder.encode_frame(channels)
    assert exc_info.value.error == ac3.FrameError.kInvalidBitrate


def test_access_unit_encoder_reports_channel_map_error():
    independent = ac3.eac3.FrameConfig(acmod=ac3.Acmod.k3_2, lfe=True, bitrate_kbps=448)
    # A 2/0 dependent (2 coded channels) with a chanmap naming four locations.
    dependent = ac3.eac3.FrameConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192, chanmap=0x0014)
    config = ac3.eac3.AccessUnitConfig(independent=independent, dependents=[dependent])
    encoder = ac3.eac3.AccessUnitEncoder(config)

    # ac3::eac3::AccessUnitEncoder's own constructor validates eagerly and builds no substreams
    # for an invalid config - channel_count is 0, and encode_access_unit() is how the real reason
    # (an invalid channel map) surfaces.
    assert encoder.channel_count == 0
    with pytest.raises(ac3.Ac3EncodeError) as exc_info:
        encoder.encode_access_unit([])
    assert exc_info.value.error == ac3.FrameError.kInvalidChannelMap


def test_frame_config_rejects_unknown_kwarg():
    with pytest.raises(TypeError):
        ac3.eac3.FrameConfig(bitrat_kbps=192)


def test_frame_encoder_latency():
    config = ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0)
    encoder = ac3.eac3.FrameEncoder(config)
    assert encoder.latency_samples == encoder.latency.total_samples
    assert encoder.latency.holdback_samples == 0  # transient_prenoise defaults off

    config.transient_prenoise = True
    encoder2 = ac3.eac3.FrameEncoder(config)
    assert encoder2.latency.holdback_samples == ac3.SAMPLES_PER_FRAME
