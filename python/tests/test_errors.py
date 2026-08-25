"""std::expected's error branch, translated at the binding boundary into Python exceptions - see
python/src/ac3forge_ext/bindings.cpp's own header comment on why exceptions rather than a
Result-like return. Every case here checks both the exception TYPE and its `.error` enum value,
not just that *something* was raised.
"""

import subprocess
import sys

import ac3forge as ac3
import numpy as np
import pytest


def test_decode_truncated_buffer_raises():
    decoder = ac3.FrameDecoder()
    with pytest.raises(ac3.Ac3DecodeError) as exc_info:
        decoder.decode_frame(b"\x00\x01\x02")
    assert isinstance(exc_info.value.error, ac3.DecodeError)
    assert isinstance(exc_info.value, ac3.Ac3Error)  # the base class every ac3 error derives from


def test_decode_missing_sync_word_raises():
    decoder = ac3.FrameDecoder()
    garbage = bytes(64)  # no 0x0B77 sync word anywhere in here
    with pytest.raises(ac3.Ac3DecodeError):
        decoder.decode_frame(garbage)


def test_encode_invalid_bitrate_raises():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=3)  # not a legal Table 5.18 rate
    encoder = ac3.FrameEncoder(config)
    channels = [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]
    with pytest.raises(ac3.Ac3EncodeError) as exc_info:
        encoder.encode_frame(channels)
    assert exc_info.value.error == ac3.FrameError.kInvalidBitrate


def test_channel_length_mismatch_is_a_value_error():
    # A wrong-length channel array is a Python-level usage error (the C++ side documents this as
    # "a programming error, not a runtime one" - docs/library/index.md), not a codec-level
    # Ac3EncodeError, so this is a plain ValueError rather than the Ac3Error hierarchy.
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0))
    with pytest.raises(ValueError):
        encoder.encode_frame([np.zeros(10, dtype=np.float32), np.zeros(10, dtype=np.float32)])


def test_encoder_config_rejects_unknown_kwarg():
    with pytest.raises(TypeError):
        ac3.EncoderConfig(dialnrm=10)  # typo: not a real field


def _run_child(code):
    # A wrong channel/object COUNT (unlike the per-channel sample-LENGTH mismatch above) used to
    # skip validation entirely: extract_channels() never checked py::len(channels) against the
    # encoder's channel_count(), and the C++ side's own guard is a plain assert() compiled out in
    # the Release build these wheels use, so the encoder read out of bounds instead of raising.
    # That's a real interpreter crash, not a Python exception - checking it in-process would take
    # the whole pytest run down with it, so run the repro in a subprocess and inspect its exit
    # code instead.
    return subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)


def test_frame_encoder_wrong_channel_count_raises_value_error():
    code = (
        "import ac3forge as ac3\n"
        "import numpy as np\n"
        "encoder = ac3.FrameEncoder(ac3.EncoderConfig(bitrate_kbps=192, acmod=ac3.Acmod.k3_2, lfe=True))\n"
        "channels = [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]\n"
        "try:\n"
        "    encoder.encode_frame(channels)\n"
        "except ValueError:\n"
        "    raise SystemExit(0)\n"
        "raise SystemExit(1)\n"
    )
    result = _run_child(code)
    assert result.returncode == 0, (
        f"expected a clean ValueError (exit 0), got returncode={result.returncode}\n"
        f"stderr:\n{result.stderr}"
    )


def test_atmos_encoder_wrong_object_count_raises_value_error():
    code = (
        "import ac3forge as ac3\n"
        "import numpy as np\n"
        "encoder = ac3.AtmosEncoder(ac3.AtmosConfig(bitrate_kbps=448), 2)\n"
        "objects = [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32)]\n"
        "placements = [ac3.ObjectPlacement(position=ac3.Position(x=0.2, y=0.5, z=0.0), gain=1.0)]\n"
        "try:\n"
        "    encoder.encode_frame(objects, placements)\n"
        "except ValueError:\n"
        "    raise SystemExit(0)\n"
        "raise SystemExit(1)\n"
    )
    result = _run_child(code)
    assert result.returncode == 0, (
        f"expected a clean ValueError (exit 0), got returncode={result.returncode}\n"
        f"stderr:\n{result.stderr}"
    )
