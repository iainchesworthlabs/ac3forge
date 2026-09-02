use std::ffi::CStr;
use std::fmt;

/// Mirrors `ac3forge_status_t`, one-for-one, with one deliberate difference: this enum carries
/// an [`Error::Other`] fallback rather than being a closed set.
///
/// A plain C `enum` crossing an FFI boundary is an open set in a way a Rust `enum` normally
/// isn't: nothing in `ac3forge_c/ac3forge.h` documents whether a future minor version may add a
/// new status code (see `rust/README.md`'s "header defects found" section, item 3), and this
/// crate has no way to tell "the library I linked added a code I don't know about" apart from
/// "something is badly wrong" if it tried to force every raw value into a fixed set of variants.
/// [`Error::Other`] keeps that distinction representable instead of silently mapping an unknown
/// code onto the wrong known one, or panicking.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    InvalidArgument,
    OutOfMemory,
    /// An exception crossed the C boundary and was caught there — see `ac3forge.h`'s own
    /// comment on `AC3FORGE_ERROR_INTERNAL`.
    Internal,
    EncodeInvalidBitrate,
    EncodeInvalidDialnorm,
    EncodeInvalidSubstream,
    EncodeInvalidChannelMap,
    EncodeTooManyChannels,
    EncodeInvalidMixLevel,
    EncodeInvalidObjectAudio,
    EncodeInvalidBsi,
    DecodeTruncated,
    DecodeBadSyncWord,
    DecodeBadCrc,
    DecodeReservedValue,
    DecodeUnsupported,
    DecodeInvalidStream,
    /// A raw `ac3forge_status_t` value this crate doesn't recognize. `ac3forge_status_message`
    /// still gives a human-readable string for it (`"unknown status"` for a value the C library
    /// itself doesn't recognize either — see `src/capi/src/common.cpp`'s own fallback), so
    /// [`Error`]'s `Display` impl works for this variant exactly like every other one.
    Other(u32),
}

impl Error {
    /// `None` for `AC3FORGE_OK`, `Some(Error)` otherwise — matches how every raw entry point
    /// returns a status alongside its real result.
    pub(crate) fn from_status(status: ac3forge_sys::ac3forge_status_t) -> Option<Error> {
        use ac3forge_sys::*;
        #[allow(non_upper_case_globals)]
        Some(match status {
            s if s == ac3forge_status_AC3FORGE_OK => return None,
            s if s == ac3forge_status_AC3FORGE_ERROR_INVALID_ARGUMENT => Error::InvalidArgument,
            s if s == ac3forge_status_AC3FORGE_ERROR_OUT_OF_MEMORY => Error::OutOfMemory,
            s if s == ac3forge_status_AC3FORGE_ERROR_INTERNAL => Error::Internal,
            s if s == ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_BITRATE => {
                Error::EncodeInvalidBitrate
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM => {
                Error::EncodeInvalidDialnorm
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM => {
                Error::EncodeInvalidSubstream
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP => {
                Error::EncodeInvalidChannelMap
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS => {
                Error::EncodeTooManyChannels
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL => {
                Error::EncodeInvalidMixLevel
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO => {
                Error::EncodeInvalidObjectAudio
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_BSI => Error::EncodeInvalidBsi,
            s if s == ac3forge_status_AC3FORGE_ERROR_DECODE_TRUNCATED => Error::DecodeTruncated,
            s if s == ac3forge_status_AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD => {
                Error::DecodeBadSyncWord
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_DECODE_BAD_CRC => Error::DecodeBadCrc,
            s if s == ac3forge_status_AC3FORGE_ERROR_DECODE_RESERVED_VALUE => {
                Error::DecodeReservedValue
            }
            s if s == ac3forge_status_AC3FORGE_ERROR_DECODE_UNSUPPORTED => Error::DecodeUnsupported,
            s if s == ac3forge_status_AC3FORGE_ERROR_DECODE_INVALID_STREAM => {
                Error::DecodeInvalidStream
            }
            // `as u32`, not a plain move: bindgen types C enums i32 on MSVC and u32 on the
            // Unix targets, so the raw discriminant's own type is platform-dependent - found
            // by this crate's first Windows build. The stored value is the same bit pattern
            // either way.
            other => Error::Other(other as u32),
        })
    }

    /// `Ok(())` for `AC3FORGE_OK`, `Err(Error)` otherwise.
    pub(crate) fn check(status: ac3forge_sys::ac3forge_status_t) -> Result<(), Error> {
        match Error::from_status(status) {
            Some(e) => Err(e),
            None => Ok(()),
        }
    }

    fn raw(self) -> ac3forge_sys::ac3forge_status_t {
        use ac3forge_sys::*;
        match self {
            Error::InvalidArgument => ac3forge_status_AC3FORGE_ERROR_INVALID_ARGUMENT,
            Error::OutOfMemory => ac3forge_status_AC3FORGE_ERROR_OUT_OF_MEMORY,
            Error::Internal => ac3forge_status_AC3FORGE_ERROR_INTERNAL,
            Error::EncodeInvalidBitrate => ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_BITRATE,
            Error::EncodeInvalidDialnorm => ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM,
            Error::EncodeInvalidSubstream => {
                ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM
            }
            Error::EncodeInvalidChannelMap => {
                ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP
            }
            Error::EncodeTooManyChannels => ac3forge_status_AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS,
            Error::EncodeInvalidMixLevel => ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL,
            Error::EncodeInvalidObjectAudio => {
                ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO
            }
            Error::EncodeInvalidBsi => ac3forge_status_AC3FORGE_ERROR_ENCODE_INVALID_BSI,
            Error::DecodeTruncated => ac3forge_status_AC3FORGE_ERROR_DECODE_TRUNCATED,
            Error::DecodeBadSyncWord => ac3forge_status_AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD,
            Error::DecodeBadCrc => ac3forge_status_AC3FORGE_ERROR_DECODE_BAD_CRC,
            Error::DecodeReservedValue => ac3forge_status_AC3FORGE_ERROR_DECODE_RESERVED_VALUE,
            Error::DecodeUnsupported => ac3forge_status_AC3FORGE_ERROR_DECODE_UNSUPPORTED,
            Error::DecodeInvalidStream => ac3forge_status_AC3FORGE_ERROR_DECODE_INVALID_STREAM,
            // The mirror of from_status's cast, same platform reasoning.
            #[allow(clippy::unnecessary_cast)]
            Error::Other(raw) => raw as _,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // SAFETY: ac3forge_status_message() returns a pointer to library-owned storage valid
        // for the process lifetime for every possible input, including a value it doesn't
        // recognize (src/capi/src/common.cpp falls through to "unknown status") - never NULL,
        // never freed here.
        let message = unsafe { CStr::from_ptr(ac3forge_sys::ac3forge_status_message(self.raw())) };
        write!(f, "{}", message.to_string_lossy())
    }
}

impl std::error::Error for Error {}
