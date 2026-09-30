use std::ffi::CStr;
use std::fmt;

/// Mirrors `iclforge_status_t`, one-for-one, with one deliberate difference: this enum carries
/// an [`Error::Other`] fallback rather than being a closed set.
///
/// A plain C `enum` crossing an FFI boundary is an open set in a way a Rust `enum` normally
/// isn't: nothing in `iclforge_c/iclforge.h` documents whether a future minor version may add a
/// new status code (see `rust/README.md`'s "header defects found" section, item 3), and this
/// crate has no way to tell "the library I linked added a code I don't know about" apart from
/// "something is badly wrong" if it tried to force every raw value into a fixed set of variants.
/// [`Error::Other`] keeps that distinction representable instead of silently mapping an unknown
/// code onto the wrong known one, or panicking.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    InvalidArgument,
    OutOfMemory,
    /// An exception crossed the C boundary and was caught there — see `iclforge.h`'s own
    /// comment on `ICLFORGE_ERROR_INTERNAL`.
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
    /// `ac4::DecodeError` (a syntax element ran past the end of its substream).
    Ac4DecodeTruncated,
    /// `ac4::DecodeError` (`ac4::parse_raw_frame` refused the table of contents).
    Ac4DecodeInvalidToc,
    /// `ac4::DecodeError` (a value the syntax cannot follow).
    Ac4DecodeInvalidStream,
    /// `ac4::DecodeError` (legal AC-4 this decoder does not read yet).
    Ac4DecodeUnsupported,
    /// `ac4::DecodeError` (a non-I-frame needs configuration no I-frame has sent).
    Ac4DecodeMissingIFrame,
    /// `ac4::EncodeError` (a configuration outside what the encoder writes).
    Ac4EncodeInvalidConfig,
    /// `ac4::EncodeError` (a channel count/length mismatch, or a non-finite sample).
    Ac4EncodeInvalidInput,
    /// A raw `iclforge_status_t` value this crate doesn't recognize. `iclforge_status_message`
    /// still gives a human-readable string for it (`"unknown status"` for a value the C library
    /// itself doesn't recognize either — see `src/capi/src/common.cpp`'s own fallback), so
    /// [`Error`]'s `Display` impl works for this variant exactly like every other one.
    Other(u32),
}

impl Error {
    /// `None` for `ICLFORGE_OK`, `Some(Error)` otherwise — matches how every raw entry point
    /// returns a status alongside its real result.
    pub(crate) fn from_status(status: iclforge_sys::iclforge_status_t) -> Option<Error> {
        use iclforge_sys::*;
        #[allow(non_upper_case_globals)]
        Some(match status {
            s if s == iclforge_status_ICLFORGE_OK => return None,
            s if s == iclforge_status_ICLFORGE_ERROR_INVALID_ARGUMENT => Error::InvalidArgument,
            s if s == iclforge_status_ICLFORGE_ERROR_OUT_OF_MEMORY => Error::OutOfMemory,
            s if s == iclforge_status_ICLFORGE_ERROR_INTERNAL => Error::Internal,
            s if s == iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_BITRATE => {
                Error::EncodeInvalidBitrate
            }
            s if s == iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_DIALNORM => {
                Error::EncodeInvalidDialnorm
            }
            s if s == iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_SUBSTREAM => {
                Error::EncodeInvalidSubstream
            }
            s if s == iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP => {
                Error::EncodeInvalidChannelMap
            }
            s if s == iclforge_status_ICLFORGE_ERROR_ENCODE_TOO_MANY_CHANNELS => {
                Error::EncodeTooManyChannels
            }
            s if s == iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_MIX_LEVEL => {
                Error::EncodeInvalidMixLevel
            }
            s if s == iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO => {
                Error::EncodeInvalidObjectAudio
            }
            s if s == iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_BSI => Error::EncodeInvalidBsi,
            s if s == iclforge_status_ICLFORGE_ERROR_DECODE_TRUNCATED => Error::DecodeTruncated,
            s if s == iclforge_status_ICLFORGE_ERROR_DECODE_BAD_SYNC_WORD => {
                Error::DecodeBadSyncWord
            }
            s if s == iclforge_status_ICLFORGE_ERROR_DECODE_BAD_CRC => Error::DecodeBadCrc,
            s if s == iclforge_status_ICLFORGE_ERROR_DECODE_RESERVED_VALUE => {
                Error::DecodeReservedValue
            }
            s if s == iclforge_status_ICLFORGE_ERROR_DECODE_UNSUPPORTED => Error::DecodeUnsupported,
            s if s == iclforge_status_ICLFORGE_ERROR_DECODE_INVALID_STREAM => {
                Error::DecodeInvalidStream
            }
            s if s == iclforge_status_ICLFORGE_ERROR_AC4_DECODE_TRUNCATED => {
                Error::Ac4DecodeTruncated
            }
            s if s == iclforge_status_ICLFORGE_ERROR_AC4_DECODE_INVALID_TOC => {
                Error::Ac4DecodeInvalidToc
            }
            s if s == iclforge_status_ICLFORGE_ERROR_AC4_DECODE_INVALID_STREAM => {
                Error::Ac4DecodeInvalidStream
            }
            s if s == iclforge_status_ICLFORGE_ERROR_AC4_DECODE_UNSUPPORTED => {
                Error::Ac4DecodeUnsupported
            }
            s if s == iclforge_status_ICLFORGE_ERROR_AC4_DECODE_MISSING_IFRAME => {
                Error::Ac4DecodeMissingIFrame
            }
            s if s == iclforge_status_ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG => {
                Error::Ac4EncodeInvalidConfig
            }
            s if s == iclforge_status_ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT => {
                Error::Ac4EncodeInvalidInput
            }
            // `as u32`, not a plain move: bindgen types C enums i32 on MSVC and u32 on the
            // Unix targets, so the raw discriminant's own type is platform-dependent - found
            // by this crate's first Windows build. The stored value is the same bit pattern
            // either way.
            // (and the allow is for the Unix targets, where u32 -> u32 trips
            // clippy::unnecessary_cast - same annotation as to_raw() below.)
            #[allow(clippy::unnecessary_cast)]
            other => Error::Other(other as u32),
        })
    }

    /// `Ok(())` for `ICLFORGE_OK`, `Err(Error)` otherwise.
    pub(crate) fn check(status: iclforge_sys::iclforge_status_t) -> Result<(), Error> {
        match Error::from_status(status) {
            Some(e) => Err(e),
            None => Ok(()),
        }
    }

    fn raw(self) -> iclforge_sys::iclforge_status_t {
        use iclforge_sys::*;
        match self {
            Error::InvalidArgument => iclforge_status_ICLFORGE_ERROR_INVALID_ARGUMENT,
            Error::OutOfMemory => iclforge_status_ICLFORGE_ERROR_OUT_OF_MEMORY,
            Error::Internal => iclforge_status_ICLFORGE_ERROR_INTERNAL,
            Error::EncodeInvalidBitrate => iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_BITRATE,
            Error::EncodeInvalidDialnorm => iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_DIALNORM,
            Error::EncodeInvalidSubstream => {
                iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_SUBSTREAM
            }
            Error::EncodeInvalidChannelMap => {
                iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP
            }
            Error::EncodeTooManyChannels => iclforge_status_ICLFORGE_ERROR_ENCODE_TOO_MANY_CHANNELS,
            Error::EncodeInvalidMixLevel => iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_MIX_LEVEL,
            Error::EncodeInvalidObjectAudio => {
                iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO
            }
            Error::EncodeInvalidBsi => iclforge_status_ICLFORGE_ERROR_ENCODE_INVALID_BSI,
            Error::DecodeTruncated => iclforge_status_ICLFORGE_ERROR_DECODE_TRUNCATED,
            Error::DecodeBadSyncWord => iclforge_status_ICLFORGE_ERROR_DECODE_BAD_SYNC_WORD,
            Error::DecodeBadCrc => iclforge_status_ICLFORGE_ERROR_DECODE_BAD_CRC,
            Error::DecodeReservedValue => iclforge_status_ICLFORGE_ERROR_DECODE_RESERVED_VALUE,
            Error::DecodeUnsupported => iclforge_status_ICLFORGE_ERROR_DECODE_UNSUPPORTED,
            Error::DecodeInvalidStream => iclforge_status_ICLFORGE_ERROR_DECODE_INVALID_STREAM,
            Error::Ac4DecodeTruncated => iclforge_status_ICLFORGE_ERROR_AC4_DECODE_TRUNCATED,
            Error::Ac4DecodeInvalidToc => iclforge_status_ICLFORGE_ERROR_AC4_DECODE_INVALID_TOC,
            Error::Ac4DecodeInvalidStream => {
                iclforge_status_ICLFORGE_ERROR_AC4_DECODE_INVALID_STREAM
            }
            Error::Ac4DecodeUnsupported => iclforge_status_ICLFORGE_ERROR_AC4_DECODE_UNSUPPORTED,
            Error::Ac4DecodeMissingIFrame => {
                iclforge_status_ICLFORGE_ERROR_AC4_DECODE_MISSING_IFRAME
            }
            Error::Ac4EncodeInvalidConfig => {
                iclforge_status_ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG
            }
            Error::Ac4EncodeInvalidInput => iclforge_status_ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT,
            // The mirror of from_status's cast, same platform reasoning.
            #[allow(clippy::unnecessary_cast)]
            Error::Other(raw) => raw as _,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // SAFETY: iclforge_status_message() returns a pointer to library-owned storage valid
        // for the process lifetime for every possible input, including a value it doesn't
        // recognize (src/capi/src/common.cpp falls through to "unknown status") - never NULL,
        // never freed here.
        let message = unsafe { CStr::from_ptr(iclforge_sys::iclforge_status_message(self.raw())) };
        write!(f, "{}", message.to_string_lossy())
    }
}

impl std::error::Error for Error {}
