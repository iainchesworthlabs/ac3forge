//! Shared signal helpers for the integration tests. Each test crate uses its own subset, hence
//! the blanket `dead_code` allow.
#![allow(dead_code)]

use ac3forge::SAMPLES_PER_FRAME;

/// One frame of a sine tone, phase-continuous across `frame_index`.
pub fn tone(
    frequency_hz: f32,
    sample_rate_hz: f32,
    amplitude: f32,
    frame_index: usize,
) -> Vec<f32> {
    (0..SAMPLES_PER_FRAME)
        .map(|i| {
            let n = (frame_index * SAMPLES_PER_FRAME + i) as f32;
            amplitude * (2.0 * std::f32::consts::PI * frequency_hz * n / sample_rate_hz).sin()
        })
        .collect()
}

pub fn rms(samples: &[f32]) -> f32 {
    (samples.iter().map(|s| s * s).sum::<f32>() / samples.len().max(1) as f32).sqrt()
}

/// The lag (0..max_lag) at which `decoded` best matches `reference`, by normalized
/// cross-correlation over `window` samples starting at `start` in the reference.
pub fn best_lag(
    reference: &[f32],
    decoded: &[f32],
    start: usize,
    window: usize,
    max_lag: usize,
) -> usize {
    let mut best = (0usize, f64::MIN);
    for lag in 0..max_lag {
        if start + lag + window > decoded.len() || start + window > reference.len() {
            break;
        }
        let (mut xy, mut xx, mut yy) = (0f64, 0f64, 0f64);
        for i in 0..window {
            let x = reference[start + i] as f64;
            let y = decoded[start + lag + i] as f64;
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        let score = xy / (xx * yy).sqrt().max(1e-12);
        if score > best.1 {
            best = (lag, score);
        }
    }
    best.0
}

/// Signal-to-noise ratio in dB of `decoded[start+lag..]` against `reference[start..]`.
pub fn snr_db(reference: &[f32], decoded: &[f32], start: usize, window: usize, lag: usize) -> f64 {
    let (mut signal, mut noise) = (0f64, 0f64);
    for i in 0..window {
        let x = reference[start + i] as f64;
        let y = decoded[start + lag + i] as f64;
        signal += x * x;
        noise += (x - y) * (x - y);
    }
    10.0 * (signal / noise.max(1e-20)).log10()
}
