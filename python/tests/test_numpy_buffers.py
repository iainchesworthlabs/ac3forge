"""Roadmap AP6: zero-copy numpy encode/decode, 2-D planar channel arrays, and the
decode_*_into(out=) caller-buffer forms.

Encode input and decode_*_into's `out` are validated by the binding layer itself (extract_
channel_views/extract_out_views in bindings.cpp) rather than by the C++ side's own assert() -
same "raise, don't rely on a release-build-compiled-out assert" policy PR #409 established for
channel COUNTS (see test_errors.py), extended here to shape/dtype/contiguity/writability. Every
validation-failure case below runs in-process (unlike test_errors.py's subprocess-based count
checks): the C++ decode/encode call is never reached on the invalid path, so there is nothing
left that could crash the interpreter.
"""

import ac3forge as ac3
import numpy as np
import pytest


def _tone(freq_hz, n, sample_rate=48000, amplitude=0.2, phase=0.0):
    t = np.arange(n, dtype=np.float64) / sample_rate
    return (amplitude * np.sin(2 * np.pi * freq_hz * t + phase)).astype(np.float32)


# --- encode: 2-D planar array input, parity with the list-of-1-D-arrays form ------------------


def test_frame_encoder_2d_planar_matches_list_of_arrays():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192)
    channels = [
        _tone(440.0, ac3.SAMPLES_PER_FRAME),
        _tone(523.25, ac3.SAMPLES_PER_FRAME, phase=0.7),
    ]
    planar = np.stack(channels)
    assert planar.shape == (2, ac3.SAMPLES_PER_FRAME)

    from_list = ac3.FrameEncoder(config).encode_frame(channels)
    from_planar = ac3.FrameEncoder(config).encode_frame(planar)
    assert from_list == from_planar


def test_frame_encoder_2d_planar_wrong_channel_count_raises():
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k3_2, lfe=True, bitrate_kbps=384))
    planar = np.zeros((2, ac3.SAMPLES_PER_FRAME), dtype=np.float32)  # 6 channels expected, not 2
    with pytest.raises(ValueError):
        encoder.encode_frame(planar)


def test_frame_encoder_2d_planar_wrong_sample_length_raises():
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0))
    planar = np.zeros((2, 10), dtype=np.float32)
    with pytest.raises(ValueError):
        encoder.encode_frame(planar)


def test_eac3_frame_encoder_2d_planar_matches_list_of_arrays():
    config = ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0)
    channels = [_tone(1000.0, ac3.SAMPLES_PER_FRAME), _tone(800.0, ac3.SAMPLES_PER_FRAME)]
    planar = np.stack(channels)

    from_list = ac3.eac3.FrameEncoder(config).encode_frame(channels)
    from_planar = ac3.eac3.FrameEncoder(config).encode_frame(planar)
    assert from_list == from_planar


def test_access_unit_encoder_2d_planar_matches_list_of_arrays():
    config = ac3.eac3.access_unit_config_for_layout(ac3.eac3.LayoutId.k71, bitrate_kbps=448)
    tones = [1000.0, 1200.0, 800.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0, 500.0, 1700.0]
    channels = [_tone(hz, ac3.SAMPLES_PER_FRAME) for hz in tones]
    planar = np.stack(channels)

    from_list = ac3.eac3.AccessUnitEncoder(config).encode_access_unit(channels).bytes
    from_planar = ac3.eac3.AccessUnitEncoder(config).encode_access_unit(planar).bytes
    assert from_list == from_planar


def test_atmos_encoder_2d_planar_matches_list_of_arrays():
    config = ac3.AtmosConfig(bitrate_kbps=448)
    objects = [_tone(300.0, ac3.SAMPLES_PER_FRAME), _tone(900.0, ac3.SAMPLES_PER_FRAME, phase=0.3)]
    placements = [
        ac3.ObjectPlacement(position=ac3.Position(x=0.2, y=0.5, z=0.0), gain=1.0),
        ac3.ObjectPlacement(position=ac3.Position(x=-0.4, y=0.1, z=0.2), gain=0.8),
    ]
    planar = np.stack(objects)

    from_list = ac3.AtmosEncoder(config, 2).encode_frame(objects, placements)
    from_planar = ac3.AtmosEncoder(config, 2).encode_frame(planar, placements)
    assert from_list == from_planar


# --- decode: .channels/.object_audio are zero-copy read-only views -----------------------------


def test_decoded_frame_channels_are_readonly_views():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192)
    channels = [
        _tone(440.0, ac3.SAMPLES_PER_FRAME),
        _tone(523.25, ac3.SAMPLES_PER_FRAME, phase=0.7),
    ]
    frame = ac3.FrameEncoder(config).encode_frame(channels)
    decoded = ac3.FrameDecoder().decode_frame(frame)

    for arr in decoded.channels:
        assert arr.flags.writeable is False
        assert arr.flags.owndata is False
        assert arr.base is not None
        with pytest.raises(ValueError):
            arr[0] = 1.0  # numpy itself refuses to write through a non-writeable view


def test_decoded_frame_channels_view_survives_parent_deletion():
    # The view's `base` holds a real reference to the DecodedFrame instance (not just its data
    # pointer), so the underlying buffer must stay valid and readable after `decoded` itself goes
    # out of scope - proves this is a genuine keep-alive, not a dangling pointer that happens not
    # to have been reused yet.
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192)
    channels = [
        _tone(440.0, ac3.SAMPLES_PER_FRAME),
        _tone(523.25, ac3.SAMPLES_PER_FRAME, phase=0.7),
    ]
    frame = ac3.FrameEncoder(config).encode_frame(channels)
    decoded = ac3.FrameDecoder().decode_frame(frame)

    view = decoded.channels[0]
    expected = np.array(view)  # an owned copy, taken before `decoded` is dropped
    del decoded
    assert np.array_equal(view, expected)


# --- decode_frame_into / decode_access_unit_into ------------------------------------------------


def test_decode_frame_into_matches_decode_frame():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k3_2, lfe=True, bitrate_kbps=384)
    tones = [(220.0, 0.0), (330.0, 0.1), (440.0, 0.2), (150.0, 0.3), (200.0, 0.4), (60.0, 0.5)]
    channels = [_tone(freq, ac3.SAMPLES_PER_FRAME, phase=phase) for freq, phase in tones]
    frame = ac3.FrameEncoder(config).encode_frame(channels)

    plain = ac3.FrameDecoder().decode_frame(frame)

    out = [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(ac3.MAX_AC3_CHANNELS)]
    into = ac3.FrameDecoder().decode_frame_into(frame, out)

    assert into.acmod == plain.acmod
    assert into.channels == []  # the _into form never populates its own DecodedFrame.channels
    for ch in range(6):
        assert np.array_equal(out[ch], plain.channels[ch])
    # Buffers beyond what this acmod codes are left untouched (still zero).
    for ch in range(6, ac3.MAX_AC3_CHANNELS):
        assert np.all(out[ch] == 0.0)


def test_decode_frame_into_2d_out_matches_decode_frame():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192)
    channels = [
        _tone(440.0, ac3.SAMPLES_PER_FRAME),
        _tone(523.25, ac3.SAMPLES_PER_FRAME, phase=0.7),
    ]
    frame = ac3.FrameEncoder(config).encode_frame(channels)

    plain = ac3.FrameDecoder().decode_frame(frame)
    out = np.zeros((ac3.MAX_AC3_CHANNELS, ac3.SAMPLES_PER_FRAME), dtype=np.float32)
    ac3.FrameDecoder().decode_frame_into(frame, out)

    assert np.array_equal(out[0], plain.channels[0])
    assert np.array_equal(out[1], plain.channels[1])


def test_decode_frame_into_too_few_channel_buffers_raises():
    decoder = ac3.FrameDecoder()
    frame = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0)).encode_frame(
        [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]
    )
    out = [
        np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(ac3.MAX_AC3_CHANNELS - 1)
    ]
    with pytest.raises(ValueError):
        decoder.decode_frame_into(frame, out)


def test_decode_frame_into_wrong_dtype_raises():
    decoder = ac3.FrameDecoder()
    frame = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0)).encode_frame(
        [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]
    )
    out = [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float64) for _ in range(ac3.MAX_AC3_CHANNELS)]
    with pytest.raises(TypeError):
        decoder.decode_frame_into(frame, out)


def test_decode_frame_into_non_writeable_buffer_raises():
    decoder = ac3.FrameDecoder()
    frame = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0)).encode_frame(
        [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]
    )
    out = [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(ac3.MAX_AC3_CHANNELS)]
    out[0].setflags(write=False)
    with pytest.raises(ValueError):
        decoder.decode_frame_into(frame, out)


def test_decode_frame_into_non_contiguous_buffer_raises():
    decoder = ac3.FrameDecoder()
    frame = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0)).encode_frame(
        [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]
    )
    backing = np.zeros(ac3.SAMPLES_PER_FRAME * 2, dtype=np.float32)
    out = [backing[::2]] + [
        np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(ac3.MAX_AC3_CHANNELS - 1)
    ]
    assert not out[0].flags["C_CONTIGUOUS"]
    with pytest.raises(ValueError):
        decoder.decode_frame_into(frame, out)


def test_decode_frame_into_short_buffer_raises():
    decoder = ac3.FrameDecoder()
    frame = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0)).encode_frame(
        [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]
    )
    out = [
        np.zeros(ac3.SAMPLES_PER_FRAME - 1, dtype=np.float32) for _ in range(ac3.MAX_AC3_CHANNELS)
    ]
    with pytest.raises(ValueError):
        decoder.decode_frame_into(frame, out)


def test_decode_access_unit_into_matches_decode_access_unit():
    config = ac3.eac3.access_unit_config_for_layout(ac3.eac3.LayoutId.k71, bitrate_kbps=448)
    encoder = ac3.eac3.AccessUnitEncoder(config)
    tones = [1000.0, 1200.0, 800.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0, 500.0, 1700.0]

    plain_decoder = ac3.Eac3Decoder()
    into_decoder = ac3.Eac3Decoder()
    out = [
        np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32)
        for _ in range(ac3.eac3.MAX_RENDER_CHANNELS)
    ]

    saw_decode = False
    for i in range(3):
        channels = [_tone(hz, ac3.SAMPLES_PER_FRAME, phase=i) for hz in tones]
        unit = encoder.encode_access_unit(channels).bytes

        plain = plain_decoder.decode_access_unit(unit)
        into = into_decoder.decode_access_unit_into(unit, out)
        if plain is None:
            assert into is None
            continue
        assert into is not None
        saw_decode = True
        assert into.channels == []
        assert len(plain.channels) == 8
        for ch in range(8):
            assert np.array_equal(out[ch], plain.channels[ch])

    assert saw_decode


def test_decode_access_unit_into_too_few_channel_buffers_raises():
    config = ac3.eac3.access_unit_config_for_layout(ac3.eac3.LayoutId.k71, bitrate_kbps=448)
    encoder = ac3.eac3.AccessUnitEncoder(config)
    tones = [1000.0, 1200.0, 800.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0, 500.0, 1700.0]
    unit = encoder.encode_access_unit([_tone(hz, ac3.SAMPLES_PER_FRAME) for hz in tones]).bytes

    decoder = ac3.Eac3Decoder()
    out = [
        np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32)
        for _ in range(ac3.eac3.MAX_RENDER_CHANNELS - 1)
    ]
    with pytest.raises(ValueError):
        decoder.decode_access_unit_into(unit, out)
