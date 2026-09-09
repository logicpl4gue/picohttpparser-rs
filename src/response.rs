//! Response-line parser (Milestone 3): `phr_parse_response` minus the FFI seam.
//!
//! Operates on a byte slice, borrows the reason phrase out of it, and streams
//! headers into the caller's [`HeaderSink`](crate::core::HeaderSink).
//! Mirrors C's `parse_response`.

use crate::core::{
    Error, HeaderSink, get_token_to_eol, is_complete, parse_headers, parse_http_version,
};

/// Progressively published outputs, mirroring C exactly: `*minor_version` is
/// written when the version digit parses, `*status` after *each* status digit
/// (so a mid-code failure still leaves the partial value behind), and
/// `*msg`/`*msg_len` when the reason line scans — before the leading-space
/// validation that may still reject it.
#[derive(Default)]
pub(crate) struct Progress {
    /// `None` until the version digit parses.
    pub version: Option<i32>,
    /// Partial status value (`100*d1`, then `+10*d2`, then `+d3`).
    pub status: Option<i32>,
    /// `None` until the reason line scans (possibly empty).
    pub msg: Option<(usize, usize)>,
    /// Completed headers (prefix survives errors).
    pub headers: usize,
}

/// Three-digit status code (C's `PARSE_INT_3`). Needs 4 bytes to attempt
/// (3 digits + 1 lookahead, exactly like C). Publishes the partial value
/// after every digit: `"12X"` leaves `120` behind on failure.
fn parse_status(buf: &[u8], pos: usize, prog: &mut Progress) -> Result<usize, Error> {
    if buf.len().saturating_sub(pos) < 4 {
        return Err(Error::Partial);
    }
    // Four bytes proven above: `pos..pos+3` cannot panic on hostile input.
    let mut v = 0i32;
    for (i, mul) in [100, 10, 1].iter().enumerate() {
        let d = buf[pos + i];
        if !d.is_ascii_digit() {
            // C also advances past the bad byte, unobservable (fails right away).
            return Err(Error::Malformed);
        }
        v += mul * (d - b'0') as i32;
        prog.status = Some(v);
    }
    Ok(pos + 3)
}

/// Full response parse. Returns bytes consumed. Every completed stage is
/// recorded into `prog` before the next (fallible) stage runs.
pub(crate) fn parse_response<S: HeaderSink>(
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

    let (minor_version, p) = parse_http_version(buf, pos)?;
    prog.version = Some(minor_version);
    pos = p;
    // Exactly one space is required first (C rejects HTAB here); then all
    // spaces are skipped. `pos` is provably in-bounds (version consumed 8 of
    // at least 9 bytes), but `get` keeps it total anyway.
    match buf.get(pos) {
        Some(b' ') => {}
        Some(_) => return Err(Error::Malformed),
        None => return Err(Error::Partial),
    }
    loop {
        pos += 1;
        match buf.get(pos) {
            None => return Err(Error::Partial),
            Some(b' ') => {}
            Some(_) => break,
        }
    }

    pos = parse_status(buf, pos, prog)?;
    let ((ms, ml), p) = get_token_to_eol(buf, pos)?;
    // Published before validation: C assigns *msg/*msg_len, *then* checks.
    prog.msg = Some((ms, ml));
    if ml != 0 {
        // `ms < ms + ml <= len`: indexing cannot panic on hostile input.
        if buf[ms] == b' ' {
            // Strip ALL leading spaces (HTAB is not stripped — and a
            // leading HTAB fails below instead).
            let mut s = ms;
            let mut l = ml;
            while l > 0 && buf[s] == b' ' {
                s += 1;
                l -= 1;
            }
            prog.msg = Some((s, l));
        } else {
            // Garbage right after the status code (msg already published).
            return Err(Error::Malformed);
        }
    }

    let pos = parse_headers(buf, p, sink, &mut prog.headers)?;
    Ok(pos)
}
