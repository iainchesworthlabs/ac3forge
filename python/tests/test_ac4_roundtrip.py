"""AC-4 encode -> decode round trip through the Python bindings (ac3.ac4).

Same validation discipline as test_ac3_roundtrip.py (see CONTRIBUTING.md): real tone content, not
silence or DC, distinct per channel, so a channel-order bug would show up as a failed correlation
rather than passing by coincidence; several frames, not one. Unlike AC-3, AC-4's encoder and
decoder each carry a real, documented delay (Encoder.delay_samples, Encoder.decoder_delay_samples -
see ac4enc/include/ac4enc/encoder.hpp's own comment on how the two relate), so this test aligns by
that amount before comparing rather than searching a small window blind; _best_correlation's own
small +/-64 sample search then only has to cover rounding, not the delay itself.

This is a binding smoke test, not a codec correctness suite - that lives in this project's C++
Catch2 suite (tests/ac4). It only has to show the Python surface carries a real signal through.
"""

import ac3forge as ac3
import numpy as np
import pytest

ac4 = ac3.ac4

# frame_rate_index 13: the 2048-sample frame, the one index this decoder needs no internal sample
# rate converter for (ac4enc/encoder.hpp), so the encoder/decoder delay figures the test aligns by
# are exact sample counts rather than "to the nearest sample".
FRAME_RATE_INDEX = 13
FRAME_LEN = 2048
SAMPLE_RATE_HZ = 48000
N_FRAMES = 8


def _tone(freq_hz, n, sample_rate=SAMPLE_RATE_HZ, amplitude=0.2, phase=0.0):
    t = np.arange(n, dtype=np.float64) / sample_rate
    return (amplitude * np.sin(2 * np.pi * freq_hz * t + phase)).astype(np.float32)


def _best_correlation(ref, test, max_shift=64):
    best = -1.0
    for shift in range(-max_shift, max_shift + 1):
        if shift >= 0:
            a, b = ref[: len(ref) - shift], test[shift:]
        else:
            a, b = ref[-shift:], test[: len(test) + shift]
        if len(a) < 256:
            continue
        a = a - a.mean()
        b = b - b.mean()
        denom = np.sqrt(np.sum(a * a) * np.sum(b * b))
        if denom < 1e-12:
            continue
        best = max(best, float(np.sum(a * b) / denom))
    return best


def _encode_decode(channels, channel_tones, bitrate_kbps):
    """Encode N_FRAMES worth of distinct tones and decode every frame produced.

    Returns (originals, decoded_full, delay): `decoded_full` is one concatenated array per
    channel, and `delay` (Encoder.delay_samples + Encoder.decoder_delay_samples) is how far into
    it the corresponding original sample lands - the exact relationship
    ac4enc/include/ac4enc/encoder.hpp documents for delay_samples()/decoder_delay_samples().
    """
    config = ac4.EncoderConfig(
        channels=channels,
        sample_rate_hz=SAMPLE_RATE_HZ,
        frame_rate_index=FRAME_RATE_INDEX,
        bitrate_kbps=bitrate_kbps,
        iframe_interval=1,
    )
    encoder = ac4.Encoder.create(config)
    decoder = ac4.Decoder()
    assert encoder.codec_mode != ac4.CodecMode.kAuto

    total = N_FRAMES * FRAME_LEN
    originals = [_tone(freq, total, phase=phase) for freq, phase in channel_tones]

    decoded_channels = [[] for _ in originals]
    saw_iframe = False
    for i in range(N_FRAMES):
        start, end = i * FRAME_LEN, (i + 1) * FRAME_LEN
        frames = encoder.encode([o[start:end] for o in originals])
        for frame in frames:
            assert isinstance(frame.data, bytes)
            assert len(frame.data) > 0
            saw_iframe = saw_iframe or frame.iframe
            decoded = decoder.decode(frame.data)
            if decoded is None:
                continue
            assert len(decoded.channels) == len(originals)
            for ch, buf in enumerate(decoded.channels):
                decoded_channels[ch].append(np.asarray(buf))
    for frame in encoder.flush():
        decoded = decoder.decode(frame.data)
        if decoded is not None:
            for ch, buf in enumerate(decoded.channels):
                decoded_channels[ch].append(np.asarray(buf))

    assert saw_iframe, "expected at least one I-frame (iframe_interval=1)"

    decoded_full = [
        np.concatenate(chan) if chan else np.zeros(0, dtype=np.float32) for chan in decoded_channels
    ]
    delay = encoder.delay_samples + encoder.decoder_delay_samples
    return originals, decoded_full, delay


def test_stereo_roundtrip_distinct_channels():
    originals, decoded, delay = _encode_decode(2, [(440.0, 0.0), (523.25, 0.7)], 192)

    # Capped at the input's own length: flush() drains the encoder's end-of-stream delay as
    # extra trailing frames, so the decoded stream runs longer than the original input - compare
    # only over the range where a real original sample exists.
    usable = min(min(len(d) for d in decoded) - delay, len(originals[0]))
    assert usable > FRAME_LEN, "expected several samples of decoded, delay-aligned output"
    for ch, original in enumerate(originals):
        aligned = decoded[ch][delay : delay + usable]
        corr = _best_correlation(original[:usable], aligned)
        assert corr > 0.9, f"channel {ch}: correlation {corr:.3f}"


def test_51_roundtrip_channel_order():
    # EncoderConfig.channels' documented 5.1 order: L R C LFE Ls Rs.
    tones = [
        (220.0, 0.0),  # L
        (330.0, 0.1),  # R
        (440.0, 0.2),  # C
        (60.0, 0.0),  # LFE
        (150.0, 0.3),  # Ls
        (200.0, 0.4),  # Rs
    ]
    originals, decoded, delay = _encode_decode(6, tones, 384)

    # Capped at the input's own length: flush() drains the encoder's end-of-stream delay as
    # extra trailing frames, so the decoded stream runs longer than the original input - compare
    # only over the range where a real original sample exists.
    usable = min(min(len(d) for d in decoded) - delay, len(originals[0]))
    assert usable > FRAME_LEN, "expected several samples of decoded, delay-aligned output"
    for ch, original in enumerate(originals):
        aligned = decoded[ch][delay : delay + usable]
        corr = _best_correlation(original[:usable], aligned)
        assert corr > 0.9, f"channel {ch}: correlation {corr:.3f}"


def test_decoder_reports_presentation_and_delay_agree_with_encoder():
    _originals, _decoded, _delay = _encode_decode(2, [(440.0, 0.0), (523.25, 0.7)], 192)
    # Re-run with direct access to the encoder/decoder to check the surface beyond audio content.
    config = ac4.EncoderConfig(
        channels=2, sample_rate_hz=SAMPLE_RATE_HZ, frame_rate_index=FRAME_RATE_INDEX,
        bitrate_kbps=192, iframe_interval=1,
    )
    encoder = ac4.Encoder.create(config)
    decoder = ac4.Decoder()

    tones = [_tone(440.0, FRAME_LEN), _tone(523.25, FRAME_LEN, phase=0.7)]
    decoded_once = False
    for frame in encoder.encode(tones):
        decoded = decoder.decode(frame.data)
        if decoded is None:
            continue
        decoded_once = True
        assert decoded.presentation_id is None or isinstance(decoded.presentation_id, int)
        assert decoder.latency_samples == encoder.decoder_delay_samples
        presentations = decoder.presentations
        assert len(presentations) >= 1
        assert presentations[0].decodable is True

    toc = encoder.toc
    assert isinstance(toc.build_dac4(), bytes)
    assert toc.dac4_refusal() == ""
    assert toc.samples_per_frame() == FRAME_LEN

    if not decoded_once:
        # This encoder/decoder pairing (frame_rate_index 13, iframe_interval=1) should never need
        # more than one frame to produce output - if it does, that is itself worth failing loudly
        # on rather than silently passing an empty test.
        pytest.fail("expected the first frame to decode with iframe_interval=1")


def test_prove_correlation_check_can_fail():
    # CONTRIBUTING.md's "prove the test can fail" discipline: comparing a channel against pure
    # noise (rather than its own decoded reconstruction) must NOT pass the same correlation bar.
    original = _tone(440.0, FRAME_LEN)
    rng = np.random.default_rng(0)
    noise = (0.2 * rng.standard_normal(FRAME_LEN)).astype(np.float32)
    assert _best_correlation(original, noise) < 0.5


def test_encoder_create_rejects_unsupported_channel_count():
    # 4 input channels is not a valid EncoderConfig.channels value under any configuration this
    # binding exposes (1/2/5/6/9/10 need no experimental flag; 7/8/11/12 need experimental flags
    # this binding does not expose at all - see ac4enc/include/ac4enc/encoder.hpp).
    with pytest.raises(ValueError):
        ac4.Encoder.create(ac4.EncoderConfig(channels=4, sample_rate_hz=SAMPLE_RATE_HZ,
                                             frame_rate_index=FRAME_RATE_INDEX))


def test_sync_frame_wraps_with_sync_word():
    config = ac4.EncoderConfig(
        channels=2, sample_rate_hz=SAMPLE_RATE_HZ, frame_rate_index=FRAME_RATE_INDEX,
        bitrate_kbps=192, iframe_interval=1,
    )
    encoder = ac4.Encoder.create(config)
    tones = [_tone(440.0, FRAME_LEN), _tone(523.25, FRAME_LEN, phase=0.7)]
    frames = encoder.encode(tones)
    assert frames, "expected at least one frame from a full frame's worth of input"

    wrapped = ac4.sync_frame(frames[0].data, False)
    assert isinstance(wrapped, bytes)
    # Annex G.3.1: sync_word (0xAC40, no crc_word) then frame_size then the raw frame.
    assert wrapped[0:2] == b"\xac\x40"
    assert wrapped.endswith(frames[0].data)
