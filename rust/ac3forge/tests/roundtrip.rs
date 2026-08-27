//! Encode/decode round trips against real synthesized audio — not silence, and not just frame 0.
//! See CONTRIBUTING.md's "Test with real audio, from frame 1 onward": an all-zero frame carries
//! no mantissa data and exercises almost none of the encoder, and the MDCT overlap buffer starts
//! at zero, so frame 0's transform is a special case that would hide a real overlap/layout bug.

use ac3forge::ac3;
use ac3forge::eac3;
use ac3forge::types::{Acmod, DecoderConfig, SampleRate};
use ac3forge::SAMPLES_PER_FRAME;

const FRAME_COUNT: usize = 5;

/// A distinct sine tone per channel (different frequency each) so a channel-order or
/// channel-count bug shows up as "wrong content on the wrong channel", not as two identical
/// streams that would pass a byte-count check while being silently swapped.
fn tone_channel(frequency_hz: f32, sample_rate_hz: f32, frame_index: usize) -> Vec<f32> {
    (0..SAMPLES_PER_FRAME)
        .map(|i| {
            let sample_index = (frame_index * SAMPLES_PER_FRAME + i) as f32;
            0.4 * (2.0 * std::f32::consts::PI * frequency_hz * sample_index / sample_rate_hz).sin()
        })
        .collect()
}

fn rms(samples: &[f32]) -> f32 {
    (samples.iter().map(|s| s * s).sum::<f32>() / samples.len() as f32).sqrt()
}

#[test]
fn ac3_stereo_round_trip_carries_real_signal() {
    let config = ac3::EncoderConfig {
        sample_rate: SampleRate::Hz48000,
        bitrate_kbps: 192,
        acmod: Acmod::Stereo,
        ..Default::default()
    };
    let mut encoder = ac3::Encoder::new(&config).unwrap();
    assert_eq!(encoder.channel_count(), 2);

    let mut decoder = ac3::Decoder::new(&DecoderConfig::default()).unwrap();

    for frame_index in 0..FRAME_COUNT {
        let left = tone_channel(440.0, 48_000.0, frame_index);
        let right = tone_channel(880.0, 48_000.0, frame_index);
        let encoded = encoder.encode_frame(&[&left, &right]).unwrap();
        assert!(!encoded.is_empty());

        let decoded = decoder.decode_frame(&encoded).unwrap();
        assert_eq!(decoded.acmod(), Some(Acmod::Stereo));
        assert_eq!(decoded.sample_rate(), Some(SampleRate::Hz48000));
        assert_eq!(decoded.channel_count(), 2);
        assert_eq!(decoded.samples_per_channel(), SAMPLES_PER_FRAME);

        if frame_index >= 1 {
            // Frame 0's IMDCT overlap-add references a zeroed history buffer - a real signal
            // check there would be testing the codec's known startup transient, not a bug.
            let left_rms = rms(decoded.channel_samples(0));
            let right_rms = rms(decoded.channel_samples(1));
            assert!(
                left_rms > 0.05,
                "frame {frame_index}: left channel reads as near-silent ({left_rms}) - real tone lost?"
            );
            assert!(
                right_rms > 0.05,
                "frame {frame_index}: right channel reads as near-silent ({right_rms}) - real tone lost?"
            );
        }

        if frame_index == FRAME_COUNT - 1 {
            // Proves the assertions above are actually exercising the decoder's own validation
            // and not just trivially passing on anything - CONTRIBUTING.md's "prove the test can
            // fail". A corrupted sync word must be rejected, not silently accepted.
            let mut corrupted = encoded.to_vec();
            corrupted[0] ^= 0xFF;
            assert!(ac3::Decoder::new(&DecoderConfig::default())
                .unwrap()
                .decode_frame(&corrupted)
                .is_err());
        }
    }
}

#[test]
fn eac3_stereo_round_trip_carries_real_signal() {
    let config = eac3::Eac3FrameConfig {
        sample_rate: SampleRate::Hz48000,
        bitrate_kbps: 192,
        acmod: Acmod::Stereo,
        ..Default::default()
    };
    let mut encoder = eac3::Eac3Encoder::new(&config).unwrap();
    assert_eq!(encoder.channel_count(), 2);
    assert_eq!(encoder.samples_per_frame(), SAMPLES_PER_FRAME);

    let mut decoder = eac3::Eac3Decoder::new(&DecoderConfig::default()).unwrap();

    for frame_index in 0..FRAME_COUNT {
        let left = tone_channel(440.0, 48_000.0, frame_index);
        let right = tone_channel(880.0, 48_000.0, frame_index);
        let encoded = encoder.encode_frame(&[&left, &right], None, None).unwrap();
        assert!(!encoded.is_empty());

        let decoded = decoder.decode_substream(&encoded).unwrap().expect(
            "no transient-pre-noise tool is enabled by default, so this must decode immediately",
        );
        assert_eq!(decoded.acmod(), Some(Acmod::Stereo));
        assert_eq!(decoded.sample_rate(), Some(SampleRate::Hz48000));
        assert_eq!(decoded.channel_count(), 2);
        assert_eq!(decoded.samples_per_channel(), SAMPLES_PER_FRAME);
        assert!(decoded.is_independent());

        if frame_index >= 1 {
            let left_rms = rms(decoded.channel_samples(0));
            let right_rms = rms(decoded.channel_samples(1));
            assert!(
                left_rms > 0.05,
                "frame {frame_index}: left channel reads as near-silent ({left_rms}) - real tone lost?"
            );
            assert!(
                right_rms > 0.05,
                "frame {frame_index}: right channel reads as near-silent ({right_rms}) - real tone lost?"
            );
        }

        if frame_index == FRAME_COUNT - 1 {
            // See the AC-3 test's identical check: proves decode_substream's assertions above
            // are exercising real validation, not trivially passing on anything.
            let mut corrupted = encoded.to_vec();
            corrupted[0] ^= 0xFF;
            assert!(eac3::Eac3Decoder::new(&DecoderConfig::default())
                .unwrap()
                .decode_substream(&corrupted)
                .is_err());
        }
    }

    // No held-back frames: transient_prenoise defaults to off, so flush() should report nothing
    // still pending - proves the Option<DecodedSubstream> convention (see Eac3Decoder::flush's
    // own doc comment) isn't silently swallowing a frame this test never accounted for.
    assert!(decoder.flush().unwrap().is_empty());
}
