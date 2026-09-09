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

/// Shared call epilogue (C's `Exit` label): compact the unprocessed tail
/// behind the decoded bytes, then apply the framing-overhead rule to
/// incomplete exits. `Complete` exits share this path with `err == None`.
///
/// Returns `(status, decoded_len)`; the seam always publishes `decoded_len`
/// (C sets `*bufsz` on every path) and maps the status to the C return.
fn finish(
    dec: &mut PhrChunkedDecoder,
    buf: &mut [u8],
    dst: usize,
    src: usize,
    bufsz: usize,
    err: Option<Error>,
) -> (Result<usize, Error>, usize) {
    // `src <= bufsz` on every exit path, and `dst <= src` is an invariant
    // (both advance together), so both ranges below are in-bounds and this
    // cannot panic on hostile input.
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

/// Decode one call's worth of chunked data in place. `dec` carries the
/// cross-call state (must be zero-filled before first use, per contract).
/// Returns `(status, decoded_len)`; `status` is `Ok(leftover)` when the
/// terminal zero chunk completed (trailers consumed or not, per
/// `consume_trailer`), `Err(Partial)` for "feed more bytes".
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

    loop {
        // See `is_in_data`: the cast is for unsigned-`char` platforms.
        #[allow(clippy::unnecessary_cast)]
        let state = dec._state as i8;
        match state {
            ST_SIZE => {
                loop {
                    if src == bufsz {
                        return finish(dec, buf, dst, src, bufsz, Some(Error::Partial));
                    }
                    // In-bounds: just proven `src < bufsz`.
                    match hex_val(buf[src]) {
                        None => {
                            if dec._hex_count == 0 {
                                return finish(dec, buf, dst, src, bufsz, Some(Error::Malformed));
                            }
                            // Only BWS, semicolon, or CRLF may follow the size.
                            match buf[src] {
                                b' ' | b'\t' | b';' | b'\n' | b'\r' => {}
                                _ => {
                                    return finish(
                                        dec,
                                        buf,
                                        dst,
                                        src,
                                        bufsz,
                                        Some(Error::Malformed),
                                    );
                                }
                            }
                            break;
                        }
                        Some(v) => {
                            // 16 hex digits max on 64-bit (8 on 32-bit),
                            // exactly like C's `sizeof(size_t) * 2`: the
                            // multiply below can never overflow.
                            if dec._hex_count as usize == size_of::<usize>() * 2 {
                                return finish(dec, buf, dst, src, bufsz, Some(Error::Malformed));
                            }
                            // Wrapping matches C in every profile (the gate above
                            // bounds legitimate accumulation; only a forged decoder
                            // could reach the wrap, where C wraps too).
                            dec.bytes_left_in_chunk = dec
                                .bytes_left_in_chunk
                                .wrapping_mul(16)
                                .wrapping_add(v as usize);
                            dec._hex_count += 1;
                        }
                    }
                    src += 1;
                }
                dec._hex_count = 0;
                dec._state = ST_EXT as c_char;
            }
            ST_EXT => {
                // Chunk extensions run to CR (RFC 7230 A.2: no line folding);
                // a bare LF is malformed. `src` sits on the size terminator.
                loop {
                    if src == bufsz {
                        return finish(dec, buf, dst, src, bufsz, Some(Error::Partial));
                    }
                    if buf[src] == b'\r' {
                        break;
                    } else if buf[src] == b'\n' {
                        return finish(dec, buf, dst, src, bufsz, Some(Error::Malformed));
                    }
                    src += 1;
                }
                src += 1;
                dec._state = ST_EXPECT_LF as c_char;
            }
            ST_EXPECT_LF => {
                if src == bufsz {
                    return finish(dec, buf, dst, src, bufsz, Some(Error::Partial));
                }
                if buf[src] != b'\n' {
                    return finish(dec, buf, dst, src, bufsz, Some(Error::Malformed));
                }
                src += 1;
                if dec.bytes_left_in_chunk == 0 {
                    if dec.consume_trailer != 0 {
                        dec._state = ST_TRAIL_HEAD as c_char;
                        continue;
                    } else {
                        return finish(dec, buf, dst, src, bufsz, None);
                    }
                }
                dec._state = ST_DATA as c_char;
            }
            ST_DATA => {
                // `src <= bufsz` on every path into DATA, so no underflow.
                let avail = bufsz - src;
                if avail < dec.bytes_left_in_chunk {
                    // Short data: bank what arrived, stay in DATA.
                    buf.copy_within(src..src + avail, dst);
                    src += avail;
                    dst += avail;
                    dec.bytes_left_in_chunk -= avail;
                    return finish(dec, buf, dst, src, bufsz, Some(Error::Partial));
                }
                let n = dec.bytes_left_in_chunk;
                // `dst <= src` invariant and `src + n <= bufsz` (just proven
                // `avail >= n`): in-bounds on both ends.
                buf.copy_within(src..src + n, dst);
                src += n;
                dst += n;
                dec.bytes_left_in_chunk = 0;
                dec._state = ST_DATA_CR as c_char;
            }
            ST_DATA_CR => {
                if src == bufsz {
                    return finish(dec, buf, dst, src, bufsz, Some(Error::Partial));
                }
                if buf[src] != b'\r' {
                    return finish(dec, buf, dst, src, bufsz, Some(Error::Malformed));
                }
                src += 1;
                dec._state = ST_DATA_LF as c_char;
            }
            ST_DATA_LF => {
                if src == bufsz {
                    return finish(dec, buf, dst, src, bufsz, Some(Error::Partial));
                }
                if buf[src] != b'\n' {
                    return finish(dec, buf, dst, src, bufsz, Some(Error::Malformed));
                }
                src += 1;
                dec._state = ST_SIZE as c_char;
            }
            ST_TRAIL_HEAD => {
                loop {
                    if src == bufsz {
                        return finish(dec, buf, dst, src, bufsz, Some(Error::Partial));
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
                    return finish(dec, buf, dst, src, bufsz, None);
                }
                dec._state = ST_TRAIL_MID as c_char;
            }
            ST_TRAIL_MID => {
                // Trailer content runs to LF (CRs included, like C).
                loop {
                    if src == bufsz {
                        return finish(dec, buf, dst, src, bufsz, Some(Error::Partial));
                    }
                    if buf[src] == b'\n' {
                        break;
                    }
                    src += 1;
                }
                src += 1;
                dec._state = ST_TRAIL_HEAD as c_char;
            }
            // Corrupt state (forged `_state`, unreachable from any real call
            // sequence): fail closed. C aborts here with asserts enabled and
            // *hangs* without them; returning an error is strictly safer and
            // is recorded in `docs/divergences.md`.
            _ => unreachable!("chunked decoder is corrupt"),
        }
    }
}
