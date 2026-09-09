//! Response-parser unit tests (Milestone 3): hand-verified vectors through the
//! real FFI entry point. Exhaustive behavioral equality lives in the
//! differential harness (`scripts/diff_response.sh`); these tests pin the
//! obvious contract points in-repo so regressions fail `cargo test` directly.

use picohttpparser_rs::PhrHeader;
use picohttpparser_rs::ffi::phr_parse_response;

/// Output bundle for one parse call. Header storage is caller-owned.
struct Out {
    ret: i32,
    ver: i32,
    status: i32,
    msg: Vec<u8>,
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
    let (mut ver, mut status) = (0, 0);
    let (mut msg, mut msg_len) = (std::ptr::null(), 0usize);
    let mut n = cap;
    // SAFETY: all outputs point to valid writable storage; input is borrowed.
    let ret = unsafe {
        phr_parse_response(
            input.as_ptr() as *const _,
            input.len(),
            &mut ver,
            &mut status,
            &mut msg,
            &mut msg_len,
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
        ver,
        status,
        msg: span(msg, msg_len),
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
fn basic_response() {
    // 17 + 19 + 2 = 38 bytes.
    let o = parse(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n", 16, 0);
    assert_eq!(o.ret, 38);
    assert_eq!(o.ver, 1);
    assert_eq!(o.status, 200);
    assert_eq!(o.msg, b"OK");
    assert_eq!(
        o.headers,
        vec![(Some(b"Content-Length".to_vec()), b"3".to_vec())]
    );
}

#[test]
fn empty_reason_and_multispace() {
    // No reason phrase: msg is empty (non-null: points at the line break).
    let o = parse(b"HTTP/1.1 404\r\n\r\n", 16, 0);
    assert_eq!(o.ret, 16); // 8 + 1 + 3 + 2 + 2
    assert_eq!(o.status, 404);
    assert!(o.msg.is_empty());
    // Extra spaces skipped; leading spaces of the reason stripped, trailing
    // spaces kept (8 + 3 + 3 + 3 + 2 + 3 + 2 + 2 = 26).
    let o = parse(b"HTTP/1.1   200   OK   \r\n\r\n", 16, 0);
    assert_eq!(o.ret, 26);
    assert_eq!(o.status, 200);
    assert_eq!(o.msg, b"OK   ");
}

#[test]
fn partial_status_published() {
    // `"HTTP/1.1 2X"`: version done, first digit (200) published, then -1.
    let o = parse(b"HTTP/1.1 2X0 OK\r\n\r\n", 16, 0);
    assert_eq!(o.ret, -1);
    assert_eq!(o.ver, 1);
    assert_eq!(o.status, 200);
    assert!(o.msg.is_empty());
    // Truncated mid-code asks for more bytes, nothing published yet.
    let o = parse(b"HTTP/1.1 20", 16, 0);
    assert_eq!(o.ret, -2);
    assert_eq!(o.status, 0);
}

#[test]
fn garbage_after_status() {
    // Reason must start with a space (after strip): `*OK` fails, but the
    // scanned reason stays published.
    let o = parse(b"HTTP/1.1 200*OK\r\n\r\n", 16, 0);
    assert_eq!(o.ret, -1);
    assert_eq!(o.status, 200);
    assert_eq!(o.msg, b"*OK");
    // HTAB is not a space here either.
    assert_eq!(parse(b"HTTP/1.1 200\tOK\r\n\r\n", 16, 0).ret, -1);
}

#[test]
fn caps_and_streaming() {
    // Cap of 0 with headers present: version/status/reason published, -1.
    let o = parse(b"HTTP/1.1 200 OK\r\nH: v\r\n\r\n", 0, 0);
    assert_eq!(o.ret, -1);
    assert_eq!((o.ver, o.status), (1, 200));
    assert_eq!(o.msg, b"OK");
    assert!(o.headers.is_empty());
    // Streaming hint with a complete head parses normally (17 + 2 = 19).
    let o = parse(b"HTTP/1.1 200 OK\r\n\r\n", 16, 10);
    assert_eq!(o.ret, 19);
    // Incomplete head with a hint stays -2.
    assert_eq!(parse(b"HTTP/1.1 200 OK\r\nH: v", 16, 8).ret, -2);
}
