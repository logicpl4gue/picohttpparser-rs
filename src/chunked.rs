//! Chunked-transfer decoder (Milestone 5): `phr_decode_chunked` minus the FFI
//! seam, plus the state predicate behind `phr_decode_chunked_is_in_data`.
//!
//! A stateful, in-place, allocation-free rewrite of C's decoder. The caller
//! owns a [`PhrChunkedDecoder`](crate::ffi::PhrChunkedDecoder) across calls;
//! each call compacts decoded bytes to the front of the buffer and reports
//! how many were produced. Mirrors C's state machine transition-for-transition,
//! including the framing-overhead rule (incomplete framing >= 100 KiB with
//! data below a quarter of the read, i.e. overhead above three quarters).

use core::ffi::c_char;
use core::mem::size_of;

use crate::core::Error;
use crate::ffi::PhrChunkedDecoder;

/// C's anonymous state enum, values pinned (ABI-visible `_state`).
/// `IN_DATA` (3) is what `phr_decode_chunked_is_in_data` tests for.
pub(crate) const ST_SIZE: i8 = 0;
pub(crate) const ST_EXT: i8 = 1;
pub(crate) const ST_EXPECT_LF: i8 = 2;
pub(crate) const ST_DATA: i8 = 3;
pub(crate) const ST_DATA_CR: i8 = 4;
pub(crate) const ST_DATA_LF: i8 = 5;
pub(crate) const ST_TRAIL_HEAD: i8 = 6;
pub(crate) const ST_TRAIL_MID: i8 = 7;

/// Whether the decoder sits mid-chunk-data (C's `is_in_data` predicate).
pub(crate) fn is_in_data(dec: &PhrChunkedDecoder) -> bool {
    // The `as i8` is a no-op here (`c_char` is `i8` on this target) but keeps
    // compiling where C `char` is unsigned.
    #[allow(clippy::unnecessary_cast)]
    let state = dec._state as i8;
    state == ST_DATA
}

fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'A'..=b'F' => Some(b - b'A' + 0xa),
        b'a'..=b'f' => Some(b - b'a' + 0xa),
        _ => None,
    }
}

/// Decode one call's worth of chunked data in place. `dec` carries the
/// cross-call state (must be zero-filled before first use, per contract).
/// Returns `(status, decoded_len)`; `status` is `Ok(leftover)` when the
/// terminal zero chunk completed (trailers consumed or not, per
/// `consume_trailer`), `Err(Partial)` for "feed more bytes".
///
/// Hot state (`_state`, `bytes_left_in_chunk`, `_hex_count`) lives in locals
/// for the call and is written back once, in the single epilogue below —
/// mirroring C's fallthrough dispatch and shared `Exit`/`Complete` labels
/// instead of re-reading caller memory per transition.
pub(crate) fn decode_chunked(
    dec: &mut PhrChunkedDecoder,
    buf: &mut [u8],
) -> (Result<usize, Error>, usize) {
    let bufsz = buf.len();
    let mut dst = 0usize;
    let mut src = 0usize;
    // Totals only grow by `<= bufsz` per call; wrapping matches C's practice
    // and keeps the core panic-free no matter the decoder's history.
    dec._total_read = dec._total_read.wrapping_add(bufsz as u64);
    // See `is_in_data`: the casts are for unsigned-`char` platforms.
    #[allow(clippy::unnecessary_cast)]
    let mut st: i8 = dec._state as i8;
    let mut left = dec.bytes_left_in_chunk;
    let mut hex: c_char = dec._hex_count;
    // Every exit below records its outcome here, then breaks to the single
    // epilogue — so all state writeback happens in exactly one place.
    let err: Option<Error>;
    'out: loop {
        match st {
            ST_SIZE => {
                loop {
                    if src == bufsz {
                        err = Some(Error::Partial);
                        break 'out;
                    }
                    // In-bounds: just proven `src < bufsz`.
                    match hex_val(buf[src]) {
                        None => {
                            if hex == 0 {
                                err = Some(Error::Malformed);
                                break 'out;
                            }
                            // Only BWS, semicolon, or CRLF may follow the size.
                            match buf[src] {
                                b' ' | b'\t' | b';' | b'\n' | b'\r' => {}
                                _ => {
                                    err = Some(Error::Malformed);
                                    break 'out;
                                }
                            }
                            break;
                        }
                        Some(v) => {
                            // 16 hex digits max on 64-bit (8 on 32-bit),
                            // exactly like C's `sizeof(size_t) * 2`: the
                            // multiply below can never overflow.
                            if hex as usize == size_of::<usize>() * 2 {
                                err = Some(Error::Malformed);
                                break 'out;
                            }
                            // Wrapping matches C in every profile (the gate above
                            // bounds legitimate accumulation; only a forged decoder
                            // could reach the wrap, where C wraps too).
                            left = left.wrapping_mul(16).wrapping_add(v as usize);
                            hex += 1;
                        }
                    }
                    src += 1;
                }
                hex = 0;
                st = ST_EXT;
            }
            ST_EXT => {
                // Chunk extensions run to CR (RFC 7230 A.2: no line folding);
                // a bare LF is malformed. `src` sits on the size terminator.
                loop {
                    if src == bufsz {
                        err = Some(Error::Partial);
                        break 'out;
                    }
                    if buf[src] == b'\r' {
                        break;
                    } else if buf[src] == b'\n' {
                        err = Some(Error::Malformed);
                        break 'out;
                    }
                    src += 1;
                }
                src += 1;
                st = ST_EXPECT_LF;
            }
            ST_EXPECT_LF => {
                if src == bufsz {
                    err = Some(Error::Partial);
                    break 'out;
                }
                if buf[src] != b'\n' {
                    err = Some(Error::Malformed);
                    break 'out;
                }
                src += 1;
                if left == 0 {
                    if dec.consume_trailer != 0 {
                        st = ST_TRAIL_HEAD;
                        continue;
                    } else {
                        err = None;
                        break 'out;
                    }
                }
                st = ST_DATA;
            }
            ST_DATA => {
                // `src <= bufsz` on every path into DATA, so no underflow.
                let avail = bufsz - src;
                if avail < left {
                    // Short data: bank what arrived, stay in DATA. The guard
                    // skips a full self-copy when a call starts mid-chunk
                    // (dst == src == 0) — C does the same check.
                    if dst != src {
                        buf.copy_within(src..src + avail, dst);
                    }
                    src += avail;
                    dst += avail;
                    left -= avail;
                    err = Some(Error::Partial);
                    break 'out;
                }
                let n = left;
                // `dst <= src` invariant and `src + n <= bufsz` (just proven
                // `avail >= n`): in-bounds on both ends.
                if dst != src {
                    buf.copy_within(src..src + n, dst);
                }
                src += n;
                dst += n;
                left = 0;
                st = ST_DATA_CR;
            }
            ST_DATA_CR => {
                if src == bufsz {
                    err = Some(Error::Partial);
                    break 'out;
                }
                if buf[src] != b'\r' {
                    err = Some(Error::Malformed);
                    break 'out;
                }
                src += 1;
                st = ST_DATA_LF;
            }
            ST_DATA_LF => {
                if src == bufsz {
                    err = Some(Error::Partial);
                    break 'out;
                }
                if buf[src] != b'\n' {
                    err = Some(Error::Malformed);
                    break 'out;
                }
                src += 1;
                st = ST_SIZE;
            }
            ST_TRAIL_HEAD => {
                loop {
                    if src == bufsz {
                        err = Some(Error::Partial);
                        break 'out;
                    }
                    if buf[src] != b'\r' {
                        break;
                    }
                    src += 1;
                }
                // An empty trailer line (immediate LF) ends the body; the LF
                // is consumed (C's post-increment read).
                let b = buf[src];
                src += 1;
                if b == b'\n' {
                    err = None;
                    break 'out;
                }
                st = ST_TRAIL_MID;
            }
            ST_TRAIL_MID => {
                // Trailer content runs to LF (CRs included, like C).
                loop {
                    if src == bufsz {
                        err = Some(Error::Partial);
                        break 'out;
                    }
                    if buf[src] == b'\n' {
                        break;
                    }
                    src += 1;
                }
                src += 1;
                st = ST_TRAIL_HEAD;
            }
            // Corrupt state (forged `_state`, unreachable from any real call
            // sequence): fail closed. C aborts here with asserts enabled and
            // *hangs* without them; returning an error is strictly safer and
            // is recorded in `docs/divergences.md`.
            _ => unreachable!("chunked decoder is corrupt"),
        }
    }
    // Single epilogue (C's `Exit`/`Complete` labels): publish hot state,
    // compact the unprocessed tail, apply the overhead rule to incomplete
    // exits. `src <= bufsz` on every exit path, and `dst <= src` is an
    // invariant (both advance together), so the ranges below are in-bounds
    // and this cannot panic on hostile input.
    #[allow(clippy::unnecessary_cast)]
    {
        dec._state = st as c_char;
    }
    dec.bytes_left_in_chunk = left;
    dec._hex_count = hex;
    if dst != src {
        let n = bufsz - src;
        buf.copy_within(src..src + n, dst);
    }
    match err {
        // Complete: leftover octets start where the framing ended.
        None => (Ok(bufsz - src), dst),
        Some(Error::Malformed) => (Err(Error::Malformed), dst),
        Some(Error::Partial) => {
            dec._total_overhead = dec
                ._total_overhead
                .wrapping_add(bufsz.wrapping_sub(dst) as u64);
            // Incomplete framing >= 100 KiB with data below a quarter of the
            // read (overhead above three quarters): hostile input.
            if dec._total_overhead >= 100 * 1024
                && dec._total_read.wrapping_sub(dec._total_overhead) < dec._total_read / 4
            {
                (Err(Error::Malformed), dst)
            } else {
                (Err(Error::Partial), dst)
            }
        }
    }
}
