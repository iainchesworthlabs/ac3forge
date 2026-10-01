"""AC-4 objects through the Python bindings (ac3.ac4): A-JOC and direct-coded object scenes
encoded and decoded back, the metadata update ramps the decoder now reports, the object
configuration's limits, the I-frame lists and the experimental flags.

Python binds iclforge::ac4::EncoderConfig, iclforge::ac4::ObjectsConfig and the rest pybind11-direct, so there is no
conversion layer whose output could differ from the C++ API's: what the bindings add is the
keyword constructors, the property accessors and the `objects` view of the encoder configuration's
one object substream. Each scene is therefore configured twice, by keywords and by assigning
attributes, and the two streams have to be the same bytes; the decoder then reads every object back
within what each field's code can hold, with its own tone, and a metadata update at the sample its
input sample comes out. tests/capi/test_capi.cpp holds the C API to iclforge::ac4::Encoder byte for byte.
"""

import iclforge as ac3
import numpy as np
import pytest

ac4 = ac3.ac4

RATE = 48_000
FRAME = 2048
# Each object's tone sits at the middle of a QMF subband of its own (a parameter band of its own in
# A-JOC's matrices), the LFE's at 47 Hz, as tests/ac4enc/test_ac4enc_objects.cpp has them.
SUBBANDS = [1, 3, 5, 7, 9, 12, 16, 22]
LFE_HZ = 47.0
# Six frames of input; the update sits at sample 5000, in the third.
SAMPLES = 6 * FRAME
UPDATE_SAMPLE = 5000
UPDATE_RAMP = 1024


def dynamic(x, y, z, gain_db, **more):
    return ac4.ObjectConfig(properties=ac4.ObjectProperties(x=x, y=y, z=z, gain_db=gain_db, **more))


def lfe():
    return ac4.ObjectConfig(lfe=True)


def with_update(objects):
    """The scene's first dynamic object moves at the update's sample, over UPDATE_RAMP."""
    first = next(i for i, o in enumerate(objects.objects) if not o.lfe and o.bed is None)
    update = ac4.ObjectMetadataUpdate(
        object=first,
        sample=UPDATE_SAMPLE,
        ramp_samples=UPDATE_RAMP,
        properties=ac4.ObjectProperties(x=0.75, y=0.25, z=0.4, gain_db=-12.0),
    )
    return objects, update


def ajoc_scene():
    """A-JOC over a computed downmix of two signals: the LFE among the objects, a bed object and
    three dynamic objects whose metadata takes every field a dynamic object sends."""
    b = dynamic(
        0.9,
        0.5,
        7 / 15,
        -6.0,
        priority=16 / 31,
        width_x=0.2,
        width_y=0.4,
        width_z=0.6,
        zone_mask=3,
        screen_factor=0.5,
    )
    bed = ac4.ObjectConfig(
        bed=ac4.BedChannel.kLeft,
        properties=ac4.ObjectProperties(
            gain_db=-12.0, trim_disabled=True, headphone_render_mode=1, head_track_disabled=True
        ),
    )
    # The screen factor and the depth exponent share a group of fields with no code for a factor
    # of 0: a factor is given with the exponent.
    c = dynamic(
        0.5,
        1.0,
        -0.6,
        -4.4,
        snap=True,
        enable_elevation=False,
        screen_factor=0.25,
        depth_exponent=2.0,
        distance=4.0,
        divergence=0.75,
    )
    return with_update(
        ac4.ObjectsConfig(
            objects=[dynamic(0.1, 0.2, 0.0, -3.0), lfe(), b, bed, c], downmix_signals=2
        )
    )


def direct_scene():
    """Direct-coded: four dynamic objects, one of them inactive, and the LFE."""
    b = dynamic(0.33, 0.66, 0.2, -6.0, width_x=0.1, width_y=0.3, width_z=0.5, zone_mask=5)
    quiet = dynamic(0.67, 0.34, -0.2, 0.0, active=False)
    return with_update(
        ac4.ObjectsConfig(
            objects=[dynamic(0.0, 0.0, 0.0, -3.0), b, lfe(), quiet, dynamic(1.0, 1.0, 1.0, -9.0)],
            coding=ac4.ObjectCoding.kDirect,
        )
    )


def tones(objects, samples=SAMPLES):
    t = np.arange(samples, dtype=np.float64) / RATE
    out, k = [], 0
    for o in objects.objects:
        if o.lfe:
            hz = LFE_HZ
        else:
            hz = (SUBBANDS[k % len(SUBBANDS)] + 0.5) * RATE / 128.0
            k += 1
        out.append((0.1 * np.sin(2 * np.pi * hz * t)).astype(np.float32))
    return np.stack(out)


def config_by_keywords(objects):
    return ac4.EncoderConfig(
        bitrate_kbps=256, experimental=ac4.Experimental(objects=True), objects=objects
    )


def config_by_attributes(objects):
    config = ac4.EncoderConfig()
    config.bitrate_kbps = 256
    config.experimental.objects = True  # experimental is a reference into the config
    config.objects = objects
    return config


def encode(config, input, updates=()):
    encoder = ac4.Encoder.create(config)
    lag = encoder.delay_samples + encoder.decoder_delay_samples
    frames = list(encoder.encode(input, updates=list(updates)))
    frames += encoder.flush()
    return [f.data for f in frames], lag


def decode(frames):
    """The objects' samples end to end, the last frame's objects, and every update at its sample of
    the decoder's output: (object, sample, ramp, properties)."""
    decoder = ac4.Decoder()
    samples, last, updates, start = None, None, [], 0
    for data in frames:
        frame = decoder.decode(data)
        if frame is None:
            continue
        if samples is None:
            samples = [[] for _ in frame.objects]
        for o, obj in enumerate(frame.objects):
            samples[o].append(np.array(obj.samples))
            for u in obj.updates:
                updates.append((o, start + u.sample, u.ramp_samples, u.properties))
        start += frame.samples
        last = frame.objects
    return [np.concatenate(s) for s in samples], last, updates


def decoded_order(objects):
    """The LFE first, then the bed objects, then the dynamic ones, each in the order listed."""
    o = objects.objects
    return (
        [i for i in range(len(o)) if o[i].lfe]
        + [i for i in range(len(o)) if not o[i].lfe and o[i].bed is not None]
        + [i for i in range(len(o)) if not o[i].lfe and o[i].bed is None]
    )


def correlation(reference, decoded, lag):
    """Normalised, `decoded` `lag` samples later, from two frames in to a frame before the end."""
    end = min(len(reference) - FRAME, len(decoded) - lag)
    a = reference[2 * FRAME : end].astype(np.float64)
    b = decoded[2 * FRAME + lag : end + lag].astype(np.float64)
    return float(np.dot(a, b) / np.sqrt(max(np.dot(a, a) * np.dot(b, b), 1e-300)))


def assert_properties_near(got, want, dynamic):
    """What one property's code can hold: X and Y in 62 steps, Z in 15, the gain in 1 dB, the
    priority and each width in 31, the screen factor in 8, the divergence and the distance in the
    tables of Annex F. An inactive object sends none of them."""
    assert got.active == want.active
    if not want.active:
        return
    assert abs(got.gain_db - want.gain_db) <= 0.5 + 1e-9
    assert abs(got.priority - want.priority) <= 1 / 62 + 1e-9
    assert got.trim_disabled == want.trim_disabled
    assert got.headphone_render_mode == want.headphone_render_mode
    assert got.head_track_disabled == (
        want.headphone_render_mode is not None and want.head_track_disabled
    )
    if not dynamic:
        return
    assert abs(got.x - want.x) <= 1 / 124 + 1e-9
    assert abs(got.y - want.y) <= 1 / 124 + 1e-9
    assert abs(got.z - want.z) <= 1 / 30 + 1e-9
    assert got.zone_mask == want.zone_mask
    assert got.enable_elevation == want.enable_elevation
    assert got.snap == want.snap
    for axis in ("width_x", "width_y", "width_z"):
        assert abs(getattr(got, axis) - getattr(want, axis)) <= 1 / 62 + 1e-9, axis
    assert abs(got.screen_factor - want.screen_factor) <= 1 / 16 + 1e-9
    assert got.depth_exponent == want.depth_exponent
    assert (got.distance is None) == (want.distance is None)
    if want.distance is not None:
        assert abs(got.distance - want.distance) <= 0.1 * want.distance
    assert abs(got.divergence - want.divergence) <= 0.02


@pytest.mark.parametrize("scene", [ajoc_scene, direct_scene], ids=["ajoc", "direct"])
def test_object_scene_round_trips_within_the_codecs_tolerance(scene):
    objects, update = scene()
    input = tones(objects)
    by_keywords, lag = encode(config_by_keywords(objects), input, [update])
    by_attributes, _ = encode(config_by_attributes(objects), input, [update])
    assert len(by_keywords) > 0
    # Two independent ways to configure the same scene write the same bytes.
    assert by_keywords == by_attributes

    samples, last, updates = decode(by_keywords)
    order = decoded_order(objects)
    assert len(last) == len(order)
    for d, o in enumerate(order):
        configured, decoded = objects.objects[o], last[d]
        assert decoded.lfe == configured.lfe, d
        if configured.lfe:
            assert decoded.speaker == ac4.Speaker.kLfe
        elif configured.bed is not None:
            assert decoded.kind == ac4.ObjectKind.kBed
            assert decoded.speaker == ac4.Speaker.kLeft
        else:
            assert decoded.kind == ac4.ObjectKind.kDyn
        # The last frame's properties are the update's for the object it moved, the
        # configuration's for the rest.
        want = update.properties if update.object == o else configured.properties
        assert_properties_near(
            decoded.properties, want, dynamic=not configured.lfe and configured.bed is None
        )
        # Its audio is its own tone, and no other object's.
        if configured.properties.active:
            assert correlation(input[o], samples[d], lag) > 0.98, o
            other = order[(d + 1) % len(order)]
            assert abs(correlation(input[other], samples[d], lag)) < 0.5, (o, other)

    # The update comes out where its input sample does, to within 32 samples, with the ramp it was
    # given, and the decoder reports it through ObjectUpdate.
    moved = order.index(update.object)
    found = next(
        u
        for u in updates
        if u[0] == moved
        and abs(u[3].x - update.properties.x) <= 1 / 124 + 1e-9
        and abs(u[3].y - update.properties.y) <= 1 / 124 + 1e-9
    )
    assert update.sample + lag - 32 < found[1] <= update.sample + lag
    assert found[2] == update.ramp_samples
    assert_properties_near(found[3], update.properties, dynamic=True)


def test_the_object_defaults_are_the_encoders_not_zeroes():
    p = ac4.ObjectProperties()
    assert (p.active, p.gain_db, p.priority) == (True, 0.0, 1.0)
    assert (p.x, p.y, p.z) == (0.5, 0.5, 0.0)
    assert (p.width_x, p.width_y, p.width_z) == (0.0, 0.0, 0.0)
    assert p.depth_exponent == 1.0 and p.enable_elevation is True
    assert p.distance is None and p.headphone_render_mode is None
    # Attributes are settable, and the elements of position and width are independent.
    p.y = 0.75
    p.width_z = 0.5
    assert (p.x, p.y, p.z, p.width_x, p.width_z) == (0.5, 0.75, 0.0, 0.0, 0.5)
    with pytest.raises(TypeError):
        ac4.ObjectProperties(gain=1.0)  # not a field

    config = ac4.EncoderConfig()
    assert config.objects is None
    assert config.iframes == [] and config.fragment_starts == []
    assert (
        config.experimental.objects is False
        and config.experimental.seven_x == ac4.AdditionalPair.kNone
    )
    objects = ac4.ObjectsConfig()
    assert objects.coding == ac4.ObjectCoding.kAjoc
    assert objects.downmix == ac4.AjocDownmix.kComputed
    assert objects.downmix_signals is None and objects.parameter_bands is None
    assert ac4.ObjectConfig().bed is None and ac4.ObjectConfig().properties.priority == 1.0
    assert ac4.ObjectMetadataUpdate().properties.depth_exponent == 1.0

    # An objects configuration is the encoder configuration's one object substream: assigning
    # None takes it away again, and reading a list gives a copy.
    config.objects = ac4.ObjectsConfig(objects=[lfe()])
    assert len(config.objects.objects) == 1
    config.objects.objects.append(lfe())
    assert len(config.objects.objects) == 1
    config.objects = None
    assert config.objects is None


def many(count, kbps, signals=None, **more):
    return ac4.EncoderConfig(
        bitrate_kbps=kbps,
        experimental=ac4.Experimental(objects=True),
        objects=ac4.ObjectsConfig(
            objects=[ac4.ObjectConfig() for _ in range(count)], downmix_signals=signals
        ),
        **more,
    )


def test_the_limits_and_refusals_are_the_encoders():
    objects, _ = ajoc_scene()
    reason = ac4.Encoder.refusal_reason
    assert reason(config_by_keywords(objects)) == ""

    config = config_by_keywords(objects)
    config.experimental.objects = False
    assert reason(config) == "objects without experimental.objects"
    with pytest.raises(
        ac3.Ac4EncodeError, match=r"objects without experimental\.objects"
    ) as excinfo:
        ac4.Encoder.create(config)
    assert excinfo.value.error == ac4.EncodeError.kInvalidConfig

    config = config_by_keywords(objects)
    config.frame_rate_index = 2
    assert reason(config) == "objects at a frame_rate_index other than 13"

    assert reason(many(64, 384)) == ""
    ac4.Encoder.create(many(64, 384))
    assert reason(many(65, 384)) == "more than 64 objects"
    assert reason(many(11, 512, 11)) == ""
    assert reason(many(12, 512, 12)) == (
        "a computed downmix of no signal, of more than 11 or of more than its full-band objects"
    )

    config = config_by_keywords(objects)
    config.objects = ac4.ObjectsConfig(objects=objects.objects, coding=ac4.ObjectCoding.kDirect)
    assert reason(config) == "bed objects in direct-coded object substreams"
    config.objects = ac4.ObjectsConfig(
        objects=objects.objects, downmix=ac4.AjocDownmix.kStatic50, downmix_signals=None
    )
    assert reason(config) == "an LFE object with a static 5.0 downmix"
    config.objects = ac4.ObjectsConfig(objects=objects.objects, parameter_bands=10)
    assert reason(config) == (
        "A-JOC parameter bands other than Table 78's 23, 15, 12, 9, 7, 5, 3 or 1"
    )
    # With objects, codec_mode is the object substream's.
    config = config_by_keywords(objects)
    config.codec_mode = ac4.CodecMode.kAspxAcpl2
    assert reason(config) == ("an object substream's codec mode other than kAuto, kSimple or kAspx")
    config.codec_mode = ac4.CodecMode.kSimple
    assert reason(config) == ""
    assert ac4.Encoder.create(config).codec_mode == ac4.CodecMode.kSimple

    # A property off its range.
    bad = config_by_keywords(ac4.ObjectsConfig(objects=[dynamic(1.5, 0.5, 0.0, 0.0)]))
    assert reason(bad) == "an object's properties off the ranges ObjectProperties gives them"
    assert (
        reason(
            config_by_keywords(ac4.ObjectsConfig(objects=[dynamic(0.5, 0.5, 0.0, float("-inf"))]))
        )
        == ""
    )


def test_encode_refuses_input_the_encoder_refuses():
    objects, _ = ajoc_scene()
    input = tones(objects, 2 * FRAME)
    encoder = ac4.Encoder.create(config_by_keywords(objects))

    def refused(updates, channels=input):
        with pytest.raises(ac3.Ac4EncodeError) as excinfo:
            encoder.encode(channels, updates=updates)
        assert excinfo.value.error == ac4.EncodeError.kInvalidInput
        return excinfo.value

    # An object the configuration lacks, an input sample before the first, a property off its
    # range, and the wrong number of objects.
    refused([ac4.ObjectMetadataUpdate(object=len(input))])
    refused([ac4.ObjectMetadataUpdate(sample=-1)])
    refused([ac4.ObjectMetadataUpdate(properties=ac4.ObjectProperties(priority=2.0))])
    refused([], channels=input[1:])
    # The refusals left the encoder as it was: the same input encodes, with an update or without.
    assert isinstance(encoder.encode(input, updates=[ac4.ObjectMetadataUpdate()]), list)
    assert isinstance(encoder.encode(input), list)

    # An encoder of channels has no object for an update to name.
    stereo = ac4.Encoder.create(ac4.EncoderConfig())
    with pytest.raises(ac3.Ac4EncodeError):
        stereo.encode(input[:2], updates=[ac4.ObjectMetadataUpdate()])
    assert isinstance(stereo.encode(input[:2]), list)


def stream_tones(channels, frames):
    t = np.arange(frames * FRAME, dtype=np.float64) / RATE
    return np.stack(
        [
            (0.2 * np.sin(2 * np.pi * (500.0 + 170.0 * c) * t)).astype(np.float32)
            for c in range(channels)
        ]
    )


def iframes_of(config, input):
    encoder = ac4.Encoder.create(config)
    frames = list(encoder.encode(input)) + list(encoder.flush())
    return [i for i, f in enumerate(frames) if f.iframe]


def test_the_iframe_lists_reach_the_encoder():
    input = stream_tones(2, 8)
    base = {"bitrate_kbps": 96, "iframe_interval": 1000}  # the first frame alone, without the lists
    assert iframes_of(ac4.EncoderConfig(**base), input) == [0]
    assert iframes_of(ac4.EncoderConfig(iframes=[5, 2], **base), input) == [0, 2, 5]  # any order
    # Frame 2's output starts at sample 4096 exactly; frame 5's is the first to start after 9000
    # (frame 4's is 8192, frame 5's 10240).
    assert iframes_of(ac4.EncoderConfig(fragment_starts=[4096, 9000], **base), input) == [0, 2, 5]


def test_the_experimental_flags_reach_the_encoder():
    def reason(experimental=None, **fields):
        if experimental is not None:
            fields["experimental"] = ac4.Experimental(**experimental)
        return ac4.Encoder.refusal_reason(ac4.EncoderConfig(**fields))

    # ASPX_ACPL_1 in stereo needs acpl.
    stereo = {"bitrate_kbps": 64, "codec_mode": ac4.CodecMode.kAspxAcpl1}
    assert reason(**stereo) != ""
    assert reason(experimental={"acpl": True}, **stereo) == ""
    # 7.1 needs its additional pair, and 5.1 refuses one.
    seven_one = {"channels": 8, "bitrate_kbps": 448}
    assert reason(**seven_one) == (
        "seven or eight channels without experimental.seven_x's additional pair"
    )
    for pair in (ac4.AdditionalPair.kBack, ac4.AdditionalPair.kWide, ac4.AdditionalPair.kTopFront):
        assert reason(experimental={"seven_x": pair}, **seven_one) == ""
    assert reason(experimental={"seven_x": ac4.AdditionalPair.kBack}, channels=6) == (
        "experimental.seven_x's additional pair without seven or eight channels"
    )
    # 7.1.4 needs back_pair, ASPX_AJCC needs ajcc, and coding_configs is refused beside A-CPL.
    back = {"channels": 12, "bitrate_kbps": 768}
    assert "experimental.back_pair" in reason(**back)
    assert reason(experimental={"back_pair": True}, **back) == ""
    ajcc = {"channels": 10, "bitrate_kbps": 448, "codec_mode": ac4.CodecMode.kAspxAjcc}
    assert reason(**ajcc) == "ASPX_AJCC without experimental.ajcc"
    assert reason(experimental={"ajcc": True}, **ajcc) == ""
    acpl_2 = {"channels": 6, "bitrate_kbps": 128, "codec_mode": ac4.CodecMode.kAspxAcpl2}
    assert reason(**acpl_2) == ""
    assert reason(experimental={"coding_configs": True}, **acpl_2) == (
        "an A-CPL codec mode with experimental.coding_configs"
    )


def test_the_aspx_options_change_the_stream():
    # Stereo in the ASPX mode over a steady tone above the crossover, with an attack in both
    # channels every 1 100 samples: each option on its own changes the stream. Set by keywords
    # and by attribute, they write the same bytes.
    input = stream_tones(2, 10)
    t = np.arange(input.shape[1], dtype=np.float64) / RATE
    input += (0.05 * np.sin(2 * np.pi * 12000.0 * t)).astype(np.float32)
    rng = np.random.default_rng(12345)
    for at in range(3000, input.shape[1] - 200, 1100):
        input[0, at : at + 150] += (0.5 * rng.uniform(-1, 1, 150)).astype(np.float32)
        input[1, at : at + 150] += (
            (0.5 if at % 2 == 0 else 0.05) * rng.uniform(-1, 1, 150)
        ).astype(np.float32)

    base = {"bitrate_kbps": 48, "codec_mode": ac4.CodecMode.kAspx}

    def stream(config):
        encoder = ac4.Encoder.create(config)
        return [f.data for f in list(encoder.encode(input)) + list(encoder.flush())]

    plain = stream(ac4.EncoderConfig(**base))
    for option in ("aspx_balance", "aspx_varvar", "aspx_interleave"):
        by_keywords = stream(
            ac4.EncoderConfig(experimental=ac4.Experimental(**{option: True}), **base)
        )
        config = ac4.EncoderConfig(**base)
        setattr(config.experimental, option, True)
        assert stream(config) == by_keywords, option
        assert by_keywords != plain, option
