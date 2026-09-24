//! The AC-3 surface (`ac3::Encoder`/`Decoder`/`DecodedFrame`, `EncoderConfig`, the shared value
//! types) beyond the stereo happy path roundtrip.rs covers: every acmod/LFE combination, every
//! AC-3 sample rate, DRC/heavy/mix-level/dual-mono configs, argument validation, latency, and an
//! aligned SNR check that proves the decoded waveform is the encoded one.

mod common;

use ac3forge::ac3::{Decoder, Encoder, EncoderConfig};
use ac3forge::types::{
    Acmod, CentreMixLevel, DecoderConfig, DrcProfile, HeavyConfig, Latency, SampleRate,
    SurroundMixLevel,
};
use ac3forge::{Error, SAMPLES_PER_FRAME};
use common::{best_lag, rms, snr_db, tone};

const ALL_ACMODS: [Acmod; 8] = [
    Acmod::DualMono,
    Acmod::Mono,
    Acmod::Stereo,
    Acmod::Channels3_0,
    Acmod::Channels2_1,
    Acmod::Channels3_1,
    Acmod::Channels2_2,
    Acmod::Channels3_2,
];

fn channels_for(count: usize, sample_rate_hz: f32, frame_index: usize) -> Vec<Vec<f32>> {
    (0..count)
        .map(|c| tone(250.0 + 170.0 * c as f32, sample_rate_hz, 0.25, frame_index))
        .collect()
}

fn views(channels: &[Vec<f32>]) -> Vec<&[f32]> {
    channels.iter().map(|c| c.as_slice()).collect()
}

#[test]
fn encoder_config_default_mirrors_the_c_initializer() {
    let config = EncoderConfig::default();
    assert_eq!(config.sample_rate, SampleRate::Hz48000);
    assert_eq!(config.bitrate_kbps, 192);
    assert_eq!(config.dialnorm, 31);
    assert_eq!(config.acmod, Acmod::Stereo);
    assert!(!config.lfe);
    assert_eq!(config.dialnorm2, None);
    assert_eq!(config.chbwcod, None);
    assert_eq!(config.cplbegf, None);
    assert_eq!(config.cplendf, None);
    assert_eq!(config.drc, None);
    assert_eq!(config.heavy, None);
    assert_eq!(config.drc2, None);
    assert_eq!(config.heavy2, None);
    // Clone + PartialEq are part of the public contract of the config type.
    assert_eq!(config.clone(), config);
    assert!(format!("{config:?}").contains("EncoderConfig"));
}

/// `CentreMixLevel::default()`/`SurroundMixLevel::default()` are the library's own downmix
/// defaults - -4.5 dB and -6 dB, what `ac3forge_encoder_config_init()` (and so the C++
/// `ac3::EncoderConfig`, docs/library/encoding-ac3.md) sets - not merely the first variant.
/// They used to derive `Minus3Db` for both, so a config built field-by-field with
/// `..Default::default()` on the enum disagreed with `EncoderConfig::default()`.
#[test]
fn mix_level_defaults_match_the_c_initializer() {
    // SAFETY: config_init fills every field; the zeroed value is never read.
    let mut raw: ac3forge_sys::ac3forge_encoder_config_t = unsafe { std::mem::zeroed() };
    unsafe { ac3forge_sys::ac3forge_encoder_config_init(&mut raw) };
    assert_eq!(
        raw.cmixlev,
        ac3forge_sys::ac3forge_centre_mix_level_AC3FORGE_CMIXLEV_MINUS_4_5DB
    );
    assert_eq!(
        raw.surmixlev,
        ac3forge_sys::ac3forge_surround_mix_level_AC3FORGE_SURMIXLEV_MINUS_6DB
    );
    assert_eq!(CentreMixLevel::default(), CentreMixLevel::Minus4_5Db);
    assert_eq!(SurroundMixLevel::default(), SurroundMixLevel::Minus6Db);
    let config = EncoderConfig::default();
    assert_eq!(config.cmixlev, CentreMixLevel::default());
    assert_eq!(config.surmixlev, SurroundMixLevel::default());
}

#[test]
fn shared_value_type_defaults() {
    let heavy = HeavyConfig::default();
    assert_eq!(heavy.dialogue_target_dbfs, -20.0);
    assert_eq!(heavy.peak_ceiling_dbfs, -0.5);
    assert_eq!(heavy.release_db_per_second, 20.0);
    let decoder = DecoderConfig::default();
    assert_eq!(decoder.drc_scale, 0.0);
    assert!(!decoder.heavy_compression);
    assert_eq!(Latency::default().total_samples(), 0);
    let latency = Latency {
        frame_samples: 1,
        transform_samples: 20,
        lookahead_samples: 300,
        holdback_samples: 4000,
    };
    assert_eq!(latency.total_samples(), 4321);
}

#[test]
fn sample_rate_hz_and_acmod_channel_counts() {
    let rates = [
        (SampleRate::Hz48000, 48_000),
        (SampleRate::Hz44100, 44_100),
        (SampleRate::Hz32000, 32_000),
        (SampleRate::Hz24000, 24_000),
        (SampleRate::Hz22050, 22_050),
        (SampleRate::Hz16000, 16_000),
    ];
    for (rate, hz) in rates {
        assert_eq!(rate.hz(), hz);
    }
    let counts = [2, 1, 2, 3, 3, 4, 4, 5];
    for (acmod, count) in ALL_ACMODS.iter().zip(counts) {
        assert_eq!(acmod.full_bandwidth_channel_count(), count, "{acmod:?}");
    }
}

/// Every acmod, with and without LFE: the encoder asks for the right span count, and the decoder
/// reports back the same acmod/LFE/channel count with signal in every channel.
#[test]
fn every_acmod_and_lfe_combination_round_trips() {
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    assert_eq!(decoder.latency_samples(), 0);
    for acmod in ALL_ACMODS {
        for lfe in [false, true] {
            let config = EncoderConfig {
                acmod,
                lfe,
                bitrate_kbps: 448,
                // Dual mono carries a second programme, which needs its own dialnorm.
                dialnorm2: (acmod == Acmod::DualMono).then_some(31),
                ..Default::default()
            };
            let mut encoder = Encoder::new(&config).unwrap();
            let expected = acmod.full_bandwidth_channel_count() + usize::from(lfe);
            assert_eq!(encoder.channel_count(), expected, "{acmod:?} lfe={lfe}");

            let mut last = None;
            for frame_index in 0..2 {
                let input = channels_for(expected, 48_000.0, frame_index);
                let frame = encoder.encode_frame(&views(&input)).unwrap();
                last = Some(decoder.decode_frame(&frame).unwrap());
            }
            let decoded = last.unwrap();
            assert_eq!(decoded.acmod(), Some(acmod));
            assert_eq!(decoded.lfe(), lfe);
            assert_eq!(decoded.channel_count(), expected);
            assert_eq!(decoded.bitrate_kbps(), 448);
            assert_eq!(decoded.samples_per_channel(), SAMPLES_PER_FRAME);
            // Full-bandwidth channels carry their tones; the LFE channel (low-passed at 120 Hz)
            // carries the 250+ Hz tone attenuated, so only the full-bandwidth ones are gated.
            for channel in 0..acmod.full_bandwidth_channel_count() {
                let level = rms(decoded.channel_samples(channel));
                assert!(
                    level > 0.05,
                    "{acmod:?} lfe={lfe} channel {channel} level {level}"
                );
            }
        }
    }
}

#[test]
fn ac3_sample_rates_round_trip_and_reduced_rates_are_rejected() {
    for (rate, hz) in [
        (SampleRate::Hz48000, 48_000.0),
        (SampleRate::Hz44100, 44_100.0),
        (SampleRate::Hz32000, 32_000.0),
    ] {
        let config = EncoderConfig {
            sample_rate: rate,
            ..Default::default()
        };
        let mut encoder = Encoder::new(&config).unwrap();
        let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
        let input = channels_for(2, hz, 1);
        let frame = encoder.encode_frame(&views(&input)).unwrap();
        // A/52 Table 5.18: frame size scales with 1/fs at a fixed bitrate (words * 2 bytes).
        let expected_bytes = match rate {
            SampleRate::Hz48000 => 768,
            SampleRate::Hz44100 => 834,
            _ => 1152,
        };
        assert_eq!(frame.len(), expected_bytes, "{rate:?}");
        let decoded = decoder.decode_frame(&frame).unwrap();
        assert_eq!(decoded.sample_rate(), Some(rate));
    }
    // The Annex E reduced rates are E-AC-3 only; the AC-3 encoder refuses them (the C side
    // reports it as a bitrate that has no frame size at that rate).
    for rate in [
        SampleRate::Hz24000,
        SampleRate::Hz22050,
        SampleRate::Hz16000,
    ] {
        let mut encoder = Encoder::new(&EncoderConfig {
            sample_rate: rate,
            ..Default::default()
        })
        .unwrap();
        let input = channels_for(2, 48_000.0, 0);
        assert_eq!(
            encoder.encode_frame(&views(&input)).err().unwrap(),
            Error::EncodeInvalidBitrate,
            "{rate:?}"
        );
    }
}

#[test]
fn metadata_configs_encode_and_dialnorm_survives() {
    let heavy = HeavyConfig {
        dialogue_target_dbfs: -24.0,
        peak_ceiling_dbfs: -1.0,
        release_db_per_second: 10.0,
    };
    let profiles = [
        DrcProfile::FilmStandard,
        DrcProfile::FilmLight,
        DrcProfile::MusicStandard,
        DrcProfile::MusicLight,
        DrcProfile::Speech,
    ];
    let cmix = [
        CentreMixLevel::Minus3Db,
        CentreMixLevel::Minus4_5Db,
        CentreMixLevel::Minus6Db,
    ];
    let surmix = [
        SurroundMixLevel::Minus3Db,
        SurroundMixLevel::Minus6Db,
        SurroundMixLevel::Silent,
    ];
    for (index, profile) in profiles.into_iter().enumerate() {
        let config = EncoderConfig {
            acmod: Acmod::Channels3_2,
            lfe: true,
            bitrate_kbps: 384,
            dialnorm: 20 + index as i32,
            drc: Some(profile),
            heavy: Some(heavy),
            cmixlev: cmix[index % 3],
            surmixlev: surmix[index % 3],
            coupling: true,
            fast_mdct: false,
            chbwcod: Some(40),
            ..Default::default()
        };
        let mut encoder = Encoder::new(&config).unwrap();
        let mut decoder = Decoder::new(&DecoderConfig {
            drc_scale: 1.0,
            heavy_compression: index % 2 == 0,
        })
        .unwrap();
        let input = channels_for(6, 48_000.0, 1);
        let frame = encoder.encode_frame(&views(&input)).unwrap();
        let decoded = decoder.decode_frame(&frame).unwrap();
        assert_eq!(decoded.dialnorm(), 20 + index as i32, "{profile:?}");
        assert_eq!(decoded.channel_count(), 6);
    }
}

#[test]
fn dual_mono_with_per_channel_metadata() {
    let config = EncoderConfig {
        acmod: Acmod::DualMono,
        dialnorm: 27,
        dialnorm2: Some(12),
        drc: Some(DrcProfile::FilmLight),
        drc2: Some(DrcProfile::Speech),
        heavy: Some(HeavyConfig::default()),
        heavy2: Some(HeavyConfig::default()),
        ..Default::default()
    };
    let mut encoder = Encoder::new(&config).unwrap();
    assert_eq!(encoder.channel_count(), 2);
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    for frame_index in 0..3 {
        let ch1 = tone(300.0, 48_000.0, 0.3, frame_index);
        let ch2 = tone(3_000.0, 48_000.0, 0.3, frame_index);
        let frame = encoder.encode_frame(&[&ch1, &ch2]).unwrap();
        let decoded = decoder.decode_frame(&frame).unwrap();
        assert_eq!(decoded.acmod(), Some(Acmod::DualMono));
        // dialnorm() reports Ch1's programme.
        assert_eq!(decoded.dialnorm(), 27);
        if frame_index > 0 {
            // Two independent programmes: neither channel may leak into the other.
            assert!(rms(decoded.channel_samples(0)) > 0.1);
            assert!(rms(decoded.channel_samples(1)) > 0.1);
        }
    }
}

#[test]
fn dual_mono_requires_a_second_dialnorm() {
    let mut encoder = Encoder::new(&EncoderConfig {
        acmod: Acmod::DualMono,
        dialnorm2: None,
        ..Default::default()
    })
    .unwrap();
    let s = tone(440.0, 48_000.0, 0.3, 0);
    assert_eq!(
        encoder.encode_frame(&[&s, &s]).err().unwrap(),
        Error::EncodeInvalidDialnorm
    );
    for dialnorm2 in [0, 32] {
        let mut encoder = Encoder::new(&EncoderConfig {
            acmod: Acmod::DualMono,
            dialnorm2: Some(dialnorm2),
            ..Default::default()
        })
        .unwrap();
        assert_eq!(
            encoder.encode_frame(&[&s, &s]).err().unwrap(),
            Error::EncodeInvalidDialnorm,
            "dialnorm2 {dialnorm2}"
        );
    }
}

#[test]
fn encode_frame_validates_span_count_and_length() {
    let mut encoder = Encoder::new(&EncoderConfig::default()).unwrap();
    let good = vec![0.1f32; SAMPLES_PER_FRAME];
    let short = vec![0.1f32; SAMPLES_PER_FRAME - 1];
    let long = vec![0.1f32; SAMPLES_PER_FRAME + 1];
    assert_eq!(
        encoder.encode_frame(&[]).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(
        encoder.encode_frame(&[&good]).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(
        encoder.encode_frame(&[&good, &good, &good]).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(
        encoder.encode_frame(&[&good, &short]).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(
        encoder.encode_frame(&[&long, &good]).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(
        encoder.encode_frame(&[&[], &[]]).err().unwrap(),
        Error::InvalidArgument
    );
    // A rejected call leaves the encoder usable.
    assert!(!encoder.encode_frame(&[&good, &good]).unwrap().is_empty());
}

#[test]
fn latency_is_constant_and_matches_the_documented_terms() {
    let encoder = Encoder::new(&EncoderConfig::default()).unwrap();
    let latency = encoder.latency();
    assert_eq!(latency.frame_samples, SAMPLES_PER_FRAME as i32);
    assert_eq!(latency.transform_samples, 256);
    assert_eq!(latency.holdback_samples, 0);
    assert_eq!(
        latency.total_samples(),
        latency.frame_samples + latency.transform_samples + latency.lookahead_samples
    );
    assert_eq!(encoder.latency(), latency);
}

/// A waveform-level check, not just an energy one: after aligning by the best cross-correlation
/// lag, the decoded stereo stream matches the input to a codec-grade SNR, and the two channels
/// are not swapped.
#[test]
fn stereo_round_trip_snr_after_alignment() {
    const FRAMES: usize = 6;
    let mut encoder = Encoder::new(&EncoderConfig {
        bitrate_kbps: 384,
        ..Default::default()
    })
    .unwrap();
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    let (mut in_l, mut in_r, mut out_l, mut out_r) = (vec![], vec![], vec![], vec![]);
    for frame_index in 0..FRAMES {
        let left = tone(441.0, 48_000.0, 0.4, frame_index);
        let right = tone(1_234.0, 48_000.0, 0.4, frame_index);
        let frame = encoder.encode_frame(&[&left, &right]).unwrap();
        let decoded = decoder.decode_frame(&frame).unwrap();
        in_l.extend_from_slice(&left);
        in_r.extend_from_slice(&right);
        out_l.extend_from_slice(decoded.channel_samples(0));
        out_r.extend_from_slice(decoded.channel_samples(1));
    }
    assert_eq!(out_l.len(), FRAMES * SAMPLES_PER_FRAME);

    let start = SAMPLES_PER_FRAME;
    let window = 2 * SAMPLES_PER_FRAME;
    let lag = best_lag(&in_l, &out_l, start, window, 1024);
    // The decoded signal trails the input by exactly the transform overlap.
    assert_eq!(lag as i32, encoder.latency().transform_samples);
    let snr_left = snr_db(&in_l, &out_l, start, window, lag);
    let snr_right = snr_db(&in_r, &out_r, start, window, lag);
    assert!(snr_left > 20.0, "left SNR {snr_left:.1} dB");
    assert!(snr_right > 20.0, "right SNR {snr_right:.1} dB");
    // Swapped channels would score far below zero.
    let crossed = snr_db(&in_l, &out_r, start, window, lag);
    assert!(crossed < 3.0, "crossed SNR {crossed:.1} dB");
}

#[test]
fn block_switching_flags_a_transient_but_not_a_steady_tone() {
    let mut encoder = Encoder::new(&EncoderConfig {
        acmod: Acmod::Mono,
        bitrate_kbps: 128,
        ..Default::default()
    })
    .unwrap();
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();

    let steady = tone(500.0, 48_000.0, 0.3, 0);
    let frame = encoder.encode_frame(&[&steady]).unwrap();
    let decoded = decoder.decode_frame(&frame).unwrap();
    assert!((0..6).all(|block| !decoded.block_switched(0, block)));

    // Silence, then a full-scale click in the middle of the frame.
    let mut click = vec![0.0f32; SAMPLES_PER_FRAME];
    for sample in &mut click[800..840] {
        *sample = 0.95;
    }
    let mut any_switched = false;
    for _ in 0..2 {
        let frame = encoder.encode_frame(&[&click]).unwrap();
        let decoded = decoder.decode_frame(&frame).unwrap();
        any_switched |= (0..6).any(|block| decoded.block_switched(0, block));
    }
    assert!(
        any_switched,
        "a click after silence must engage block switching"
    );
}

#[test]
#[should_panic(expected = "channel index out of range")]
fn channel_samples_out_of_range_panics() {
    let mut encoder = Encoder::new(&EncoderConfig::default()).unwrap();
    let s = vec![0.0f32; SAMPLES_PER_FRAME];
    let frame = encoder.encode_frame(&[&s, &s]).unwrap();
    let decoded = Decoder::new(&DecoderConfig::default())
        .unwrap()
        .decode_frame(&frame)
        .unwrap();
    let _ = decoded.channel_samples(2);
}

#[test]
fn handles_are_send_and_outlive_their_creators() {
    fn assert_send<T: Send>() {}
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send::<Encoder>();
    assert_send::<Decoder>();
    assert_send::<ac3forge::ac3::DecodedFrame>();
    assert_send_sync::<ac3forge::Bytes>();

    // Frames and decoded frames own their storage: dropping the encoder and decoder first must
    // leave both fully readable (Drop order independence), including across a thread hop.
    let (frame, decoded) = {
        let mut encoder = Encoder::new(&EncoderConfig::default()).unwrap();
        let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
        let s = tone(700.0, 48_000.0, 0.3, 0);
        let frame = encoder.encode_frame(&[&s, &s]).unwrap();
        let decoded = decoder.decode_frame(&frame).unwrap();
        (frame, decoded)
    };
    let handle = std::thread::spawn(move || {
        assert_eq!(frame.len(), 768);
        assert_eq!(&frame[..2], &[0x0B, 0x77]);
        assert!(format!("{frame:?}").contains("len: 768"));
        decoded.channel_samples(1).len()
    });
    assert_eq!(handle.join().unwrap(), SAMPLES_PER_FRAME);

    // Many short-lived handles: create/drop churn must neither leak nor double-free.
    for _ in 0..50 {
        drop(Encoder::new(&EncoderConfig::default()).unwrap());
        drop(Decoder::new(&DecoderConfig::default()).unwrap());
    }
}
