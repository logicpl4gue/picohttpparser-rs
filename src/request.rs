//! Request-line parser (Milestone 2): `phr_parse_request` minus the FFI seam.
//!
//! Operates on a byte slice, borrows method/path out of it, and streams
//! headers into the caller's [`HeaderSink`](crate::core::HeaderSink).
//! Mirrors C's `parse_request`.

use crate::core::{Error, HeaderSink, is_complete, parse_headers, parse_http_version, parse_token};

/// Progressively published outputs, mirroring C exactly: C writes
/// `*method`/`*path`/`*minor_version` the moment each token scan completes
/// (even empty ones — only a *failed* scan leaves NULL/0/-1), and counts
/// completed headers as it goes. The FFI seam publishes this struct on
/// success AND error alike, so error paths leave identical memory behind.
#[derive(Default)]
pub(crate) struct Progress {
    /// `None` until the method token scan completes (possibly empty).
    pub method: Option<(usize, usize)>,
    /// `None` until the path token scan completes (possibly empty).
    pub path: Option<(usize, usize)>,
    /// `None` until the version digit parses.
    pub version: Option<i32>,
    /// Completed headers (prefix survives errors).
    pub headers: usize,
}

/// Request target (C's `ADVANCE_TOKEN`): runs to the next SP; HTAB, controls
/// and DEL are malformed; bytes >= 0x80 pass through. Returns
/// `((offset, len), pos_of_space)`.
fn parse_path(buf: &[u8], pos: usize) -> Result<((usize, usize), usize), Error> {
    let start = pos;
    let mut p = pos;
    loop {
        let b = *buf.get(p).ok_or(Error::Partial)?;
        if b == b' ' {
            break;
        }
        // Printable ASCII passes; so do bytes >= 0x80. Controls and DEL fail.
        // (Folded to match C's `findchar_fast` + slow loop pair exactly.)
        let printable = b.wrapping_sub(0x20) < 0x5f;
        if !printable && (b < 0x20 || b == 0x7f) {
            return Err(Error::Malformed);
        }
        p += 1;
    }
    Ok(((start, p - start), p))
}

/// Skip ASCII spaces after a token (C skips `' '` only — never HTAB).
fn skip_spaces(buf: &[u8], mut pos: usize) -> Result<usize, Error> {
    pos += 1; // move past the delimiter the caller stopped at
    loop {
        match buf.get(pos) {
            None => return Err(Error::Partial),
            Some(b' ') => pos += 1,
            Some(_) => return Ok(pos),
        }
    }
}

/// Full request parse. Returns `(head, bytes_consumed)`.
///
/// `last_len` is the streaming hint: nonzero triggers C's slowloris
/// pre-check (`is_complete` from `last_len - 3`).
/// Full request parse. Returns bytes consumed. Every completed stage is
/// recorded into `prog` before the next (fallible) stage runs, so callers
/// observe C-identical state on all paths.
pub(crate) fn parse_request<S: HeaderSink>(
    buf: &[u8],
    sink: &mut S,
    last_len: usize,
    prog: &mut Progress,
) -> Result<usize, Error> {
    // Slowloris pre-check comes before any parsing, exactly like C.
    if last_len != 0 {
        is_complete(buf, last_len)?;
    }
    let mut pos = 0;

    // Skip one leading empty line (some clients add CRLF after POST content).
    match buf.get(pos) {
        None => return Err(Error::Partial),
        Some(b'\r') => {
            pos += 1;
            match buf.get(pos) {
                None => return Err(Error::Partial),
                Some(b'\n') => pos += 1,
                Some(_) => return Err(Error::Malformed),
            }
        }
        Some(b'\n') => pos += 1,
        Some(_) => {}
    }

    let ((ms, ml), p) = parse_token(buf, pos, b' ')?;
    prog.method = Some((ms, ml));
    let pos = skip_spaces(buf, p)?;
    let ((ps, pl), p) = parse_path(buf, pos)?;
    prog.path = Some((ps, pl));
    let pos = skip_spaces(buf, p)?;
    if ml == 0 || pl == 0 {
        return Err(Error::Malformed);
    }
    let (minor_version, mut pos) = parse_http_version(buf, pos)?;
    prog.version = Some(minor_version);
    match buf.get(pos) {
        None => return Err(Error::Partial),
        Some(b'\r') => {
            pos += 1;
            match buf.get(pos) {
                None => return Err(Error::Partial),
                Some(b'\n') => pos += 1,
                Some(_) => return Err(Error::Malformed),
            }
        }
        Some(b'\n') => pos += 1,
        Some(_) => return Err(Error::Malformed),
    }

    let pos = parse_headers(buf, pos, sink, &mut prog.headers)?;
    Ok(pos)
}
