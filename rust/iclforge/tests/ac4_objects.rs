//! The AC-4 encoder's object substream (A-JOC and direct-coded), its I-frame lists and
//! experimental flags, and the decoder's update ramps, through the safe crate.
//!
//! An object scene is encoded through `iclforge::ac4` and, independently, through the raw
//! `iclforge_sys` calls with structs this file builds by hand (never through the crate's own
//! conversions): the two streams have to be the same bytes, so a field the wrapper drops or
//! misplaces shows. `tests/capi/test_capi.cpp` holds the raw C API to `ac4::Encoder` byte for
//! byte, so the three agree. The crate's decoder then reads the scene back with every object's
//! metadata within what each field's code can hold, its own tone, and a metadata update at the
//! sample its input sample comes out.

use iclforge::ac4::{
    AdditionalPair, AjocDownmix, BedChannel, CodecMode, Decoder, DecoderConfig, Encoder,
    EncoderConfig, Experimental, ObjectCoding, ObjectConfig, ObjectKind, ObjectMetadataUpdate,
    ObjectProperties, ObjectsConfig, Speaker,
};
use iclforge::Error;
use iclforge_sys as sys;

const RATE: f64 = 48_000.0;
const FRAME: usize = 2048;
// Each object's tone sits at the middle of a QMF subband of its own (a parameter band of its own
// in A-JOC's matrices), the LFE's at 47 Hz, as tests/ac4enc/test_ac4enc_objects.cpp has them.
const SUBBANDS: [usize; 8] = [1, 3, 5, 7, 9, 12, 16, 22];
const LFE_HZ: f64 = 47.0;
// Six frames of input; the update sits at sample 5000, in the third.
const SAMPLES: usize = 6 * FRAME;
const UPDATE_SAMPLE: i64 = 5000;
const UPDATE_RAMP: i32 = 1024;

struct Scene {
    objects: ObjectsConfig,
    update: ObjectMetadataUpdate,
}

fn dynamic(x: f64, y: f64, z: f64, gain_db: f64) -> ObjectConfig {
    ObjectConfig {
        properties: ObjectProperties {
            position: [x, y, z],
            gain_db,
            ..Default::default()
        },
        ..Default::default()
    }
}

fn lfe() -> ObjectConfig {
    ObjectConfig {
        lfe: true,
        ..Default::default()
    }
}

/// A scene with its first dynamic object moving at the update's sample, over `UPDATE_RAMP`.
fn scene_of(objects: ObjectsConfig) -> Scene {
    let first_dynamic = objects
        .objects
        .iter()
        .position(|o| !o.lfe && o.bed.is_none())
        .unwrap();
    let update = ObjectMetadataUpdate {
        object: first_dynamic,
        sample: UPDATE_SAMPLE,
        ramp_samples: UPDATE_RAMP,
        properties: ObjectProperties {
            position: [0.75, 0.25, 0.4],
            gain_db: -12.0,
            ..Default::default()
        },
    };
    Scene { objects, update }
}

/// A-JOC over a computed downmix of two signals: the LFE among the objects, a bed object and
/// three dynamic objects whose metadata takes every field a dynamic object sends.
fn ajoc_scene() -> Scene {
    let a = dynamic(0.1, 0.2, 0.0, -3.0);
    let mut b = dynamic(0.9, 0.5, 7.0 / 15.0, -6.0);
    b.properties.priority = 16.0 / 31.0;
    b.properties.width = [0.2, 0.4, 0.6];
    b.properties.zone_mask = 3;
    b.properties.screen_factor = 0.5;
    let mut bed = ObjectConfig {
        bed: Some(BedChannel::Left),
        ..Default::default()
    };
    bed.properties.gain_db = -12.0;
    bed.properties.trim_disabled = true;
    bed.properties.headphone_render_mode = Some(1);
    bed.properties.head_track_disabled = true;
    let mut c = dynamic(0.5, 1.0, -0.6, -4.4);
    c.properties.snap = true;
    c.properties.enable_elevation = false;
    // The screen factor and the depth exponent share a group of fields with no code for a
    // factor of 0: a factor is given with the exponent.
    c.properties.screen_factor = 0.25;
    c.properties.depth_exponent = 2.0;
    c.properties.distance = Some(4.0);
    c.properties.divergence = 0.75;
    scene_of(ObjectsConfig {
        objects: vec![a, lfe(), b, bed, c],
        downmix_signals: Some(2),
        ..Default::default()
    })
}

/// Direct-coded: four dynamic objects, one of them inactive, and the LFE.
fn direct_scene() -> Scene {
    let mut b = dynamic(0.33, 0.66, 0.2, -6.0);
    b.properties.width = [0.1, 0.3, 0.5];
    b.properties.zone_mask = 5;
    let mut quiet = dynamic(0.67, 0.34, -0.2, 0.0);
    quiet.properties.active = false;
    scene_of(ObjectsConfig {
        objects: vec![
            dynamic(0.0, 0.0, 0.0, -3.0),
            b,
            lfe(),
            quiet,
            dynamic(1.0, 1.0, 1.0, -9.0),
        ],
        coding: ObjectCoding::Direct,
        ..Default::default()
    })
}

fn config_of(objects: ObjectsConfig) -> EncoderConfig {
    EncoderConfig {
        bitrate_kbps: 256,
        experimental: Experimental {
            objects: true,
            ..Default::default()
        },
        objects: Some(objects),
        ..Default::default()
    }
}

fn input_of(objects: &ObjectsConfig, samples: usize) -> Vec<Vec<f32>> {
    let mut tones = 0;
    objects
        .objects
        .iter()
        .map(|o| {
            let hz = if o.lfe {
                LFE_HZ
            } else {
                tones += 1;
                (SUBBANDS[(tones - 1) % SUBBANDS.len()] as f64 + 0.5) * RATE / 128.0
            };
            (0..samples)
                .map(|n| (0.1 * (2.0 * std::f64::consts::PI * hz * n as f64 / RATE).sin()) as f32)
                .collect()
        })
        .collect()
}

fn views(input: &[Vec<f32>]) -> Vec<&[f32]> {
    input.iter().map(|c| c.as_slice()).collect()
}

/// The scene through the safe crate: every frame's bytes, and the encoder's two delays.
fn encode_with_crate(scene: &Scene, input: &[Vec<f32>]) -> (Vec<Vec<u8>>, i64) {
    let config = config_of(scene.objects.clone());
    let mut encoder =
        Encoder::new(&config).unwrap_or_else(|_| panic!("{}", Encoder::refusal_reason(&config)));
    let lag = i64::from(encoder.delay_samples() + encoder.decoder_delay_samples());
    let mut frames = encoder
        .encode_objects(&views(input), std::slice::from_ref(&scene.update))
        .unwrap();
    frames.extend(encoder.flush().unwrap());
    (frames.iter().map(|f| f.data().to_vec()).collect(), lag)
}

fn raw_properties(p: &ObjectProperties) -> sys::iclforge_ac4_object_properties_t {
    let mut raw: sys::iclforge_ac4_object_properties_t = unsafe { std::mem::zeroed() };
    unsafe { sys::iclforge_ac4_object_properties_init(&mut raw) };
    raw.active = i32::from(p.active);
    raw.gain_db = p.gain_db;
    raw.priority = p.priority;
    raw.x = p.position[0];
    raw.y = p.position[1];
    raw.z = p.position[2];
    raw.zone_mask = p.zone_mask;
    raw.enable_elevation = i32::from(p.enable_elevation);
    raw.snap = i32::from(p.snap);
    raw.width_x = p.width[0];
    raw.width_y = p.width[1];
    raw.width_z = p.width[2];
    raw.screen_factor = p.screen_factor;
    raw.depth_exponent = p.depth_exponent;
    raw.has_distance = i32::from(p.distance.is_some());
    raw.distance = p.distance.unwrap_or(0.0);
    raw.divergence = p.divergence;
    raw.trim_disabled = i32::from(p.trim_disabled);
    raw.has_headphone_render_mode = i32::from(p.headphone_render_mode.is_some());
    raw.headphone_render_mode = p.headphone_render_mode.unwrap_or(0);
    raw.head_track_disabled = i32::from(p.head_track_disabled);
    raw
}

/// The scene through the raw C API, structs built by hand: every frame's bytes.
fn encode_with_raw_c_api(scene: &Scene, input: &[Vec<f32>]) -> Vec<Vec<u8>> {
    let objects: Vec<sys::iclforge_ac4_object_config_t> = scene
        .objects
        .objects
        .iter()
        .map(|o| {
            let mut raw: sys::iclforge_ac4_object_config_t = unsafe { std::mem::zeroed() };
            unsafe { sys::iclforge_ac4_object_config_init(&mut raw) };
            if let Some(bed) = o.bed {
                assert_eq!(
                    bed,
                    BedChannel::Left,
                    "this scene's only bed is L (Table 66's code 0)"
                );
                raw.has_bed = 1;
                raw.bed = 0;
            }
            raw.lfe = i32::from(o.lfe);
            raw.properties = raw_properties(&o.properties);
            raw
        })
        .collect();
    let mut objects_config: sys::iclforge_ac4_objects_config_t = unsafe { std::mem::zeroed() };
    unsafe { sys::iclforge_ac4_objects_config_init(&mut objects_config) };
    objects_config.objects = objects.as_ptr();
    objects_config.object_count = objects.len();
    objects_config.coding = match scene.objects.coding {
        ObjectCoding::Ajoc => 0,
        ObjectCoding::Direct => 1,
    };
    if let Some(signals) = scene.objects.downmix_signals {
        objects_config.has_downmix_signals = 1;
        objects_config.downmix_signals = signals;
    }
    let mut config: sys::iclforge_ac4_encoder_config_t = unsafe { std::mem::zeroed() };
    unsafe { sys::iclforge_ac4_encoder_config_init(&mut config) };
    config.bitrate_kbps = 256;
    config.experimental.objects = 1;
    config.objects = &objects_config;

    let mut encoder: *mut sys::iclforge_ac4_encoder_t = std::ptr::null_mut();
    let status = unsafe { sys::iclforge_ac4_encoder_create(&config, &mut encoder) };
    assert_eq!(status, sys::iclforge_status_ICLFORGE_OK);
    let mut update: sys::iclforge_ac4_object_metadata_update_t = unsafe { std::mem::zeroed() };
    unsafe { sys::iclforge_ac4_object_metadata_update_init(&mut update) };
    update.object = scene.update.object;
    update.sample = scene.update.sample;
    update.ramp_samples = scene.update.ramp_samples;
    update.properties = raw_properties(&scene.update.properties);

    let pointers: Vec<*const f32> = input.iter().map(|c| c.as_ptr()).collect();
    let mut frames: *mut *mut sys::iclforge_ac4_encoded_frame_t = std::ptr::null_mut();
    let mut count = 0usize;
    let mut out = Vec::new();
    let mut take = |frames: *mut *mut sys::iclforge_ac4_encoded_frame_t, count: usize| {
        for i in 0..count {
            let frame = unsafe { *frames.add(i) };
            let bytes = unsafe {
                std::slice::from_raw_parts(
                    sys::iclforge_ac4_encoded_frame_data(frame),
                    sys::iclforge_ac4_encoded_frame_size(frame),
                )
            };
            out.push(bytes.to_vec());
        }
        unsafe { sys::iclforge_ac4_encoded_frame_array_destroy(frames, count) };
    };
    let status = unsafe {
        sys::iclforge_ac4_encoder_encode_objects(
            encoder,
            pointers.as_ptr(),
            pointers.len(),
            input[0].len(),
            &update,
            1,
            &mut frames,
            &mut count,
        )
    };
    assert_eq!(status, sys::iclforge_status_ICLFORGE_OK);
    take(frames, count);
    frames = std::ptr::null_mut();
    count = 0;
    let status = unsafe { sys::iclforge_ac4_encoder_flush(encoder, &mut frames, &mut count) };
    assert_eq!(status, sys::iclforge_status_ICLFORGE_OK);
    take(frames, count);
    unsafe { sys::iclforge_ac4_encoder_destroy(encoder) };
    out
}

/// The decoded objects in the decoder's order, the samples of each end to end, and every update
/// at its sample of the decoder's output: (object, sample, ramp, properties).
struct Decoded {
    objects: Vec<iclforge::ac4::DecodedObject>,
    samples: Vec<Vec<f32>>,
    updates: Vec<(usize, i64, i32, ObjectProperties)>,
}

fn decode(frames: &[Vec<u8>]) -> Decoded {
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    let mut out = Decoded {
        objects: Vec::new(),
        samples: Vec::new(),
        updates: Vec::new(),
    };
    let mut start = 0i64;
    for bytes in frames {
        let Some(frame) = decoder.decode(bytes).unwrap() else {
            continue;
        };
        let objects = frame.objects();
        if out.samples.is_empty() {
            out.samples = vec![Vec::new(); objects.len()];
        }
        for (o, object) in objects.iter().enumerate() {
            out.samples[o].extend_from_slice(&object.samples);
            for update in &object.updates {
                out.updates.push((
                    o,
                    start + update.sample as i64,
                    update.ramp_samples,
                    update.properties,
                ));
            }
        }
        start += frame.samples_per_channel() as i64;
        out.objects = objects;
    }
    out
}

/// The decoded objects' order: the LFE first, then the bed objects, then the dynamic ones, each
/// group in the order the configuration lists it.
fn decoded_order(objects: &ObjectsConfig) -> Vec<usize> {
    let mut order: Vec<usize> = (0..objects.objects.len())
        .filter(|&o| objects.objects[o].lfe)
        .collect();
    for beds in [true, false] {
        order.extend((0..objects.objects.len()).filter(|&o| {
            let object = &objects.objects[o];
            !object.lfe && object.bed.is_some() == beds
        }));
    }
    order
}

/// The normalised correlation of `decoded`, `lag` samples later, with `reference`, from two
/// frames in to a frame before its end.
fn correlation(reference: &[f32], decoded: &[f32], lag: usize) -> f64 {
    let (mut xx, mut yy, mut xy) = (0f64, 0f64, 0f64);
    let mut n = 2 * FRAME;
    while n + FRAME < reference.len() && n + lag < decoded.len() {
        let (x, y) = (f64::from(reference[n]), f64::from(decoded[n + lag]));
        xx += x * x;
        yy += y * y;
        xy += x * y;
        n += 1;
    }
    xy / (xx * yy).max(1e-300).sqrt()
}

/// What one property's code can hold: X and Y in 62 steps, Z in 15, the gain in 1 dB, the
/// priority and each width in 31, the screen factor in 8, the divergence and the distance in
/// the tables of Annex F. An inactive object sends none of them.
fn assert_properties_near(got: &ObjectProperties, want: &ObjectProperties, dynamic: bool) {
    assert_eq!(got.active, want.active);
    if !want.active {
        return;
    }
    assert!(
        (got.gain_db - want.gain_db).abs() <= 0.5 + 1e-9,
        "{got:?} {want:?}"
    );
    assert!((got.priority - want.priority).abs() <= 1.0 / 62.0 + 1e-9);
    assert_eq!(got.trim_disabled, want.trim_disabled);
    assert_eq!(got.headphone_render_mode, want.headphone_render_mode);
    assert_eq!(
        got.head_track_disabled,
        want.headphone_render_mode.is_some() && want.head_track_disabled
    );
    if !dynamic {
        return;
    }
    assert!((got.position[0] - want.position[0]).abs() <= 1.0 / 124.0 + 1e-9);
    assert!((got.position[1] - want.position[1]).abs() <= 1.0 / 124.0 + 1e-9);
    assert!((got.position[2] - want.position[2]).abs() <= 1.0 / 30.0 + 1e-9);
    assert_eq!(got.zone_mask, want.zone_mask);
    assert_eq!(got.enable_elevation, want.enable_elevation);
    assert_eq!(got.snap, want.snap);
    for axis in 0..3 {
        assert!(
            (got.width[axis] - want.width[axis]).abs() <= 1.0 / 62.0 + 1e-9,
            "{axis}"
        );
    }
    assert!((got.screen_factor - want.screen_factor).abs() <= 1.0 / 16.0 + 1e-9);
    assert_eq!(got.depth_exponent, want.depth_exponent);
    assert_eq!(got.distance.is_some(), want.distance.is_some());
    if let (Some(got), Some(want)) = (got.distance, want.distance) {
        assert!((got - want).abs() <= 0.1 * want);
    }
    assert!((got.divergence - want.divergence).abs() <= 0.02);
}

fn check_scene(scene: &Scene) {
    let input = input_of(&scene.objects, SAMPLES);
    let (frames, lag) = encode_with_crate(scene, &input);
    assert!(!frames.is_empty());

    // The safe crate writes what the raw C API writes from the same scene.
    let raw = encode_with_raw_c_api(scene, &input);
    assert_eq!(frames.len(), raw.len());
    for (f, (a, b)) in frames.iter().zip(&raw).enumerate() {
        assert!(a == b, "frame {f} differs from the raw C API's");
    }

    let decoded = decode(&frames);
    let order = decoded_order(&scene.objects);
    assert_eq!(decoded.objects.len(), order.len());
    for (d, &o) in order.iter().enumerate() {
        let configured = &scene.objects.objects[o];
        let object = &decoded.objects[d];
        assert_eq!(object.lfe, configured.lfe, "decoded object {d}");
        if configured.lfe {
            assert_eq!(object.speaker, Some(Speaker::Lfe));
        } else if configured.bed.is_some() {
            assert_eq!(object.kind, ObjectKind::Bed);
            assert_eq!(object.speaker, Some(Speaker::Left));
        } else {
            assert_eq!(object.kind, ObjectKind::Dyn);
        }
        // The last frame's properties are the update's for the object it moved, the
        // configuration's for the rest.
        let moved = scene.update.object == o;
        let want = if moved {
            &scene.update.properties
        } else {
            &configured.properties
        };
        assert_properties_near(
            &object.properties,
            want,
            !configured.lfe && configured.bed.is_none(),
        );
        // Its audio is its own tone, and no other object's.
        if configured.properties.active {
            let own = correlation(&input[o], &decoded.samples[d], lag as usize);
            assert!(own > 0.98, "object {o} correlates {own}");
            let other = order[(d + 1) % order.len()];
            let cross = correlation(&input[other], &decoded.samples[d], lag as usize);
            assert!(cross.abs() < 0.5, "object {o} against {other}: {cross}");
        }
    }

    // The update comes out where its input sample does, to within 32 samples, with the ramp it
    // was given.
    let moved_decoded = order
        .iter()
        .position(|&o| o == scene.update.object)
        .unwrap();
    let want = &scene.update.properties;
    let found = decoded
        .updates
        .iter()
        .find(|(object, _, _, p)| {
            *object == moved_decoded
                && (p.position[0] - want.position[0]).abs() <= 1.0 / 124.0 + 1e-9
                && (p.position[1] - want.position[1]).abs() <= 1.0 / 124.0 + 1e-9
        })
        .expect("the update was decoded");
    assert!(found.1 <= scene.update.sample + lag, "{found:?}");
    assert!(found.1 > scene.update.sample + lag - 32, "{found:?}");
    assert_eq!(found.2, scene.update.ramp_samples);
    assert_properties_near(&found.3, want, true);
}

#[test]
fn an_ajoc_scene_encodes_as_the_c_api_does_and_decodes_back() {
    check_scene(&ajoc_scene());
}

#[test]
fn a_direct_coded_scene_encodes_as_the_c_api_does_and_decodes_back() {
    check_scene(&direct_scene());
}

#[test]
fn object_defaults_are_the_encoders_not_zeroes() {
    let properties = ObjectProperties::default();
    assert!(properties.active);
    assert_eq!(properties.gain_db, 0.0);
    assert_eq!(properties.priority, 1.0);
    assert_eq!(properties.position, [0.5, 0.5, 0.0]);
    assert_eq!(properties.depth_exponent, 1.0);
    assert!(properties.enable_elevation);
    assert_eq!(properties.distance, None);
    // A default configuration is one the encoder accepts, and the object types' defaults are
    // built from it.
    let config = config_of(ObjectsConfig {
        objects: vec![ObjectConfig::default(), lfe()],
        ..Default::default()
    });
    assert_eq!(Encoder::refusal_reason(&config), "");
    assert_eq!(ObjectMetadataUpdate::default().properties, properties);
    assert_eq!(EncoderConfig::default().objects, None);
    assert_eq!(
        EncoderConfig::default().experimental,
        Experimental::default()
    );
}

#[test]
fn the_limits_and_refusals_are_the_encoders() {
    let objects = ajoc_scene().objects;
    let refusal = |change: &dyn Fn(&mut EncoderConfig)| {
        let mut config = config_of(objects.clone());
        change(&mut config);
        Encoder::refusal_reason(&config)
    };
    assert_eq!(refusal(&|_| {}), "");

    assert_eq!(
        refusal(&|c| c.experimental.objects = false),
        "objects without experimental.objects"
    );
    assert_eq!(
        refusal(&|c| c.frame_rate_index = 2),
        "objects at a frame_rate_index other than 13"
    );
    let many = |count: usize, kbps: i32, signals: Option<i32>| {
        let mut config = config_of(ObjectsConfig {
            objects: vec![ObjectConfig::default(); count],
            downmix_signals: signals,
            ..Default::default()
        });
        config.bitrate_kbps = kbps;
        config
    };
    let most = sys::ICLFORGE_AC4_MAX_OBJECTS as usize;
    assert_eq!(Encoder::refusal_reason(&many(most, 384, None)), "");
    assert!(Encoder::new(&many(most, 384, None)).is_ok());
    assert_eq!(
        Encoder::refusal_reason(&many(most + 1, 384, None)),
        "more than 64 objects"
    );
    assert_eq!(
        Encoder::new(&many(most + 1, 384, None)).err(),
        Some(Error::Ac4EncodeInvalidConfig)
    );
    let signals = sys::ICLFORGE_AC4_MAX_DOWNMIX_SIGNALS as i32;
    assert_eq!(
        Encoder::refusal_reason(&many(signals as usize, 512, Some(signals))),
        ""
    );
    assert_eq!(
        Encoder::refusal_reason(&many(signals as usize + 1, 512, Some(signals + 1))),
        "a computed downmix of no signal, of more than 11 or of more than its full-band objects"
    );
    assert_eq!(
        refusal(&|c| {
            c.objects.as_mut().unwrap().coding = ObjectCoding::Direct;
        }),
        "bed objects in direct-coded object substreams"
    );
    assert_eq!(
        refusal(&|c| {
            let objects = c.objects.as_mut().unwrap();
            objects.downmix = AjocDownmix::Static50;
        }),
        "an LFE object with a static 5.0 downmix"
    );
    assert_eq!(
        refusal(&|c| c.objects.as_mut().unwrap().parameter_bands = Some(10)),
        "A-JOC parameter bands other than Table 78's 23, 15, 12, 9, 7, 5, 3 or 1"
    );
    assert_eq!(
        refusal(&|c| c.codec_mode = CodecMode::AspxAcpl2),
        "an object substream's codec mode other than kAuto, kSimple or kAspx"
    );
    assert_eq!(
        refusal(&|c| c.objects.as_mut().unwrap().objects[0].properties.position[0] = 1.5),
        "an object's properties off the ranges ObjectProperties gives them"
    );
}

#[test]
fn encode_objects_refuses_input_the_encoder_refuses() {
    let scene = ajoc_scene();
    let input = input_of(&scene.objects, 2 * FRAME);
    let mut encoder = Encoder::new(&config_of(scene.objects.clone())).unwrap();
    let update = |change: &dyn Fn(&mut ObjectMetadataUpdate)| {
        let mut update = ObjectMetadataUpdate::default();
        change(&mut update);
        update
    };
    let invalid_input = Some(Error::Ac4EncodeInvalidInput);
    // An object the configuration lacks, an input sample before the first, a property off its
    // range, and the wrong number of objects.
    for bad in [
        update(&|u| u.object = input.len()),
        update(&|u| u.object = usize::MAX),
        update(&|u| u.sample = -1),
        update(&|u| u.properties.priority = 2.0),
    ] {
        assert_eq!(
            encoder.encode_objects(&views(&input), &[bad]).err(),
            invalid_input
        );
    }
    assert_eq!(
        encoder.encode_objects(&views(&input)[1..], &[]).err(),
        invalid_input
    );
    // Slices of different lengths, and none, are not input at all.
    let mut ragged = views(&input);
    ragged[1] = &input[1][..100];
    assert_eq!(
        encoder.encode_objects(&ragged, &[]).err(),
        Some(Error::InvalidArgument)
    );
    assert_eq!(
        encoder.encode_objects(&[], &[]).err(),
        Some(Error::InvalidArgument)
    );
    // The refusals left the encoder as it was: the same input encodes, with an update or
    // without (and by encode(), which takes none).
    assert!(encoder
        .encode_objects(&views(&input), &[update(&|_| {})])
        .is_ok());
    assert!(encoder.encode(&views(&input)).is_ok());

    // An encoder of channels has no object for an update to name.
    let mut stereo = Encoder::new(&EncoderConfig::default()).unwrap();
    assert_eq!(
        stereo
            .encode_objects(&views(&input)[..2], &[ObjectMetadataUpdate::default()])
            .err(),
        invalid_input
    );
    assert!(stereo.encode_objects(&views(&input)[..2], &[]).is_ok());
}

fn tones(channels: usize, frames: usize) -> Vec<Vec<f32>> {
    (0..channels)
        .map(|c| {
            (0..frames * FRAME)
                .map(|n| {
                    let hz = 500.0 + 170.0 * c as f64;
                    (0.2 * (2.0 * std::f64::consts::PI * hz * n as f64 / RATE).sin()) as f32
                })
                .collect()
        })
        .collect()
}

/// Which frames of the stream `config` writes for `input` are I-frames.
fn iframes_of(config: &EncoderConfig, input: &[Vec<f32>]) -> Vec<usize> {
    let mut encoder = Encoder::new(config).unwrap();
    let mut frames = encoder.encode(&views(input)).unwrap();
    frames.extend(encoder.flush().unwrap());
    frames
        .iter()
        .enumerate()
        .filter(|(_, f)| f.iframe())
        .map(|(i, _)| i)
        .collect()
}

#[test]
fn the_iframe_lists_reach_the_encoder() {
    let input = tones(2, 8);
    let base = EncoderConfig {
        bitrate_kbps: 96,
        iframe_interval: 1000, // the first frame alone, without the lists
        ..Default::default()
    };
    assert_eq!(iframes_of(&base, &input), vec![0]);

    let listed = EncoderConfig {
        iframes: vec![5, 2], // in any order
        ..base.clone()
    };
    assert_eq!(iframes_of(&listed, &input), vec![0, 2, 5]);

    // Frame 2's output starts at sample 4096 exactly; frame 5's is the first to start after
    // 9000 (frame 4's is 8192, frame 5's 10240).
    let fragments = EncoderConfig {
        fragment_starts: vec![4096, 9000],
        ..base
    };
    assert_eq!(iframes_of(&fragments, &input), vec![0, 2, 5]);
}

#[test]
fn the_experimental_flags_reach_the_encoder() {
    let refusal = |config: EncoderConfig| Encoder::refusal_reason(&config);

    // ASPX_ACPL_1 in stereo needs acpl.
    let stereo = EncoderConfig {
        bitrate_kbps: 64,
        codec_mode: CodecMode::AspxAcpl1,
        ..Default::default()
    };
    assert!(!refusal(stereo.clone()).is_empty());
    let with_acpl = EncoderConfig {
        experimental: Experimental {
            acpl: true,
            ..Default::default()
        },
        ..stereo
    };
    assert_eq!(refusal(with_acpl.clone()), "");
    assert!(Encoder::new(&with_acpl).is_ok());

    // 7.1 needs its additional pair, and 5.1 refuses one.
    let seven_one = EncoderConfig {
        channels: 8,
        bitrate_kbps: 448,
        ..Default::default()
    };
    assert_eq!(
        refusal(seven_one.clone()),
        "seven or eight channels without experimental.seven_x's additional pair"
    );
    for pair in [
        AdditionalPair::Back,
        AdditionalPair::Wide,
        AdditionalPair::TopFront,
    ] {
        let config = EncoderConfig {
            experimental: Experimental {
                seven_x: pair,
                ..Default::default()
            },
            ..seven_one.clone()
        };
        assert_eq!(refusal(config), "");
    }
    let five_one = EncoderConfig {
        channels: 6,
        experimental: Experimental {
            seven_x: AdditionalPair::Back,
            ..Default::default()
        },
        ..Default::default()
    };
    assert_eq!(
        refusal(five_one),
        "experimental.seven_x's additional pair without seven or eight channels"
    );

    // 7.1.4 needs back_pair, ASPX_AJCC needs ajcc, and coding_configs is refused beside A-CPL.
    let back = EncoderConfig {
        channels: 12,
        bitrate_kbps: 768,
        ..Default::default()
    };
    assert!(refusal(back.clone()).contains("experimental.back_pair"));
    let back = EncoderConfig {
        experimental: Experimental {
            back_pair: true,
            ..Default::default()
        },
        ..back
    };
    assert_eq!(refusal(back), "");
    let ajcc = EncoderConfig {
        channels: 10,
        bitrate_kbps: 448,
        codec_mode: CodecMode::AspxAjcc,
        ..Default::default()
    };
    assert_eq!(refusal(ajcc.clone()), "ASPX_AJCC without experimental.ajcc");
    let ajcc = EncoderConfig {
        experimental: Experimental {
            ajcc: true,
            ..Default::default()
        },
        ..ajcc
    };
    assert_eq!(refusal(ajcc), "");
    let acpl_2 = EncoderConfig {
        channels: 6,
        bitrate_kbps: 128,
        codec_mode: CodecMode::AspxAcpl2,
        ..Default::default()
    };
    assert_eq!(refusal(acpl_2.clone()), "");
    let acpl_2 = EncoderConfig {
        experimental: Experimental {
            coding_configs: true,
            ..Default::default()
        },
        ..acpl_2
    };
    assert_eq!(
        refusal(acpl_2),
        "an A-CPL codec mode with experimental.coding_configs"
    );
}
