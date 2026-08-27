//! Encodes five frames of a stereo tone to E-AC-3 and decodes them back — the same walkthrough
//! as `examples/capi_encode_eac3.c`'s single-substream case, from Rust. Run with
//! `cargo run --example encode_decode_eac3`.

use ac3forge::eac3::{Eac3Decoder, Eac3Encoder, Eac3FrameConfig};
use ac3forge::types::{Acmod, DecoderConfig, SampleRate};
use ac3forge::SAMPLES_PER_FRAME;

fn tone(frequency_hz: f32, frame_index: usize) -> Vec<f32> {
    (0..SAMPLES_PER_FRAME)
        .map(|i| {
            let t = (frame_index * SAMPLES_PER_FRAME + i) as f32 / 48_000.0;
            0.4 * (2.0 * std::f32::consts::PI * frequency_hz * t).sin()
        })
        .collect()
}

fn main() {
    let config = Eac3FrameConfig {
        sample_rate: SampleRate::Hz48000,
        bitrate_kbps: 192,
        acmod: Acmod::Stereo,
        auto_tools: true,
        ..Default::default()
    };
    let mut encoder = Eac3Encoder::new(&config).expect("failed to create E-AC-3 encoder");
    println!(
        "E-AC-3 encoder: {} channels, {} samples/frame, {} sample latency",
        encoder.channel_count(),
        encoder.samples_per_frame(),
        encoder.latency().total_samples()
    );

    let mut decoder =
        Eac3Decoder::new(&DecoderConfig::default()).expect("failed to create E-AC-3 decoder");

    for frame_index in 0..5 {
        let left = tone(440.0, frame_index);
        let right = tone(880.0, frame_index);
        let encoded = encoder
            .encode_frame(&[&left, &right], None, None)
            .expect("encode_frame failed");

        let decoded = decoder
            .decode_substream(&encoded)
            .expect("decode_substream failed")
            .expect("auto_tools without transient pre-noise never holds a frame back");
        println!(
            "frame {frame_index}: {} bytes -> {} channels @ {} Hz, dialnorm {}",
            encoded.len(),
            decoded.channel_count(),
            decoded.sample_rate().map(SampleRate::hz).unwrap_or(0),
            decoded.dialnorm()
        );
    }
}
