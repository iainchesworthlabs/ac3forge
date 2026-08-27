//! Encodes five frames of a stereo tone to AC-3 and decodes them back — the same walkthrough as
//! `examples/capi_encode_decode.c`, from Rust. Run with `cargo run --example encode_decode_ac3`.

use ac3forge::ac3::{Decoder, Encoder, EncoderConfig};
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
    let config = EncoderConfig {
        sample_rate: SampleRate::Hz48000,
        bitrate_kbps: 192,
        acmod: Acmod::Stereo,
        ..Default::default()
    };
    let mut encoder = Encoder::new(&config).expect("failed to create AC-3 encoder");
    println!(
        "AC-3 encoder: {} channels, {} sample latency",
        encoder.channel_count(),
        encoder.latency().total_samples()
    );

    let mut decoder =
        Decoder::new(&DecoderConfig::default()).expect("failed to create AC-3 decoder");

    for frame_index in 0..5 {
        let left = tone(440.0, frame_index);
        let right = tone(880.0, frame_index);
        let encoded = encoder
            .encode_frame(&[&left, &right])
            .expect("encode_frame failed");

        let decoded = decoder.decode_frame(&encoded).expect("decode_frame failed");
        println!(
            "frame {frame_index}: {} bytes -> {} channels @ {} Hz, dialnorm {}",
            encoded.len(),
            decoded.channel_count(),
            decoded.sample_rate().map(SampleRate::hz).unwrap_or(0),
            decoded.dialnorm()
        );
    }
}
