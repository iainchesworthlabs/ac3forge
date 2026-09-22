//! Raw, unsafe FFI bindings to `ac3forge_c/ac3forge.h`, generated at build time by `bindgen`
//! against the exact header this crate's `build.rs` also compiled `libac3forge_c` from — see
//! that file and `rust/README.md` for how the two are kept from drifting apart.
//!
//! Nothing here is hand-written or hand-maintained: every declaration below tracks the header
//! automatically. Prefer the `ac3forge` crate, which wraps this one in a safe, idiomatic API.
#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
