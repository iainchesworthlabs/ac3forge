//! Atmos/JOC object encode — `ac3::oba::AtmosEncoder` via `ac3forge_atmos_encoder_t`.
//!
//! Objects in, one ordinary-looking 5.1 E-AC-3 access unit out: the objects are panned into a
//! 5.1 bed a legacy decoder plays unchanged, and OAMD (where each object is) plus JOC (how to
//! pull it back out of the bed) ride alongside in an EMDF container. See
//! `docs/library/spatial-and-atmos.md` for the model; this module is the C API's mirror of it,
//! covered here because a binding that stops at channel beds misses the one capability that
//! distinguishes this codec surface.

use ac3forge_sys as sys;
use std::ptr;

use crate::bytes::Bytes;
use crate::error::Error;
use crate::types::{Latency, SampleRate};

/// Mirrors `ac3forge_atmos_config_t`. Construct with [`AtmosConfig::default`] (which calls the
/// raw `ac3forge_atmos_config_init()`) and override only what you need.
#[derive(Debug, Clone, PartialEq)]
pub struct AtmosConfig {
    pub sample_rate: SampleRate,
    /// The metadata competes with the mantissas for the same frame, so an object stream needs
    /// headroom a plain 5.1 stream does not — default 448.
    pub bitrate_kbps: u32,
    pub dialnorm: i32,
    /// Index into `ac3::oba::joc::kNumBands` (Table 50); default 4 (nine bands).
    pub num_bands_idx: i32,
    /// §6.3.3.7's finer quantizer: half the step, roughly one more bit per coefficient.
    pub fine_quant: bool,
    /// Off drops the whole OAMD/JOC container and the TS 103 420 §8.3.1 marker with it — the
    /// stream degrades to a plain 5.1 bed. See `AtmosConfig`'s own C++ comment on when that is
    /// the right call.
    pub emit_object_metadata: bool,
    pub fast_mdct: bool,
}

impl Default for AtmosConfig {
    fn default() -> Self {
        // SAFETY: config_init fills every field; the zeroed value is never read.
        let mut raw: sys::ac3forge_atmos_config_t = unsafe { std::mem::zeroed() };
        unsafe { sys::ac3forge_atmos_config_init(&mut raw) };
        AtmosConfig {
            sample_rate: SampleRate::from_raw(raw.sample_rate).unwrap_or(SampleRate::Hz48000),
            bitrate_kbps: raw.bitrate_kbps,
            dialnorm: raw.dialnorm,
            num_bands_idx: raw.num_bands_idx,
            fine_quant: raw.fine_quant != 0,
            emit_object_metadata: raw.emit_object_metadata != 0,
            fast_mdct: raw.fast_mdct != 0,
        }
    }
}

impl AtmosConfig {
    fn to_raw(&self) -> sys::ac3forge_atmos_config_t {
        sys::ac3forge_atmos_config_t {
            sample_rate: self.sample_rate.to_raw(),
            bitrate_kbps: self.bitrate_kbps,
            dialnorm: self.dialnorm,
            num_bands_idx: self.num_bands_idx,
            fine_quant: self.fine_quant as i32,
            emit_object_metadata: self.emit_object_metadata as i32,
            fast_mdct: self.fast_mdct as i32,
        }
    }
}

/// One object's placement for one frame — `ac3::oba::ObjectPlacement` via
/// `ac3forge_object_placement_t`. Position is §4.2.1's room-anchored system (`x`/`y` in
/// `[0, 1]`, `z` in `[-1, 1]`).
///
/// [`ObjectPlacement::default`] calls the raw `ac3forge_object_placement_init()` — room centre,
/// unity gain, no LFE send. That C initializer exists because this crate's first pass found a
/// zero-initialized placement silently encoding a MUTED object (`rust/README.md`, header
/// defects, item 1); deriving `Default` from zeroes here would reintroduce exactly that bug in
/// Rust, so it is implemented by calling the C fix instead.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ObjectPlacement {
    pub x: f64,
    pub y: f64,
    pub z: f64,
    /// Linear, default 1.0.
    pub gain: f64,
    /// Linear, default 0.0 — the only route an object reaches the LFE.
    pub lfe_send: f64,
}

impl Default for ObjectPlacement {
    fn default() -> Self {
        // SAFETY: placement_init fills every field; the zeroed value is never read.
        let mut raw: sys::ac3forge_object_placement_t = unsafe { std::mem::zeroed() };
        unsafe { sys::ac3forge_object_placement_init(&mut raw) };
        ObjectPlacement {
            x: raw.x,
            y: raw.y,
            z: raw.z,
            gain: raw.gain,
            lfe_send: raw.lfe_send,
        }
    }
}

impl ObjectPlacement {
    fn to_raw(self) -> sys::ac3forge_object_placement_t {
        sys::ac3forge_object_placement_t {
            x: self.x,
            y: self.y,
            z: self.z,
            gain: self.gain,
            lfe_send: self.lfe_send,
        }
    }
}

/// The object encoder itself. `object_count` mono essences in, one E-AC-3 access unit out per
/// frame.
pub struct AtmosEncoder {
    raw: ptr::NonNull<sys::ac3forge_atmos_encoder_t>,
    object_count: usize,
}

unsafe impl Send for AtmosEncoder {}

impl AtmosEncoder {
    pub fn new(config: &AtmosConfig, object_count: usize) -> Result<Self, Error> {
        let raw_config = config.to_raw();
        let mut out: *mut sys::ac3forge_atmos_encoder_t = ptr::null_mut();
        let count = i32::try_from(object_count).map_err(|_| Error::InvalidArgument)?;
        let status = unsafe { sys::ac3forge_atmos_encoder_create(&raw_config, count, &mut out) };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_atmos_encoder_create returned OK with a null encoder");
        Ok(AtmosEncoder { raw, object_count })
    }

    pub fn dynamic_object_count(&self) -> usize {
        let count = unsafe { sys::ac3forge_atmos_encoder_dynamic_object_count(self.raw.as_ptr()) };
        usize::try_from(count).unwrap_or(0)
    }

    /// The OBJECT path's budget — bed overlap plus the §7.1 QMF filterbank's own delay, since
    /// JOC's reconstruction runs there. With `emit_object_metadata` off this collapses to
    /// [`AtmosEncoder::bed_latency`].
    pub fn latency(&self) -> Latency {
        // SAFETY: zero is a valid ac3forge_latency_t and the C call overwrites it.
        let mut raw: sys::ac3forge_latency_t = unsafe { std::mem::zeroed() };
        unsafe { sys::ac3forge_atmos_encoder_latency(self.raw.as_ptr(), &mut raw) };
        Latency::from_raw(raw)
    }

    pub fn latency_samples(&self) -> i32 {
        unsafe { sys::ac3forge_atmos_encoder_latency_samples(self.raw.as_ptr()) }
    }

    /// The 5.1 BED's budget: what a legacy decoder that ignores the container hears.
    pub fn bed_latency(&self) -> Latency {
        // SAFETY: zero is a valid ac3forge_latency_t and the C call overwrites it.
        let mut raw: sys::ac3forge_latency_t = unsafe { std::mem::zeroed() };
        unsafe { sys::ac3forge_atmos_encoder_bed_latency(self.raw.as_ptr(), &mut raw) };
        Latency::from_raw(raw)
    }

    /// `objects`: one mono span per object (the count given to [`AtmosEncoder::new`]), each
    /// exactly [`crate::SAMPLES_PER_FRAME`] samples; `placements`: the same count, one per
    /// object in the same order. Returns one complete E-AC-3 access unit — a single independent
    /// substream carrying the 5.1 bed, with the EMDF object container in its aux data when the
    /// config asked for one.
    pub fn encode_frame(
        &mut self,
        objects: &[&[f32]],
        placements: &[ObjectPlacement],
    ) -> Result<Bytes, Error> {
        if objects.len() != self.object_count
            || placements.len() != self.object_count
            || objects.iter().any(|o| o.len() != crate::SAMPLES_PER_FRAME)
        {
            return Err(Error::InvalidArgument);
        }
        let pointers: Vec<*const f32> = objects.iter().map(|o| o.as_ptr()).collect();
        let raw_placements: Vec<sys::ac3forge_object_placement_t> =
            placements.iter().map(|p| p.to_raw()).collect();
        let mut out: *mut sys::ac3forge_bytes_t = ptr::null_mut();
        let status = unsafe {
            sys::ac3forge_atmos_encoder_encode_frame(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len(),
                crate::SAMPLES_PER_FRAME,
                raw_placements.as_ptr(),
                raw_placements.len(),
                &mut out,
            )
        };
        Error::check(status)?;
        Ok(unsafe { Bytes::from_raw(out) })
    }
}

impl Drop for AtmosEncoder {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_atmos_encoder_destroy(self.raw.as_ptr()) };
    }
}
