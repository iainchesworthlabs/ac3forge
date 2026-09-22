//! Loudness metering — `ac3::meta::LoudnessMeter` via `ac3forge_loudness_meter_t`: BS.1770
//! integrated/momentary/short-term loudness, loudness range and true peak, fed incrementally
//! over a whole programme.

use ac3forge_sys as sys;
use std::ptr;

use crate::error::Error;
use crate::types::{Acmod, SampleRate};

/// The meter itself. Every gated measurement reports `None` until enough audio has been pushed
/// for it to mean anything — the C header's has_/value pairing folded onto `Option` the same
/// way every other optional in this crate is.
pub struct LoudnessMeter {
    raw: ptr::NonNull<sys::ac3forge_loudness_meter_t>,
    channel_count: usize,
}

unsafe impl Send for LoudnessMeter {}

impl LoudnessMeter {
    /// BS.1770-4 Annex 1's basic algorithm, keyed on acmod/lfe. [`LoudnessMeter::push`] then
    /// expects spans in the coded order the acmod implies (Table 5.8), LFE last.
    pub fn new(sample_rate: SampleRate, acmod: Acmod, lfe: bool) -> Result<Self, Error> {
        let mut out: *mut sys::ac3forge_loudness_meter_t = ptr::null_mut();
        let status = unsafe {
            sys::ac3forge_loudness_meter_create(
                sample_rate.to_raw(),
                acmod.to_raw(),
                lfe as i32,
                &mut out,
            )
        };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_loudness_meter_create returned OK with a null meter");
        let channel_count =
            usize::try_from(unsafe { sys::ac3forge_loudness_meter_channel_count(raw.as_ptr()) })
                .unwrap_or(0);
        Ok(LoudnessMeter { raw, channel_count })
    }

    /// BS.1770-5 Annex 3's extended algorithm over a rendered Table E2.5 layout — the wide
    /// layouts (7.1, 5.1.2, 5.1.4, 7.1.4) an acmod cannot name. `chanmap` is the same Table
    /// E2.5 bitmask a dependent substream's config carries; [`LoudnessMeter::push`] then
    /// expects spans in that map's own bit order.
    pub fn for_chanmap(sample_rate: SampleRate, chanmap: u16) -> Result<Self, Error> {
        let mut out: *mut sys::ac3forge_loudness_meter_t = ptr::null_mut();
        let status = unsafe {
            sys::ac3forge_loudness_meter_create_for_chanmap(sample_rate.to_raw(), chanmap, &mut out)
        };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("ac3forge_loudness_meter_create_for_chanmap returned OK with a null meter");
        let channel_count =
            usize::try_from(unsafe { sys::ac3forge_loudness_meter_channel_count(raw.as_ptr()) })
                .unwrap_or(0);
        Ok(LoudnessMeter { raw, channel_count })
    }

    pub fn channel_count(&self) -> usize {
        self.channel_count
    }

    /// Feeds planar audio. Any span length works — a meter runs over a whole programme, not one
    /// frame at a time — but every span must be the same length and there must be exactly
    /// [`LoudnessMeter::channel_count`] of them.
    pub fn push(&mut self, channels: &[&[f32]]) -> Result<(), Error> {
        if channels.len() != self.channel_count {
            return Err(Error::InvalidArgument);
        }
        let samples = channels.first().map_or(0, |c| c.len());
        if channels.iter().any(|c| c.len() != samples) {
            return Err(Error::InvalidArgument);
        }
        let pointers: Vec<*const f32> = channels.iter().map(|c| c.as_ptr()).collect();
        let status = unsafe {
            sys::ac3forge_loudness_meter_push(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len(),
                samples,
            )
        };
        Error::check(status)
    }

    pub fn integrated_lkfs(&self) -> Option<f64> {
        let present =
            unsafe { sys::ac3forge_loudness_meter_has_integrated_lkfs(self.raw.as_ptr()) };
        (present != 0)
            .then(|| unsafe { sys::ac3forge_loudness_meter_integrated_lkfs(self.raw.as_ptr()) })
    }

    pub fn momentary_lkfs(&self) -> Option<f64> {
        let present = unsafe { sys::ac3forge_loudness_meter_has_momentary_lkfs(self.raw.as_ptr()) };
        (present != 0)
            .then(|| unsafe { sys::ac3forge_loudness_meter_momentary_lkfs(self.raw.as_ptr()) })
    }

    pub fn short_term_lkfs(&self) -> Option<f64> {
        let present =
            unsafe { sys::ac3forge_loudness_meter_has_short_term_lkfs(self.raw.as_ptr()) };
        (present != 0)
            .then(|| unsafe { sys::ac3forge_loudness_meter_short_term_lkfs(self.raw.as_ptr()) })
    }

    pub fn loudness_range(&self) -> Option<f64> {
        let present = unsafe { sys::ac3forge_loudness_meter_has_loudness_range(self.raw.as_ptr()) };
        (present != 0)
            .then(|| unsafe { sys::ac3forge_loudness_meter_loudness_range(self.raw.as_ptr()) })
    }

    pub fn true_peak_dbtp(&self) -> Option<f64> {
        let present = unsafe { sys::ac3forge_loudness_meter_has_true_peak_dbtp(self.raw.as_ptr()) };
        (present != 0)
            .then(|| unsafe { sys::ac3forge_loudness_meter_true_peak_dbtp(self.raw.as_ptr()) })
    }
}

impl Drop for LoudnessMeter {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_loudness_meter_destroy(self.raw.as_ptr()) };
    }
}

/// §5.4.2.8: how many dB dialogue sits below digital 100 percent, valid 1..=31 — the field an
/// encoder config wants once [`LoudnessMeter::integrated_lkfs`] has measured a programme.
pub fn dialnorm_from_lkfs(lkfs: f64) -> i32 {
    unsafe { sys::ac3forge_dialnorm_from_lkfs(lkfs) }
}
