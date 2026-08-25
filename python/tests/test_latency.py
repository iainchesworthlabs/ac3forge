"""The latency budget (roadmap PF6) through the Python bindings.

The numbers themselves are established empirically on the C++ side
(tests/decoder/test_latency.cpp: an impulse and a tone burst through a real
encode -> decode, located to the sample by two independent methods). What is
checked here is that the bindings hand the same budget across, and - for the
one term that is a sample-domain shift - that a round trip driven entirely
from Python really does move the signal by exactly that many samples.
"""

import ac3forge as ac3
import numpy as np


def _impulse(n, at, amplitude=0.9):
    pcm = np.zeros(n, dtype=np.float32)
    pcm[at] = amplitude
    return pcm


N_FRAMES = 8
IMPULSE_AT = 3 * ac3.SAMPLES_PER_FRAME + 512


def test_ac3_encoder_reports_the_documented_budget():
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192))
    budget = encoder.latency

    assert budget.frame_samples == ac3.SAMPLES_PER_FRAME
    assert budget.transform_samples == ac3.TRANSFORM_DELAY_SAMPLES
    assert budget.lookahead_samples == 0
    assert budget.holdback_samples == 0
    assert budget.total_samples == 1792
    assert encoder.latency_samples == 1792
    assert 37.3 < budget.milliseconds(ac3.SampleRate.k48000) < 37.4
    # The coded rate is really read, not assumed to be 48 kHz.
    assert 40.6 < budget.milliseconds(ac3.SampleRate.k44100) < 40.7
    assert "total=1792" in repr(budget)


def test_ac3_round_trip_shifts_the_signal_by_the_transform_term():
    total = N_FRAMES * ac3.SAMPLES_PER_FRAME
    pcm = _impulse(total, IMPULSE_AT)

    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k1_0, bitrate_kbps=448))
    decoder = ac3.FrameDecoder()
    out = []
    for frame in range(N_FRAMES):
        block = pcm[frame * ac3.SAMPLES_PER_FRAME : (frame + 1) * ac3.SAMPLES_PER_FRAME]
        decoded = decoder.decode_frame(encoder.encode_frame([block]))
        out.append(np.asarray(decoded.channels[0], dtype=np.float32))
    reconstructed = np.concatenate(out)

    assert reconstructed.size == total
    assert int(np.argmax(np.abs(reconstructed))) == IMPULSE_AT + encoder.latency.transform_samples
    # The decoder adds nothing of its own on top of that.
    assert decoder.latency_samples == 0


def test_atmos_object_path_costs_a_second_transform_overlap():
    encoder = ac3.AtmosEncoder(ac3.AtmosConfig(bitrate_kbps=448), 2)
    assert encoder.bed_latency.transform_samples == ac3.TRANSFORM_DELAY_SAMPLES
    assert encoder.latency.transform_samples == 2 * ac3.TRANSFORM_DELAY_SAMPLES
    assert (encoder.latency_samples
            == encoder.bed_latency.total_samples + ac3.TRANSFORM_DELAY_SAMPLES)

    config = ac3.AtmosConfig(bitrate_kbps=448)
    config.emit_object_metadata = False
    plain = ac3.AtmosEncoder(config, 2)
    assert plain.latency.transform_samples == ac3.TRANSFORM_DELAY_SAMPLES


def test_eac3_decoder_reports_no_holdback_before_it_has_decoded_anything():
    assert ac3.Eac3Decoder().latency_samples == 0
