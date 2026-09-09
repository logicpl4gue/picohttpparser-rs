//! Request-parser unit tests (Milestone 2): hand-verified vectors through the
//! real FFI entry point. Exhaustive behavioral equality (37k+ cases incl.
//! every prefix and header cap) lives in the differential harness
//! (`scripts/diff_request.sh`); these tests pin the obvious contract points
//! in-repo so regressions fail `cargo test` directly.

use picohttpparser_rs::PhrHeader;
use picohttpparser_rs::ffi::phr_parse_request;

/// Output bundle for one parse call. Header storage is caller-owned.
struct Out {
    ret: i32,
    method: Vec<u8>,
    path: Vec<u8>,
    ver: i32,
    headers: Vec<(Option<Vec<u8>>, Vec<u8>)>,
}

/// Parse `input` with room for `cap` headers (`last_len` streaming hint).
/// Null-ness is preserved: `None` method/path would mean NULL (never happens
/// past entry, but headers use `None` for continuation lines).
fn parse(input: &[u8], cap: usize, last_len: usize) -> Out {
    let mut headers = vec![
        PhrHeader {
            name: std::ptr::null(),
            name_len: usize::MAX,
            value: std::ptr::null(),
            value_len: usize::MAX,
        };
        cap
    ];
    let (mut method, mut method_len) = (std::ptr::null(), 0usize);
    let (mut path, mut path_len) = (std::ptr::null(), 0usize);
    let mut ver = 0;
    let mut n = cap;
    // SAFETY: all outputs point to valid writable storage; input is borrowed.
    let ret = unsafe {
        phr_parse_request(
            input.as_ptr() as *const _,
            input.len(),
            &mut method,
            &mut method_len,
            &mut path,
            &mut path_len,
            &mut ver,
            headers.as_mut_ptr(),
            &mut n,
            last_len,
        )
    };
    // NULL with len 0 (nothing scanned yet) reads as empty, like C's contract.
    let span = |p: *const i8, l: usize| {
        if p.is_null() {
            assert_eq!(l, 0);
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(p as *const u8, l).to_vec() }
        }
    };
    Out {
        ret,
        method: span(method, method_len),
        path: span(path, path_len),
        ver,
        headers: headers[..n]
            .iter()
            .map(|h| {
                let name = if h.name.is_null() {
                    assert_eq!(h.name_len, 0);
                    None
                } else {
                    Some(unsafe {
                        std::slice::from_raw_parts(h.name as *const u8, h.name_len).to_vec()
                    })
                };
                assert!(!h.value.is_null());
                let value = unsafe {
                    std::slice::from_raw_parts(h.value as *const u8, h.value_len).to_vec()
                };
                (name, value)
            })
            .collect(),
    }
}

#[test]
fn basic_get() {
    let o = parse(b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", 16, 0);
    assert_eq!(o.ret, 37);
    assert_eq!(o.method, b"GET");
    assert_eq!(o.path, b"/");
    assert_eq!(o.ver, 1);
    assert_eq!(
        o.headers,
        vec![(Some(b"Host".to_vec()), b"example.com".to_vec())]
    );
}

#[test]
fn lf_only_and_leading_blank() {
    let o = parse(b"\nGET /i HTTP/1.0\nH: v\n\n", 16, 0);
    assert_eq!(o.ret, 23); // 1 + 16 + 5 + 1
    assert_eq!(o.method, b"GET");
    assert_eq!(o.ver, 0);
    let o = parse(b"\r\nGET / HTTP/1.1\r\n\r\n", 16, 0);
    assert_eq!(o.ret, 20);
}

#[test]
fn partial_and_malformed() {
    // Strict prefixes of a valid message ask for more bytes.
    let full = b"GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    for k in [0, 1, 7] {
        let o = parse(&full[..k], 16, 0);
        assert_eq!(o.ret, -2, "prefix {k}");
        assert_eq!(o.ver, -1);
        assert!(o.headers.is_empty());
    }
    // The version scan completes at k=15, so from there the version is
    // published even though the message is still partial (C parity).
    for k in [15, 16, 24, 25] {
        let o = parse(&full[..k], 16, 0);
        assert_eq!(o.ret, -2, "prefix {k}");
        assert_eq!(o.ver, 1, "prefix {k}");
    }
    // Garbage can never become valid.
    for bad in [
        &b"GET / HTTP/2.0\r\n\r\n"[..],
        b"GET / HTTP/1.1 X\r\n\r\n",
        b"GET /a\tb HTTP/1.1\r\n\r\n",
        b"GET / HTTP/1.1\r\nNoColon\r\n\r\n",
        b"GET / HTTP/1.1\r\n: v\r\n\r\n",
    ] {
        assert_eq!(parse(bad, 16, 0).ret, -1);
    }
}

#[test]
fn progressive_outputs_on_error() {
    // C publishes each completed token before a later stage fails: method is
    // visible even though the header block overflows the cap.
    let o = parse(b"GET / HTTP/1.1\r\nH: v\r\n\r\n", 0, 0);
    assert_eq!(o.ret, -1);
    assert_eq!(o.method, b"GET");
    assert_eq!(o.path, b"/");
    assert_eq!(o.ver, 1);
    assert!(o.headers.is_empty());
    // Cap of 1 with 2 headers: first header + count survive the failure.
    let o = parse(b"GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\n\r\n", 1, 0);
    assert_eq!(o.ret, -1);
    assert_eq!(o.headers, vec![(Some(b"A".to_vec()), b"1".to_vec())]);
}

#[test]
fn streaming_last_len() {
    // Slowloris pre-check: incomplete head with a hint stays -2, and a
    // complete head behind a hint parses normally.
    let part = b"GET / HTTP/1.1\r\nHost: a";
    assert_eq!(parse(part, 16, 0).ret, -2);
    assert_eq!(parse(part, 16, 12).ret, -2);
    let full = b"GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    let o = parse(full, 16, part.len());
    assert_eq!(o.ret, 27); // 16 + 9 + 2
    assert_eq!(o.headers.len(), 1);
}

#[test]
fn value_trimming_and_continuation() {
    // 16 + 13 + 7 + 4 + 2 = 42 bytes consumed; ` cont` is a continuation
    // (NULL name) of X, `Y:` has an empty value.
    let o = parse(
        b"GET / HTTP/1.1\r\nX:   a b \t \r\n cont\r\nY:\r\n\r\n",
        16,
        0,
    );
    assert_eq!(o.ret, 42);
    assert_eq!(
        o.headers,
        vec![
            (Some(b"X".to_vec()), b"a b".to_vec()),
            // Continuation keeps its leading space: C skips SP/HTAB only
            // after the colon of a named header, never on continuations.
            (None, b" cont".to_vec()),
            (Some(b"Y".to_vec()), b"".to_vec()),
        ]
    );
}
