//! C ABI surface of picohttpparser-rs (Milestone 1 shell).
//!
//! Signatures and struct layouts mirror `reference/picohttpparser.h` at pin
//! `f4d94b48b31e0abae029ebeafcfd9ca0680ede58`. All five functions are
//! **stubs**: they exist to pin the ABI (exported names, calling convention,
//! argument/return types, struct layouts) and return failure until the real
//! parsers land in Milestone 2+. Nothing here dereferences its arguments, so
//! the stubs are safe to call with null pointers for ABI-smoke purposes.

use core::ffi::{c_char, c_int};

/// Mirror of C `struct phr_header` (2 field pairs of pointer + length).
///
/// `name == NULL` marks a continuation line of a multiline header (upstream
/// contract). Layout is pinned by the compile-time assertions below and by
/// `tests/abi.rs`.
#[repr(C)]
pub struct PhrHeader {
    name: *const c_char,
    name_len: usize,
    value: *const c_char,
    value_len: usize,
}

/// Mirror of C `struct phr_chunked_decoder` (caller-owned decoder state).
///
/// Must be zero-filled before first use per the upstream contract. The
/// underscored fields are internal decoder state and must not be touched by
/// callers; `consume_trailer` selects whether trailing headers are consumed.
#[repr(C)]
pub struct PhrChunkedDecoder {
    bytes_left_in_chunk: usize,
    consume_trailer: c_char,
    _hex_count: c_char,
    _state: c_char,
    _total_read: u64,
    _total_overhead: u64,
}

/// Parses an HTTP request head.
///
/// Returns bytes consumed (>= 0), `-2` for partial input, `-1` on failure.
/// **Milestone 1 stub**: always returns `-1`; real parsing is added in
/// Milestone 2. Argument names mirror the C parameter list in
/// `reference/picohttpparser.h`.
///
/// # Safety
///
/// The stub does not dereference any argument. When real parsing lands, all
/// pointers must be valid: `buf`/`len` describe the input buffer, `headers`
/// must point to `*num_headers` entries, and the output pointers must point
/// to writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_request(
    _buf: *const c_char,
    _len: usize,
    _method: *mut *const c_char,
    _method_len: *mut usize,
    _path: *mut *const c_char,
    _path_len: *mut usize,
    _minor_version: *mut c_int,
    _headers: *mut PhrHeader,
    _num_headers: *mut usize,
    _last_len: usize,
) -> c_int {
    -1
}

/// Parses an HTTP response head.
///
/// Returns bytes consumed (>= 0), `-2` for partial input, `-1` on failure.
/// **Milestone 1 stub**: always returns `-1`; real parsing is added in
/// Milestone 3.
///
/// # Safety
///
/// The stub does not dereference any argument. When real parsing lands, all
/// pointers must be valid: `_buf`/`len` describe the input buffer, `headers`
/// must point to `*num_headers` entries, and the output pointers must point
/// to writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_response(
    _buf: *const c_char,
    _len: usize,
    _minor_version: *mut c_int,
    _status: *mut c_int,
    _msg: *mut *const c_char,
    _msg_len: *mut usize,
    _headers: *mut PhrHeader,
    _num_headers: *mut usize,
    _last_len: usize,
) -> c_int {
    -1
}

/// Parses a standalone header block.
///
/// Returns bytes consumed (>= 0), `-2` for partial input, `-1` on failure.
/// **Milestone 1 stub**: always returns `-1`; real parsing is added in
/// Milestone 4.
///
/// # Safety
///
/// The stub does not dereference any argument. When real parsing lands, all
/// pointers must be valid: `buf`/`len` describe the input buffer, `headers`
/// must point to `*num_headers` entries, and `num_headers` must point to
/// writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_headers(
    _buf: *const c_char,
    _len: usize,
    _headers: *mut PhrHeader,
    _num_headers: *mut usize,
    _last_len: usize,
) -> c_int {
    -1
}

/// Decodes chunked-transfer encoding in place.
///
/// Returns octets left undecoded (>= 0), `-2` for incomplete input, `-1` on
/// failure. **Milestone 1 stub**: always returns `-1`; real decoding is added
/// in Milestone 5.
///
/// # Safety
///
/// The stub does not dereference any argument. When real decoding lands,
/// `decoder` must point to a zero-initialized `PhrChunkedDecoder` that lives
/// across calls, and `buf`/`*bufsz` must describe a writable buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_decode_chunked(
    _decoder: *mut PhrChunkedDecoder,
    _buf: *mut c_char,
    _bufsz: *mut usize,
) -> isize {
    -1
}

/// Queries whether the chunked decoder is in the middle of chunked data.
///
/// **Milestone 1 stub**: always returns `0` (not in data). Real state
/// tracking is added in Milestone 5.
///
/// # Safety
///
/// The stub does not dereference its argument. When real tracking lands,
/// `decoder` must point to a valid `PhrChunkedDecoder`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_decode_chunked_is_in_data(
    _decoder: *const PhrChunkedDecoder,
) -> c_int {
    0
}

// --- compile-time ABI pins ------------------------------------------------
//
// Each alias below spells out the exact C ABI type of one exported symbol;
// the `const _: Alias = symbol;` line fails to compile if the function's type
// ever drifts from the pinned C signature.

/// Exported C ABI type of `phr_parse_request`.
type ParseRequestFn = unsafe extern "C" fn(
    *const c_char,
    usize,
    *mut *const c_char,
    *mut usize,
    *mut *const c_char,
    *mut usize,
    *mut c_int,
    *mut PhrHeader,
    *mut usize,
    usize,
) -> c_int;
const _: ParseRequestFn = phr_parse_request;

/// Exported C ABI type of `phr_parse_response`.
type ParseResponseFn = unsafe extern "C" fn(
    *const c_char,
    usize,
    *mut c_int,
    *mut c_int,
    *mut *const c_char,
    *mut usize,
    *mut PhrHeader,
    *mut usize,
    usize,
) -> c_int;
const _: ParseResponseFn = phr_parse_response;

/// Exported C ABI type of `phr_parse_headers`.
type ParseHeadersFn =
    unsafe extern "C" fn(*const c_char, usize, *mut PhrHeader, *mut usize, usize) -> c_int;
const _: ParseHeadersFn = phr_parse_headers;

/// Exported C ABI type of `phr_decode_chunked` (`ssize_t` == `isize`).
type DecodeChunkedFn =
    unsafe extern "C" fn(*mut PhrChunkedDecoder, *mut c_char, *mut usize) -> isize;
const _: DecodeChunkedFn = phr_decode_chunked;

/// Exported C ABI type of `phr_decode_chunked_is_in_data`.
type DecodeChunkedIsInDataFn = unsafe extern "C" fn(*const PhrChunkedDecoder) -> c_int;
const _: DecodeChunkedIsInDataFn = phr_decode_chunked_is_in_data;

// --- struct layout pins (64-bit targets) ----------------------------------
//
// These match C on LP64 and LLP64 targets (the vast majority in use). On
// 32-bit targets pointers and size_t are 4 bytes, so every offset/size below
// differs; re-pin those with a 32-bit toolchain when one is exercised.

#[cfg(target_pointer_width = "64")]
const _: () = {
    use std::mem::{align_of, offset_of, size_of};

    // struct phr_header: name (ptr, 8) + name_len (size_t, 8) + value (ptr, 8) + value_len (size_t, 8).
    assert!(size_of::<PhrHeader>() == 32);
    assert!(align_of::<PhrHeader>() == 8);
    assert!(offset_of!(PhrHeader, name) == 0);
    assert!(offset_of!(PhrHeader, name_len) == 8);
    assert!(offset_of!(PhrHeader, value) == 16);
    assert!(offset_of!(PhrHeader, value_len) == 24);

    // struct phr_chunked_decoder: bytes_left_in_chunk (size_t, 8) at 0, three
    // char fields at 8..=10, padding to 16, then two uint64_t at 16 and 24.
    assert!(size_of::<PhrChunkedDecoder>() == 32);
    assert!(align_of::<PhrChunkedDecoder>() == 8);
    assert!(offset_of!(PhrChunkedDecoder, bytes_left_in_chunk) == 0);
    assert!(offset_of!(PhrChunkedDecoder, consume_trailer) == 8);
    assert!(offset_of!(PhrChunkedDecoder, _hex_count) == 9);
    assert!(offset_of!(PhrChunkedDecoder, _state) == 10);
    assert!(offset_of!(PhrChunkedDecoder, _total_read) == 16);
    assert!(offset_of!(PhrChunkedDecoder, _total_overhead) == 24);
};
