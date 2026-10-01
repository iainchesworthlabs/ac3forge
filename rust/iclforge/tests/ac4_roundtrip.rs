//! AC-4 encode/decode round trips against real synthesized audio - the same "real signal, not
//! frame 0" discipline as roundtrip.rs's AC-3/E-AC-3 tests (CONTRIBUTING.md).
//!
//! Unlike AC-3/E-AC-3, AC-4's frame length is not a crate-wide constant (it varies by frame
//! rate - see iclforge::ac4::DecodedFrame::samples_per_channel()'s own doc comment), so this file picks
//! the default configuration's own length (frame_rate_index 13, 2048 samples) explicitly rather
//! than importing SAMPLES_PER_FRAME.

use iclforge::ac4::{Decoder, DecoderConfig, Encoder, EncoderConfig};

const FRAME_SAMPLES: usize = 2048; // frame_rate_index 13's frame, this crate's EncoderConfig default
const FRAME_COUNT: usize = 5;

fn tone_channel(frequency_hz: f32, sample_rate_hz: f32, frame_index: usize) -> Vec<f32> {
    (0..FRAME_SAMPLES)
        .map(|i| {
            let n = (frame_index * FRAME_SAMPLES + i) as f32;
            0.4 * (2.0 * std::f32::consts::PI * frequency_hz * n / sample_rate_hz).sin()
        })
        .collect()
}

fn rms(samples: &[f32]) -> f32 {
    (samples.iter().map(|s| s * s).sum::<f32>() / samples.len().max(1) as f32).sqrt()
}

#[test]
fn ac4_stereo_round_trip_carries_real_signal() {
    let config = EncoderConfig {
        channels: 2,
        bitrate_kbps: 96,
        ..Default::default()
    };
    let mut encoder = Encoder::new(&config).unwrap();
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();

    let mut decoded_channels: Vec<Vec<f32>> = Vec::new();
    for frame_index in 0..FRAME_COUNT {
        let left = tone_channel(1000.0, 48_000.0, frame_index);
        let right = tone_channel(800.0, 48_000.0, frame_index);
        let frames = encoder.encode(&[&left, &right]).unwrap();
        for frame in &frames {
            assert!(!frame.data().is_empty());
            if let Some(decoded) = decoder.decode(frame.data()).unwrap() {
                assert_eq!(decoded.sample_rate_hz(), 48_000);
                assert_eq!(decoded.channel_count(), 2);
                if decoded_channels.is_empty() {
                    decoded_channels = vec![Vec::new(); decoded.channel_count()];
                }
                for (ch, buf) in decoded_channels.iter_mut().enumerate() {
                    buf.extend_from_slice(decoded.channel_samples(ch));
                }
                assert!(
                    decoded.objects().is_empty(),
                    "channel-based content carries no objects"
                );
            }
        }
    }
    for frame in encoder.flush().unwrap() {
        if let Some(decoded) = decoder.decode(frame.data()).unwrap() {
            for (ch, buf) in decoded_channels.iter_mut().enumerate() {
                buf.extend_from_slice(decoded.channel_samples(ch));
            }
        }
    }

    assert_eq!(decoded_channels.len(), 2, "no frame ever produced output");
    // Past the codec's warm-up: a real decoded signal, not silence.
    let tail = FRAME_SAMPLES; // skip the first frame's worth
    for (ch, buf) in decoded_channels.iter().enumerate() {
        assert!(buf.len() > tail, "channel {ch}: too little output to check");
        let level = rms(&buf[tail..]);
        assert!(
            level > 0.02,
            "channel {ch}: reads as near-silent ({level}) - real tone lost?"
        );
    }
}

#[test]
fn ac4_51_round_trip_reports_the_right_speakers() {
    let config = EncoderConfig {
        channels: 6,
        bitrate_kbps: 256,
        ..Default::default()
    };
    let mut encoder = Encoder::new(&config).unwrap();
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();

    let tones = [1000.0, 900.0, 700.0, 60.0, 500.0, 600.0];
    let mut saw_a_frame = false;
    for frame_index in 0..FRAME_COUNT {
        let channels: Vec<Vec<f32>> = tones
            .iter()
            .map(|&hz| tone_channel(hz, 48_000.0, frame_index))
            .collect();
        let refs: Vec<&[f32]> = channels.iter().map(|c| c.as_slice()).collect();
        for frame in encoder.encode(&refs).unwrap() {
            if let Some(decoded) = decoder.decode(frame.data()).unwrap() {
                assert_eq!(decoded.channel_count(), 6);
                use iclforge::ac4::Speaker;
                assert_eq!(decoded.speaker(0), Speaker::Left);
                assert_eq!(decoded.speaker(1), Speaker::Right);
                assert_eq!(decoded.speaker(2), Speaker::Centre);
                assert_eq!(decoded.speaker(3), Speaker::Lfe);
                assert_eq!(decoded.speaker(4), Speaker::LeftSurround);
                assert_eq!(decoded.speaker(5), Speaker::RightSurround);
                saw_a_frame = true;
            }
        }
    }
    assert!(saw_a_frame, "no frame ever produced output");
}

#[test]
fn ac4_decoder_rejects_a_corrupted_frame() {
    let config = EncoderConfig {
        channels: 2,
        ..Default::default()
    };
    let mut encoder = Encoder::new(&config).unwrap();

    let left = tone_channel(1000.0, 48_000.0, 0);
    let right = tone_channel(800.0, 48_000.0, 0);
    let mut frames = encoder.encode(&[&left, &right]).unwrap();
    frames.extend(encoder.flush().unwrap());
    let frame = frames
        .into_iter()
        .find(|f| !f.data().is_empty())
        .expect("no frame produced");

    let mut corrupted = frame.data().to_vec();
    for byte in corrupted.iter_mut().take(4) {
        *byte ^= 0xFF;
    }
    let mut decoder = Decoder::new(&DecoderConfig::default()).unwrap();
    assert!(decoder.decode(&corrupted).is_err());
}

#[test]
fn ac4_encoder_toc_feeds_build_dac4_and_media_timing() {
    let config = EncoderConfig {
        channels: 2,
        ..Default::default()
    };
    let mut encoder = Encoder::new(&config).unwrap();
    let left = vec![0.0f32; FRAME_SAMPLES];
    let right = vec![0.0f32; FRAME_SAMPLES];
    let frames = encoder.encode(&[&left, &right]).unwrap();
    assert!(
        !frames.is_empty(),
        "one whole frame_rate_index-13 frame must complete in one call"
    );

    let toc = encoder.toc().unwrap();
    let dac4 = toc.build_dac4().unwrap();
    assert!(!dac4.is_empty());
    assert!(toc.dac4_refusal().is_empty());

    let timing = toc
        .media_timing()
        .expect("frame_rate_index 13 has a single time scale");
    assert!(timing.timescale > 0);
    assert!(timing.sample_delta > 0);
    assert_eq!(toc.samples_per_frame(), Some(2048));
}

#[test]
fn ac4_sync_frame_wraps_a_raw_frame() {
    let raw = [0x01u8, 0x02, 0x03, 0x04];
    let wrapped = iclforge::ac4::sync_frame(&raw, false).unwrap();
    assert!(wrapped.len() > raw.len());
    assert_eq!(wrapped[0], 0xAC);
    assert_eq!(wrapped[1], 0x40);

    let wrapped_crc = iclforge::ac4::sync_frame(&raw, true).unwrap();
    assert_eq!(wrapped_crc[1], 0x41);
    assert_eq!(wrapped_crc.len(), wrapped.len() + 2);
}

#[test]
fn ac4_encoder_create_refuses_an_invalid_channel_count() {
    let config = EncoderConfig {
        channels: 3, // not one of EncoderConfig::channels' accepted counts
        ..Default::default()
    };
    match Encoder::new(&config) {
        Err(err) => assert_eq!(err, iclforge::Error::Ac4EncodeInvalidConfig),
        Ok(_) => panic!("3 channels should have been refused"),
    }
}
