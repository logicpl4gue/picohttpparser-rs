/* Differential tester: C oracle vs Rust replacement, response parser.
 *
 * The oracle (reference/picohttpparser.c) is compiled with -D renames so its
 * symbols become c_*; the Rust staticlib provides the phr_* symbols under
 * test. Both sides parse identical inputs: full buffers plus every prefix
 * (streaming; last_len = previous length), across header caps
 * {0,1,2,3,5,16,64}.
 *
 * Both implementations return pointers into the SAME input buffer, so
 * (pointer, length) equality IS byte equality — compared without
 * dereferencing, including the in-progress header slot on error paths.
 * Exit 0 iff zero mismatches.
 *
 * Usage: difftest_response <file> [<file>...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picohttpparser.h"

/* Oracle entry point (renamed at compile time, declared here by hand). */
int c_phr_parse_response(const char *buf, size_t len, int *minor_version, int *status,
                         const char **msg, size_t *msg_len, struct phr_header *headers,
                         size_t *num_headers, size_t last_len);

/* >= max cap (64) + 1 in-progress slot, with headroom for fatter corpora. */
#define MAXH 128

static const size_t CAPS[] = {0, 1, 2, 3, 5, 16, 64};
#define NCAPS (sizeof(CAPS) / sizeof(CAPS[0]))

static size_t total_cases = 0;
static size_t mismatches = 0;

/* Compare one parsed outcome. Returns 1 on mismatch (and prints it). */
static int cmp_one(const char *file, size_t cap, size_t prefix, size_t last_len, int rc, int rv,
                   int rs, const char *rm, size_t rml, struct phr_header *rh, size_t rn, int cc,
                   int cv, int cs, const char *cm, size_t cml, struct phr_header *ch, size_t cn) {
    size_t i, n;
    if (rc != cc || rv != cv || rs != cs || rml != cml || rm != cm || rn != cn) {
        goto mismatch;
    }
    /* Slots below max(rn, cn) are completed headers; slot max(rn, cn) itself
       is the in-progress slot both sides may have touched on error paths.
       Beyond that, both arrays still hold the sentinel fill. */
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
    printf("  rust: ret=%d ver=%d status=%d msg_len=%zu nhdr=%zu\n", rc, rv, rs, rml, rn);
    printf("  c:    ret=%d ver=%d status=%d msg_len=%zu nhdr=%zu\n", cc, cv, cs, cml, cn);
    return 1;
}

static int run_case(const char *file, const char *data, size_t cap, size_t prefix, size_t last_len) {
    struct phr_header rh[MAXH], ch[MAXH];
    const char *rm, *cm;
    size_t rml, rnh, cml, cnh;
    int rv, rs, cv, cs, rc, cc;

    /* Sentinel-fill so untouched slots can't hide behind stack garbage. */
    memset(rh, 0xAA, sizeof(rh));
    memset(ch, 0xAA, sizeof(ch));
    rm = cm = (const char *)0x1;
    rml = cml = (size_t)-1;
    rv = cv = -99;
    rs = cs = -99;
    rnh = cnh = cap;

    rc = phr_parse_response(data, prefix, &rv, &rs, &rm, &rml, rh, &rnh, last_len);
    cc = c_phr_parse_response(data, prefix, &cv, &cs, &cm, &cml, ch, &cnh, last_len);
    total_cases++;

    /* cmp_one compares completed slots plus the in-progress slot; anything
       beyond still holds identical sentinel fill on both sides. */
    return cmp_one(file, cap, prefix, last_len, rc, rv, rs, rm, rml, rh, rnh, cc, cv, cs, cm, cml,
                   ch, cnh);
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
            /* Every prefix, streaming (last_len = previous length). With a
               nonzero hint the slowloris gate short-circuits before parsing,
               so this sweep alone never exercises parse-path EOF handling. */
            prev = 0;
            for (k = 0; k <= (size_t)len; k++) {
                run_case(argv[f], data, cap, k, prev);
                prev = k;
            }
            /* Cold-start sweep: every strict prefix with last_len = 0, so
               parse-path EOF handling is compared without the gate. */
            for (k = 0; k < (size_t)len; k++) {
                run_case(argv[f], data, cap, k, 0);
            }
        }
        free(data);
    }
    printf("files=%zu cases=%zu mismatches=%zu\n", (size_t)(argc - 1), total_cases, mismatches);
    return mismatches == 0 ? 0 : 1;
}
