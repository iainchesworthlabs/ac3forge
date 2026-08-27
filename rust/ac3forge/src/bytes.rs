use std::fmt;
use std::ops::Deref;
use std::slice;

/// An owned, immutable byte buffer — one encoded frame's or access unit's worth of bytes.
/// Wraps `ac3forge_bytes_t`, destroying it on drop.
pub struct Bytes {
    raw: *mut ac3forge_sys::ac3forge_bytes_t,
}

// SAFETY: the underlying C type is an owned, immutable buffer with no interior mutability and no
// thread affinity of its own.
unsafe impl Send for Bytes {}
unsafe impl Sync for Bytes {}

impl Bytes {
    /// # Safety
    /// `raw` must be a non-null `ac3forge_bytes_t*` this `Bytes` now owns exclusively (i.e. it
    /// came from an `out_frame`/`out_unit` out-parameter this crate just received `AC3FORGE_OK`
    /// for, and nothing else will ever destroy it).
    pub(crate) unsafe fn from_raw(raw: *mut ac3forge_sys::ac3forge_bytes_t) -> Self {
        debug_assert!(!raw.is_null());
        Bytes { raw }
    }
}

impl Deref for Bytes {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        // SAFETY: `self.raw` is valid for as long as `self` is alive (that's the whole point of
        // owning it), and ac3forge_bytes_data()/_size() are read-only accessors on it.
        unsafe {
            let data = ac3forge_sys::ac3forge_bytes_data(self.raw);
            let size = ac3forge_sys::ac3forge_bytes_size(self.raw);
            if data.is_null() || size == 0 {
                &[]
            } else {
                slice::from_raw_parts(data, size)
            }
        }
    }
}

impl Drop for Bytes {
    fn drop(&mut self) {
        // SAFETY: `self.raw` is owned exclusively by this `Bytes` (see `from_raw`'s contract),
        // so destroying it exactly once here is correct.
        unsafe { ac3forge_sys::ac3forge_bytes_destroy(self.raw) };
    }
}

impl fmt::Debug for Bytes {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Bytes").field("len", &self.len()).finish()
    }
}
