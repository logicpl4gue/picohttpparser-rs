//! Safe, allocation-free parsing core shared by every entry point.
//!
//! Byte slices and offsets in, subslices and sink writes out. No allocation,
//! no `unsafe`, no panics on hostile input: every read is bounds-checked
//! (`.get()`), every length check uses `saturating_sub`, and wrapping
//! arithmetic is explicit. Length-verified slicing (`buf[pos..pos+n]` after
//! proving `n` bytes exist) is panic-free on hostile input by construction —
//! a violated proof would be a logic bug, which dev-profile asserts catch.
//!
//! Mirrors the scalar logic of `reference/picohttpparser.c` at pin
//! `f4d94b48`. (Upstream also has an SSE4.2 fast path, but our pinned
//! toolchain build uses the scalar path, and the two are semantically
//! identical: the SIMD scan only skips bytes the scalar loop would accept.)

/// Malformed vs incomplete, mirroring C's `-1` / `-2`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Error {
    /// Input is a strict prefix of a valid message: feed more bytes.
    Partial,
    /// Input can never become valid no matter how it is extended.
    Malformed,
}

/// Destination for parsed headers.
///
/// The core never materializes pointers: it reports `(offset, len)` pairs
/// into the input buffer, and the implementor (the FFI seam) turns them into
/// output pointers. One implementation exists; `response`/`headers` (M3/M4)
/// reuse this trait with the same sink.
///
/// Two-phase on purpose: C writes the header *name* into the caller array
/// before scanning the value, so a mid-value failure still leaves the name
/// behind in slot `count`. Mirroring the phase split keeps even that
/// out-of-contract slot byte-identical.
pub(crate) trait HeaderSink {
    /// Caller-supplied header capacity (`*num_headers` on entry).
    fn capacity(&self) -> usize;
    /// Record header `i`'s name (`None` marks a continuation line).
    fn set_name(&mut self, i: usize, name: Option<(usize, usize)>);
    /// Record header `i`'s value (trailing space already trimmed).
    fn set_value(&mut self, i: usize, value: (usize, usize));
}

/// RFC 7230 `tchar` table, mirroring C's `token_char_map` load+test exactly.
/// (A/B-tested 2026-09-09; keep or revert per results/bench-compare.json.)
static TOKEN_CHAR: [u8; 256] = build_token_table();

const fn build_token_table() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut b = 0u32;
    while b < 256 {
        // tchar: ! # $ % & ' * + - . ^ _ ` | ~ DIGIT ALPHA (RFC 7230 3.2.6).
        // The quote is 0x27 (avoids quote-escaping noise in this builder).
        t[b as usize] = match b as u8 {
            b'!'
            | b'#'
            | b'$'
            | b'%'
            | b'&'
            | 0x27
            | b'*'
            | b'+'
            | b'-'
            | b'.'
            | b'^'
            | b'_'
            | b'`'
            | b'|'
            | b'~'
            | b'0'..=b'9'
            | b'A'..=b'Z'
            | b'a'..=b'z' => 1,
            _ => 0,
        };
        b += 1;
    }
    t
}

/// RFC 7230 `tchar`: exactly the set accepted by C's `token_char_map`
/// (`!#$%&'*+-.^_`|~`, digits, letters — and nothing else).
#[inline]
fn is_token_char(b: u8) -> bool {
    // Index provably < 256 (`u8` range), so no bounds check survives.
    TOKEN_CHAR[b as usize] != 0
}

/// C's `IS_PRINTABLE_ASCII`: `(c - 0x20) < 0x5F` in wrapping arithmetic.
#[inline]
fn is_printable_ascii(b: u8) -> bool {
    b.wrapping_sub(0x20) < 0x5f
}

/// Expect one exact byte (C's `EXPECT_CHAR`): missing is [`Error::Partial`],
/// different is [`Error::Malformed`].
fn expect_byte(buf: &[u8], pos: &mut usize, ch: u8) -> Result<(), Error> {
    match buf.get(*pos) {
        None => Err(Error::Partial),
        Some(&b) if b == ch => {
            *pos += 1;
            Ok(())
        }
        Some(_) => Err(Error::Malformed),
    }
}

/// Slowloris pre-check (C's `is_complete`): succeeds once two line breaks
/// have been seen. Scans from `last_len - 3`, exactly like C.
pub(crate) fn is_complete(buf: &[u8], last_len: usize) -> Result<(), Error> {
    let mut pos = last_len.saturating_sub(3);
    let mut ret_cnt = 0;
    loop {
        let b = *buf.get(pos).ok_or(Error::Partial)?;
        if b == b'\r' {
            pos += 1;
            match buf.get(pos) {
                None => return Err(Error::Partial),
                Some(b'\n') => {
                    pos += 1;
                    ret_cnt += 1;
                }
                Some(_) => return Err(Error::Malformed),
            }
        } else if b == b'\n' {
            pos += 1;
            ret_cnt += 1;
        } else {
            pos += 1;
            ret_cnt = 0;
        }
        if ret_cnt == 2 {
            return Ok(());
        }
    }
}

/// Token terminated by `next_char` (C's `parse_token`, scalar semantics).
/// Returns `((offset, len), pos_of_terminator)`. An empty token is legal here;
/// callers reject it where C does.
pub(crate) fn parse_token(
    buf: &[u8],
    pos: usize,
    next_char: u8,
) -> Result<((usize, usize), usize), Error> {
    let start = pos;
    let mut p = pos;
    loop {
        let b = *buf.get(p).ok_or(Error::Partial)?;
        if b == next_char {
            break;
        }
        if !is_token_char(b) {
            return Err(Error::Malformed);
        }
        p += 1;
    }
    Ok(((start, p - start), p))
}

/// Header value up to the end of line (C's `get_token_to_eol`, scalar
/// semantics). HTAB and bytes >= 0x80 are legal in values; other controls
/// and DEL end the token and must be followed by CRLF/LF. Returns
/// `((offset, len_without_line_break), pos_past_line_break)`.
/// SWAR constants for [`prescan_printable`]: one per lane.
const SWAR_LO: u64 = 0x0101_0101_0101_0101;
const SWAR_HI: u64 = 0x8080_8080_8080_8080;

/// True iff any of the 8 lanes holds a byte outside `0x20..=0x7E`.
///
/// Pure filter: false positives (clean block flagged) only cost a fallback
/// to the exact loop; false negatives would skip real decisions, so the
/// property `clean-result ⟹ every byte in range` is pinned by
/// `swar_filter_sound` below over all 65,536 two-byte combinations plus
/// carry-cascade shapes (`0xFF`/`0xFE` runs adjacent to boundary bytes).
#[inline]
fn has_outside_printable(x: u64) -> bool {
    // hasless(x, 0x20): lanes holding bytes < 0x20 (& and | cannot
    // overflow, so plain operators are correct here).
    let less = (x.wrapping_sub(SWAR_LO.wrapping_mul(0x20)) & !x) & SWAR_HI;
    // Lanes holding bytes > 0x7E. Carry from a saturated low lane can only
    // *set* flags (flagging a clean neighbor), never clear a real one, so
    // this side is sound in the filter direction by construction.
    let more = (x.wrapping_add(SWAR_LO.wrapping_mul(127 - 0x7e)) | x) & SWAR_HI;
    less | more != 0
}

pub(crate) fn get_token_to_eol(buf: &[u8], pos: usize) -> Result<((usize, usize), usize), Error> {
    let start = pos;
    let mut p = pos;
    // NOTE (A/B-tested 2026-09-09): a plain 8-at-a-time bounds-amortizing
    // batch measured ~6% SLOWER (ratio 1.73 -> 1.86) — LLVM already unrolls
    // the plain loop, so amortization alone only added scaffolding. The SWAR
    // prescan below is a different mechanism (fewer classification branches
    // per byte, not fewer bounds checks); keep or revert per
    // results/bench-compare.json, never on theory.
    //
    // Prescan: skip 8-byte blocks proven free of line breaks and controls.
    // Filter-only: any block outside `0x20..=0x7E` (HTAB, CR/LF, DEL,
    // controls, bytes >= 0x80 — all legal-or-decided in values) falls through
    // to the exact byte loop, which makes the real decision. Windowed with
    // `get`, so no read ever passes `len` (guard-page safe).
    while let Some(w) = buf.get(p..p + 8) {
        let Ok(a) = <&[u8; 8]>::try_from(w) else {
            break; // unreachable: the range above is exactly 8 long
        };
        if has_outside_printable(u64::from_le_bytes(*a)) {
            break;
        }
        p += 8;
    }
    loop {
        let b = *buf.get(p).ok_or(Error::Partial)?;
        if !is_printable_ascii(b) && ((b < 0x20 && b != b'\t') || b == 0x7f) {
            if b == b'\r' {
                return match buf.get(p + 1) {
                    None => Err(Error::Partial),
                    Some(b'\n') => Ok(((start, p - start), p + 2)),
                    Some(_) => Err(Error::Malformed),
                };
            } else if b == b'\n' {
                return Ok(((start, p - start), p + 1));
            } else {
                return Err(Error::Malformed);
            }
        }
        p += 1;
    }
}

/// `HTTP/1.<digit>` (C's `parse_http_version`). Needs 9 bytes to attempt,
/// consumes 8, returns the minor version.
pub(crate) fn parse_http_version(buf: &[u8], pos: usize) -> Result<(i32, usize), Error> {
    if buf.len().saturating_sub(pos) < 9 {
        return Err(Error::Partial);
    }
    // Length proven above: this window cannot panic on hostile input.
    let w = &buf[pos..pos + 9];
    if &w[..7] != b"HTTP/1." || !w[7].is_ascii_digit() {
        return Err(Error::Malformed);
    }
    Ok(((w[7] - b'0') as i32, pos + 8))
}

/// Drop trailing SP/HTAB from `buf[start..start+len]`; returns the trimmed length.
fn rtrim(buf: &[u8], start: usize, len: usize) -> usize {
    let mut end = start + len;
    // `end - 1` is in-bounds: `end > start` implies `end - 1 >= start`, and
    // every value range comes from `get_token_to_eol`, hence lies in `buf`.
    while end > start && (buf[end - 1] == b' ' || buf[end - 1] == b'\t') {
        end -= 1;
    }
    end - start
}

/// Header block (C's `parse_headers`). Writes headers progressively into
/// `sink` exactly like C fills the caller array, stops at the blank line,
/// enforces capacity *before* parsing each header. Returns the position past
/// the blank line.
///
/// `*count` tracks completed headers *progressively* (C increments
/// `*num_headers` per completed header): on error it still holds the
/// completed prefix, so error paths leave identical observable state.
pub(crate) fn parse_headers<S: HeaderSink>(
    buf: &[u8],
    mut pos: usize,
    sink: &mut S,
    count: &mut usize,
) -> Result<usize, Error> {
    let max = sink.capacity();
    let mut n = 0usize;
    loop {
        let b = *buf.get(pos).ok_or(Error::Partial)?;
        if b == b'\r' {
            pos += 1;
            expect_byte(buf, &mut pos, b'\n')?;
            break;
        } else if b == b'\n' {
            pos += 1;
            break;
        }
        if n == max {
            return Err(Error::Malformed);
        }
        if n != 0 && (b == b' ' || b == b'\t') {
            // Continuation of the previous header's value.
            sink.set_name(n, None);
            let ((vs, vl), p) = get_token_to_eol(buf, pos)?;
            pos = p;
            sink.set_value(n, (vs, rtrim(buf, vs, vl)));
        } else {
            let ((ns, nl), p) = parse_token(buf, pos, b':')?;
            // Name hits the array before the empty check: C publishes the
            // (empty) name, then fails - so must we.
            sink.set_name(n, Some((ns, nl)));
            if nl == 0 {
                return Err(Error::Malformed);
            }
            pos = p + 1; // skip ':'
            loop {
                match buf.get(pos) {
                    Some(b' ') | Some(b'\t') => pos += 1,
                    Some(_) => break,
                    None => return Err(Error::Partial),
                }
            }
            let ((vs, vl), p) = get_token_to_eol(buf, pos)?;
            pos = p;
            sink.set_value(n, (vs, rtrim(buf, vs, vl)));
        }
        n += 1;
        *count = n;
    }
    Ok(pos)
}

#[cfg(test)]
mod tests {
    use super::has_outside_printable;

    fn clean(b: u8) -> bool {
        (0x20..=0x7e).contains(&b)
    }

    /// Filter soundness: a "clean" verdict must never skip a byte outside
    /// `0x20..=0x7E`. Exhaustive over all two-byte pairs in lanes 0–1 (SWAR
    /// carry flows low→high only, so pairs cover every interaction), plus
    /// full-width cascade shapes. False *positives* are legal (fallback).
    #[test]
    fn swar_filter_sound() {
        for a in 0..=255u32 {
            for b in 0..=255u32 {
                let x = (a | (b << 8)) as u64; // lanes 2-7 are 0x00 (dirty)
                if a == 0 && b == 0 {
                    continue; // lanes 0-1 are 0x00: dirty by rule, skip assert
                }
                if !has_outside_printable(x) {
                    assert!(clean(a as u8) && clean(b as u8), "a={a} b={b}");
                }
            }
        }
        // Full-width carry cascades: saturated lanes beside boundary bytes.
        for lanes in [
            [0xffu8; 8],
            [0xfeu8; 8],
            [0xff, 0x7e, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41],
            [0x7e, 0xff, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41],
            [0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0xff, 0x7e],
            [0x20, 0x7e, 0x09, 0x7f, 0x0d, 0x0a, 0x80, 0xff],
        ] {
            let x = u64::from_le_bytes(lanes);
            if !has_outside_printable(x) {
                assert!(lanes.iter().all(|&b| clean(b)), "{lanes:?}");
            }
        }
        // All-clean blocks must skip.
        assert!(!has_outside_printable(u64::from_le_bytes([0x41; 8])));
        assert!(!has_outside_printable(u64::from_le_bytes(*b"GET /HT ")));
    }
}
