/*
 * bench_corpus.h — plan §11 multi-category benchmark corpus (M8 suite).
 *
 * Owned by scripts/bench_compare.c (and only by it). Each request/response
 * message ends with its terminating blank line, so a complete-head parse
 * consumes the WHOLE buffer (ret == len) — mirroring bench.c's discipline
 * (`assert(ret == sizeof(REQ) - 1)`). The chunked corpus is a complete
 * chunked body whose terminal zero chunk leaves exactly the trailing CRLF as
 * leftover (ret == 2), decoded length 19.
 *
 * Per-category iteration budgets are chosen so one C-side trial lands in the
 * ~1-3 s window that the in-process clock can measure precisely. Measured
 * per-parse cost (i5-12400F, C oracle at -O2) and the resulting one-trial
 * budget:
 *
 *   tiny      37 B  GET with one header          120 000 000   ~0.02 µs  -> ~2 s
 *   typical  ~400 B browser/API-ish request       10 000 000   ~0.13 µs  -> ~1.3 s
 *   large   ~4.9 KB 64 headers + 64 B values       1 000 000   ~1.35 µs  -> ~1.4 s
 *   response ~150 B status + 4 headers            30 000 000   ~0.03 µs  -> ~1.7 s
 *   chunked   38 B two data chunks + terminal     60 000 000   ~0.03 µs  -> ~1.6 s (incl. fresh copy)
 *   malformed 18 B rejected at method token      400 000 000   ~0.004 µs -> ~1.6 s
 *   streaming ~400 B x 8 staged prefixes           3 000 000   ~0.35 µs  -> ~1.0 s (8 calls/iter)
 *
 * `large` is assembled once at startup by cat_large_build() (the literal
 * payload below) because a 4.9 KB adjacent-string literal is unreadable;
 * everything else is a plain literal.
 */
#ifndef BENCH_CORPUS_H
#define BENCH_CORPUS_H

#include <stddef.h>

/* ---- tiny ----------------------------------------------------------------
 * plan §11 "Tiny Request": minimal GET with one header. 37 bytes. */
static const char CAT_TINY_MSG[] = "GET / HTTP/1.1\r\n"
                                   "Host: example.com\r\n"
                                   "\r\n";
#define CAT_TINY_LEN (sizeof(CAT_TINY_MSG) - 1)
#define CAT_TINY_ITERS 120000000LL

/* ---- typical -------------------------------------------------------------
 * plan §11 "Typical Request": representative browser/API request with several
 * headers (~400 B, 9 headers). */
static const char CAT_TYPICAL_MSG[] =
    "POST /api/v2/items?page=3&sort=date HTTP/1.1\r\n"
    "Host: api.example.com\r\n"
    "User-Agent: pico-bench/1.0 (X11; Linux x86_64)\r\n"
    "Accept: application/json\r\n"
    "Accept-Language: en-US,en;q=0.9\r\n"
    "Content-Type: application/json; charset=utf-8\r\n"
    "Authorization: Bearer abcdefghijklmnopqrstuvwxyz0123456789\r\n"
    "Content-Length: 42\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";
#define CAT_TYPICAL_LEN (sizeof(CAT_TYPICAL_MSG) - 1)
#define CAT_TYPICAL_ITERS 10000000LL

/* ---- large ---------------------------------------------------------------
 * plan §11 "Large Headers": 64 headers with 64-byte values. Built once by
 * cat_large_build() into the caller's buffer: request line + 64 header lines
 * (X-Hdr-NN) + blank line, ~4.9 KB. Requires a header slot cap > 64 (the
 * driver uses 128). */
#define CAT_LARGE_NHDR 64
static const char CAT_LARGE_REQUEST_LINE[] = "GET /large HTTP/1.1\r\n";
static const char CAT_LARGE_PAYLOAD[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123"; /* 64 B */
#define CAT_LARGE_ITERS 1000000LL

/* cat_large_build: assemble the full large message into dst[0..cap). Returns
 * the message length, or 0 if cap is insufficient (cap >= 8 KB is expected).
 * Called once before benchmarking, never inside a timed loop. */
static size_t cat_large_build(char *dst, size_t cap)
{
    size_t o = 0;
    size_t i;
#define NEED(n) \
    do { \
        if ((n) > cap - o) \
            return 0; \
    } while (0)

    NEED(sizeof(CAT_LARGE_REQUEST_LINE) - 1);
    for (i = 0; i < sizeof(CAT_LARGE_REQUEST_LINE) - 1; i++)
        dst[o++] = CAT_LARGE_REQUEST_LINE[i];
    for (i = 0; i < CAT_LARGE_NHDR; i++) {
        size_t j;
        NEED(8 + 2 + (sizeof(CAT_LARGE_PAYLOAD) - 1) + 2);
        dst[o++] = 'X';
        dst[o++] = '-';
        dst[o++] = 'H';
        dst[o++] = 'd';
        dst[o++] = 'r';
        dst[o++] = '-';
        dst[o++] = (char)('0' + (char)(i / 10));
        dst[o++] = (char)('0' + (char)(i % 10));
        dst[o++] = ':';
        dst[o++] = ' ';
        for (j = 0; j < sizeof(CAT_LARGE_PAYLOAD) - 1; j++)
            dst[o++] = CAT_LARGE_PAYLOAD[j];
        dst[o++] = '\r';
        dst[o++] = '\n';
    }
    NEED(2);
    dst[o++] = '\r';
    dst[o++] = '\n';
    return o;
#undef NEED
}

/* ---- response ------------------------------------------------------------
 * plan §11 "Response Parsing": small status line + 4 headers (~150 B). */
static const char CAT_RESP_MSG[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Length: 4096\r\n"
    "Server: nginx/1.18.0\r\n"
    "Date: Mon, 01 Jan 2024 00:00:00 GMT\r\n"
    "\r\n";
#define CAT_RESP_LEN (sizeof(CAT_RESP_MSG) - 1)
#define CAT_RESP_ITERS 30000000LL

/* ---- chunked -------------------------------------------------------------
 * plan §11 "Chunked Decoding": a small multi-chunk stream. Three data chunks
 * (5 + 6 + 8 = 19 decoded bytes) followed by the terminal zero chunk whose
 * trailing CRLF is left over when consume_trailer == 0: decode returns 2,
 * decoded length 19. */
static const char CAT_CHUNKED_MSG[] = "5\r\nhello\r\n6\r\n world\r\n8\r\n!goodbye\r\n0\r\n\r\n";
#define CAT_CHUNKED_LEN (sizeof(CAT_CHUNKED_MSG) - 1) /* 38 */
#define CAT_CHUNKED_DATA 19
#define CAT_CHUNKED_ITERS 60000000LL

/* ---- malformed -----------------------------------------------------------
 * plan §11 "Malformed Input": rejected during the method token scan (byte 1
 * is '@', not a tchar), so both sides return -1 without touching headers.
 * An early-reject corpus; ~60 M iters keeps one trial near 2 s. */
static const char CAT_MALFORMED_MSG[] = "G@T / HTTP/1.1\r\n\r\n";
#define CAT_MALFORMED_LEN (sizeof(CAT_MALFORMED_MSG) - 1)
#define CAT_MALFORMED_ITERS 400000000LL

/* ---- streaming -----------------------------------------------------------
 * plan §11 "Streaming": the typical request replayed through 8 staged
 * prefixes (see stream_loop in bench_compare.c). Budget counts LOGICAL
 * messages; each costs 8 parse calls. */
#define CAT_STREAM_ITERS 3000000LL

#endif /* BENCH_CORPUS_H */
