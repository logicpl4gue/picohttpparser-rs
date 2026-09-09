//! Standalone header-block unit tests (Milestone 4): hand-verified vectors
//! through the real FFI entry point. Exhaustive behavioral equality lives in
//! the differential harness (`scripts/diff_headers.sh`); these tests pin the
//! obvious contract points in-repo so regressions fail `cargo test` directly.

use picohttpparser_rs::PhrHeader;
use picohttpparser_rs::ffi::phr_parse_headers;

/// Output bundle for one parse call. Header storage is caller-owned.
struct Out {
    ret: i32,
    headers: Vec<(Option<Vec<u8>>, Vec<u8>)>,
}

/// Parse `input` with room for `cap` headers (`last_len` streaming hint).
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
    let mut n = cap;
    // SAFETY: all outputs point to valid writable storage; input is borrowed.
    let ret = unsafe {
        phr_parse_headers(
            input.as_ptr() as *const _,
            input.len(),
            headers.as_mut_ptr(),
            &mut n,
            last_len,
        )
    };
    Out {
        ret,
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
fn basic_block() {
    // 19 + 6 + 2 = 27 bytes.
    let o = parse(b"Host: example.com\r\nX: y\r\n\r\n", 16, 0);
    assert_eq!(o.ret, 27);
    assert_eq!(
        o.headers,
        vec![
            (Some(b"Host".to_vec()), b"example.com".to_vec()),
            (Some(b"X".to_vec()), b"y".to_vec()),
        ]
    );
}

#[test]
fn empty_and_lone_lf_blocks() {
    // A blank line ends the block immediately: 0 headers.
    let o = parse(b"\r\n", 16, 0);
    assert_eq!((o.ret, o.headers.len()), (2, 0));
    let o = parse(b"\n", 16, 0);
    assert_eq!((o.ret, o.headers.len()), (1, 0));
}

#[test]
fn partial_and_malformed() {
    // Strict prefixes ask for more bytes (`full` is 11 bytes, so k < 11).
    let full = b"Host: a\r\n\r\n";
    for k in [0, 3, 9, 10] {
        assert_eq!(parse(&full[..k], 16, 0).ret, -2, "prefix {k}");
    }
    // Garbage can never become valid.
    for bad in [
        &b"NoColon\r\n\r\n"[..],
        b": v\r\n\r\n",
        b" X: v\r\n\r\n",
        b"H: a\x01b\r\n\r\n",
    ] {
        assert_eq!(parse(bad, 16, 0).ret, -1);
    }
}

#[test]
fn caps_and_streaming() {
    // Cap of 1 with 2 headers: first header + count survive the failure.
    let o = parse(b"A: 1\r\nB: 2\r\n\r\n", 1, 0);
    assert_eq!(o.ret, -1);
    assert_eq!(o.headers, vec![(Some(b"A".to_vec()), b"1".to_vec())]);
    // Streaming hint with a complete block parses normally (6 + 2 = 8).
    let o = parse(b"H: v\r\n\r\n", 16, 5);
    assert_eq!(o.ret, 8);
    assert_eq!(o.headers.len(), 1);
    // Incomplete block with a hint stays -2.
    assert_eq!(parse(b"H: v\r\n", 16, 3).ret, -2);
}
