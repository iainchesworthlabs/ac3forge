"""Entries of __init__.pyi no other test in this suite touched: the enum helpers, the DRC
profile/heavy-compression value types, the per-programme metadata (dialnorm2, blksw, mix levels),
the framing helpers and their error paths, read_frame_header's and scan()'s descriptor fields,
access-unit timing conversions, LatencyBudget.milliseconds and the QC presets.

Same discipline as the rest of python/tests: real tones, several frames, specific exception types.
"""

import math

import ac3forge as ac3
import numpy as np
import pytest

SPF = ac3.SAMPLES_PER_FRAME

ALL_ACMODS = [
    (ac3.Acmod.kDualMono, 2),
    (ac3.Acmod.k1_0, 1),
    (ac3.Acmod.k2_0, 2),
    (ac3.Acmod.k3_0, 3),
    (ac3.Acmod.k2_1, 3),
    (ac3.Acmod.k3_1, 4),
    (ac3.Acmod.k2_2, 4),
    (ac3.Acmod.k3_2, 5),
]


def _tone(freq_hz, frame_index, sample_rate=48000, amplitude=0.3):
    n = np.arange(frame_index * SPF, (frame_index + 1) * SPF, dtype=np.float64)
    return (amplitude * np.sin(2 * np.pi * freq_hz * n / sample_rate)).astype(np.float32)


def _rms(x):
    return float(np.sqrt(np.mean(np.asarray(x, dtype=np.float64) ** 2)))


def _stereo_ac3_stream(frames=3, **config):
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, **config))
    out = bytearray()
    for i in range(frames):
        out += encoder.encode_frame([_tone(440.0, i), _tone(660.0, i)])
    return bytes(out)


# --- enum helpers ------------------------------------------------------------------------------


@pytest.mark.parametrize(("acmod", "count"), ALL_ACMODS)
def test_fullbw_channel_count(acmod, count):
    assert ac3.fullbw_channel_count(acmod) == count


@pytest.mark.parametrize(
    ("rate", "hz"),
    [
        (ac3.SampleRate.k48000, 48000),
        (ac3.SampleRate.k44100, 44100),
        (ac3.SampleRate.k32000, 32000),
        (ac3.SampleRate.k24000, 24000),
        (ac3.SampleRate.k22050, 22050),
        (ac3.SampleRate.k16000, 16000),
    ],
)
def test_sample_rate_hz(rate, hz):
    assert ac3.sample_rate_hz(rate) == hz


def test_blocks_per_frame_constant():
    assert ac3.BLOCKS_PER_FRAME == 6
    assert ac3.SAMPLES_PER_FRAME == 256 * ac3.BLOCKS_PER_FRAME


def test_version_string():
    assert isinstance(ac3.__version__, str) and ac3.__version__


# --- DRC profiles and heavy compression --------------------------------------------------------

ALL_PROFILES = [
    ac3.ProfileId.kFilmStandard,
    ac3.ProfileId.kFilmLight,
    ac3.ProfileId.kMusicStandard,
    ac3.ProfileId.kMusicLight,
    ac3.ProfileId.kSpeech,
]


def test_profile_names_are_distinct_and_profiles_are_sane():
    names = [ac3.profile_name(p) for p in ALL_PROFILES]
    assert all(isinstance(n, str) and n for n in names)
    assert len(set(names)) == len(names)
    for pid in ALL_PROFILES:
        profile = ac3.profile_for(pid)
        for field in (
            "null_low_db",
            "null_high_db",
            "boost_ratio",
            "max_boost_db",
            "early_cut_ratio",
            "early_cut_end_db",
            "cut_ratio",
            "attack_ms",
            "release_ms",
        ):
            assert math.isfinite(getattr(profile, field)), (pid, field)
        # The null band is a band: its low edge sits at or below its high edge.
        assert profile.null_low_db <= profile.null_high_db
        assert profile.attack_ms > 0 and profile.release_ms > 0


def test_profile_and_heavy_config_kwargs_round_trip():
    profile = ac3.Profile(null_low_db=-35.0, null_high_db=-25.0, attack_ms=5.0, release_ms=500.0)
    assert profile.null_low_db == -35.0
    assert profile.null_high_db == -25.0
    assert profile.attack_ms == 5.0
    assert profile.release_ms == 500.0
    heavy = ac3.HeavyConfig(
        dialogue_target_dbfs=-24.0, peak_ceiling_dbfs=-1.0, release_db_per_second=10.0
    )
    assert (heavy.dialogue_target_dbfs, heavy.peak_ceiling_dbfs, heavy.release_db_per_second) == (
        -24.0,
        -1.0,
        10.0,
    )
    heavy.release_db_per_second = 5.0
    assert heavy.release_db_per_second == 5.0
    with pytest.raises(TypeError):
        ac3.HeavyConfig(release_db_per_secnd=1.0)
    with pytest.raises(TypeError):
        ac3.Profile(atack_ms=1.0)


@pytest.mark.parametrize("pid", ALL_PROFILES)
def test_encoder_with_drc_and_heavy_round_trips(pid):
    config = ac3.EncoderConfig(
        acmod=ac3.Acmod.k3_2,
        lfe=True,
        bitrate_kbps=384,
        dialnorm=24,
        drc=ac3.profile_for(pid),
        heavy=ac3.HeavyConfig(),
        cmixlev=ac3.CentreMixLevel.kMinus6dB,
        surmixlev=ac3.SurroundMixLevel.kSilent,
        coupling=True,
        fast_mdct=False,
    )
    assert config.drc is not None and config.heavy is not None
    assert config.cmixlev == ac3.CentreMixLevel.kMinus6dB
    assert config.surmixlev == ac3.SurroundMixLevel.kSilent
    encoder = ac3.FrameEncoder(config)
    decoder = ac3.FrameDecoder(ac3.DecoderConfig(drc_scale=1.0, heavy_compression=True))
    for i in range(3):
        frame = encoder.encode_frame([_tone(200.0 + 150.0 * c, i) for c in range(6)])
        decoded = decoder.decode_frame(frame)
    assert decoded.dialnorm == 24
    assert decoded.dialnorm2 is None
    # compr is carried when heavy compression was configured.
    assert decoded.compr is not None
    assert len(decoded.dynrng) == ac3.BLOCKS_PER_FRAME
    assert len(decoded.channels) == 6


def test_dual_mono_carries_second_programme_metadata():
    config = ac3.EncoderConfig(
        acmod=ac3.Acmod.kDualMono,
        dialnorm=27,
        dialnorm2=12,
        drc=ac3.profile_for(ac3.ProfileId.kFilmLight),
        drc2=ac3.profile_for(ac3.ProfileId.kSpeech),
        heavy=ac3.HeavyConfig(),
        heavy2=ac3.HeavyConfig(),
    )
    assert config.dialnorm2 == 12
    encoder = ac3.FrameEncoder(config)
    decoder = ac3.FrameDecoder()
    for i in range(3):
        decoded = decoder.decode_frame(encoder.encode_frame([_tone(300.0, i), _tone(3000.0, i)]))
    assert decoded.acmod == ac3.Acmod.kDualMono
    assert decoded.dialnorm == 27
    assert decoded.dialnorm2 == 12
    assert decoded.compr2 is not None
    assert len(decoded.dynrng2) == ac3.BLOCKS_PER_FRAME
    assert _rms(decoded.channels[0]) > 0.1 and _rms(decoded.channels[1]) > 0.1


def test_dual_mono_without_dialnorm2_is_rejected():
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.kDualMono))
    with pytest.raises(ac3.Ac3EncodeError) as exc_info:
        encoder.encode_frame([_tone(300.0, 0), _tone(600.0, 0)])
    assert exc_info.value.error == ac3.FrameError.kInvalidDialnorm


def test_blksw_flags_a_click_but_not_a_steady_tone():
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k1_0, bitrate_kbps=128))
    decoder = ac3.FrameDecoder()
    steady = decoder.decode_frame(encoder.encode_frame([_tone(500.0, 0)]))
    assert len(steady.blksw) == 1 and len(steady.blksw[0]) == ac3.BLOCKS_PER_FRAME
    assert not any(steady.blksw[0])
    click = np.zeros(SPF, dtype=np.float32)
    click[800:840] = 0.95
    switched = False
    for _ in range(2):
        decoded = decoder.decode_frame(encoder.encode_frame([click]))
        switched |= any(decoded.blksw[0])
    assert switched


# --- framing helpers --------------------------------------------------------------------------


def test_split_frames_and_access_units_and_bsid():
    stream = _stereo_ac3_stream(frames=3)
    frames = ac3.split_frames(stream)
    assert len(frames) == 3
    assert all(len(f) == 768 for f in frames)
    assert b"".join(frames) == stream
    assert ac3.split_access_units(stream) == frames
    assert ac3.stream_bsid(frames[0]) == 8
    assert ac3.split_frames(b"") == []

    encoder = ac3.eac3.FrameEncoder(ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0))
    eac3_frame = encoder.encode_frame([_tone(440.0, 0), _tone(660.0, 0)])
    assert ac3.stream_bsid(eac3_frame) == 16


def test_framing_helpers_reject_garbage():
    with pytest.raises(ac3.Ac3Error):
        ac3.split_frames(b"\x01\x02\x03\x04\x05")
    with pytest.raises(ac3.Ac3Error):
        ac3.split_access_units(bytes(100))
    with pytest.raises(ac3.Ac3Error):
        ac3.stream_bsid(b"\x0b\x77")


# --- read_frame_header / scan descriptor fields ----------------------------------------------


def test_read_frame_header_ac3_fields():
    stream = _stereo_ac3_stream(frames=1, dialnorm=20)
    header = ac3.read_frame_header(stream)
    assert header.kind == ac3.StreamKind.kAc3
    assert header.bytes == 768
    assert header.bsid == 8
    assert header.dialnorm == 20
    assert header.dialnorm2 is None
    assert header.compr2 is None
    assert header.acmod == ac3.Acmod.k2_0
    assert header.bitrate_kbps == 192
    assert isinstance(header.bit_rate_code, int)
    assert header.numblkscod == 3  # AC-3 is always six blocks
    assert header.reduced_rate is False
    assert header.oba_complexity_index is None
    assert isinstance(header.bsmod, int) and isinstance(header.bsmod_present, bool)
    assert isinstance(header.dsurmod, int)
    assert isinstance(header.mix_metadata, bool)


def test_read_frame_header_reduced_rate_eac3():
    config = ac3.eac3.FrameConfig(
        bitrate_kbps=96, acmod=ac3.Acmod.k2_0, sample_rate=ac3.SampleRate.k24000
    )
    encoder = ac3.eac3.FrameEncoder(config)
    frame = encoder.encode_frame([_tone(440.0, 0, 24000), _tone(660.0, 0, 24000)])
    header = ac3.read_frame_header(frame)
    assert header.kind == ac3.StreamKind.kEac3
    assert header.bsid == 16
    assert header.reduced_rate is True
    assert header.sample_rate == ac3.SampleRate.k24000
    assert header.bytes == len(frame)


def test_read_frame_header_rejects_garbage():
    with pytest.raises(ac3.Ac3Error):
        ac3.read_frame_header(bytes(16))


def test_scanned_stream_descriptor_fields_and_timing():
    stream = _stereo_ac3_stream(frames=4)
    scanned = ac3.scan(stream)
    header = ac3.read_frame_header(stream)
    assert scanned.bsmod == header.bsmod
    assert scanned.bit_rate_code == header.bit_rate_code
    assert scanned.bsmod_present == header.bsmod_present
    assert scanned.dsurmod == header.dsurmod
    assert scanned.mix_metadata == header.mix_metadata
    assert scanned.oba_complexity_index is None
    # A bitmask of the independent substream ids seen: AC-3 has no substreams at all.
    assert scanned.independent_substreams == 0
    # Slots for independent substreams 1-3, none of which an AC-3 stream can carry.
    assert len(scanned.associated_substreams) == 3
    assert not any(s.present for s in scanned.associated_substreams)
    assert scanned.programmes[0].bsmod == scanned.bsmod
    assert scanned.programmes[0].oba_complexity_index is None

    timing = ac3.access_unit_timing(scanned, 2)
    assert timing.start_sample == 2 * SPF
    assert timing.duration_samples == SPF
    assert timing.duration_seconds == pytest.approx(SPF / 48000)
    assert timing.duration_in_timescale(90000) == SPF * 90000 // 48000
    assert timing.start_in_timescale(48000) == 2 * SPF
    assert ac3.access_unit_timing(scanned, 4) is None


def test_scanned_stream_associated_services_on_a_multi_programme_stream():
    # Two independent substreams (ids 0 and 1) interleaved = a second programme.
    main = ac3.eac3.FrameEncoder(ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0))
    second = ac3.eac3.FrameEncoder(
        ac3.eac3.FrameConfig(bitrate_kbps=96, acmod=ac3.Acmod.k1_0, substreamid=1)
    )
    stream = bytearray()
    for i in range(3):
        stream += main.encode_frame([_tone(440.0, i), _tone(660.0, i)])
        stream += second.encode_frame([_tone(880.0, i)])
    scanned = ac3.scan(bytes(stream))
    # Bitmask of ids seen: substreams 0 and 1.
    assert scanned.independent_substreams == 0b11
    assert len(scanned.programmes) == 2
    services = scanned.associated_substreams
    assert [s.present for s in services] == [True, False, False]
    service = services[0]
    assert service.acmod == ac3.Acmod.k1_0
    assert service.lfe is False
    assert isinstance(service.bsmod, int)
    assert isinstance(service.bsmod_present, bool)
    assert isinstance(service.mix_metadata, bool)


# --- latency, E-AC-3 decoded metadata ---------------------------------------------------------


def test_latency_budget_milliseconds():
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0))
    budget = encoder.latency
    for rate in (ac3.SampleRate.k48000, ac3.SampleRate.k32000):
        expected = 1000.0 * budget.total_samples / ac3.sample_rate_hz(rate)
        assert budget.milliseconds(rate) == pytest.approx(expected)


def test_eac3_decoded_substream_and_access_unit_metadata():
    config = ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0, dialnorm=22)
    assert config.dialnorm == 22
    encoder = ac3.eac3.FrameEncoder(config)
    frame = encoder.encode_frame([_tone(440.0, 0), _tone(660.0, 0)])
    decoder = ac3.Eac3Decoder()
    substream = decoder.decode_substream(frame)
    assert substream.dialnorm == 22
    assert substream.dialnorm2 is None
    assert substream.compr2 is None
    assert substream.numblkscod == 3
    assert len(substream.blksw) == 2
    assert all(len(row) == ac3.BLOCKS_PER_FRAME for row in substream.blksw)
    assert len(substream.dynrng2) == ac3.BLOCKS_PER_FRAME
    assert isinstance(substream.last_dependent, bool)
    unit = decoder.decode_access_unit(frame)
    assert unit.dialnorm == 22
    assert unit.dialnorm2 is None
    assert unit.numblkscod == 3


@pytest.mark.parametrize(
    "tools",
    [
        {"auto_tools": True},
        {"coupling": True, "enhanced": True, "cplbegf": 4},
        {"spx": True, "spx_atten": True, "spxattencod": 2, "spxbegf": 2},
        {"aht": True, "gaqmod": 1},
        {"fast_mdct": False},
    ],
)
def test_eac3_coding_tools_round_trip(tools):
    config = ac3.eac3.FrameConfig(bitrate_kbps=128, acmod=ac3.Acmod.k2_0, **tools)
    for name, value in tools.items():
        assert getattr(config, name) == value
    encoder = ac3.eac3.FrameEncoder(config)
    decoder = ac3.Eac3Decoder()
    for i in range(3):
        decoded = decoder.decode_substream(encoder.encode_frame([_tone(400.0, i), _tone(900.0, i)]))
    assert _rms(decoded.channels[0]) > 0.05 and _rms(decoded.channels[1]) > 0.05


# --- meter and QC -----------------------------------------------------------------------------


def test_loudness_meter_momentary_and_steady_tone_level():
    meter = ac3.meta.LoudnessMeter(ac3.SampleRate.k48000, ac3.Acmod.k1_0, False)
    assert meter.momentary_lkfs is None
    n = np.arange(48000 * 2, dtype=np.float64)
    tone = (0.1414 * np.sin(2 * np.pi * 1000.0 * n / 48000)).astype(np.float32)
    meter.push([tone])
    # A -20 dBFS-RMS 1 kHz sine in one front channel measures -20 LKFS.
    assert meter.momentary_lkfs == pytest.approx(-20.0, abs=0.3)
    assert meter.integrated_lkfs == pytest.approx(-20.0, abs=0.3)


ALL_QC = [
    ac3.meta.QcPresetId.kEbuR128S2,
    ac3.meta.QcPresetId.kAtscA85,
    ac3.meta.QcPresetId.kAtscA85Streaming,
    ac3.meta.QcPresetId.kNetflix,
    ac3.meta.QcPresetId.kAppleMusicAtmos,
]


@pytest.mark.parametrize("pid", ALL_QC)
def test_qc_presets_gate_on_target_and_peak(pid):
    preset = ac3.meta.qc_preset(pid)
    assert preset.source
    assert preset.tolerance_lu >= 0
    assert preset.max_true_peak_dbtp <= 0
    assert preset.loudness_limit in (
        ac3.meta.QcLoudnessLimit.kBand,
        ac3.meta.QcLoudnessLimit.kCeiling,
    )

    on_target = ac3.meta.evaluate_qc_gate(
        preset, preset.target_lkfs, preset.max_true_peak_dbtp - 1.0
    )
    assert on_target.passed
    assert on_target.loudness_delta_lu == pytest.approx(0.0)
    assert on_target.true_peak_margin_dbtp == pytest.approx(1.0)

    too_loud = ac3.meta.evaluate_qc_gate(
        preset, preset.target_lkfs + preset.tolerance_lu + 3.0, -10.0
    )
    assert not too_loud.loudness_pass and not too_loud.passed

    clipping = ac3.meta.evaluate_qc_gate(
        preset, preset.target_lkfs, preset.max_true_peak_dbtp + 0.5
    )
    assert not clipping.true_peak_pass and not clipping.passed
    assert clipping.true_peak_margin_dbtp == pytest.approx(-0.5)

    if preset.loudness_limit == ac3.meta.QcLoudnessLimit.kBand:
        too_quiet = ac3.meta.evaluate_qc_gate(
            preset, preset.target_lkfs - preset.tolerance_lu - 3.0, -10.0
        )
        assert not too_quiet.loudness_pass
    else:
        quiet_ok = ac3.meta.evaluate_qc_gate(
            preset, preset.target_lkfs - preset.tolerance_lu - 3.0, -10.0
        )
        assert quiet_ok.loudness_pass
