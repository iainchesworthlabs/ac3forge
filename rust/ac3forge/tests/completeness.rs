//! Round trips for the surface AP9's completeness pass added: the wide-layout access-unit
//! encoder, the Atmos object encoder and the object-audio decode accessors, the stream
//! framing/scan helpers, and the loudness meter. Same real-signal discipline as roundtrip.rs —
//! synthesized tones, several frames, never silence and never only frame 0.

use ac3forge::atmos::{AtmosConfig, AtmosEncoder, ObjectPlacement};
use ac3forge::eac3;
use ac3forge::meter::{dialnorm_from_lkfs, LoudnessMeter};
use ac3forge::stream;
use ac3forge::types::{Acmod, DecoderConfig, SampleRate};
use ac3forge::SAMPLES_PER_FRAME;

const FRAME_COUNT: usize = 6;

fn tone_channel(frequency_hz: f32, frame_index: usize) -> Vec<f32> {
    (0..SAMPLES_PER_FRAME)
        .map(|i| {
            let sample_index = (frame_index * SAMPLES_PER_FRAME + i) as f32;
            0.3 * (2.0 * std::f32::consts::PI * frequency_hz * sample_index / 48_000.0).sin()
        })
        .collect()
}

fn rms(samples: &[f32]) -> f32 {
    (samples.iter().map(|s| s * s).sum::<f32>() / samples.len() as f32).sqrt()
}

/// 5.1.2 = a 5.1 bed plus one dependent substream adding the Vhl/Vhr height pair
/// (`AC3FORGE_CHANMAP_512_HEIGHT`) - the same recipe tests/capi/test_capi.cpp proves at the C
/// level. A wide layout the single-substream encoder cannot express is exactly what
/// AccessUnitEncoder exists for.
#[test]
fn access_unit_encoder_round_trips_a_wide_layout() {
    let independent = eac3::Eac3FrameConfig {
        sample_rate: SampleRate::Hz48000,
        bitrate_kbps: 448,
        acmod: Acmod::Channels3_2,
        lfe: true,
        ..Default::default()
    };
    let dependent = eac3::Eac3FrameConfig {
        sample_rate: SampleRate::Hz48000,
        bitrate_kbps: 192,
        acmod: Acmod::Stereo,
        chanmap: Some(ac3forge_sys::AC3FORGE_CHANMAP_512_HEIGHT as u16),
        ..Default::default()
    };
    let mut encoder =
        eac3::AccessUnitEncoder::new(&independent, std::slice::from_ref(&dependent)).unwrap();
    // 5.1 bed (6 channels) + Vhl/Vhr = 8.
    assert_eq!(encoder.channel_count(), 8);
    assert!(encoder.latency_samples() > 0);

    let mut decoder = eac3::Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    let frequencies = [220.0, 330.0, 440.0, 60.0, 550.0, 660.0, 770.0, 880.0];

    for frame_index in 0..FRAME_COUNT {
        let channels: Vec<Vec<f32>> = frequencies
            .iter()
            .map(|&f| tone_channel(f, frame_index))
            .collect();
        let views: Vec<&[f32]> = channels.iter().map(|c| c.as_slice()).collect();
        let unit = encoder.encode(&views, None).unwrap();
        assert!(!unit.is_empty());
        assert_eq!(unit.substream_count(), 2);
        let total: u32 = (0..unit.substream_count())
            .map(|i| unit.substream_bytes(i))
            .sum();
        assert_eq!(total as usize, unit.len());

        let decoded = decoder.decode_access_unit(&unit).unwrap();
        let decoded = decoded.expect("no tool here engages the section 3.7 hold-back");
        // The rendered programme is the full 5.1.2: 8 channels, every one carrying signal
        // after the transform warms up.
        assert_eq!(decoded.channel_count(), 8);
        assert_eq!(decoded.samples_per_channel(), SAMPLES_PER_FRAME);
        if frame_index >= 2 {
            for channel in 0..decoded.channel_count() {
                assert!(
                    rms(decoded.channel_samples(channel)) > 0.01,
                    "channel {channel} silent in frame {frame_index}"
                );
            }
        }
    }
}

#[test]
fn atmos_objects_round_trip_with_positions_and_audio() {
    let config = AtmosConfig {
        bitrate_kbps: 640,
        ..Default::default()
    };
    let mut encoder = AtmosEncoder::new(&config, 2).unwrap();
    assert_eq!(encoder.dynamic_object_count(), 2);
    // The object path pays the QMF filterbank on top of the bed's own overlap.
    assert!(encoder.latency_samples() > encoder.bed_latency().transform_samples);

    let placements = [
        ObjectPlacement {
            x: 0.1,
            y: 0.2,
            z: 0.5,
            ..Default::default()
        },
        ObjectPlacement {
            x: 0.9,
            y: 0.8,
            z: -0.5,
            ..Default::default()
        },
    ];
    // The C-init default is the muted-object regression this crate's first pass found — assert
    // the Rust Default rides the fixed initializer, not zeroes.
    assert_eq!(ObjectPlacement::default().gain, 1.0);

    let mut decoder = eac3::Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    let mut saw_object_audio = false;
    for frame_index in 0..FRAME_COUNT {
        let a = tone_channel(300.0, frame_index);
        let b = tone_channel(1200.0, frame_index);
        let unit = encoder.encode_frame(&[&a, &b], &placements).unwrap();
        assert!(!unit.is_empty());

        let decoded = decoder.decode_substream(&unit).unwrap();
        let decoded = decoded.expect("the atmos bed engages no hold-back");
        assert!(decoded.has_object_metadata());
        assert_eq!(decoded.dynamic_object_count(), 2);
        // Positions survive OAMD's own quantizers: x/y on 62 steps, z on 15 —
        // a tolerance well past either step keeps this a wiring check, not a
        // quantizer characterization.
        let first = decoded.dynamic_object(0);
        assert!((first.x - 0.1).abs() < 0.02 && (first.z - 0.5).abs() < 0.07);
        let second = decoded.dynamic_object(1);
        assert!((second.x - 0.9).abs() < 0.02 && (second.z + 0.5).abs() < 0.07);

        if frame_index >= 2 {
            assert_eq!(decoded.object_audio_count(), 2);
            for object in 0..2 {
                assert!(
                    rms(decoded.object_audio(object)) > 0.005,
                    "object {object} silent in frame {frame_index}"
                );
            }
            saw_object_audio = true;
        }
    }
    assert!(saw_object_audio);
}

#[test]
fn stream_helpers_split_and_scan_what_the_encoder_wrote() {
    let config = eac3::Eac3FrameConfig {
        sample_rate: SampleRate::Hz48000,
        bitrate_kbps: 192,
        acmod: Acmod::Stereo,
        ..Default::default()
    };
    let mut encoder = eac3::Eac3Encoder::new(&config).unwrap();
    let mut elementary = Vec::new();
    for frame_index in 0..FRAME_COUNT {
        let left = tone_channel(500.0, frame_index);
        let right = tone_channel(700.0, frame_index);
        let frame = encoder.encode_frame(&[&left, &right], None, None).unwrap();
        elementary.extend_from_slice(&frame);
    }

    let frames = stream::split_frames(&elementary).unwrap();
    assert_eq!(frames.len(), FRAME_COUNT);
    let units = stream::split_access_units(&elementary).unwrap();
    assert_eq!(units.len(), FRAME_COUNT);
    assert_eq!(stream::stream_bsid(frames[0]).unwrap(), 16); // Annex E

    let scanned = stream::scan(&elementary).unwrap();
    assert_eq!(scanned.kind(), Some(stream::StreamKind::Eac3));
    assert_eq!(scanned.sample_rate(), Some(SampleRate::Hz48000));
    assert_eq!(scanned.acmod(), Some(Acmod::Stereo));
    assert!(!scanned.lfe());
    assert_eq!(scanned.channels(), 2);
    assert_eq!(scanned.access_unit_count(), FRAME_COUNT);
    assert_eq!(scanned.substreams_per_unit(), 1);
    assert_eq!(scanned.bsid(), 16);
    assert_eq!(scanned.oba_complexity_index(), None);
    for (index, unit) in units.iter().enumerate() {
        assert_eq!(scanned.access_unit(index), *unit);
        assert_eq!(
            scanned.access_unit_samples(index) as usize,
            SAMPLES_PER_FRAME
        );
    }
}

#[test]
fn loudness_meter_measures_a_programme_and_derives_dialnorm() {
    let mut meter = LoudnessMeter::new(SampleRate::Hz48000, Acmod::Stereo, false).unwrap();
    assert_eq!(meter.channel_count(), 2);
    // Nothing pushed yet: every gated measurement is honestly absent.
    assert_eq!(meter.integrated_lkfs(), None);

    // Two seconds of tone — enough for the integrated gate and the momentary window alike.
    for frame_index in 0..64 {
        let left = tone_channel(997.0, frame_index);
        let right = tone_channel(997.0, frame_index);
        meter.push(&[&left, &right]).unwrap();
    }
    let integrated = meter
        .integrated_lkfs()
        .expect("two seconds is past the integrated gate");
    // A -10.5 dBFS-ish stereo sine lands in the mid -10s LKFS; the exact figure is the
    // library's business, the sanity band is this test's.
    assert!(
        integrated < 0.0 && integrated > -30.0,
        "integrated {integrated}"
    );
    assert!(meter.momentary_lkfs().is_some());
    assert!(meter.true_peak_dbtp().is_some());

    let dialnorm = dialnorm_from_lkfs(integrated);
    assert!((1..=31).contains(&dialnorm));
    // The mapping is -LKFS clamped into 1..=31 — spot-check the round trip.
    assert_eq!(dialnorm, (-integrated).round().clamp(1.0, 31.0) as i32);

    // The chanmap constructor reaches the layouts an acmod cannot name - here the
    // Ls/Rs/Lrs/Rrs rear quad a 7.1 dependent carries (AC3FORGE_CHANMAP_71_REAR).
    let wide = LoudnessMeter::for_chanmap(
        SampleRate::Hz48000,
        ac3forge_sys::AC3FORGE_CHANMAP_71_REAR as u16,
    )
    .unwrap();
    assert_eq!(wide.channel_count(), 4);
}
