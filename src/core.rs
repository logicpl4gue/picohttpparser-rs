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

/// RFC 7230 `tchar`: exactly the set accepted by C's `token_char_map`
/// (`!#$%&'*+-.^_`|~`, digits, letters — and nothing else).
#[inline]
fn is_token_char(b: u8) -> bool {
    matches!(
        b,
        b'!' | b'#'
            | b'$'
            | b'%'
            | b'&'
            | b'\''
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
            | b'a'..=b'z'
    )
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
pub(crate) fn get_token_to_eol(buf: &[u8], pos: usize) -> Result<((usize, usize), usize), Error> {
    let start = pos;
    let mut p = pos;
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
