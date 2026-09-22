//! Proves the raw bindings actually link and call into the real library at runtime, not just
//! compile against the generated declarations.

use std::ffi::CStr;

#[test]
fn version_reports_something_sane() {
    let version = unsafe { ac3forge_sys::ac3forge_version() };
    assert!(version.major >= 0);
    assert!(!version.full.is_null());
    let full = unsafe { CStr::from_ptr(version.full) }.to_str().unwrap();
    assert!(!full.is_empty());
}

#[test]
fn status_message_round_trips_ok() {
    let message =
        unsafe { ac3forge_sys::ac3forge_status_message(ac3forge_sys::ac3forge_status_AC3FORGE_OK) };
    assert!(!message.is_null());
    let message = unsafe { CStr::from_ptr(message) }.to_str().unwrap();
    assert_eq!(message, "ok");
}
