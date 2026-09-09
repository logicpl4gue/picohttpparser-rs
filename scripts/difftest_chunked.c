/* Differential tester: C oracle vs Rust replacement, chunked decoder.
 *
 * The oracle (reference/picohttpparser.c) is compiled with -D renames so its
 * symbols become c_*; the Rust cdylib provides the phr_* symbols under test.
 * Decoder state is compared as a full struct memcmp after every call (same
 * repr(C) layout), plus ret, *bufsz, is_in_data, and the entire working
 * buffer byte-for-byte (both sides start from identical copies, so any
 * divergence in moves/output shows).
 *
 * Per file, per consume_trailer {0,1}:
 *   A. single call, fresh zeroed decoder;
 *   B. two-call splits (fresh decoder + bytes[0..k], then CARRIED decoder +
 *      bytes[k..] in a fresh buffer): every k for small files, quarters for
 *      big ones (>8KB, where exhaustive prefixes would be quadratic);
 *   C. three-call splits at quarters (multi-call accumulation, incl. the
 *      framing-overhead totals);
 *   D. dirty decoders (nonzero bytes_left/hex_count in states 0..3 — all
 *      defined; forged _state values excluded since C aborts on them).
 * Exit 0 iff zero mismatches.
 *
 * Usage: difftest_chunked <file> [<file>...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picohttpparser.h"

/* Oracle entry points (renamed at compile time, declared here by hand). */
ssize_t c_phr_decode_chunked(struct phr_chunked_decoder *decoder, char *buf, size_t *bufsz);
int c_phr_decode_chunked_is_in_data(struct phr_chunked_decoder *decoder);

/* Exhaustive two-way splits only below this size (else quarters). */
#define BIG_FILE (8 * 1024)

static size_t total_cases = 0;
static size_t mismatches = 0;

typedef struct {
    ssize_t ret;
    size_t out_len;
    int in_data;
    struct phr_chunked_decoder dec;
} CallCmp;

/* Run one call on both sides and snapshot everything comparable. */
static void run_side(int is_rust, struct phr_chunked_decoder *dec, char *buf, size_t len, CallCmp *out) {
    size_t out_len = len;
    if (is_rust) {
        out->ret = phr_decode_chunked(dec, buf, &out_len);
        out->in_data = phr_decode_chunked_is_in_data(dec);
    } else {
        out->ret = c_phr_decode_chunked(dec, buf, &out_len);
        out->in_data = c_phr_decode_chunked_is_in_data(dec);
    }
    out->out_len = out_len;
    out->dec = *dec;
}

/* One streaming step: fresh copies for both sides, carried decoder state in,
 * full comparison out. Returns 1 on mismatch. Caller manages `dec_r/dec_c`
 * across steps (fresh-zeroed or carried). */
static int step(const char *file, const char *tag, const unsigned char *data, size_t len,
                struct phr_chunked_decoder *dec_r, struct phr_chunked_decoder *dec_c) {
    char *br = malloc(len + 1);
    char *bc = malloc(len + 1);
    CallCmp r, c;
    int bad = 0;
    if (!br || !bc) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    memcpy(br, data, len);
    memcpy(bc, data, len);
    run_side(1, dec_r, br, len, &r);
    run_side(0, dec_c, bc, len, &c);
    total_cases++;
    if (r.ret != c.ret || r.out_len != c.out_len || r.in_data != c.in_data ||
        memcmp(&r.dec, &c.dec, sizeof(r.dec)) != 0 || memcmp(br, bc, len) != 0) {
        mismatches++;
        bad = 1;
        printf("MISMATCH file=%s tag=%s trailer=%d\n", file, tag, (int)dec_r->consume_trailer);
        printf("  rust: ret=%d out=%zu in_data=%d left=%zu hex=%d st=%d rd=%llu oh=%llu\n", (int)r.ret,
               r.out_len, r.in_data, r.dec.bytes_left_in_chunk, (int)r.dec._hex_count,
               (int)r.dec._state, (unsigned long long)r.dec._total_read,
               (unsigned long long)r.dec._total_overhead);
        printf("  c:    ret=%d out=%zu in_data=%d left=%zu hex=%d st=%d rd=%llu oh=%llu\n", (int)c.ret,
               c.out_len, c.in_data, c.dec.bytes_left_in_chunk, (int)c.dec._hex_count,
               (int)c.dec._state, (unsigned long long)c.dec._total_read,
               (unsigned long long)c.dec._total_overhead);
    }
    free(br);
    free(bc);
    return bad;
}

static void fresh_dec(struct phr_chunked_decoder *d, int trailer) {
    memset(d, 0, sizeof(*d));
    d->consume_trailer = (char)trailer;
}

int main(int argc, char **argv) {
    int f;
    /* Dirty-decoder combos: (state, bytes_left, hex_count), all defined. */
    static const struct {
        int st;
        size_t left;
        int hex;
    } dirty[] = {{0, 5, 2}, {1, 0, 0}, {2, 0, 0}, {3, 10, 0}, {0, 0, 7}};
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file>...\n", argv[0]);
        return 2;
    }
    for (f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        unsigned char *data;
        long len;
        int t;
        if (!fp) {
            fprintf(stderr, "cannot open %s\n", argv[f]);
            return 2;
        }
        fseek(fp, 0, SEEK_END);
        len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc((size_t)len + 1);
        if (!data) {
            fprintf(stderr, "out of memory\n");
            return 2;
        }
        if (len > 0 && fread(data, 1, (size_t)len, fp) != (size_t)len) {
            fprintf(stderr, "short read %s\n", argv[f]);
            return 2;
        }
        fclose(fp);
        for (t = 0; t <= 1; t++) {
            struct phr_chunked_decoder dr, dc;
            size_t k, q[4];
            size_t i, j;
            /* A. single call, fresh decoder. */
            fresh_dec(&dr, t);
            fresh_dec(&dc, t);
            step(argv[f], "single", data, (size_t)len, &dr, &dc);
            /* B. two-call splits. */
            if ((size_t)len <= BIG_FILE) {
                for (k = 0; k <= (size_t)len; k++) {
                    fresh_dec(&dr, t);
                    fresh_dec(&dc, t);
                    step(argv[f], "split1", data, k, &dr, &dc);
                    step(argv[f], "split2", data + k, (size_t)len - k, &dr, &dc);
                }
            } else {
                for (i = 1; i <= 3; i++) {
                    k = (size_t)len * i / 4;
                    fresh_dec(&dr, t);
                    fresh_dec(&dc, t);
                    step(argv[f], "split1", data, k, &dr, &dc);
                    step(argv[f], "split2", data + k, (size_t)len - k, &dr, &dc);
                }
            }
            /* C. three-call splits at quarters (multi-call accumulation). */
            for (i = 0; i < 4; i++)
                q[i] = (size_t)len * i / 4;
            for (i = 0; i < 3; i++) {
                for (j = i + 1; j < 4; j++) {
                    fresh_dec(&dr, t);
                    fresh_dec(&dc, t);
                    step(argv[f], "tri1", data, q[i], &dr, &dc);
                    step(argv[f], "tri2", data + q[i], q[j] - q[i], &dr, &dc);
                    step(argv[f], "tri3", data + q[j], (size_t)len - q[j], &dr, &dc);
                }
            }
            /* D. dirty decoders, single call (small files only). */
            if ((size_t)len <= BIG_FILE) {
                for (i = 0; i < sizeof(dirty) / sizeof(dirty[0]); i++) {
                    fresh_dec(&dr, t);
                    fresh_dec(&dc, t);
                    dr._state = dc._state = (char)dirty[i].st;
                    dr.bytes_left_in_chunk = dc.bytes_left_in_chunk = dirty[i].left;
                    dr._hex_count = dc._hex_count = (char)dirty[i].hex;
                    step(argv[f], "dirty", data, (size_t)len, &dr, &dc);
                }
            }
        }
        free(data);
    }
    printf("files=%d cases=%zu mismatches=%zu\n", argc - 1, total_cases, mismatches);
    return mismatches == 0 ? 0 : 1;
}
