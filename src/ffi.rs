//! C ABI surface of picohttpparser-rs (Milestone 1 shell).
//!
//! Signatures and struct layouts mirror `reference/picohttpparser.h` at pin
//! `f4d94b48b31e0abae029ebeafcfd9ca0680ede58`. `phr_parse_request`
//! (Milestone 2, via `crate::request`), `phr_parse_response` (Milestone 3,
//! via `crate::response`) and `phr_parse_headers` (Milestone 4, shared
//! core wired straight through) are real parsers; `phr_decode_chunked` and
//! `phr_decode_chunked_is_in_data` are still **stubs** until Milestone 5.
//!
//! This module is the crate's entire `unsafe` surface: the parsing core only
//! reports offsets, and this seam turns them into output pointers.
//!
//! Panic policy (pinned before M2): keep `panic = "unwind"`, rely on
//! edition-2024 `extern "C"` being nounwind (a panic aborts, never unwinds
//! through the C ABI), wrap real M2+ bodies in `catch_unwind` mapping panic
//! to the documented error return, and keep the parser core panic-free
//! (`clippy::unwrap_used`/`expect_used` are denied in `Cargo.toml`).

use core::ffi::{c_char, c_int};

/// Mirror of C `struct phr_header` (2 field pairs of pointer + length).
///
/// `name == NULL` marks a continuation line of a multiline header (upstream
/// contract). Fields are public like any FFI struct: the core fills them
/// through the FFI seam, and integration tests assert on them. Layout is
/// pinned by the compile-time assertions below and by `tests/abi.rs`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PhrHeader {
    /// Header name, or NULL for a continuation line.
    pub name: *const c_char,
    /// Length of the name in bytes (0 with NULL).
    pub name_len: usize,
    /// Header value (leading space skipped, trailing space trimmed).
    pub value: *const c_char,
    /// Length of the value in bytes.
    pub value_len: usize,
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

/// [`HeaderSink`](crate::core::HeaderSink) writing straight into the
/// caller's `phr_header` array.
///
/// Offsets from the core become pointers by adding the input base — the only
/// pointer arithmetic in the crate, confined to this seam. Writes are
/// two-phase (name, then value) and progressive (header `i` completes before
/// `i + 1` starts), exactly like C filling the caller array — even the
/// in-progress slot on error paths ends up byte-identical.
struct RawSink {
    base: *const c_char,
    slots: *mut PhrHeader,
    cap: usize,
}

impl crate::core::HeaderSink for RawSink {
    fn capacity(&self) -> usize {
        self.cap
    }
    /// SAFETY (both phases): `i < cap` is enforced by the core (`n == max`
    /// errors before any write), `slots` points to `cap` writable entries
    /// (checked at entry), and every offset lies inside the input buffer by
    /// construction in the core. Unreachable with `len == 0`, so `base.add`
    /// never runs on null.
    fn set_name(&mut self, i: usize, name: Option<(usize, usize)>) {
        unsafe {
            let h = &mut *self.slots.add(i);
            match name {
                Some((off, len)) => {
                    h.name = self.base.add(off);
                    h.name_len = len;
                }
                None => {
                    h.name = std::ptr::null();
                    h.name_len = 0;
                }
            }
        }
    }
    fn set_value(&mut self, i: usize, value: (usize, usize)) {
        unsafe {
            let h = &mut *self.slots.add(i);
            h.value = self.base.add(value.0);
            h.value_len = value.1;
        }
    }
}

/// Parses an HTTP request head.
///
/// Returns bytes consumed (>= 0), `-2` for partial input, `-1` on failure.
/// Behaviorally identical to C's `phr_parse_request` (Milestone 2), including
/// the `last_len` streaming hint and zeroed outputs on every path.
/// Argument names mirror the C parameter list in `reference/picohttpparser.h`.
///
/// # Safety
///
/// `buf`/`len` must describe a readable input buffer, `headers` must point
/// to `*num_headers` writable entries, and the output pointers must point to
/// writable storage. Null outputs (or a null input with `len != 0`) fail
/// closed with `-1` instead of faulting. A panic anywhere inside maps to
/// `-1` per the crate's pinned panic policy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_request(
    buf: *const c_char,
    len: usize,
    method: *mut *const c_char,
    method_len: *mut usize,
    path: *mut *const c_char,
    path_len: *mut usize,
    minor_version: *mut c_int,
    headers: *mut PhrHeader,
    num_headers: *mut usize,
    last_len: usize,
) -> c_int {
    // `match` (not `unwrap_or`) so the -1 mapping stays explicit; `unwrap_or`
    // would also trip the crate's `clippy::unwrap_used` deny.
    #[allow(clippy::manual_unwrap_or)]
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        parse_request_inner(
            buf,
            len,
            method,
            method_len,
            path,
            path_len,
            minor_version,
            headers,
            num_headers,
            last_len,
        )
    })) {
        Ok(r) => r,
        Err(_) => -1,
    }
}

/// `phr_parse_request` minus the unwind guard. Plain function (not `extern`)
/// so every raw-pointer operation needs an explicit `unsafe` block under the
/// crate's `unsafe_op_in_unsafe_fn = "deny"` lint. Ten parameters mirror the
/// C signature 1:1 by design (same order, same meaning), so bundling them
/// would only obscure the ABI mapping.
#[allow(clippy::too_many_arguments)]
fn parse_request_inner(
    buf: *const c_char,
    len: usize,
    method: *mut *const c_char,
    method_len: *mut usize,
    path: *mut *const c_char,
    path_len: *mut usize,
    minor_version: *mut c_int,
    headers: *mut PhrHeader,
    num_headers: *mut usize,
    last_len: usize,
) -> c_int {
    if method.is_null()
        || method_len.is_null()
        || path.is_null()
        || path_len.is_null()
        || minor_version.is_null()
        || num_headers.is_null()
        || (len != 0 && buf.is_null())
    {
        return -1;
    }
    // SAFETY: outputs checked non-null above; zeroed first like C on entry.
    unsafe {
        *method = std::ptr::null();
        *method_len = 0;
        *path = std::ptr::null();
        *path_len = 0;
        *minor_version = -1;
        let max = *num_headers;
        *num_headers = 0;
        if max != 0 && headers.is_null() {
            return -1;
        }
        let data: &[u8] = if len == 0 {
            &[]
        } else {
            // SAFETY: non-null (checked) + caller-guaranteed `len` bytes.
            std::slice::from_raw_parts(buf as *const u8, len)
        };
        let mut sink = RawSink {
            base: buf,
            slots: headers,
            cap: max,
        };
        // Progressive outputs, published on success AND error: C assigns
        // *method/*path/*version as each scan completes (even empty scans),
        // so a later failure still leaves the completed prefix observable.
        let mut prog = crate::request::Progress::default();
        let r = match crate::request::parse_request(data, &mut sink, last_len, &mut prog) {
            Ok(consumed) => consumed as c_int,
            Err(crate::core::Error::Partial) => -2,
            Err(crate::core::Error::Malformed) => -1,
        };
        if let Some((off, len)) = prog.method {
            // SAFETY: offsets lie inside the input by construction; an empty
            // token still yields the (non-null) scan position, like C.
            *method = buf.add(off);
            *method_len = len;
        }
        if let Some((off, len)) = prog.path {
            *path = buf.add(off);
            *path_len = len;
        }
        if let Some(v) = prog.version {
            *minor_version = v;
        }
        *num_headers = prog.headers;
        r
    }
}

/// Parses an HTTP response head.
///
/// Returns bytes consumed (>= 0), `-2` for partial input, `-1` on failure.
/// Behaviorally identical to C's `phr_parse_response` (Milestone 3),
/// including progressive publication (partial status, scanned reason) and
/// zeroed outputs on every path.
///
/// # Safety
///
/// `buf`/`len` must describe a readable input buffer, `headers` must point
/// to `*num_headers` writable entries, and the output pointers must point to
/// writable storage. Null outputs (or a null input with `len != 0`) fail
/// closed with `-1` instead of faulting. A panic anywhere inside maps to
/// `-1` per the crate's pinned panic policy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_response(
    buf: *const c_char,
    len: usize,
    minor_version: *mut c_int,
    status: *mut c_int,
    msg: *mut *const c_char,
    msg_len: *mut usize,
    headers: *mut PhrHeader,
    num_headers: *mut usize,
    last_len: usize,
) -> c_int {
    // `match` (not `unwrap_or`) so the -1 mapping stays explicit;
    // `unwrap_or` would also trip the crate's `clippy::unwrap_used` deny.
    #[allow(clippy::manual_unwrap_or)]
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        parse_response_inner(
            buf,
            len,
            minor_version,
            status,
            msg,
            msg_len,
            headers,
            num_headers,
            last_len,
        )
    })) {
        Ok(r) => r,
        Err(_) => -1,
    }
}

/// `phr_parse_response` minus the unwind guard. Arity mirrors the C
/// signature 1:1 by design (same order, same meaning).
#[allow(clippy::too_many_arguments)]
fn parse_response_inner(
    buf: *const c_char,
    len: usize,
    minor_version: *mut c_int,
    status: *mut c_int,
    msg: *mut *const c_char,
    msg_len: *mut usize,
    headers: *mut PhrHeader,
    num_headers: *mut usize,
    last_len: usize,
) -> c_int {
    if minor_version.is_null()
        || status.is_null()
        || msg.is_null()
        || msg_len.is_null()
        || num_headers.is_null()
        || (len != 0 && buf.is_null())
    {
        return -1;
    }
    // SAFETY: outputs checked non-null above; zeroed first like C on entry.
    unsafe {
        *minor_version = -1;
        *status = 0;
        *msg = std::ptr::null();
        *msg_len = 0;
        let max = *num_headers;
        *num_headers = 0;
        if max != 0 && headers.is_null() {
            return -1;
        }
        let data: &[u8] = if len == 0 {
            &[]
        } else {
            // SAFETY: non-null (checked) + caller-guaranteed `len` bytes.
            std::slice::from_raw_parts(buf as *const u8, len)
        };
        let mut sink = RawSink {
            base: buf,
            slots: headers,
            cap: max,
        };
        let mut prog = crate::response::Progress::default();
        let r = match crate::response::parse_response(data, &mut sink, last_len, &mut prog) {
            Ok(consumed) => consumed as c_int,
            Err(crate::core::Error::Partial) => -2,
            Err(crate::core::Error::Malformed) => -1,
        };
        if let Some(v) = prog.version {
            *minor_version = v;
        }
        if let Some(v) = prog.status {
            *status = v;
        }
        if let Some((off, len)) = prog.msg {
            // SAFETY: offsets lie inside the input by construction.
            *msg = buf.add(off);
            *msg_len = len;
        }
        *num_headers = prog.headers;
        r
    }
}

/// Parses a standalone header block.
///
/// Returns bytes consumed (>= 0), `-2` for partial input, `-1` on failure.
/// Behaviorally identical to C's `phr_parse_headers` (Milestone 4): the
/// shared [`core::parse_headers`](crate::core::parse_headers) with the
/// `last_len` slowloris pre-check and the completed-header count published
/// on success and error alike.
///
/// # Safety
///
/// `buf`/`len` must describe a readable input buffer, `headers` must point
/// to `*num_headers` writable entries, and `num_headers` must point to
/// writable storage. Null `num_headers` (or a null input with `len != 0`)
/// fails closed with `-1` instead of faulting. A panic anywhere inside maps
/// to `-1` per the crate's pinned panic policy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_headers(
    buf: *const c_char,
    len: usize,
    headers: *mut PhrHeader,
    num_headers: *mut usize,
    last_len: usize,
) -> c_int {
    // `match` (not `unwrap_or`) so the -1 mapping stays explicit;
    // `unwrap_or` would also trip the crate's `clippy::unwrap_used` deny.
    #[allow(clippy::manual_unwrap_or)]
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        parse_headers_inner(buf, len, headers, num_headers, last_len)
    })) {
        Ok(r) => r,
        Err(_) => -1,
    }
}

/// `phr_parse_headers` minus the unwind guard.
fn parse_headers_inner(
    buf: *const c_char,
    len: usize,
    headers: *mut PhrHeader,
    num_headers: *mut usize,
    last_len: usize,
) -> c_int {
    if num_headers.is_null() || (len != 0 && buf.is_null()) {
        return -1;
    }
    // SAFETY: outputs checked non-null above; count zeroed first like C.
    unsafe {
        let max = *num_headers;
        *num_headers = 0;
        if max != 0 && headers.is_null() {
            return -1;
        }
        let data: &[u8] = if len == 0 {
            &[]
        } else {
            // SAFETY: non-null (checked) + caller-guaranteed `len` bytes.
            std::slice::from_raw_parts(buf as *const u8, len)
        };
        let mut sink = RawSink {
            base: buf,
            slots: headers,
            cap: max,
        };
        // Completed-header count, published on success AND error (C leaves
        // the completed prefix observable when parsing fails midway).
        let mut count = 0usize;
        // Slowloris pre-check comes before any parsing, exactly like C.
        if last_len != 0
            && let Err(e) = crate::core::is_complete(data, last_len)
        {
            return match e {
                crate::core::Error::Partial => -2,
                crate::core::Error::Malformed => -1,
            };
        }
        let r = match crate::core::parse_headers(data, 0, &mut sink, &mut count) {
            Ok(pos) => pos as c_int,
            Err(crate::core::Error::Partial) => -2,
            Err(crate::core::Error::Malformed) => -1,
        };
        *num_headers = count;
        r
    }
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
