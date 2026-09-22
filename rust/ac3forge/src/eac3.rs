//! E-AC-3 encode and decode (single substream) — `ac3::eac3::FrameEncoder`/`ac3::Eac3Decoder`
//! via `ac3forge_eac3_encoder_t`/`ac3forge_eac3_decoder_t`.
//!
//! Deliberately not covered here (see `rust/README.md`'s "explicitly out of scope" section):
//! `ac3forge_eac3_access_unit_encoder_t` (wide layouts built from several substreams),
//! `ac3forge_atmos_encoder_t` (Atmos/JOC object encode) and the OAMD/JOC object-audio decode
//! accessors on [`DecodedSubstream`].

use ac3forge_sys as sys;
use std::ptr;

use crate::bytes::Bytes;
use crate::error::Error;
use crate::types::{Acmod, DecoderConfig, Latency, SampleRate};

/// Mirrors `ac3forge_stream_type_t` (Table E1.2, §E2.3.1.2). This crate's [`Eac3Encoder`] only
/// ever emits `Independent` in practice (a standalone substream); `Dependent`/`Convertible`/
/// `Reserved` are mirrored for completeness against the raw type, matching the C header's own
/// "accepted here for a faithful mirror" stance.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum StreamType {
    #[default]
    Independent,
    Dependent,
    Convertible,
    Reserved,
}

impl StreamType {
    fn to_raw(self) -> sys::ac3forge_stream_type_t {
        match self {
            StreamType::Independent => sys::ac3forge_stream_type_AC3FORGE_STREAM_TYPE_INDEPENDENT,
            StreamType::Dependent => sys::ac3forge_stream_type_AC3FORGE_STREAM_TYPE_DEPENDENT,
            StreamType::Convertible => sys::ac3forge_stream_type_AC3FORGE_STREAM_TYPE_CONVERTIBLE,
            StreamType::Reserved => sys::ac3forge_stream_type_AC3FORGE_STREAM_TYPE_RESERVED,
        }
    }

    fn from_raw(raw: sys::ac3forge_stream_type_t) -> Option<Self> {
        #[allow(non_upper_case_globals)]
        Some(match raw {
            sys::ac3forge_stream_type_AC3FORGE_STREAM_TYPE_INDEPENDENT => StreamType::Independent,
            sys::ac3forge_stream_type_AC3FORGE_STREAM_TYPE_DEPENDENT => StreamType::Dependent,
            sys::ac3forge_stream_type_AC3FORGE_STREAM_TYPE_CONVERTIBLE => StreamType::Convertible,
            sys::ac3forge_stream_type_AC3FORGE_STREAM_TYPE_RESERVED => StreamType::Reserved,
            _ => return None,
        })
    }
}

/// Mirrors `ac3forge_eac3_frame_config_t`'s core surface. Construct with
/// [`Eac3FrameConfig::default`] (calls the raw `ac3forge_eac3_frame_config_init()`) and override
/// only what you need — see [`crate::ac3::EncoderConfig`]'s identical convention.
///
/// Not mirrored (see `ac3forge.h`'s own "What is deliberately out of scope" list, and
/// `docs/library/c-api.md`'s "E-AC-3 encoding" section): the `mixmdate`/`infomdat` metadata
/// groups, `dialnorm2`/`drc`/`heavy`, `vbr`/`numblkscod`.
#[derive(Debug, Clone, PartialEq)]
pub struct Eac3FrameConfig {
    pub sample_rate: SampleRate,
    pub bitrate_kbps: u32,
    pub dialnorm: i32,
    pub acmod: Acmod,
    pub lfe: bool,
    /// Hands the whole Annex E tool set to the encoder, chosen from the per-channel rate and the
    /// frame's own content instead of the flags below — see `docs/library/encoding-eac3.md`'s
    /// "How auto chooses".
    pub auto_tools: bool,
    pub coupling: bool,
    /// `None` is auto.
    pub cplbegf: Option<i32>,
    /// §E3.5 enhanced coupling; only meaningful with `coupling`.
    pub enhanced: bool,
    /// §E3.6 spectral extension.
    pub spx: bool,
    /// `None` is auto.
    pub spxbegf: Option<i32>,
    pub spx_atten: bool,
    /// `None` is auto, `Some(0..=3)` otherwise.
    pub spxattencod: Option<i32>,
    /// §E3.4 adaptive hybrid transform.
    pub aht: bool,
    /// `None` is auto, `Some(0..=3)` otherwise.
    pub gaqmod: Option<i32>,
    /// §3.7; the only tool that adds decoder hold-back.
    pub transient_prenoise: bool,
    pub fast_mdct: bool,
    /// Substream identity (Table E1.2) — only meaningful when hand-assembling a multi-substream
    /// access unit out of several standalone encoders; see `ac3forge.h`'s own comment.
    pub strmtyp: StreamType,
    pub substreamid: i32,
    /// Dependent substreams only.
    pub chanmap: Option<u16>,
}

impl Eac3FrameConfig {
    pub(crate) fn to_raw(&self) -> sys::ac3forge_eac3_frame_config_t {
        sys::ac3forge_eac3_frame_config_t {
            sample_rate: self.sample_rate.to_raw(),
            bitrate_kbps: self.bitrate_kbps,
            dialnorm: self.dialnorm,
            acmod: self.acmod.to_raw(),
            lfe: self.lfe as i32,
            auto_tools: self.auto_tools as i32,
            coupling: self.coupling as i32,
            cplbegf: self.cplbegf.unwrap_or(-1),
            enhanced: self.enhanced as i32,
            spx: self.spx as i32,
            spxbegf: self.spxbegf.unwrap_or(-1),
            spx_atten: self.spx_atten as i32,
            spxattencod: self.spxattencod.unwrap_or(-1),
            aht: self.aht as i32,
            gaqmod: self.gaqmod.unwrap_or(-1),
            transient_prenoise: self.transient_prenoise as i32,
            fast_mdct: self.fast_mdct as i32,
            strmtyp: self.strmtyp.to_raw(),
            substreamid: self.substreamid,
            has_chanmap: self.chanmap.is_some() as i32,
            chanmap: self.chanmap.unwrap_or_default(),
        }
    }

    fn from_raw(raw: &sys::ac3forge_eac3_frame_config_t) -> Self {
        Eac3FrameConfig {
            sample_rate: SampleRate::from_raw(raw.sample_rate)
                .expect("unrecognized sample_rate in eac3 default"),
            bitrate_kbps: raw.bitrate_kbps,
            dialnorm: raw.dialnorm,
            acmod: Acmod::from_raw(raw.acmod).expect("unrecognized acmod in eac3 default"),
            lfe: raw.lfe != 0,
            auto_tools: raw.auto_tools != 0,
            coupling: raw.coupling != 0,
            cplbegf: (raw.cplbegf >= 0).then_some(raw.cplbegf),
            enhanced: raw.enhanced != 0,
            spx: raw.spx != 0,
            spxbegf: (raw.spxbegf >= 0).then_some(raw.spxbegf),
            spx_atten: raw.spx_atten != 0,
            spxattencod: (raw.spxattencod >= 0).then_some(raw.spxattencod),
            aht: raw.aht != 0,
            gaqmod: (raw.gaqmod >= 0).then_some(raw.gaqmod),
            transient_prenoise: raw.transient_prenoise != 0,
            fast_mdct: raw.fast_mdct != 0,
            strmtyp: StreamType::from_raw(raw.strmtyp)
                .expect("unrecognized strmtyp in eac3 default"),
            substreamid: raw.substreamid,
            chanmap: (raw.has_chanmap != 0).then_some(raw.chanmap),
        }
    }
}

impl Default for Eac3FrameConfig {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: see EncoderConfig::default()'s identical comment - full-assignment init,
        // never read before written.
        unsafe { sys::ac3forge_eac3_frame_config_init(&mut raw) };
        Eac3FrameConfig::from_raw(&raw)
    }
}

/// §7.7 metadata words for one frame — `ac3forge_eac3_frame_metadata_t`. All-zero `dynrng`
/// (§7.7.1's "no change"), no `compr` by default, same as `Default::default()` would already
/// give (a plain POD value type with no growth story, unlike the `_config_init` structs — see
/// `ac3forge_eac3_frame_metadata_init()`'s own comment on why it exists purely for symmetry).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Eac3FrameMetadata {
    pub dynrng: [u8; sys::AC3FORGE_BLOCKS_PER_FRAME as usize],
    pub compr: Option<u8>,
    /// Meaningful only when `acmod` is `Acmod::DualMono`.
    pub dynrng2: [u8; sys::AC3FORGE_BLOCKS_PER_FRAME as usize],
    pub compr2: Option<u8>,
}

impl Eac3FrameMetadata {
    fn to_raw(self) -> sys::ac3forge_eac3_frame_metadata_t {
        sys::ac3forge_eac3_frame_metadata_t {
            dynrng: self.dynrng,
            has_compr: self.compr.is_some() as i32,
            compr: self.compr.unwrap_or_default(),
            dynrng2: self.dynrng2,
            has_compr2: self.compr2.is_some() as i32,
            compr2: self.compr2.unwrap_or_default(),
        }
    }
}

/// An E-AC-3 encoder (single, standalone substream) — `ac3::eac3::FrameEncoder` via
/// `ac3forge_eac3_encoder_t`.
pub struct Eac3Encoder {
    raw: ptr::NonNull<sys::ac3forge_eac3_encoder_t>,
    channel_count: usize,
    samples_per_frame: usize,
}

unsafe impl Send for Eac3Encoder {}

impl Eac3Encoder {
    pub fn new(config: &Eac3FrameConfig) -> Result<Self, Error> {
        let raw_config = config.to_raw();
        let mut out: *mut sys::ac3forge_eac3_encoder_t = ptr::null_mut();
        let status = unsafe { sys::ac3forge_eac3_encoder_create(&raw_config, &mut out) };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_eac3_encoder_create returned OK with a null encoder");
        let channel_count = unsafe { sys::ac3forge_eac3_encoder_channel_count(raw.as_ptr()) };
        let samples_per_frame =
            unsafe { sys::ac3forge_eac3_encoder_samples_per_frame(raw.as_ptr()) };
        Ok(Eac3Encoder {
            raw,
            channel_count,
            samples_per_frame,
        })
    }

    pub fn channel_count(&self) -> usize {
        self.channel_count
    }

    /// `AC3FORGE_SAMPLES_PER_FRAME` today — see `ac3forge_eac3_encoder_samples_per_frame()`'s own
    /// comment on why this is its own accessor rather than an assumed constant.
    pub fn samples_per_frame(&self) -> usize {
        self.samples_per_frame
    }

    pub fn latency(&self) -> Latency {
        let mut raw = sys::ac3forge_latency_t::default();
        unsafe { sys::ac3forge_eac3_encoder_latency(self.raw.as_ptr(), &mut raw) };
        Latency::from_raw(raw)
    }

    /// Encodes one syncframe. `channels` must have exactly [`Eac3Encoder::channel_count`]
    /// entries, each exactly [`Eac3Encoder::samples_per_frame`] samples, in AC-3 channel order
    /// with LFE last. `metadata`, when `Some`, supplies the §7.7 words explicitly instead of
    /// measuring them from `channels`. `aux`, when `Some`, carries a caller-built EMDF container
    /// in the frame's aux data.
    pub fn encode_frame(
        &mut self,
        channels: &[&[f32]],
        metadata: Option<&Eac3FrameMetadata>,
        aux: Option<&[u8]>,
    ) -> Result<Bytes, Error> {
        if channels.len() != self.channel_count
            || channels.iter().any(|c| c.len() != self.samples_per_frame)
        {
            return Err(Error::InvalidArgument);
        }

        let pointers: Vec<*const f32> = channels.iter().map(|c| c.as_ptr()).collect();
        let raw_metadata = metadata.map(|m| m.to_raw());
        let metadata_ptr = raw_metadata.as_ref().map_or(ptr::null(), |m| {
            m as *const sys::ac3forge_eac3_frame_metadata_t
        });
        let (aux_ptr, aux_len) = aux.map_or((ptr::null(), 0), |a| (a.as_ptr(), a.len()));

        let mut out: *mut sys::ac3forge_bytes_t = ptr::null_mut();
        // SAFETY: `pointers`/`metadata_ptr`/`aux_ptr` are all valid for the duration of this
        // call; `out` is a valid out-parameter.
        let status = unsafe {
            sys::ac3forge_eac3_encoder_encode_frame(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len(),
                self.samples_per_frame,
                metadata_ptr,
                aux_ptr,
                aux_len,
                &mut out,
            )
        };
        Error::check(status)?;
        Ok(unsafe { Bytes::from_raw(out) })
    }
}

impl Drop for Eac3Encoder {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_eac3_encoder_destroy(self.raw.as_ptr()) };
    }
}

/// An E-AC-3/Atmos decoder — `ac3::Eac3Decoder` via `ac3forge_eac3_decoder_t`.
pub struct Eac3Decoder {
    raw: ptr::NonNull<sys::ac3forge_eac3_decoder_t>,
}

unsafe impl Send for Eac3Decoder {}

impl Eac3Decoder {
    pub fn new(config: &DecoderConfig) -> Result<Self, Error> {
        let raw_config = config.to_raw();
        let mut out: *mut sys::ac3forge_eac3_decoder_t = ptr::null_mut();
        let status = unsafe { sys::ac3forge_eac3_decoder_create(&raw_config, &mut out) };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_eac3_decoder_create returned OK with a null decoder");
        Ok(Eac3Decoder { raw })
    }

    /// The delay this decoder adds, same contract as [`crate::ac3::Decoder::latency_samples`] —
    /// 0 until some substream's frame engages transient pre-noise processing (§3.7), then
    /// `AC3FORGE_SAMPLES_PER_FRAME` for the rest of the stream.
    pub fn latency_samples(&self) -> i32 {
        unsafe { sys::ac3forge_eac3_decoder_latency_samples(self.raw.as_ptr()) }
    }

    /// Decodes one substream's syncframe. Returns `Ok(None)` — not an error — when this frame's
    /// PCM is being held back pending transient pre-noise processing (§3.7); call
    /// [`Eac3Decoder::flush`] at end of stream to collect it. This is the one place a Rust
    /// wrapper is a strictly better fit than the C convention it mirrors: the C API's
    /// `AC3FORGE_OK` + null-out-parameter pairing collapses onto `Result<Option<_>, _>` exactly.
    pub fn decode_substream(&mut self, frame: &[u8]) -> Result<Option<DecodedSubstream>, Error> {
        let mut out: *mut sys::ac3forge_decoded_substream_t = ptr::null_mut();
        let status = unsafe {
            sys::ac3forge_eac3_decoder_decode_substream(
                self.raw.as_ptr(),
                frame.as_ptr(),
                frame.len(),
                &mut out,
            )
        };
        Error::check(status)?;
        Ok(ptr::NonNull::new(out).map(|raw| DecodedSubstream { raw }))
    }

    /// Releases whichever frames transient pre-noise processing is still holding back, in order.
    pub fn flush(&mut self) -> Result<Vec<DecodedSubstream>, Error> {
        let mut out_substreams: *mut *mut sys::ac3forge_decoded_substream_t = ptr::null_mut();
        let mut out_count: usize = 0;
        let status = unsafe {
            sys::ac3forge_eac3_decoder_flush(self.raw.as_ptr(), &mut out_substreams, &mut out_count)
        };
        Error::check(status)?;
        if out_count == 0 || out_substreams.is_null() {
            return Ok(Vec::new());
        }
        // SAFETY: AC3FORGE_OK with a non-null array and out_count > 0 guarantees `out_count`
        // valid, individually-owned handles at `out_substreams[0..out_count]` (ac3forge.h's own
        // comment on ac3forge_eac3_decoder_flush).
        let handles = unsafe { std::slice::from_raw_parts(out_substreams, out_count) };
        let result = handles
            .iter()
            .map(|&h| DecodedSubstream {
                raw: ptr::NonNull::new(h)
                    .expect("ac3forge_eac3_decoder_flush returned a null substream handle"),
            })
            .collect();
        // The array itself (not the substreams it points to, which `result`'s Drop impls now
        // own) is destroyed here, per ac3forge_decoded_substream_array_destroy()'s own contract.
        unsafe { sys::ac3forge_decoded_substream_array_destroy(out_substreams, out_count) };
        Ok(result)
    }
}

impl Drop for Eac3Decoder {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_eac3_decoder_destroy(self.raw.as_ptr()) };
    }
}

/// One decoded E-AC-3 substream — `ac3::DecodedSubstream` via `ac3forge_decoded_substream_t`.
///
/// OAMD/JOC object-audio accessors are not wrapped here yet — see this module's own header
/// comment.
pub struct DecodedSubstream {
    raw: ptr::NonNull<sys::ac3forge_decoded_substream_t>,
}

unsafe impl Send for DecodedSubstream {}

impl DecodedSubstream {
    pub fn is_independent(&self) -> bool {
        unsafe { sys::ac3forge_decoded_substream_is_independent(self.raw.as_ptr()) != 0 }
    }

    pub fn id(&self) -> i32 {
        unsafe { sys::ac3forge_decoded_substream_id(self.raw.as_ptr()) }
    }

    pub fn sample_rate(&self) -> Option<SampleRate> {
        SampleRate::from_raw(unsafe {
            sys::ac3forge_decoded_substream_sample_rate(self.raw.as_ptr())
        })
    }

    pub fn acmod(&self) -> Option<Acmod> {
        Acmod::from_raw(unsafe { sys::ac3forge_decoded_substream_acmod(self.raw.as_ptr()) })
    }

    pub fn lfe(&self) -> bool {
        unsafe { sys::ac3forge_decoded_substream_lfe(self.raw.as_ptr()) != 0 }
    }

    pub fn dialnorm(&self) -> i32 {
        unsafe { sys::ac3forge_decoded_substream_dialnorm(self.raw.as_ptr()) }
    }

    pub fn channel_count(&self) -> usize {
        unsafe { sys::ac3forge_decoded_substream_channel_count(self.raw.as_ptr()) }
    }

    pub fn samples_per_channel(&self) -> usize {
        unsafe { sys::ac3forge_decoded_substream_samples_per_channel(self.raw.as_ptr()) }
    }

    /// `channel_index` in `[0, channel_count())`. Panics if out of range.
    ///
    /// The header does not document this pointer's lifetime the way
    /// `ac3forge_decoded_frame_channel_samples()` does for the AC-3 side (see
    /// `rust/README.md`'s "header defects found" section, item 2) — this crate assumes the same
    /// "valid until the owner is destroyed" convention every other handle in the header follows,
    /// and ties the returned slice to `&self`'s lifetime regardless, so a wrong assumption here
    /// would show up as a borrow-checker error in this crate, never as a use-after-free in a
    /// caller's.
    pub fn channel_samples(&self, channel_index: usize) -> &[f32] {
        assert!(
            channel_index < self.channel_count(),
            "channel index out of range"
        );
        unsafe {
            let ptr =
                sys::ac3forge_decoded_substream_channel_samples(self.raw.as_ptr(), channel_index);
            std::slice::from_raw_parts(ptr, self.samples_per_channel())
        }
    }

    pub fn block_switched(&self, channel_index: usize, block_index: usize) -> bool {
        unsafe {
            sys::ac3forge_decoded_substream_block_switched(
                self.raw.as_ptr(),
                channel_index,
                block_index as i32,
            ) != 0
        }
    }
}

impl Drop for DecodedSubstream {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_decoded_substream_destroy(self.raw.as_ptr()) };
    }
}
