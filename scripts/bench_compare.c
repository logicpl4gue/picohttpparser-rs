/* Same-session C-vs-Rust benchmark (M8 measurement seam, Phase 0).
 *
 * The request corpus (REQ) and the C loop come VERBATIM from the pinned
 * upstream file: `bench.c` is included below with `main` and
 * `phr_parse_request` renamed, so the C trial runs the oracle on the exact
 * anchor bytes — zero corpus drift by construction. The Rust trial mirrors
 * `bench.c`'s loop body line-for-line against the release cdylib, including
 * the per-iteration `ret == len` equivalence check (C pays it via assert with
 * NDEBUG unset; the Rust side pays an identical explicit branch).
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

/* C-side prototype (definition lives in the renamed oracle object). */
int c_phr_parse_request(const char *buf, size_t len, const char **method, size_t *method_len,
                        const char **path, size_t *path_len, int *minor_version,
                        struct phr_header *headers, size_t *num_headers, size_t last_len);
int bench_c_main(void);

#define main bench_c_main
#define phr_parse_request c_phr_parse_request
#include "bench.c" /* REQ macro + C trial body; header skipped by its guard */
#undef main
#undef phr_parse_request

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

int main(void)
{
    struct timespec res;
    int t;

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
