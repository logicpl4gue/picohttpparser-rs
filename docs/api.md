# API — Verified Against Pinned Upstream

Verified directly against the pinned header
`reference/picohttpparser.h` (commit `f4d94b48b31e0abae029ebeafcfd9ca0680ede58`,
version macro `"1.dev"`). This is the ABI contract for the Rust rewrite.

## Structures

```c
/* contains name and value of a header (name == NULL if is a continuing line
 * of a multiline header */
struct phr_header {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
};

/* should be zero-filled before start */
struct phr_chunked_decoder {
    size_t bytes_left_in_chunk; /* number of bytes left in current chunk */
    char consume_trailer;       /* if trailing headers should be consumed */
    char _hex_count;
    char _state;
    uint64_t _total_read;
    uint64_t _total_overhead;
};
```

## Functions (5 exported symbols at this pin)

```c
/* returns number of bytes consumed if successful, -2 if request is partial,
 * -1 if failed */
int phr_parse_request(const char *buf, size_t len, const char **method, size_t *method_len,
                      const char **path, size_t *path_len, int *minor_version,
                      struct phr_header *headers, size_t *num_headers, size_t last_len);

/* ditto */
int phr_parse_response(const char *_buf, size_t len, int *minor_version, int *status,
                       const char **msg, size_t *msg_len, struct phr_header *headers,
                       size_t *num_headers, size_t last_len);

/* ditto */
int phr_parse_headers(const char *buf, size_t len, struct phr_header *headers,
                      size_t *num_headers, size_t last_len);

/* the function rewrites the buffer given as (buf, bufsz) removing the chunked-
 * encoding headers.  When the function returns without an error, bufsz is
 * updated to the length of the decoded data available.  Applications should
 * repeatedly call the function while it returns -2 (incomplete) every time
 * supplying newly arrived data.  If the end of the chunked-encoded data is
 * found, the function returns a non-negative number indicating the number of
 * octets left undecoded, that starts from the offset returned by `*bufsz`.
 * Returns -1 on error. */
ssize_t phr_decode_chunked(struct phr_chunked_decoder *decoder, char *buf, size_t *bufsz);

/* returns if the chunked decoder is in middle of chunked data */
int phr_decode_chunked_is_in_data(struct phr_chunked_decoder *decoder);
```

Note on `ssize_t`: the header defines `ssize_t` as `intptr_t` under MSVC.

## Return-value conventions

| Code | Meaning |
|---|---|
| `>= 0` | bytes consumed (requests/responses/headers); for `phr_decode_chunked`: number of octets left undecoded after the decoded data |
| `-2` | partial / incomplete input — caller must supply more bytes (streaming contracts) |
| `-1` | parse failure (malformed input) |
| `num_headers` | in: capacity of the caller's header array; out: number of headers found |
| `last_len` | buffer length at the previous call (used for slowloris/partial-detection and continuation logic) |

## Behavioral notes (verified against upstream source and README)

1. **The parser itself is stateless and does not allocate memory by itself**
   (upstream README): output pointers point into the caller's buffer
   (zero-copy). The Rust rewrite must preserve this.
2. **The chunked decoder IS caller-stateful.** `phr_decode_chunked` carries
   state in the caller-owned `struct phr_chunked_decoder` across calls and
   rewrites the input buffer in place. `phr_decode_chunked_is_in_data` queries
   that state. This is state, but it is not parser-internal state.
3. **README lists "four functions"**; the header at this pin exports five —
   `phr_decode_chunked_is_in_data` is not in the README's list. The header is
   authoritative for the ABI (5 symbols).
4. **Repo predates H2O.** Copyright spans 2009–2014 (Kazuho Oku et al.);
   the H2O project started ~2014 and adopted/continued this parser. "Part of
   the H2O ecosystem" is accurate; "born inside H2O" is not.
5. Chunked decoding handles chunk extensions, trailers (optionally consumed
   via `consume_trailer`), incomplete sizes, and reports `-1` on invalid hex
   (`_hex_count`/`_state` track the machine).
6. Request/response/header parses return `-2` on incomplete input, enabling
   streaming continuation — `last_len` feeds the partial-message check.

## ABI targets for the Rust rewrite (Milestone 1+)

- Export exactly these 5 symbols from a cdylib/staticlib with C calling
  convention; matching struct layout (field order/width matters), `int`
  returns, `size_t`/`uint64_t` widths, `ssize_t` (= `intptr_t` on MSVC).
- `name == NULL` marks a multiline header continuation line (from the struct
  comment) — must be preserved.
## Rust type mapping (Milestone 1 ABI shell)

`src/ffi.rs` exports the five symbols with the mapping below. Every signature
is pinned at compile time by fn-pointer const assertions, and struct layout
by in-crate const assertions plus `tests/abi.rs` (64-bit values; 32-bit
targets differ and get re-pinned when a 32-bit toolchain is exercised).

| C | Rust |
|---|---|
| `int` | `c_int` (`core::ffi`) |
| `size_t` | `usize` |
| `ssize_t` | `isize` (ABI-correct on MSVC where `ssize_t` is `intptr_t`, and on POSIX) |
| `uint64_t` | `u64` |
| `char` (decoder fields) | `c_char` |
| `const char *` | `*const c_char` |
| `char *` | `*mut c_char` |
| `const char **` | `*mut *const c_char` |
| `int *` | `*mut c_int` |
| `size_t *` | `*mut usize` |
| `struct phr_header *` | `*mut PhrHeader` |
| `struct phr_chunked_decoder *` | `*mut PhrChunkedDecoder` (query fn uses `*const`, ABI-identical) |

Rust signatures (all five entry points live since M2–M5; no stubs remain):

```rust
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_request(
    buf: *const c_char, len: usize,
    method: *mut *const c_char, method_len: *mut usize,
    path: *mut *const c_char, path_len: *mut usize,
    minor_version: *mut c_int, headers: *mut PhrHeader,
    num_headers: *mut usize, last_len: usize,
) -> c_int;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_response(
    _buf: *const c_char, len: usize,
    minor_version: *mut c_int, status: *mut c_int,
    msg: *mut *const c_char, msg_len: *mut usize,
    headers: *mut PhrHeader, num_headers: *mut usize, last_len: usize,
) -> c_int;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_parse_headers(
    buf: *const c_char, len: usize,
    headers: *mut PhrHeader, num_headers: *mut usize, last_len: usize,
) -> c_int;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_decode_chunked(
    decoder: *mut PhrChunkedDecoder, buf: *mut c_char, bufsz: *mut usize,
) -> isize;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn phr_decode_chunked_is_in_data(
    decoder: *const PhrChunkedDecoder,
) -> c_int;
```

Note: `#[unsafe(no_mangle)]` is the edition-2024 spelling of `#[no_mangle]`
(unsafe attributes must be written in the `#[unsafe(...)]` form under
`unsafe_attr_outside_unsafe`).
