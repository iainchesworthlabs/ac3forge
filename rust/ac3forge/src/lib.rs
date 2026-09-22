//! Safe, idiomatic Rust bindings for [`ac3forge_c`](https://docs.rs/ac3forge-sys), ac3forge's C
//! API — AC-3 and E-AC-3 encode and decode. See `rust/README.md` for build prerequisites, what
//! this crate covers versus what's explicitly deferred, and the real header defects found while
//! building it.
//!
//! ```no_run
//! use ac3forge::ac3::{Encoder, EncoderConfig};
//! use ac3forge::types::{Acmod, SampleRate};
//!
//! let config = EncoderConfig {
//!     sample_rate: SampleRate::Hz48000,
//!     bitrate_kbps: 192,
//!     acmod: Acmod::Stereo,
//!     ..Default::default()
//! };
//! let mut encoder = Encoder::new(&config).unwrap();
//! let silence = vec![0.0f32; 1536];
//! let channels: Vec<&[f32]> = vec![&silence, &silence];
//! let frame = encoder.encode_frame(&channels).unwrap();
//! assert!(!frame.is_empty());
//! ```

pub mod ac3;
pub mod atmos;
mod bytes;
pub mod eac3;
mod error;
pub mod meter;
pub mod stream;
pub mod types;
mod version;

pub use bytes::Bytes;
pub use error::Error;
pub use version::{version, Version};

/// One audio block is always 256 samples (A/52 §4.1); one syncframe is always six blocks —
/// `AC3FORGE_SAMPLES_PER_FRAME`. Both codecs' single-substream encode/decode paths in this crate
/// use this fixed frame size (`numblkscod`/short syncframes aren't exposed by the C API's
/// `_frame_config_t` structs today — see `rust/README.md`).
pub const SAMPLES_PER_FRAME: usize = ac3forge_sys::AC3FORGE_SAMPLES_PER_FRAME as usize;
