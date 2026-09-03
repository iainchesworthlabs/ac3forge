//! Stream framing and scanning — `ac3::split_frames`/`split_access_units`/`stream_bsid` and
//! `ac3::io::scan` via their C mirrors.
//!
//! The split results and every access-unit span a scan reports are views into the CALLER's
//! stream buffer (the C header's own lifetime contract), which Rust expresses directly: the
//! returned slices borrow the input, so the compiler enforces what the C caller has to
//! remember.

use ac3forge_sys as sys;
use std::ptr;

use crate::error::Error;
use crate::types::{Acmod, SampleRate};

/// Collects one `ac3forge_spans_t` into borrowed slices and destroys the handle — the spans
/// point into `stream`, so only the index list is owned by the C side and nothing is copied.
fn collect_spans(stream: &[u8], spans: *mut sys::ac3forge_spans_t) -> Vec<&[u8]> {
    let count = unsafe { sys::ac3forge_spans_count(spans) };
    let mut out = Vec::with_capacity(count);
    for index in 0..count {
        // A span is an (offset, length) pair INTO the caller's stream, not a pointer - which
        // makes the borrow trivially safe to express: plain slice indexing, bounds-checked by
        // Rust itself.
        let span = unsafe { sys::ac3forge_spans_get(spans, index) };
        out.push(&stream[span.offset..span.offset + span.length]);
    }
    unsafe { sys::ac3forge_spans_destroy(spans) };
    out
}

/// Splits a raw elementary stream into syncframes by sync word and declared size. Handles both
/// AC-3 and E-AC-3.
pub fn split_frames(stream: &[u8]) -> Result<Vec<&[u8]>, Error> {
    let mut out: *mut sys::ac3forge_spans_t = ptr::null_mut();
    let status = unsafe { sys::ac3forge_split_frames(stream.as_ptr(), stream.len(), &mut out) };
    Error::check(status)?;
    Ok(collect_spans(stream, out))
}

/// Groups syncframes into access units — a new one begins at each independent substream.
pub fn split_access_units(stream: &[u8]) -> Result<Vec<&[u8]>, Error> {
    let mut out: *mut sys::ac3forge_spans_t = ptr::null_mut();
    let status =
        unsafe { sys::ac3forge_split_access_units(stream.as_ptr(), stream.len(), &mut out) };
    Error::check(status)?;
    Ok(collect_spans(stream, out))
}

/// bsid at bit 40, without committing to either generation. Fails only if `frame` is too short
/// to hold a header.
pub fn stream_bsid(frame: &[u8]) -> Result<i32, Error> {
    let mut bsid = 0;
    let status = unsafe { sys::ac3forge_stream_bsid(frame.as_ptr(), frame.len(), &mut bsid) };
    Error::check(status)?;
    Ok(bsid)
}

/// Mirrors `ac3forge_stream_kind_t` / `ac3::io::StreamKind`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum StreamKind {
    Ac3,
    Eac3,
    /// §E2.3.1.2 legacy-core delivery: an AC-3 bed extended by Annex E dependent substreams.
    Ac3CoreEac3Extension,
}

impl StreamKind {
    fn from_raw(raw: sys::ac3forge_stream_kind_t) -> Option<Self> {
        #[allow(non_upper_case_globals)]
        Some(match raw {
            sys::ac3forge_stream_kind_AC3FORGE_STREAM_KIND_AC3 => StreamKind::Ac3,
            sys::ac3forge_stream_kind_AC3FORGE_STREAM_KIND_EAC3 => StreamKind::Eac3,
            sys::ac3forge_stream_kind_AC3FORGE_STREAM_KIND_AC3_CORE_EAC3_EXTENSION => {
                StreamKind::Ac3CoreEac3Extension
            }
            _ => return None,
        })
    }
}

/// A whole-stream scan — sample rate, layout, access-unit boundaries, the descriptor-facing
/// service fields — without decoding any audio. `ac3::io::scan`/`ScannedStream` via
/// `ac3forge_scanned_stream_t`.
///
/// Borrows the stream it scanned: every [`ScannedStream::access_unit`] span points into that
/// buffer, so the handle carries the buffer's lifetime and the borrow checker enforces the C
/// header's "must stay valid and unmodified" clause.
pub struct ScannedStream<'a> {
    raw: ptr::NonNull<sys::ac3forge_scanned_stream_t>,
    buffer: &'a [u8],
}

unsafe impl Send for ScannedStream<'_> {}

/// Scans `stream` end to end. Fails on a stream no generation of the format explains.
pub fn scan(stream: &[u8]) -> Result<ScannedStream<'_>, Error> {
    let mut out: *mut sys::ac3forge_scanned_stream_t = ptr::null_mut();
    let status = unsafe { sys::ac3forge_scan(stream.as_ptr(), stream.len(), &mut out) };
    Error::check(status)?;
    let raw = ptr::NonNull::new(out).expect("ac3forge_scan returned OK with a null stream");
    Ok(ScannedStream {
        raw,
        buffer: stream,
    })
}

impl ScannedStream<'_> {
    pub fn kind(&self) -> Option<StreamKind> {
        StreamKind::from_raw(unsafe { sys::ac3forge_scanned_stream_kind(self.raw.as_ptr()) })
    }

    pub fn sample_rate(&self) -> Option<SampleRate> {
        SampleRate::from_raw(unsafe { sys::ac3forge_scanned_stream_sample_rate(self.raw.as_ptr()) })
    }

    pub fn acmod(&self) -> Option<Acmod> {
        Acmod::from_raw(unsafe { sys::ac3forge_scanned_stream_acmod(self.raw.as_ptr()) })
    }

    pub fn lfe(&self) -> bool {
        unsafe { sys::ac3forge_scanned_stream_lfe(self.raw.as_ptr()) != 0 }
    }

    /// The RENDERED channel count — the bed plus every dependent's additions, which for a wide
    /// layout is more than the acmod alone implies.
    pub fn channels(&self) -> i32 {
        unsafe { sys::ac3forge_scanned_stream_channels(self.raw.as_ptr()) }
    }

    pub fn access_unit_count(&self) -> usize {
        unsafe { sys::ac3forge_scanned_stream_access_unit_count(self.raw.as_ptr()) }
    }

    /// Access unit `index`'s bytes — a view into the scanned buffer. Panics if out of range.
    pub fn access_unit(&self, index: usize) -> &[u8] {
        assert!(index < self.access_unit_count(), "access unit out of range");
        let span = unsafe { sys::ac3forge_scanned_stream_access_unit(self.raw.as_ptr(), index) };
        // An (offset, length) pair into the borrowed buffer.
        &self.buffer[span.offset..span.offset + span.length]
    }

    /// Samples access unit `index` carries — `SAMPLES_PER_FRAME` ordinarily, less for a short
    /// E-AC-3 syncframe (§E2.3.1.4).
    pub fn access_unit_samples(&self, index: usize) -> u32 {
        assert!(index < self.access_unit_count(), "access unit out of range");
        unsafe { sys::ac3forge_scanned_stream_access_unit_samples(self.raw.as_ptr(), index) }
    }

    pub fn substreams_per_unit(&self) -> usize {
        unsafe { sys::ac3forge_scanned_stream_substreams_per_unit(self.raw.as_ptr()) }
    }

    pub fn bsid(&self) -> i32 {
        unsafe { sys::ac3forge_scanned_stream_bsid(self.raw.as_ptr()) }
    }

    /// TS 103 420 §8.3.2.2's Atmos object-complexity index, when the stream advertised an
    /// object layer.
    pub fn oba_complexity_index(&self) -> Option<i32> {
        let present =
            unsafe { sys::ac3forge_scanned_stream_has_oba_complexity_index(self.raw.as_ptr()) };
        if present == 0 {
            return None;
        }
        Some(unsafe { sys::ac3forge_scanned_stream_oba_complexity_index(self.raw.as_ptr()) })
    }
}

impl Drop for ScannedStream<'_> {
    fn drop(&mut self) {
        unsafe { sys::ac3forge_scanned_stream_destroy(self.raw.as_ptr()) };
    }
}
