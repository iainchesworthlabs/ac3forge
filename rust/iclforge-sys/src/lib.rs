//! Raw, unsafe FFI bindings to `iclforge_c/iclforge.h`, generated at build time by `bindgen`
//! against the exact header this crate's `build.rs` also compiled `libiclforge_c` from — see
//! that file and `rust/README.md` for how the two are kept from drifting apart.
//!
//! Nothing here is hand-written or hand-maintained: every declaration below tracks the header
//! automatically. Prefer the `iclforge` crate, which wraps this one in a safe, idiomatic API.
#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
