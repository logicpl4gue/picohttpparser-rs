//! Chunked-decoder unit tests (Milestone 5): hand-verified vectors through the
//! real FFI entry point. Exhaustive behavioral equality (streaming splits,
//! both trailer modes, dirty decoder states) lives in the differential
//! harness (`scripts/diff_chunked.sh`); these tests pin the obvious contract
//! points in-repo so regressions fail `cargo test` directly.

use picohttpparser_rs::PhrChunkedDecoder;
use picohttpparser_rs::ffi::{phr_decode_chunked, phr_decode_chunked_is_in_data};

/// Caller-owned decoder, zero-filled per contract (trailer mode selectable).
fn decoder(trailer: bool) -> PhrChunkedDecoder {
    let mut d: PhrChunkedDecoder = unsafe { std::mem::zeroed() };
    d.consume_trailer = i8::from(trailer);
    d
}

/// Feed `input` through a caller-seeded decoder; returns (ret, decoded).
fn decode_with(mut dec: PhrChunkedDecoder, input: &[u8]) -> (isize, Vec<u8>) {
    let mut buf = input.to_vec();
    let mut len = buf.len();
    let ret = unsafe { phr_decode_chunked(&mut dec, buf.as_mut_ptr() as *mut _, &mut len) };
    buf.truncate(len);
    (ret, buf)
}

/// Overhead-ratio boundary, pinned both sides of the flip: with the
/// framer at exactly the 100 KiB line, a 13-byte data deficit fires (-1)
/// while a large one holds (-2). Mirrors C's `>= 100*1024 &&
/// read-overhead < read/4` expression exactly.
#[test]
fn overhead_ratio_boundary() {
    // Fires: overhead lands at 102403, deficit 13 < 25604.
    let mut dec = decoder(false);
    dec._total_overhead = 102400;
    dec._total_read = 102410;
    assert_eq!(decode_with(dec, b"5\r\nhel").0, -1);
    // Holds: same overhead, deficit 197603 not < 75001.
    let mut dec = decoder(false);
    dec._total_overhead = 102400;
    dec._total_read = 300000;
    assert_eq!(decode_with(dec, b"5\r\nhel").0, -2);
}

/// Feed `input` through a fresh decoder; returns (ret, decoded bytes).
/// SAFETY: the decoder is valid and the buffer is owned + writable.
fn decode_once(input: &[u8], trailer: bool) -> (isize, Vec<u8>) {
    let mut dec = decoder(trailer);
    let mut buf = input.to_vec();
    let mut len = buf.len();
    let ret = unsafe { phr_decode_chunked(&mut dec, buf.as_mut_ptr() as *mut _, &mut len) };
    buf.truncate(len);
    (ret, buf)
}

#[test]
fn single_and_multi_chunk() {
    // Without trailer consumption the final CRLF stays as 2 leftover octets.
    let (r, out) = decode_once(b"5\r\nhello\r\n0\r\n\r\n", false);
    assert_eq!(r, 2);
    assert_eq!(out, b"hello");
    // Two chunks concatenate; same trailing CRLF leftover.
    let (r, out) = decode_once(b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", false);
    assert_eq!(r, 2);
    assert_eq!(out, b"hello world");
    // ...but consumed when the trailer flag is set.
    let (r, out) = decode_once(b"5\r\nhello\r\n0\r\nX-T: v\r\n\r\n", true);
    assert_eq!(r, 0);
    assert_eq!(out, b"hello");
}

#[test]
fn empty_body_and_extensions() {
    let (r, out) = decode_once(b"0\r\n\r\n", false);
    assert_eq!(r, 2);
    assert_eq!(out, b"");
    // Extensions and blank wire space around the size are framing.
    let (r, out) = decode_once(b"5;ext=1 \t ;x\r\nhello\r\n0\r\n\r\n", false);
    assert_eq!(r, 2);
    assert_eq!(out, b"hello");
}

#[test]
fn errors_and_incomplete() {
    // Non-hex first char: malformed, nothing decoded.
    assert_eq!(decode_once(b"ZZZ\r\n", false).0, -1);
    // Junk right after the data: malformed.
    assert_eq!(decode_once(b"5\r\nhello\rX\r\n0\r\n\r\n", false).0, -1);
    // Truncated mid-data: incomplete.
    assert_eq!(decode_once(b"5\r\nhel", false).0, -2);
    // 17 hex digits overflow the 64-bit size slot: malformed.
    assert_eq!(decode_once(b"FFFFFFFFFFFFFFFFF\r\n", false).0, -1);
}

#[test]
fn streaming_split() {
    // Feed one logical body in two calls; the decoder carries the state.
    let mut dec = decoder(false);
    let mut b1 = b"5\r\nhel".to_vec();
    let mut l1 = b1.len();
    let r1 = unsafe { phr_decode_chunked(&mut dec, b1.as_mut_ptr() as *mut _, &mut l1) };
    assert_eq!(r1, -2);
    assert_eq!(unsafe { phr_decode_chunked_is_in_data(&dec) }, 1);
    let mut b2 = b"lo\r\n0\r\n\r\n".to_vec();
    let mut l2 = b2.len();
    let r2 = unsafe { phr_decode_chunked(&mut dec, b2.as_mut_ptr() as *mut _, &mut l2) };
    // Final CRLF left over, like the single-call shape.
    assert_eq!(r2, 2);
    // Call 1 banked "hel" (3 of 5); call 2's buffer holds only its own tail.
    assert_eq!((l1, &b2[..l2]), (3, b"lo".as_slice()));
}

#[test]
fn forged_state_fails_closed() {
    use picohttpparser_rs::ffi::phr_decode_chunked_is_in_data;
    // A forged `_state` outside 0–7 can never arise from the real machine
    // (transitions only write 0–7), but the seam must fail closed rather
    // than hang or abort like C does. Recorded in docs/divergences.md row 2.
    let mut dec = decoder(false);
    dec._state = 8;
    let mut buf = b"5\r\nhello\r\n0\r\n\r\n".to_vec();
    let mut len = buf.len();
    let before = dec._total_read;
    let r = unsafe { phr_decode_chunked(&mut dec, buf.as_mut_ptr() as *mut _, &mut len) };
    assert_eq!(r, -1);
    // Only the entry `_total_read` advance survives; nothing was copied.
    assert_eq!(dec._total_read, before.wrapping_add(buf.len() as u64));
    assert_eq!(unsafe { phr_decode_chunked_is_in_data(&dec) }, 0);
}

#[test]
fn null_corners_fail_closed() {
    use picohttpparser_rs::ffi::phr_decode_chunked_is_in_data;
    // Null buffer with zero length reads as empty (-2), like C which never
    // dereferences it then. Recorded in docs/divergences.md row 3.
    let mut dec = decoder(false);
    let mut zero = 0usize;
    let r = unsafe { phr_decode_chunked(&mut dec, std::ptr::null_mut(), &mut zero) };
    assert_eq!((r, zero), (-2, 0));
    // Null buffer with nonzero length still fails closed.
    let mut dec = decoder(false);
    let mut n = 4usize;
    assert_eq!(
        unsafe { phr_decode_chunked(&mut dec, std::ptr::null_mut(), &mut n) },
        -1
    );
    // Null query fails closed with 0 (C would segfault).
    assert_eq!(
        unsafe { phr_decode_chunked_is_in_data(std::ptr::null()) },
        0
    );
}

#[test]
fn overhead_bomb_rejected() {
    // ~150 KB of unterminated chunk-ext framing: incomplete, and the
    // 100 KiB / sub-quarter-data overhead rule fires instead of -2.
    let mut input = b"1;x=".to_vec();
    input.extend(std::iter::repeat_n(b'x', 150_000));
    assert_eq!(decode_once(&input, false).0, -1);
}
