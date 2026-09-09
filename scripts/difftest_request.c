/* Differential tester: C oracle vs Rust replacement, request parser.
 *
 * The oracle (reference/picohttpparser.c) is compiled with -D renames so its
 * symbols become c_*; the Rust staticlib provides the phr_* symbols under
 * test. Both sides parse identical inputs: full buffers plus every prefix
 * (streaming; last_len = previous length), across header caps
 * {0,1,2,3,5,16,64}.
 *
 * Both implementations return pointers into the SAME input buffer, so the
 * comparison is pointer equality + lengths + bytes: any observable difference
 * (ret, method, path, version, count, names, values — including the
 * in-progress header slot on error paths, where C leaves the scanned name
 * behind) prints a MISMATCH line. Exit 0 iff zero mismatches.
 *
 * Usage: difftest_request <file> [<file>...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picohttpparser.h"

/* Oracle entry point (renamed at compile time, declared here by hand). */
int c_phr_parse_request(const char *buf, size_t len, const char **method, size_t *method_len,
                        const char **path, size_t *path_len, int *minor_version,
                        struct phr_header *headers, size_t *num_headers, size_t last_len);

/* >= max cap (64) + 1 in-progress slot, with headroom for fatter corpora. */
#define MAXH 128

static const size_t CAPS[] = {0, 1, 2, 3, 5, 16, 64};
#define NCAPS (sizeof(CAPS) / sizeof(CAPS[0]))

static size_t total_cases = 0;
static size_t mismatches = 0;

/* Both sides parse the SAME input buffer, so (pointer, length) equality IS
   byte equality — compared without dereferencing, which also keeps untouched
   sentinel slots crash-proof. */

/* Compare one parsed outcome. Returns 1 on mismatch (and prints it). */
static int cmp_one(const char *file, size_t cap, size_t prefix, size_t last_len, int rc, const char *rm,
                   size_t rml, const char *rp, size_t rpl, int rv, struct phr_header *rh, size_t rn,
                   int cc, const char *cm, size_t cml, const char *cp, size_t cpl, int cv,
                   struct phr_header *ch, size_t cn) {
    size_t i, n;
    if (rc != cc || rv != cv || rn != cn || rml != cml || rpl != cpl || rm != cm || rp != cp) {
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
    printf("MISMATCH file=%s cap=%zu prefix=%zu last_len=%zu\n", file, (size_t)cap,
           (size_t)prefix, (size_t)last_len);
    printf("  rust: ret=%d ver=%d nhdr=%zu method_len=%zu path_len=%zu\n", rc, rv, rn, rml,
           rpl);
    printf("  c:    ret=%d ver=%d nhdr=%zu method_len=%zu path_len=%zu\n", cc, cv, cn, cml,
           cpl);
    return 1;
}

static int run_case(const char *file, const char *data, size_t cap, size_t prefix, size_t last_len) {
    struct phr_header rh[MAXH], ch[MAXH];
    const char *rm, *rp, *cm, *cp;
    size_t rml, rpl, rnh, cml, cpl, cnh;
    int rv, cv, rc, cc;

    /* Sentinel-fill so untouched slots can't hide behind stack garbage. */
    memset(rh, 0xAA, sizeof(rh));
    memset(ch, 0xAA, sizeof(ch));
    rm = rp = cm = cp = (const char *)0x1;
    rml = rpl = cml = cpl = (size_t)-1;
    rv = cv = -99;
    rnh = cnh = cap;

    rc = phr_parse_request(data, prefix, &rm, &rml, &rp, &rpl, &rv, rh, &rnh, last_len);
    cc = c_phr_parse_request(data, prefix, &cm, &cml, &cp, &cpl, &cv, ch, &cnh, last_len);
    total_cases++;

    /* cmp_one compares completed slots plus the in-progress slot; anything
       beyond still holds identical sentinel fill on both sides. */
    return cmp_one(file, cap, prefix, last_len, rc, rm, rml, rp, rpl, rv, rh, rnh, cc, cm, cml, cp,
                   cpl, cv, ch, cnh);
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
        }
        free(data);
    }
    printf("files=%d cases=%zu mismatches=%zu\n", argc - 1, total_cases, mismatches);
    return mismatches == 0 ? 0 : 1;
}
