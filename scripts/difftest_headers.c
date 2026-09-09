/* Differential tester: C oracle vs Rust replacement, standalone header parser.
 *
 * The oracle (reference/picohttpparser.c) is compiled with -D renames so its
 * symbols become c_*; the Rust staticlib provides the phr_* symbols under
 * test. Both sides parse identical inputs: full buffers plus every prefix
 * streaming (last_len = previous length) plus every strict prefix cold
 * (last_len = 0, so parse-path EOF handling is compared without the
 * slowloris gate), across header caps {0,1,2,3,5,16,64}.
 *
 * Both implementations write into caller arrays; slots 0..=max(count) are
 * compared as (pointer, length) equality into the shared input buffer (which
 * IS byte equality) without dereferencing. Exit 0 iff zero mismatches.
 *
 * Usage: difftest_headers <file> [<file>...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picohttpparser.h"

/* Oracle entry point (renamed at compile time, declared here by hand). */
int c_phr_parse_headers(const char *buf, size_t len, struct phr_header *headers, size_t *num_headers,
                        size_t last_len);

/* >= max cap (64) + 1 in-progress slot, with headroom for fatter corpora. */
#define MAXH 128

static const size_t CAPS[] = {0, 1, 2, 3, 5, 16, 64};
#define NCAPS (sizeof(CAPS) / sizeof(CAPS[0]))

static size_t total_cases = 0;
static size_t mismatches = 0;

/* Compare one parsed outcome. Returns 1 on mismatch (and prints it). */
static int cmp_one(const char *file, size_t cap, size_t prefix, size_t last_len, int rc,
                   struct phr_header *rh, size_t rn, int cc, struct phr_header *ch, size_t cn) {
    size_t i, n;
    if (rc != cc || rn != cn) {
        goto mismatch;
    }
    /* Slots below max(rn, cn) are completed headers; slot max(rn, cn) itself
       is the in-progress slot both sides may have touched on error paths
       (C writes the name before scanning the value). Beyond that, both
       arrays still hold the sentinel fill. */
    n = rn > cn ? rn : cn;
    for (i = 0; i <= n && i < MAXH; i++) {
        if (rh[i].name != ch[i].name || rh[i].name_len != ch[i].name_len ||
            rh[i].value != ch[i].value || rh[i].value_len != ch[i].value_len) {
            goto mismatch;
        }
    }
    return 0;
mismatch:
    mismatches++;
    printf("MISMATCH file=%s cap=%zu prefix=%zu last_len=%zu\n", file, cap, prefix, last_len);
    printf("  rust: ret=%d nhdr=%zu\n", rc, rn);
    printf("  c:    ret=%d nhdr=%zu\n", cc, cn);
    return 1;
}

static int run_case(const char *file, const char *data, size_t cap, size_t prefix, size_t last_len) {
    struct phr_header rh[MAXH], ch[MAXH];
    size_t rnh, cnh;
    int rc, cc;

    /* Sentinel-fill so untouched slots can't hide behind stack garbage. */
    memset(rh, 0xAA, sizeof(rh));
    memset(ch, 0xAA, sizeof(ch));
    rnh = cnh = cap;

    rc = phr_parse_headers(data, prefix, rh, &rnh, last_len);
    cc = c_phr_parse_headers(data, prefix, ch, &cnh, last_len);
    total_cases++;

    /* cmp_one compares completed slots plus the in-progress slot; anything
       beyond still holds identical sentinel fill on both sides. */
    return cmp_one(file, cap, prefix, last_len, rc, rh, rnh, cc, ch, cnh);
}

int main(int argc, char **argv) {
    int f;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file>...\n", argv[0]);
        return 2;
    }
    for (f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        char *data;
        long len;
        size_t c, k, prev;
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
        for (c = 0; c < NCAPS; c++) {
            size_t cap = CAPS[c];
            /* Full buffer, cold start. */
            run_case(argv[f], data, cap, (size_t)len, 0);
            /* Every prefix, streaming (last_len = previous length). */
            prev = 0;
            for (k = 0; k <= (size_t)len; k++) {
                run_case(argv[f], data, cap, k, prev);
                prev = k;
            }
            /* Cold-start sweep: every strict prefix with last_len = 0. */
            for (k = 0; k < (size_t)len; k++) {
                run_case(argv[f], data, cap, k, 0);
            }
        }
        free(data);
    }
    printf("files=%zu cases=%zu mismatches=%zu\n", (size_t)(argc - 1), total_cases, mismatches);
    return mismatches == 0 ? 0 : 1;
}
