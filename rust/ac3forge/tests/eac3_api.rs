//! The E-AC-3 surface beyond roundtrip.rs/completeness.rs: config defaults, the Annex E reduced
//! sample rates, the coding-tool switches, explicit §7.7 metadata and aux data, substream
//! identity, argument validation, the §3.7 hold-back signal, and the access-unit pair's
//! single-substream and error paths.

mod common;

use ac3forge::eac3::{
    AccessUnitEncoder, DecodedSubstream, Eac3Decoder, Eac3Encoder, Eac3FrameConfig,
    Eac3FrameMetadata, StreamType,
};
use ac3forge::types::{Acmod, DecoderConfig, SampleRate};
use ac3forge::{Error, SAMPLES_PER_FRAME};
use common::{best_lag, rms, snr_db, tone};

fn stereo(bitrate_kbps: u32) -> Eac3FrameConfig {
    Eac3FrameConfig {
        acmod: Acmod::Stereo,
        bitrate_kbps,
        ..Default::default()
    }
}

/// Encodes `frames` frames of a two-tone stereo signal and decodes each; returns the decoded
/// substreams (all must decode immediately - none of these configs engage the hold-back).
fn round_trip(config: &Eac3FrameConfig, rate_hz: f32, frames: usize) -> Vec<DecodedSubstream> {
    let mut encoder = Eac3Encoder::new(config).unwrap();
    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    (0..frames)
        .map(|frame_index| {
            let left = tone(400.0, rate_hz, 0.3, frame_index);
            let right = tone(900.0, rate_hz, 0.3, frame_index);
            let frame = encoder.encode_frame(&[&left, &right], None, None).unwrap();
            decoder
                .decode_substream(&frame)
                .unwrap()
                .expect("no hold-back expected")
        })
        .collect()
}

#[test]
fn frame_config_default_mirrors_the_c_initializer() {
    let config = Eac3FrameConfig::default();
    assert_eq!(config.sample_rate, SampleRate::Hz48000);
    assert_eq!(config.bitrate_kbps, 192);
    assert_eq!(config.dialnorm, 31);
    assert_eq!(config.acmod, Acmod::Stereo);
    assert!(!config.lfe);
    assert!(!config.auto_tools);
    assert!(!config.transient_prenoise);
    assert_eq!(config.strmtyp, StreamType::Independent);
    assert_eq!(config.substreamid, 0);
    assert_eq!(config.chanmap, None);
    assert_eq!(config.gaqmod, None);
    assert_eq!(config.spxattencod, None);
    assert_eq!(StreamType::default(), StreamType::Independent);
    assert_eq!(config.clone(), config);

    let metadata = Eac3FrameMetadata::default();
    assert_eq!(metadata.dynrng, [0; 6]);
    assert_eq!(metadata.compr, None);
    assert_eq!(metadata.compr2, None);
}

#[test]
fn reduced_sample_rates_round_trip() {
    for (rate, hz, bitrate) in [
        (SampleRate::Hz44100, 44_100.0, 192),
        (SampleRate::Hz32000, 32_000.0, 192),
        (SampleRate::Hz24000, 24_000.0, 96),
        (SampleRate::Hz22050, 22_050.0, 96),
        (SampleRate::Hz16000, 16_000.0, 64),
    ] {
        let config = Eac3FrameConfig {
            sample_rate: rate,
            ..stereo(bitrate)
        };
        let decoded = round_trip(&config, hz, 3);
        let last = decoded.last().unwrap();
        assert_eq!(last.sample_rate(), Some(rate), "{rate:?}");
        assert_eq!(last.channel_count(), 2);
        assert_eq!(last.samples_per_channel(), SAMPLES_PER_FRAME);
        assert!(rms(last.channel_samples(0)) > 0.05, "{rate:?}");
        assert!(rms(last.channel_samples(1)) > 0.05, "{rate:?}");
    }
}

#[test]
fn coding_tools_each_round_trip_with_signal() {
    let configs = [
        Eac3FrameConfig {
            auto_tools: true,
            ..stereo(128)
        },
        Eac3FrameConfig {
            coupling: true,
            enhanced: true,
            cplbegf: Some(4),
            ..stereo(128)
        },
        Eac3FrameConfig {
            spx: true,
            spx_atten: true,
            spxattencod: Some(2),
            spxbegf: Some(2),
            ..stereo(96)
        },
        Eac3FrameConfig {
            aht: true,
            gaqmod: Some(1),
            ..stereo(96)
        },
        Eac3FrameConfig {
            fast_mdct: false,
            ..stereo(192)
        },
    ];
    for config in configs {
        let decoded = round_trip(&config, 48_000.0, 3);
        for substream in &decoded[1..] {
            assert!(substream.is_independent());
            assert_eq!(substream.acmod(), Some(Acmod::Stereo));
            assert!(rms(substream.channel_samples(0)) > 0.05, "{config:?}");
            assert!(rms(substream.channel_samples(1)) > 0.05, "{config:?}");
        }
    }
}

#[test]
fn five_one_with_lfe_round_trips_and_reports_dialnorm() {
    let config = Eac3FrameConfig {
        acmod: Acmod::Channels3_2,
        lfe: true,
        bitrate_kbps: 384,
        dialnorm: 24,
        ..Default::default()
    };
    let mut encoder = Eac3Encoder::new(&config).unwrap();
    assert_eq!(encoder.channel_count(), 6);
    let mut decoder = Eac3Decoder::new(&DecoderConfig {
        drc_scale: 0.5,
        heavy_compression: false,
    })
    .unwrap();
    for frame_index in 0..2 {
        let input: Vec<Vec<f32>> = (0..6)
            .map(|c| tone(200.0 + 150.0 * c as f32, 48_000.0, 0.2, frame_index))
            .collect();
        let spans: Vec<&[f32]> = input.iter().map(|c| c.as_slice()).collect();
        let frame = encoder.encode_frame(&spans, None, None).unwrap();
        let decoded = decoder.decode_substream(&frame).unwrap().unwrap();
        assert_eq!(decoded.channel_count(), 6);
        assert!(decoded.lfe());
        assert_eq!(decoded.dialnorm(), 24);
        assert_eq!(decoded.id(), 0);
        // Only the five full-bandwidth channels have block-switch entries; a steady tone never
        // switches.
        for channel in 0..5 {
            assert!((0..6).all(|block| !decoded.block_switched(channel, block)));
        }
    }
}

#[test]
fn explicit_metadata_and_aux_data_are_accepted() {
    let mut encoder = Eac3Encoder::new(&stereo(192)).unwrap();
    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    let metadata = Eac3FrameMetadata {
        dynrng: [0x10, 0x20, 0x30, 0x40, 0x50, 0x60],
        compr: Some(0x20),
        ..Default::default()
    };
    let aux = [0xA5u8; 64];
    for frame_index in 0..3 {
        let left = tone(500.0, 48_000.0, 0.3, frame_index);
        let right = tone(800.0, 48_000.0, 0.3, frame_index);
        let with_both = encoder
            .encode_frame(&[&left, &right], Some(&metadata), Some(&aux))
            .unwrap();
        // Fixed-rate coding: metadata and aux ride inside the same frame size.
        assert_eq!(with_both.len(), 768);
        let decoded = decoder.decode_substream(&with_both).unwrap().unwrap();
        assert_eq!(decoded.channel_count(), 2);
        let empty_aux = encoder
            .encode_frame(&[&left, &right], None, Some(&[]))
            .unwrap();
        assert_eq!(empty_aux.len(), 768);
    }
}

#[test]
fn substream_identity() {
    // A standalone Dependent substream is legal to emit and decodes as not-independent.
    let dependent = Eac3FrameConfig {
        strmtyp: StreamType::Dependent,
        chanmap: Some(ac3forge_sys::AC3FORGE_CHANMAP_512_HEIGHT as u16),
        ..stereo(192)
    };
    let decoded = round_trip(&dependent, 48_000.0, 1);
    assert!(!decoded[0].is_independent());

    // Convertible/Reserved cannot be emitted by the standalone encoder.
    for strmtyp in [StreamType::Convertible, StreamType::Reserved] {
        let mut encoder = Eac3Encoder::new(&Eac3FrameConfig {
            strmtyp,
            ..stereo(192)
        })
        .unwrap();
        let s = tone(440.0, 48_000.0, 0.3, 0);
        assert_eq!(
            encoder.encode_frame(&[&s, &s], None, None).err().unwrap(),
            Error::EncodeInvalidSubstream,
            "{strmtyp:?}"
        );
    }
}

#[test]
fn encoder_rejects_bad_rates_and_dialnorm() {
    let s = tone(440.0, 48_000.0, 0.3, 0);
    for bitrate in [1, 5_000] {
        let mut encoder = Eac3Encoder::new(&stereo(bitrate)).unwrap();
        assert_eq!(
            encoder.encode_frame(&[&s, &s], None, None).err().unwrap(),
            Error::EncodeInvalidBitrate
        );
    }
    let mut encoder = Eac3Encoder::new(&Eac3FrameConfig {
        dialnorm: 0,
        ..stereo(192)
    })
    .unwrap();
    assert_eq!(
        encoder.encode_frame(&[&s, &s], None, None).err().unwrap(),
        Error::EncodeInvalidDialnorm
    );
}

#[test]
fn encode_frame_validates_span_count_and_length() {
    let mut encoder = Eac3Encoder::new(&stereo(192)).unwrap();
    let good = vec![0.0f32; SAMPLES_PER_FRAME];
    let short = vec![0.0f32; 256];
    for spans in [
        vec![],
        vec![good.as_slice()],
        vec![good.as_slice(), good.as_slice(), good.as_slice()],
        vec![good.as_slice(), short.as_slice()],
    ] {
        assert_eq!(
            encoder.encode_frame(&spans, None, None).err().unwrap(),
            Error::InvalidArgument
        );
    }
    assert!(encoder.encode_frame(&[&good, &good], None, None).is_ok());
}

#[test]
fn latency_reports_holdback_only_with_transient_prenoise() {
    let plain = Eac3Encoder::new(&stereo(192)).unwrap().latency();
    assert_eq!(plain.frame_samples, SAMPLES_PER_FRAME as i32);
    assert_eq!(plain.holdback_samples, 0);
    let tpn = Eac3Encoder::new(&Eac3FrameConfig {
        transient_prenoise: true,
        ..stereo(192)
    })
    .unwrap()
    .latency();
    assert_eq!(tpn.holdback_samples, SAMPLES_PER_FRAME as i32);
    assert_eq!(
        tpn.total_samples() - plain.total_samples(),
        SAMPLES_PER_FRAME as i32
    );
}

/// The §3.7 hold-back: silence decodes immediately, the first transient frame comes back as
/// `Ok(None)` and switches the decoder's reported latency to one frame, and the stream keeps
/// flowing (one frame behind) afterwards.
///
/// `flush()` is deliberately NOT called after the hold-back engages: with frames pending it
/// double-frees (see the bug report accompanying this suite), so only its empty case is covered.
#[test]
fn transient_prenoise_holds_back_one_frame() {
    let config = Eac3FrameConfig {
        transient_prenoise: true,
        ..stereo(192)
    };
    let mut encoder = Eac3Encoder::new(&config).unwrap();
    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    let silence = vec![0.0f32; SAMPLES_PER_FRAME];
    let mut burst = vec![0.0f32; SAMPLES_PER_FRAME];
    for (i, sample) in burst.iter_mut().enumerate() {
        let polarity = if i % 2 == 0 { 1.0 } else { -1.0 };
        *sample = polarity * if (i / 128) % 2 == 0 { 0.0005 } else { 0.9 };
    }

    let mut outcomes = Vec::new();
    for input in [&silence, &silence, &burst, &burst, &burst] {
        let frame = encoder.encode_frame(&[input, input], None, None).unwrap();
        let decoded = decoder.decode_substream(&frame).unwrap();
        outcomes.push((decoded.is_some(), decoder.latency_samples()));
    }
    assert_eq!(
        outcomes,
        vec![
            (true, 0),
            (true, 0),
            (false, SAMPLES_PER_FRAME as i32),
            (true, SAMPLES_PER_FRAME as i32),
            (true, SAMPLES_PER_FRAME as i32),
        ]
    );
    // Dropping a decoder that still holds a frame back must be clean.
    drop(decoder);
}

#[test]
fn flush_without_holdback_is_empty() {
    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    assert!(decoder.flush().unwrap().is_empty());
    assert_eq!(decoder.latency_samples(), 0);
    let decoded = round_trip(&stereo(192), 48_000.0, 2);
    assert_eq!(decoded.len(), 2);
    assert!(decoder.flush().unwrap().is_empty());
}

#[test]
fn eac3_round_trip_snr_after_alignment() {
    const FRAMES: usize = 5;
    let mut encoder = Eac3Encoder::new(&stereo(256)).unwrap();
    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    let (mut input, mut output) = (Vec::new(), Vec::new());
    for frame_index in 0..FRAMES {
        let left = tone(523.0, 48_000.0, 0.4, frame_index);
        let right = tone(2_000.0, 48_000.0, 0.1, frame_index);
        let frame = encoder.encode_frame(&[&left, &right], None, None).unwrap();
        let decoded = decoder.decode_substream(&frame).unwrap().unwrap();
        input.extend_from_slice(&left);
        output.extend_from_slice(decoded.channel_samples(0));
    }
    let start = SAMPLES_PER_FRAME;
    let window = 2 * SAMPLES_PER_FRAME;
    let lag = best_lag(&input, &output, start, window, 1024);
    assert_eq!(lag as i32, encoder.latency().transform_samples);
    let snr = snr_db(&input, &output, start, window, lag);
    assert!(snr > 20.0, "SNR {snr:.1} dB");
}

#[test]
fn access_unit_encoder_with_no_dependents_is_a_plain_substream() {
    let mut encoder = AccessUnitEncoder::new(&stereo(192), &[]).unwrap();
    assert_eq!(encoder.channel_count(), 2);
    let latency = encoder.latency();
    assert_eq!(latency.frame_samples, SAMPLES_PER_FRAME as i32);
    assert_eq!(encoder.latency_samples(), latency.total_samples());

    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    for frame_index in 0..3 {
        let left = tone(600.0, 48_000.0, 0.3, frame_index);
        let right = tone(1_100.0, 48_000.0, 0.3, frame_index);
        let unit = encoder.encode(&[&left, &right], Some(&[1, 2, 3])).unwrap();
        assert_eq!(unit.substream_count(), 1);
        assert_eq!(unit.substream_bytes(0) as usize, unit.len());
        assert_eq!(unit.as_slice(), &unit[..]);
        assert_eq!(&unit[..2], &[0x0B, 0x77]);
        let decoded = decoder.decode_access_unit(&unit).unwrap().unwrap();
        assert_eq!(decoded.channel_count(), 2);
        assert_eq!(decoded.samples_per_channel(), SAMPLES_PER_FRAME);
        assert!(!decoded.has_object_metadata());
        assert_eq!(decoded.dynamic_object_count(), 0);
        assert_eq!(decoded.object_audio_count(), 0);
        if frame_index > 0 {
            assert!(rms(decoded.channel_samples(0)) > 0.05);
            assert!(rms(decoded.channel_samples(1)) > 0.05);
        }
    }
}

#[test]
fn access_unit_encoder_validates_spans() {
    let mut encoder = AccessUnitEncoder::new(&stereo(192), &[]).unwrap();
    let good = vec![0.0f32; SAMPLES_PER_FRAME];
    let short = vec![0.0f32; 100];
    assert_eq!(
        encoder.encode(&[&good], None).err().unwrap(),
        Error::InvalidArgument
    );
    assert_eq!(
        encoder.encode(&[&good, &short], None).err().unwrap(),
        Error::InvalidArgument
    );
    assert!(encoder.encode(&[&good, &good], None).is_ok());
}

#[test]
#[should_panic(expected = "substream index out of range")]
fn substream_bytes_out_of_range_panics() {
    let mut encoder = AccessUnitEncoder::new(&stereo(192), &[]).unwrap();
    let s = vec![0.0f32; SAMPLES_PER_FRAME];
    let unit = encoder.encode(&[&s, &s], None).unwrap();
    let _ = unit.substream_bytes(1);
}

#[test]
#[should_panic(expected = "channel index out of range")]
fn decoded_substream_channel_out_of_range_panics() {
    let decoded = round_trip(&stereo(192), 48_000.0, 1);
    let _ = decoded[0].channel_samples(2);
}

#[test]
#[should_panic(expected = "object index out of range")]
fn decoded_substream_object_out_of_range_panics() {
    let decoded = round_trip(&stereo(192), 48_000.0, 1);
    assert_eq!(decoded[0].dynamic_object_count(), 0);
    let _ = decoded[0].dynamic_object(0);
}

#[test]
#[should_panic(expected = "object index out of range")]
fn decoded_substream_object_audio_out_of_range_panics() {
    let decoded = round_trip(&stereo(192), 48_000.0, 1);
    assert_eq!(decoded[0].object_audio_count(), 0);
    let _ = decoded[0].object_audio(0);
}

#[test]
fn eac3_decoder_also_decodes_plain_ac3() {
    let mut encoder = ac3forge::ac3::Encoder::new(&Default::default()).unwrap();
    let s = tone(440.0, 48_000.0, 0.3, 0);
    let frame = encoder.encode_frame(&[&s, &s]).unwrap();
    let mut decoder = Eac3Decoder::new(&DecoderConfig::default()).unwrap();
    let substream = decoder.decode_substream(&frame).unwrap().unwrap();
    assert!(substream.is_independent());
    assert_eq!(substream.channel_count(), 2);
    let unit = decoder.decode_access_unit(&frame).unwrap().unwrap();
    assert_eq!(unit.channel_count(), 2);
}

#[test]
fn handles_are_send() {
    fn assert_send<T: Send>() {}
    assert_send::<Eac3Encoder>();
    assert_send::<Eac3Decoder>();
    assert_send::<DecodedSubstream>();
    assert_send::<AccessUnitEncoder>();
    assert_send::<ac3forge::eac3::AccessUnit>();
    assert_send::<ac3forge::eac3::DecodedAccessUnit>();

    // An access unit outlives its encoder and crosses threads intact.
    let unit = {
        let mut encoder = AccessUnitEncoder::new(&stereo(192), &[]).unwrap();
        let s = tone(440.0, 48_000.0, 0.3, 0);
        encoder.encode(&[&s, &s], None).unwrap()
    };
    let len = std::thread::spawn(move || {
        assert_eq!(unit.substream_count(), 1);
        unit.len()
    })
    .join()
    .unwrap();
    assert_eq!(len, 768);
}
