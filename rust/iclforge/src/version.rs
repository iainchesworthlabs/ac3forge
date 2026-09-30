use std::ffi::CStr;

/// The version of `iclforge` actually linked at runtime — mirrors `iclforge_version_t`.
///
/// There is no ABI-compatibility promise before v1.0 (roadmap item AP1): this crate's `build.rs`
/// always compiles `libiclforge_c` from the exact source tree it's part of, so `full` will
/// always match this crate's own commit in practice, but a caller linking against a
/// separately-built copy of the library should still check this rather than assume.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Version {
    pub major: i32,
    pub minor: i32,
    pub patch: i32,
    /// Semver plus prerelease suffix, e.g. `"0.9.0-beta.1"`.
    pub full: String,
}

/// The version of `iclforge` this crate is linked against.
pub fn version() -> Version {
    // SAFETY: iclforge_version() takes no arguments and returns a value type whose `full`
    // pointer is library-owned storage valid for the process lifetime (iclforge.h's own doc
    // comment) - never NULL, never freed here.
    let raw = unsafe { iclforge_sys::iclforge_version() };
    let full = unsafe { CStr::from_ptr(raw.full) }
        .to_string_lossy()
        .into_owned();
    Version {
        major: raw.major,
        minor: raw.minor,
        patch: raw.patch,
        full,
    }
}
