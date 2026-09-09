//! ABI integration tests (Milestone 1 shell; null-guard wording updated
//! Milestones 2–4 as each parser went live).
//!
//! Layout pins plus fail-closed entry guards. `phr_parse_request` is a real
//! parser since M2, but these all-null calls exercise only its null-pointer
//! guards (which return -1 without touching memory) — if those guards ever
//! regress, this test segfaults loudly instead of failing quietly, and
//! that is the point.

use picohttpparser_rs::{PhrChunkedDecoder, PhrHeader};
use std::mem::{align_of, size_of};
use std::ptr::{null, null_mut};

/// C `struct phr_header` and `struct phr_chunked_decoder` layout on 64-bit
/// targets (LP64/LLP64): ptr/size_t = 8, char = 1, uint64_t = 8.
/// Field offsets are pinned in-crate (`src/ffi.rs` layout consts).
#[cfg(target_pointer_width = "64")]
#[test]
fn layouts_match_c_on_64_bit() {
    assert_eq!(size_of::<PhrHeader>(), 32);
    assert_eq!(align_of::<PhrHeader>(), 8);
    assert_eq!(size_of::<PhrChunkedDecoder>(), 32);
    assert_eq!(align_of::<PhrChunkedDecoder>(), 8);
}

#[test]
fn request_null_guards_return_error() {
    let ret = unsafe {
        picohttpparser_rs::ffi::phr_parse_request(
            null(),
            0,
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            0,
        )
    };
    assert_eq!(ret, -1);
}

#[test]
fn response_null_guards_return_error() {
    let ret = unsafe {
        picohttpparser_rs::ffi::phr_parse_response(
            null(),
            0,
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            0,
        )
    };
    assert_eq!(ret, -1);
}

#[test]
fn headers_null_guards_return_error() {
    let ret =
        unsafe { picohttpparser_rs::ffi::phr_parse_headers(null(), 0, null_mut(), null_mut(), 0) };
    assert_eq!(ret, -1);
}

#[test]
fn stub_phr_decode_chunked_returns_error() {
    let ret =
        unsafe { picohttpparser_rs::ffi::phr_decode_chunked(null_mut(), null_mut(), null_mut()) };
    assert_eq!(ret, -1);
}

#[test]
fn stub_phr_decode_chunked_is_in_data_returns_not_in_data() {
    let ret = unsafe { picohttpparser_rs::ffi::phr_decode_chunked_is_in_data(null()) };
    assert_eq!(ret, 0);
}
