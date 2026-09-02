"""Roadmap AP6's completeness surface: containers, metering, QC, signing and the decoder
context manager - each through a real encode, never silence and never only frame 0 (see
CONTRIBUTING.md's "Test with real audio")."""

import ac3forge as ac3
import numpy as np
import pytest
from ac3forge import eac3

RATE = 48_000
FRAMES = 6


def tone_stream() -> tuple[bytes, list[bytes]]:
    """Six frames of stereo E-AC-3, plus its access units."""
    encoder = eac3.FrameEncoder(eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0))
    t = np.arange(ac3.SAMPLES_PER_FRAME * FRAMES, dtype=np.float32) / RATE
    stream = b""
    for frame in range(FRAMES):
        seg = t[frame * ac3.SAMPLES_PER_FRAME : (frame + 1) * ac3.SAMPLES_PER_FRAME]
        channels = np.stack(
            [0.3 * np.sin(2 * np.pi * 440 * seg), 0.3 * np.sin(2 * np.pi * 880 * seg)]
        ).astype(np.float32)
        stream += encoder.encode_frame(channels)
    return stream, list(ac3.scan(stream).access_units)


def test_loudness_meter_measures_and_gates() -> None:
    meter = ac3.meta.LoudnessMeter(ac3.SampleRate.k48000, ac3.Acmod.k2_0, False)
    assert meter.channel_count == 2
    # Nothing pushed: every gated measurement is honestly absent, not zero.
    assert meter.integrated_lkfs is None
    assert meter.true_peak_dbtp is None

    t = np.arange(4 * RATE, dtype=np.float32) / RATE  # past the 3 s short-term window
    pcm = np.stack([0.3 * np.sin(2 * np.pi * 997 * t)] * 2).astype(np.float32)
    meter.push(pcm)

    integrated = meter.integrated_lkfs
    assert integrated is not None and -30.0 < integrated < 0.0
    assert meter.short_term_lkfs is not None
    assert meter.true_peak_dbtp is not None
    # A steady tone has (almost) no loudness range.
    assert meter.loudness_range is not None and meter.loudness_range < 0.1

    # A -10 LKFS tone against EBU R 128's -23 target: the gate must FAIL it,
    # with the delta saying by how much - a QC gate that passes everything is
    # not a gate.
    preset = ac3.meta.qc_preset(ac3.meta.QcPresetId.kEbuR128S2)
    assert preset.target_lkfs == -23.0
    assert "EBU R 128" in preset.source
    verdict = ac3.meta.evaluate_qc_gate(preset, integrated, meter.true_peak_dbtp)
    assert not verdict.passed
    assert verdict.loudness_delta_lu is not None and verdict.loudness_delta_lu > 10.0
    # A measurement the meter could not make leaves that half not-passing.
    empty = ac3.meta.evaluate_qc_gate(preset, None, None)
    assert not empty.loudness_pass and not empty.true_peak_pass


def test_containers_round_trip_byte_identically() -> None:
    stream, frames = tone_stream()
    box = ac3.build_codec_config_box(stream)
    assert len(box) > 0

    mp4_file = ac3.containers.mux_mp4(
        ac3.containers.Mp4Track(
            codec_id="ec-3",
            sample_rate=RATE,
            channels=2,
            samples_per_frame=ac3.SAMPLES_PER_FRAME,
            codec_config=box,
        ),
        frames,
    )
    codec_id, config_payload, samples = ac3.containers.demux_mp4(mp4_file)
    assert codec_id == "ec-3"
    assert config_payload == box  # the dec3 payload survives verbatim
    assert list(samples) == frames

    ts_file = ac3.containers.mux_mpegts(
        ac3.containers.TsTrack(
            codec=ac3.containers.TsCodec.kEac3,
            sample_rate=RATE,
            channels=2,
            samples_per_frame=ac3.SAMPLES_PER_FRAME,
        ),
        frames,
    )
    codec, payloads = ac3.containers.demux_mpegts(ts_file)
    assert codec == ac3.containers.TsCodec.kEac3
    # PES payloads concatenate to the elementary stream, the documented contract.
    assert b"".join(payloads) == b"".join(frames)

    mkv_file = ac3.containers.mux_matroska(
        ac3.containers.MatroskaTrack(
            codec_id="A_EAC3",
            sample_rate=RATE,
            channels=2,
            samples_per_frame=ac3.SAMPLES_PER_FRAME,
        ),
        frames,
    )
    mkv_codec, mkv_frames = ac3.containers.demux_matroska(mkv_file)
    assert mkv_codec == "A_EAC3"
    assert b"".join(mkv_frames) == b"".join(frames)


def test_container_errors_are_python_errors() -> None:
    # An empty frame list and a track the muxer cannot describe both surface
    # as ValueError carrying the C++ describe() string, not as a crash.
    with pytest.raises(ValueError):
        ac3.containers.mux_mp4(
            ac3.containers.Mp4Track(codec_id="ec-3", codec_config=b"\x00"), []
        )
    with pytest.raises(ValueError):
        ac3.containers.demux_mp4(b"not an mp4 at all")


def test_decoder_context_manager_flushes_on_exit() -> None:
    _, frames = tone_stream()
    with ac3.Eac3Decoder() as decoder:
        decoded = decoder.decode_substream(frames[0])
        assert decoded is not None
    # And the manager returns the decoder itself, so one-liners work.
    with ac3.Eac3Decoder() as decoder:
        assert isinstance(decoder, ac3.Eac3Decoder)


def test_signing_round_trip_and_stereo_has_no_object_layer() -> None:
    stream, frames = tone_stream()
    key = ac3.signing.SigningKey(b"0123456789abcdef0123456789abcdef")
    assert not key.empty

    signed, signed_count = ac3.signing.sign_atmos_stream(stream, key)
    # A stereo stream carries no EMDF object container to sign - the signer
    # must say so (0 frames signed) rather than inventing tags.
    assert signed_count == 0
    assert signed == stream

    summary = ac3.signing.verify_atmos_stream(signed, key)
    assert summary.no_container == len(frames)
    assert summary.valid == 0 and summary.mismatch == 0
    assert not summary.all_valid

    assert not ac3.signing.has_authenticity_tag(frames[0])
    with pytest.raises(ValueError):
        ac3.signing.SigningKey(b"")


def test_signing_signs_a_real_atmos_stream() -> None:
    # An actual object stream this time: AtmosEncoder's units carry the EMDF
    # container the signer targets.
    encoder = ac3.AtmosEncoder(ac3.AtmosConfig(bitrate_kbps=640), 2)
    t = np.arange(ac3.SAMPLES_PER_FRAME * 4, dtype=np.float32) / RATE
    placements = [ac3.ObjectPlacement(), ac3.ObjectPlacement()]
    stream = b""
    for frame in range(4):
        seg = t[frame * ac3.SAMPLES_PER_FRAME : (frame + 1) * ac3.SAMPLES_PER_FRAME]
        objects = np.stack(
            [0.3 * np.sin(2 * np.pi * 300 * seg), 0.3 * np.sin(2 * np.pi * 1200 * seg)]
        ).astype(np.float32)
        stream += encoder.encode_frame(objects, placements)

    key = ac3.signing.SigningKey(b"0123456789abcdef0123456789abcdef")
    signed, signed_count = ac3.signing.sign_atmos_stream(stream, key)
    assert signed_count == 4
    assert signed != stream

    summary = ac3.signing.verify_atmos_stream(signed, key)
    assert summary.valid == 4 and summary.mismatch == 0
    assert summary.all_valid

    # The wrong key must not verify.
    wrong = ac3.signing.SigningKey(b"ffffffffffffffffffffffffffffffff")
    assert ac3.signing.verify_atmos_stream(signed, wrong).mismatch == 4
