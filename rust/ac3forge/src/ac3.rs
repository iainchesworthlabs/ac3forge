//! AC-3 encode and decode — `ac3::FrameEncoder`/`ac3::FrameDecoder` via
//! `ac3forge_encoder_t`/`ac3forge_decoder_t`.

use ac3forge_sys as sys;
use std::ptr;

use crate::bytes::Bytes;
use crate::error::Error;
use crate::types::{
    Acmod, CentreMixLevel, DecoderConfig, DrcProfile, HeavyConfig, Latency, SampleRate,
    SurroundMixLevel,
};

/// Mirrors `ac3forge_encoder_config_t`. Construct with [`EncoderConfig::default`] (which calls
/// the raw `ac3forge_encoder_config_init()` — see that function's own doc comment on why a
/// zero-initialized config is *not* equivalent, e.g. `dialnorm` 0 is invalid where the real
/// default is 31) and override only the fields you need, the same "`_init()` first, then
/// selective overrides" pattern the C API itself is documented to expect.
#[derive(Debug, Clone, PartialEq)]
pub struct EncoderConfig {
    pub sample_rate: SampleRate,
    pub bitrate_kbps: u32,
    /// 1..31, §5.4.2.8.
    pub dialnorm: i32,
    /// Dual mono (`acmod == Acmod::DualMono`) only.
    pub dialnorm2: Option<i32>,
    /// 0..60; `None` is auto-from-bitrate.
    pub chbwcod: Option<i32>,
    pub acmod: Acmod,
    pub lfe: bool,
    pub coupling: bool,
    /// `None` is auto.
    pub cplbegf: Option<i32>,
    /// `None` is auto.
    pub cplendf: Option<i32>,
    pub fast_mdct: bool,
    pub drc: Option<DrcProfile>,
    pub heavy: Option<HeavyConfig>,
    /// Ch2's own DRC/heavy — dual mono only, no fallback to `drc`/`heavy` above.
    pub drc2: Option<DrcProfile>,
    pub heavy2: Option<HeavyConfig>,
    pub cmixlev: CentreMixLevel,
    pub surmixlev: SurroundMixLevel,
}

impl EncoderConfig {
    pub(crate) fn to_raw(&self) -> sys::ac3forge_encoder_config_t {
        sys::ac3forge_encoder_config_t {
            sample_rate: self.sample_rate.to_raw(),
            bitrate_kbps: self.bitrate_kbps,
            dialnorm: self.dialnorm,
            has_dialnorm2: self.dialnorm2.is_some() as i32,
            dialnorm2: self.dialnorm2.unwrap_or_default(),
            chbwcod: self.chbwcod.unwrap_or(-1),
            acmod: self.acmod.to_raw(),
            lfe: self.lfe as i32,
            coupling: self.coupling as i32,
            cplbegf: self.cplbegf.unwrap_or(-1),
            cplendf: self.cplendf.unwrap_or(-1),
            fast_mdct: self.fast_mdct as i32,
            has_drc: self.drc.is_some() as i32,
            drc_profile: self.drc.unwrap_or(DrcProfile::FilmStandard).to_raw(),
            has_heavy: self.heavy.is_some() as i32,
            heavy: self.heavy.unwrap_or_default().to_raw(),
            has_drc2: self.drc2.is_some() as i32,
            drc2_profile: self.drc2.unwrap_or(DrcProfile::FilmStandard).to_raw(),
            has_heavy2: self.heavy2.is_some() as i32,
            heavy2: self.heavy2.unwrap_or_default().to_raw(),
            cmixlev: self.cmixlev.to_raw(),
            surmixlev: self.surmixlev.to_raw(),
        }
    }

    fn from_raw(raw: &sys::ac3forge_encoder_config_t) -> Self {
        EncoderConfig {
            sample_rate: SampleRate::from_raw(raw.sample_rate)
                .expect("unrecognized sample_rate in encoder default"),
            bitrate_kbps: raw.bitrate_kbps,
            dialnorm: raw.dialnorm,
            dialnorm2: (raw.has_dialnorm2 != 0).then_some(raw.dialnorm2),
            chbwcod: (raw.chbwcod >= 0).then_some(raw.chbwcod),
            acmod: Acmod::from_raw(raw.acmod).expect("unrecognized acmod in encoder default"),
            lfe: raw.lfe != 0,
            coupling: raw.coupling != 0,
            cplbegf: (raw.cplbegf >= 0).then_some(raw.cplbegf),
            cplendf: (raw.cplendf >= 0).then_some(raw.cplendf),
            fast_mdct: raw.fast_mdct != 0,
            drc: (raw.has_drc != 0).then(|| {
                DrcProfile::from_raw(raw.drc_profile)
                    .expect("unrecognized drc_profile in encoder default")
            }),
            heavy: (raw.has_heavy != 0).then(|| HeavyConfig::from_raw(raw.heavy)),
            drc2: (raw.has_drc2 != 0).then(|| {
                DrcProfile::from_raw(raw.drc2_profile)
                    .expect("unrecognized drc2_profile in encoder default")
            }),
            heavy2: (raw.has_heavy2 != 0).then(|| HeavyConfig::from_raw(raw.heavy2)),
            cmixlev: CentreMixLevel::from_raw(raw.cmixlev)
                .expect("unrecognized cmixlev in encoder default"),
            surmixlev: SurroundMixLevel::from_raw(raw.surmixlev)
                .expect("unrecognized surmixlev in encoder default"),
        }
    }
}

impl Default for EncoderConfig {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: ac3forge_encoder_config_init() unconditionally overwrites every field of
        // `raw` via a full struct assignment - never read before being written. This is the
        // one sanctioned way to obtain the real EncoderConfig{} defaults (dialnorm 31, etc.)
        // rather than guessing at them Rust-side, per ac3forge.h's own `_config_init` comment.
        unsafe { sys::ac3forge_encoder_config_init(&mut raw) };
        EncoderConfig::from_raw(&raw)
    }
}

/// An AC-3 encoder — `ac3::FrameEncoder` via `ac3forge_encoder_t`.
pub struct Encoder {
    raw: ptr::NonNull<sys::ac3forge_encoder_t>,
    channel_count: usize,
}

// SAFETY: the underlying handle has no thread affinity; `&mut self` on every mutating method
// already prevents concurrent use from safe code.
unsafe impl Send for Encoder {}

impl Encoder {
    pub fn new(config: &EncoderConfig) -> Result<Self, Error> {
        let raw_config = config.to_raw();
        let mut out: *mut sys::ac3forge_encoder_t = ptr::null_mut();
        // SAFETY: `raw_config` is a fully-initialized value (see `to_raw`); `out` is a valid
        // out-parameter.
        let status = unsafe { sys::ac3forge_encoder_create(&raw_config, &mut out) };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_encoder_create returned OK with a null encoder");
        // SAFETY: `raw` was just created above and is exclusively owned by this `Encoder`.
        let channel_count = unsafe { sys::ac3forge_encoder_channel_count(raw.as_ptr()) };
        Ok(Encoder { raw, channel_count })
    }

    /// Full-bandwidth channels (per `config.acmod`) plus, when `config.lfe` was set, the LFE
    /// channel — the channel count [`Encoder::encode_frame`] expects.
    pub fn channel_count(&self) -> usize {
        self.channel_count
    }

    /// This encoder's latency budget - constant for its whole life.
    pub fn latency(&self) -> Latency {
        let mut raw = sys::ac3forge_latency_t::default();
        // SAFETY: `self.raw` is valid; `raw` is a valid out-parameter.
        unsafe { sys::ac3forge_encoder_latency(self.raw.as_ptr(), &mut raw) };
        Latency::from_raw(raw)
    }

    /// Encodes one syncframe. `channels` must have exactly [`Encoder::channel_count`] entries,
    /// each exactly `AC3FORGE_SAMPLES_PER_FRAME` (1536) samples nominally in `[-1, 1)`, in AC-3
    /// channel order (Table 5.8) with LFE last.
    pub fn encode_frame(&mut self, channels: &[&[f32]]) -> Result<Bytes, Error> {
        if channels.len() != self.channel_count {
            return Err(Error::InvalidArgument);
        }
        let samples_per_channel = sys::AC3FORGE_SAMPLES_PER_FRAME as usize;
        if channels.iter().any(|c| c.len() != samples_per_channel) {
            return Err(Error::InvalidArgument);
        }

        let pointers: Vec<*const f32> = channels.iter().map(|c| c.as_ptr()).collect();
        let mut out: *mut sys::ac3forge_bytes_t = ptr::null_mut();
        // SAFETY: `pointers` holds `channel_count` valid pointers, each to
        // `samples_per_channel` live f32s for the duration of this call; `out` is a valid
        // out-parameter.
        let status = unsafe {
            sys::ac3forge_encoder_encode_frame(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len(),
                samples_per_channel,
                &mut out,
            )
        };
        Error::check(status)?;
        // SAFETY: AC3FORGE_OK guarantees `out` was written to a valid, exclusively-owned handle.
        Ok(unsafe { Bytes::from_raw(out) })
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        // SAFETY: `self.raw` is owned exclusively by this `Encoder`.
        unsafe { sys::ac3forge_encoder_destroy(self.raw.as_ptr()) };
    }
}

/// An AC-3 decoder — `ac3::FrameDecoder` via `ac3forge_decoder_t`.
pub struct Decoder {
    raw: ptr::NonNull<sys::ac3forge_decoder_t>,
}

unsafe impl Send for Decoder {}

impl Decoder {
    pub fn new(config: &DecoderConfig) -> Result<Self, Error> {
        let raw_config = config.to_raw();
        let mut out: *mut sys::ac3forge_decoder_t = ptr::null_mut();
        let status = unsafe { sys::ac3forge_decoder_create(&raw_config, &mut out) };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_decoder_create returned OK with a null decoder");
        Ok(Decoder { raw })
    }

    /// The delay this decoder adds on top of the encoder's own budget - always 0 for AC-3 (see
    /// `ac3forge_decoder_latency_samples`'s own comment).
    pub fn latency_samples(&self) -> i32 {
        unsafe { sys::ac3forge_decoder_latency_samples(self.raw.as_ptr()) }
    }

    /// Decodes one syncframe. `frame` must be exactly one syncframe's bytes.
    pub fn decode_frame(&mut self, frame: &[u8]) -> Result<DecodedFrame, Error> {
        let mut out: *mut sys::ac3forge_decoded_frame_t = ptr::null_mut();
        // SAFETY: `frame` is a valid slice for the duration of this call; `out` is a valid
        // out-parameter.
        let status = unsafe {
            sys::ac3forge_decoder_decode_frame(
                self.raw.as_ptr(),
                frame.as_ptr(),
                frame.len(),
                &mut out,
            )
        };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_decoder_decode_frame returned OK with a null frame");
        Ok(DecodedFrame { raw })
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_decoder_destroy(self.raw.as_ptr()) };
    }
}

/// One decoded AC-3 syncframe — `ac3::DecodedFrame` via `ac3forge_decoded_frame_t`. Owns its
/// PCM; every accessor borrows from `&self`.
pub struct DecodedFrame {
    raw: ptr::NonNull<sys::ac3forge_decoded_frame_t>,
}

unsafe impl Send for DecodedFrame {}

impl DecodedFrame {
    pub fn sample_rate(&self) -> Option<SampleRate> {
        SampleRate::from_raw(unsafe { sys::ac3forge_decoded_frame_sample_rate(self.raw.as_ptr()) })
    }

    pub fn bitrate_kbps(&self) -> u32 {
        unsafe { sys::ac3forge_decoded_frame_bitrate_kbps(self.raw.as_ptr()) }
    }

    pub fn acmod(&self) -> Option<Acmod> {
        Acmod::from_raw(unsafe { sys::ac3forge_decoded_frame_acmod(self.raw.as_ptr()) })
    }

    pub fn lfe(&self) -> bool {
        unsafe { sys::ac3forge_decoded_frame_lfe(self.raw.as_ptr()) != 0 }
    }

    pub fn dialnorm(&self) -> i32 {
        unsafe { sys::ac3forge_decoded_frame_dialnorm(self.raw.as_ptr()) }
    }

    pub fn channel_count(&self) -> usize {
        unsafe { sys::ac3forge_decoded_frame_channel_count(self.raw.as_ptr()) }
    }

    pub fn samples_per_channel(&self) -> usize {
        unsafe { sys::ac3forge_decoded_frame_samples_per_channel(self.raw.as_ptr()) }
    }

    /// `channel_index` in `[0, channel_count())`, AC-3 coded order (Table 5.8), LFE last when
    /// [`DecodedFrame::lfe`] is set. Panics if out of range.
    pub fn channel_samples(&self, channel_index: usize) -> &[f32] {
        assert!(
            channel_index < self.channel_count(),
            "channel index out of range"
        );
        // SAFETY: ac3forge_decoded_frame_channel_samples()'s pointer is valid until `frame` is
        // destroyed (ac3forge.h's own doc comment); it's tied to `&self`'s lifetime here, which
        // never outlives `self.raw`. samples_per_channel() gives the real length.
        unsafe {
            let ptr = sys::ac3forge_decoded_frame_channel_samples(self.raw.as_ptr(), channel_index);
            std::slice::from_raw_parts(ptr, self.samples_per_channel())
        }
    }

    /// True where `block_index` used the short (block-switched) transform. `channel_index` only
    /// ranges over the full-bandwidth channels (no LFE entry).
    pub fn block_switched(&self, channel_index: usize, block_index: usize) -> bool {
        unsafe {
            sys::ac3forge_decoded_frame_block_switched(
                self.raw.as_ptr(),
                channel_index,
                block_index as i32,
            ) != 0
        }
    }
}

impl Drop for DecodedFrame {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_decoded_frame_destroy(self.raw.as_ptr()) };
    }
}
