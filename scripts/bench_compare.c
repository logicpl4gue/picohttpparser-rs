/* Same-session C-vs-Rust benchmark (M8 measurement seam).
 *
 * Two modes:
 *
 * 1. No argv — THE ANCHOR, unchanged from the M8 Phase-0 seam: the request
 *    corpus (REQ) and the C loop come VERBATIM from the pinned upstream file
 *    (`reference/bench.c` is included below with `main`/`phr_parse_request`
 *    renamed), so the C trial runs the oracle on the exact anchor bytes —
 *    zero corpus drift by construction. The Rust trial mirrors bench.c's
 *    loop body line-for-line against the release cdylib, including the
 *    per-iteration `ret == len` equivalence check (C pays it via assert with
 *    NDEBUG unset; the Rust side pays an identical explicit branch).
 *
 * 2. `./bench_compare <category>` — plan §11 multi-category suite, corpus in
 *    scripts/bench_corpus.h. Prints:
 *        CATEGORY <name>
 *        ITERS <n>
 *        TIMER clock_gettime(CLOCK_MONOTONIC) res_ns=<res>
 *        TRIAL i C <ns> R <ns>      (i = 0..6, trial 0 = warmup)
 *    and exits nonzero on any parse mismatch on either side. C loops call
 *    the renamed oracle symbols (c_phr_*), Rust loops call the cdylib
 *    symbols; every iteration checks ret == expected on both sides (the
 *    bench.c equivalence discipline, parameterized per corpus: request/
 *    response corpora consume the whole buffer, malformed rejects with -1,
 *    chunked leaves 2 octets, streaming returns -2 until the 8th stage).
 *
 * Trials interleave (C,R / R,C alternating) so turbo/thermal/background drift
 * is common-mode, not a silent bias. Trial 0 is warmup (discarded).
 * Timing is in-process `clock_gettime(CLOCK_MONOTONIC)`; resolution is
 * printed from `clock_getres` (a real in-process granularity figure).
 *
 * Build (see scripts/bench_compare.sh): this TU + renamed-symbol oracle
 * object + cdylib import lib. Exit nonzero on any parse mismatch.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "picohttpparser.h" /* real decls: struct layout + Rust-side symbols */

/* C-side prototypes (definitions live in the renamed oracle object). */
int c_phr_parse_request(const char *buf, size_t len, const char **method, size_t *method_len,
                        const char **path, size_t *path_len, int *minor_version,
                        struct phr_header *headers, size_t *num_headers, size_t last_len);
int c_phr_parse_response(const char *buf, size_t len, int *minor_version, int *status,
                         const char **msg, size_t *msg_len, struct phr_header *headers,
                         size_t *num_headers, size_t last_len);
ssize_t c_phr_decode_chunked(struct phr_chunked_decoder *decoder, char *buf, size_t *bufsz);
int bench_c_main(void);

#define main bench_c_main
#define phr_parse_request c_phr_parse_request
#include "bench.c" /* REQ macro + C trial body; header skipped by its guard */
#undef main
#undef phr_parse_request

#include "bench_corpus.h"

#define ITERS 10000000

/* Mirror of bench.c's loop against the Rust cdylib (out-of-line call, the
   honest shipped-artifact number per the bench-seam rule). */
static int rust_loop(void)
{
    const char *method;
    size_t method_len;
    const char *path;
    size_t path_len;
    int minor_version;
    struct phr_header headers[32];
    size_t num_headers;
    int i, ret;

    for (i = 0; i < ITERS; i++) {
        num_headers = sizeof(headers) / sizeof(headers[0]);
        ret = phr_parse_request(REQ, sizeof(REQ) - 1, &method, &method_len, &path, &path_len,
                                &minor_version, headers, &num_headers, 0);
        if (ret != (int)(sizeof(REQ) - 1))
            return 1;
    }

    return 0;
}

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* ============================ category mode ============================== */

/* Parser function-pointer types: the oracle and the cdylib share exact C
   ABI signatures, so one loop body drives both sides symmetrically. */
typedef int (*req_fn)(const char *buf, size_t len, const char **method, size_t *method_len,
                      const char **path, size_t *path_len, int *minor_version,
                      struct phr_header *headers, size_t *num_headers, size_t last_len);
typedef int (*resp_fn)(const char *buf, size_t len, int *minor_version, int *status,
                       const char **msg, size_t *msg_len, struct phr_header *headers,
                       size_t *num_headers, size_t last_len);
typedef ssize_t (*chunk_fn)(struct phr_chunked_decoder *decoder, char *buf, size_t *bufsz);

/* Header slots: 128 > 64 (large corpus) + in-progress slot. Reused by every
   request/response category; num_headers is reset to the cap each call. */
#define CAT_HDR_CAP 128
static struct phr_header cat_headers[CAT_HDR_CAP];

/* Message corpora are `const`; only the chunked decode rewrites in place, so
   the chunked loop copies CAT_CHUNKED_MSG into a writable stack buffer per
   iteration (both sides identically). */
static char cat_chunk_buf[CAT_CHUNKED_LEN + 1];

/* Large-message buffer built once by cat_large_build(). */
static char cat_large_buf[8 * 1024];
static size_t cat_large_len;

/* req_loop: full-buffer request parses; complete heads consume everything,
   so the equivalence check is ret == len on both sides. */
static int req_loop(req_fn fn, const char *msg, size_t len, long long iters)
{
    const char *method;
    size_t method_len;
    const char *path;
    size_t path_len;
    int minor_version;
    size_t num_headers;
    long long i;
    int ret;

    for (i = 0; i < iters; i++) {
        num_headers = CAT_HDR_CAP;
        method = NULL;
        path = NULL;
        ret = fn(msg, len, &method, &method_len, &path, &path_len, &minor_version, cat_headers,
                 &num_headers, 0);
        if (ret != (int)len)
            return 1;
    }

    return 0;
}

/* resp_loop: same discipline for the response entry. */
static int resp_loop(resp_fn fn, const char *msg, size_t len, long long iters)
{
    const char *rp;
    size_t rp_len;
    int minor_version, status;
    size_t num_headers;
    long long i;
    int ret;

    for (i = 0; i < iters; i++) {
        num_headers = CAT_HDR_CAP;
        rp = NULL;
        ret = fn(msg, len, &minor_version, &status, &rp, &rp_len, cat_headers, &num_headers, 0);
        if (ret != (int)len)
            return 1;
    }

    return 0;
}

/* mal_loop: an early-reject corpus; the equivalence check is ret == -1 (the
   documented malformed code) on both sides. */
static int mal_loop(req_fn fn, const char *msg, size_t len, long long iters)
{
    const char *method;
    size_t method_len;
    const char *path;
    size_t path_len;
    int minor_version;
    size_t num_headers;
    long long i;
    int ret;

    (void)len;
    for (i = 0; i < iters; i++) {
        num_headers = CAT_HDR_CAP;
        method = NULL;
        path = NULL;
        ret = fn(msg, CAT_MALFORMED_LEN, &method, &method_len, &path, &path_len, &minor_version,
                 cat_headers, &num_headers, 0);
        if (ret != -1)
            return 1;
    }

    return 0;
}

/* dec_loop: chunked decode needs a zeroed decoder and a FRESH mutable copy
 * of the stream per iteration (decode compacts in place). Expected: ret == 2
 * (terminal zero chunk, consume_trailer == 0 -> trailing CRLF left over) and
 * bufsz == 19 decoded bytes — both sides, every iteration. */
static int dec_loop(chunk_fn fn, long long iters)
{
    long long i;

    for (i = 0; i < iters; i++) {
        struct phr_chunked_decoder dec;
        char *buf = cat_chunk_buf;
        size_t bufsz = CAT_CHUNKED_LEN;
        ssize_t ret;

        memset(&dec, 0, sizeof(dec));
        memcpy(buf, CAT_CHUNKED_MSG, CAT_CHUNKED_LEN);
        ret = fn(&dec, buf, &bufsz);
        if (ret != 2 || bufsz != CAT_CHUNKED_DATA)
            return 1;
    }

    return 0;
}

/* stream_loop: plan §11 "Streaming" — the typical request is delivered over
 * 8 staged calls per logical message. Stage k parses prefix p_k = len*k/8 of
 * the SAME buffer with the slowloris hint last_len = p_{k-1} (0 for k = 1),
 * exactly the upstream README incremental pattern. The request's blank line
 * lives only in the final 1/8, so stages 1..7 must return -2 (partial: the
 * is_complete gate scans only the newest ~len/8 bytes and finds no empty
 * line) and the 8th must consume the whole buffer (ret == len). Both sides
 * receive byte-identical call sequences and must agree on every stage. */
static int stream_loop(req_fn fn, const char *msg, size_t len, long long iters)
{
    long long i;
    int k;

    for (i = 0; i < iters; i++) {
        size_t prev = 0;
        for (k = 1; k <= 8; k++) {
            const char *method;
            size_t method_len;
            const char *path;
            size_t path_len;
            int minor_version;
            size_t num_headers;
            size_t pf = (size_t)((len * (size_t)k) / 8);
            int ret;

            num_headers = CAT_HDR_CAP;
            method = NULL;
            path = NULL;
            ret = fn(msg, pf, &method, &method_len, &path, &path_len, &minor_version, cat_headers,
                     &num_headers, prev);
            if (ret != (k == 8 ? (int)len : -2))
                return 1;
            prev = pf;
        }
    }

    return 0;
}

/* ---- per-category C/R wrappers ------------------------------------------ */
static int c_tiny_loop(void) { return req_loop(c_phr_parse_request, CAT_TINY_MSG, CAT_TINY_LEN, CAT_TINY_ITERS); }
static int r_tiny_loop(void) { return req_loop(phr_parse_request, CAT_TINY_MSG, CAT_TINY_LEN, CAT_TINY_ITERS); }
static int c_typical_loop(void) { return req_loop(c_phr_parse_request, CAT_TYPICAL_MSG, CAT_TYPICAL_LEN, CAT_TYPICAL_ITERS); }
static int r_typical_loop(void) { return req_loop(phr_parse_request, CAT_TYPICAL_MSG, CAT_TYPICAL_LEN, CAT_TYPICAL_ITERS); }
static int c_large_loop(void) { return req_loop(c_phr_parse_request, cat_large_buf, cat_large_len, CAT_LARGE_ITERS); }
static int r_large_loop(void) { return req_loop(phr_parse_request, cat_large_buf, cat_large_len, CAT_LARGE_ITERS); }
static int c_resp_loop(void) { return resp_loop(c_phr_parse_response, CAT_RESP_MSG, CAT_RESP_LEN, CAT_RESP_ITERS); }
static int r_resp_loop(void) { return resp_loop(phr_parse_response, CAT_RESP_MSG, CAT_RESP_LEN, CAT_RESP_ITERS); }
static int c_chunked_loop(void) { return dec_loop(c_phr_decode_chunked, CAT_CHUNKED_ITERS); }
static int r_chunked_loop(void) { return dec_loop(phr_decode_chunked, CAT_CHUNKED_ITERS); }
static int c_mal_loop(void) { return mal_loop(c_phr_parse_request, CAT_MALFORMED_MSG, CAT_MALFORMED_LEN, CAT_MALFORMED_ITERS); }
static int r_mal_loop(void) { return mal_loop(phr_parse_request, CAT_MALFORMED_MSG, CAT_MALFORMED_LEN, CAT_MALFORMED_ITERS); }
static int c_stream_loop(void) { return stream_loop(c_phr_parse_request, CAT_TYPICAL_MSG, CAT_TYPICAL_LEN, CAT_STREAM_ITERS); }
static int r_stream_loop(void) { return stream_loop(phr_parse_request, CAT_TYPICAL_MSG, CAT_TYPICAL_LEN, CAT_STREAM_ITERS); }

/* Shared trial runner for a category: CATEGORY/ITERS/TIMER headers followed
 * by 7 interleaved C/R trials (trial 0 warmup, discarded by callers).
 * Returns 0 iff every trial passed on both sides. */
static int run_category(const char *name, long long iters, int (*c_loop)(void), int (*r_loop)(void))
{
    struct timespec res;
    int t;

    printf("CATEGORY %s\n", name);
    printf("ITERS %lld\n", iters);
    clock_getres(CLOCK_MONOTONIC, &res);
    printf("TIMER clock_gettime(CLOCK_MONOTONIC) res_ns=%ld\n",
           (long)(res.tv_sec * 1000000000L + res.tv_nsec));
    for (t = 0; t < 7; t++) {
        long long start, c_ns, r_ns;
        int rc, rr;
        if (t % 2 == 0) {
            start = now_ns();
            rc = c_loop();
            c_ns = now_ns() - start;
            start = now_ns();
            rr = r_loop();
            r_ns = now_ns() - start;
        } else {
            start = now_ns();
            rr = r_loop();
            r_ns = now_ns() - start;
            start = now_ns();
            rc = c_loop();
            c_ns = now_ns() - start;
        }
        if (rc != 0 || rr != 0) {
            printf("TRIAL %d FAILED rc=%d rr=%d\n", t, rc, rr);
            return 1;
        }
        printf("TRIAL %d C %lld R %lld\n", t, c_ns, r_ns);
    }

    return 0;
}

/* Build once-before-any-trial corpora (never inside a timed loop). */
static int init_category_corpora(void)
{
    cat_large_len = cat_large_build(cat_large_buf, sizeof(cat_large_buf));
    if (cat_large_len == 0 || cat_large_len + 1 > sizeof(cat_large_buf))
        return 1;
    return 0;
}

static int category_main(const char *name)
{
    if (init_category_corpora() != 0) {
        fprintf(stderr, "corpus init failed\n");
        return 2;
    }
    if (strcmp(name, "tiny") == 0)
        return run_category("tiny", CAT_TINY_ITERS, c_tiny_loop, r_tiny_loop);
    if (strcmp(name, "typical") == 0)
        return run_category("typical", CAT_TYPICAL_ITERS, c_typical_loop, r_typical_loop);
    if (strcmp(name, "large") == 0)
        return run_category("large", CAT_LARGE_ITERS, c_large_loop, r_large_loop);
    if (strcmp(name, "response") == 0)
        return run_category("response", CAT_RESP_ITERS, c_resp_loop, r_resp_loop);
    if (strcmp(name, "chunked") == 0)
        return run_category("chunked", CAT_CHUNKED_ITERS, c_chunked_loop, r_chunked_loop);
    if (strcmp(name, "malformed") == 0)
        return run_category("malformed", CAT_MALFORMED_ITERS, c_mal_loop, r_mal_loop);
    if (strcmp(name, "streaming") == 0)
        return run_category("streaming", CAT_STREAM_ITERS, c_stream_loop, r_stream_loop);
    fprintf(stderr,
            "usage: bench_compare [tiny|typical|large|response|chunked|malformed|streaming]\n");
    return 2;
}

/* ============================ anchor mode ================================ */

int main(int argc, char **argv)
{
    struct timespec res;
    int t;

    if (argc > 1)
        return category_main(argv[1]);

    clock_getres(CLOCK_MONOTONIC, &res);
    printf("TIMER clock_gettime(CLOCK_MONOTONIC) res_ns=%ld\n",
           (long)(res.tv_sec * 1000000000L + res.tv_nsec));
    printf("ITERS %d\n", ITERS);
    for (t = 0; t < 7; t++) {
        long long start, c_ns, r_ns;
        int rc, rr;
        if (t % 2 == 0) {
            start = now_ns();
            rc = bench_c_main();
            c_ns = now_ns() - start;
            start = now_ns();
            rr = rust_loop();
            r_ns = now_ns() - start;
        } else {
            start = now_ns();
            rr = rust_loop();
            r_ns = now_ns() - start;
            start = now_ns();
            rc = bench_c_main();
            c_ns = now_ns() - start;
        }
        if (rc != 0 || rr != 0) {
            printf("TRIAL %d FAILED rc=%d rr=%d\n", t, rc, rr);
            return 1;
        }
        printf("TRIAL %d C %lld R %lld\n", t, c_ns, r_ns);
    }
    return 0;
}
