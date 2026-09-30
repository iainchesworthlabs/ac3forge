"""AC-4's failures are typed: Ac4DecodeError for a frame that will not decode and Ac4EncodeError
for a configuration or an input the encoder refuses, both under Ac4Error, which is a ValueError:
what ac3.ac4 raised before they had types of their own, so nothing that catches ValueError stops
catching them. Each carries the C++ enumerator as `.error`, as test_errors.py checks of the AC-3
hierarchy (Ac3Error is a RuntimeError, which is why these are not under it).
"""

import iclforge as ac3
import numpy as np
import pytest

ac4 = ac3.ac4

FRAME = 2048


def test_the_hierarchy_and_the_exports():
    assert issubclass(ac3.Ac4Error, ValueError)
    assert issubclass(ac3.Ac4DecodeError, ac3.Ac4Error)
    assert issubclass(ac3.Ac4EncodeError, ac3.Ac4Error)
    assert not issubclass(ac3.Ac4Error, ac3.Ac3Error)  # a ValueError, not a RuntimeError
    assert not issubclass(ac3.Ac4EncodeError, ac3.Ac4DecodeError)
    for name in ("Ac4Error", "Ac4DecodeError", "Ac4EncodeError"):
        assert name in ac3.__all__
        assert getattr(ac3, name).__name__ == name


def test_an_encoder_the_configuration_refuses_raises_ac4_encode_error():
    config = ac4.EncoderConfig(channels=3)  # not a channel count the encoder writes
    reason = ac4.Encoder.refusal_reason(config)
    assert reason != ""
    with pytest.raises(ac3.Ac4EncodeError) as excinfo:
        ac4.Encoder.create(config)
    assert str(excinfo.value) == reason
    assert excinfo.value.error == ac4.EncodeError.kInvalidConfig
    assert isinstance(excinfo.value, ac3.Ac4Error)
    # What `except ValueError` caught before still catches it.
    with pytest.raises(ValueError):
        ac4.Encoder.create(config)


def test_input_the_encoder_refuses_raises_ac4_encode_error():
    encoder = ac4.Encoder.create(ac4.EncoderConfig())
    one_channel = [np.zeros(FRAME, dtype=np.float32)]  # a stereo encoder given one channel
    with pytest.raises(ac3.Ac4EncodeError) as excinfo:
        encoder.encode(one_channel)
    assert excinfo.value.error == ac4.EncodeError.kInvalidInput
    assert str(excinfo.value) != ""
    encoder.flush()
    with pytest.raises(ac3.Ac4EncodeError) as excinfo:  # a flushed encoder takes no input
        encoder.encode([np.zeros(FRAME, dtype=np.float32)] * 2)
    assert excinfo.value.error == ac4.EncodeError.kInvalidInput


def test_a_frame_that_will_not_decode_raises_ac4_decode_error():
    t = np.arange(2 * FRAME, dtype=np.float64) / 48_000
    tone = (0.3 * np.sin(2 * np.pi * 1000 * t)).astype(np.float32)
    encoder = ac4.Encoder.create(ac4.EncoderConfig(iframe_interval=1))
    frames = list(encoder.encode([tone, tone])) + list(encoder.flush())
    corrupted = bytearray(frames[0].data)
    for i in range(4):
        corrupted[i] ^= 0xFF
    decoder = ac4.Decoder()
    with pytest.raises(ac3.Ac4DecodeError) as excinfo:
        decoder.decode(bytes(corrupted))
    assert isinstance(excinfo.value.error, ac4.DecodeError)
    assert str(excinfo.value) != ""
    assert isinstance(excinfo.value, ac3.Ac4Error)
    with pytest.raises(ValueError):
        ac4.Decoder().decode(bytes(corrupted))
    # A decoder that has read a good frame still decodes the next one.
    assert ac4.Decoder().decode(frames[0].data) is not None


def test_a_misshapen_argument_is_still_a_plain_value_error():
    # A channel that is not a 1-D array is a usage error of this binding, not the encoder's
    # refusal of anything: a ValueError that is not an Ac4Error.
    encoder = ac4.Encoder.create(ac4.EncoderConfig())
    with pytest.raises(ValueError) as excinfo:
        encoder.encode([np.zeros((2, 2), dtype=np.float32), np.zeros((2, 2), dtype=np.float32)])
    assert not isinstance(excinfo.value, ac3.Ac4Error)
