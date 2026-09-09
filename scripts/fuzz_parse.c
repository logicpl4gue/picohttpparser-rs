/* Deterministic differential fuzz driver: C oracle vs Rust replacement.
 *
 * Stateless parsers only (request / response / headers). The oracle
 * (reference/picohttpparser.c) is compiled with -D renames (c_* symbols);
 * the Rust release cdylib provides the phr_* symbols under test. Both sides
 * parse the SAME mutated input buffer, so (pointer, length) equality IS byte
 * equality — compared without dereferencing, exactly like
 * scripts/difftest_request.c, whose per-entry compare logic is reused here.
 *
 * Usage:
 *   fuzz_parse SEED ITERATIONS [ENTRY] [SEED_FILE ...]
 *
 *   SEED       PRNG seed (printed at startup; deterministic run).
 *   ITERATIONS number of mutated-input comparisons.
 *   ENTRY      request | response | headers | all  (default all).
 *   SEED_FILE  corpus files to draw mutations from; classified by the
 *              "request"/"response"/"headers" directory in their path.
 *
 * Per iteration: pick a seed, apply 1-4 random mutations, choose a random
 * header cap from {0,1,2,3,5,16,64} and a random last_len in [0,len], then
 * compare every observable output of both implementations. On mismatch the
 * input is written to $FUZZ_CASE_DIR/fuzz-parse-case-N.bin (N = mismatch
 * ordinal). Stops after 5 mismatches with exit 1; exit 0 iff zero mismatches.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picohttpparser.h"

/* Oracle entry points (renamed at compile time; declared here by hand). */
int c_phr_parse_request(const char *buf, size_t len, const char **method, size_t *method_len,
                        const char **path, size_t *path_len, int *minor_version,
                        struct phr_header *headers, size_t *num_headers, size_t last_len);
int c_phr_parse_response(const char *buf, size_t len, int *minor_version, int *status,
                         const char **msg, size_t *msg_len, struct phr_header *headers,
                         size_t *num_headers, size_t last_len);
int c_phr_parse_headers(const char *buf, size_t len, struct phr_header *headers,
                        size_t *num_headers, size_t last_len);

#define MAXH 128      /* > max cap (64) + in-progress slot */
#define MAXLEN 65536  /* mutation working-buffer bound */
#define MAXSEEDS 512  /* per-pool corpus cap */
#define MAX_CASES 5   /* stop after this many mismatches */

static const size_t CAPS[] = {0, 1, 2, 3, 5, 16, 64};
#define NCAPS (sizeof(CAPS) / sizeof(CAPS[0]))

/* ---- deterministic PRNG: xorshift64star, splitmix-initialized ---------- */
static uint64_t xs_state;

static void xs_seed(uint64_t s)
{
    /* splitmix64-style avalanche so even seed 0 yields a nonzero state. */
    xs_state = s + UINT64_C(0x9E3779B97F4A7C15);
    xs_state = (xs_state ^ (xs_state >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    xs_state = (xs_state ^ (xs_state >> 27)) * UINT64_C(0x94D049BB133111EB);
    xs_state ^= xs_state >> 31;
}

static uint64_t xs_next(void)
{
    xs_state ^= xs_state >> 12;
    xs_state ^= xs_state << 25;
    xs_state ^= xs_state >> 27;
    return xs_state * UINT64_C(0x2545F4914F6CDD1D);
}

static size_t rnd_below(size_t n) { return n == 0 ? 0 : (size_t)(xs_next() % n); }

/* ---- seed pool ----------------------------------------------------------- */
typedef struct {
    unsigned char *data;
    size_t len;
    char name[256];
} Seed;

/* Pools by entry type: 0=request, 1=response, 2=headers. */
static Seed pool[3][MAXSEEDS];
static size_t pool_n[3];

static int classify(const char *path)
{
    if (strstr(path, "request") != NULL)
        return 0;
    if (strstr(path, "response") != NULL)
        return 1;
    if (strstr(path, "headers") != NULL)
        return 2;
    return -1;
}

static int load_seeds(char **paths, int npaths)
{
    int loaded = 0;
    int p;
    for (p = 0; p < npaths; p++) {
        FILE *fp = fopen(paths[p], "rb");
        long flen;
        int type;
        Seed *s;
        if (fp == NULL) {
            fprintf(stderr, "cannot open seed %s\n", paths[p]);
            return -1;
        }
        fseek(fp, 0, SEEK_END);
        flen = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (flen < 0 || flen > (long)MAXLEN) {
            fprintf(stderr, "seed %s: bad length %ld\n", paths[p], flen);
            fclose(fp);
            return -1;
        }
        type = classify(paths[p]);
        if (type < 0) {
            fclose(fp);
            continue; /* not one of our corpus trees */
        }
        if (pool_n[type] >= MAXSEEDS) {
            fprintf(stderr, "pool %d full (%d seeds); skipping %s\n", type, MAXSEEDS, paths[p]);
            fclose(fp);
            continue;
        }
        s = &pool[type][pool_n[type]];
        s->data = malloc((size_t)flen + 1);
        if (s->data == NULL) {
            fclose(fp);
            fprintf(stderr, "out of memory\n");
            return -1;
        }
        if (flen > 0 && fread(s->data, 1, (size_t)flen, fp) != (size_t)flen) {
            fclose(fp);
            fprintf(stderr, "short read %s\n", paths[p]);
            return -1;
        }
        s->len = (size_t)flen;
        snprintf(s->name, sizeof(s->name), "%s", paths[p]);
        pool_n[type]++;
        loaded++;
        fclose(fp);
    }
    return loaded;
}

/* ---- interesting byte pool ---------------------------------------------- */
static const unsigned char INTERESTING[] = {
    0x00, 0x01, 0x08, 0x09, 0x0a, 0x0b, 0x0d, 0x1a, 0x1f, /* controls, CR/LF/HT */
    0x20, 0x21, 0x22, 0x23, 0x27, 0x28, 0x29, 0x2a, 0x2c, 0x2f, /* space ! " # ' ( ) * , / */
    0x3a, 0x3b, 0x3d, 0x5b, 0x5c, 0x5d, 0x5f, 0x60, 0x7b, 0x7c, 0x7d, 0x7e, /* : ; = [ \ ] _ ` { | } ~ */
    0x7f, 0x80, 0x9f, 0xa0, 0xc3, 0xff /* DEL + high-bit bytes */
};
#define NINTERESTING (sizeof(INTERESTING) / sizeof(INTERESTING[0]))

static unsigned char rand_byte(void)
{
    if ((xs_next() & 1) != 0)
        return INTERESTING[rnd_below(NINTERESTING)];
    return (unsigned char)(xs_next() & 0xff);
}

/* ---- working buffer + mutation trace ------------------------------------ */
typedef struct {
    unsigned char buf[MAXLEN];
    size_t len;
    char mt[512]; /* mutation trace */
} Work;

static void mt_add(char *trace, size_t tracesz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void mt_add(char *trace, size_t tracesz, const char *fmt, ...)
{
    size_t used = strlen(trace);
    va_list ap;
    if (used + 1 >= tracesz)
        return;
    va_start(ap, fmt);
    used += (size_t)vsnprintf(trace + used, tracesz - used, fmt, ap);
    va_end(ap);
    /* vsnprintf returns the would-be length when truncated: used can now
       exceed the buffer. Only append the separator when both bytes fit. */
    if (used + 1 >= tracesz) {
        trace[tracesz - 1] = '\0';
        return;
    }
    trace[used] = ';';
    trace[used + 1] = '\0';
}

/* Insert srclen bytes from src into buf at pos (0..*len). Returns new len. */
static size_t insert_at(unsigned char *buf, size_t len, size_t pos, const unsigned char *src,
                        size_t srclen)
{
    if (len + srclen > MAXLEN)
        return len; /* bounded growth: drop the mutation */
    if (pos > len)
        pos = len;
    memmove(buf + pos + srclen, buf + pos, len - pos);
    memcpy(buf + pos, src, srclen);
    return len + srclen;
}

/* Apply one random mutation to work. */
static void mutate(Work *w, int selpool)
{
    unsigned char *b = w->buf;
    size_t len = w->len;
    int op = (int)rnd_below(6);

    switch (op) {
    case 0: { /* byte substitute */
        size_t pos;
        unsigned char v;
        if (len == 0) {
            mt_add(w->mt, sizeof(w->mt), "sub?empty");
            break;
        }
        pos = rnd_below(len);
        v = rand_byte();
        b[pos] = v;
        mt_add(w->mt, sizeof(w->mt), "sub(%llu,0x%02x)", (unsigned long long)pos, v);
        break;
    }
    case 1: { /* insert one byte */
        size_t pos = rnd_below(len + 1);
        unsigned char v = rand_byte();
        w->len = insert_at(b, len, pos, &v, 1);
        mt_add(w->mt, sizeof(w->mt), "ins(%llu,0x%02x)", (unsigned long long)pos, v);
        break;
    }
    case 2: { /* delete a span */
        size_t start, span;
        if (len == 0) {
            mt_add(w->mt, sizeof(w->mt), "del?empty");
            break;
        }
        start = rnd_below(len);
        span = 1 + rnd_below(len - start);
        memmove(b + start, b + start + span, len - start - span);
        w->len = len - span;
        mt_add(w->mt, sizeof(w->mt), "del(%llu,%llu)", (unsigned long long)start, (unsigned long long)span);
        break;
    }
    case 3: { /* truncate */
        size_t nl = rnd_below(len + 1);
        w->len = nl;
        mt_add(w->mt, sizeof(w->mt), "trunc(%llu)", (unsigned long long)nl);
        break;
    }
    case 4: { /* splice: head of current + tail of a random pool seed */
        const Seed *other;
        size_t cut, oq, tail, avail;
        if (pool_n[selpool] == 0) {
            mt_add(w->mt, sizeof(w->mt), "splice?nopool");
            break;
        }
        other = &pool[selpool][rnd_below(pool_n[selpool])];
        cut = rnd_below(len + 1);
        oq = rnd_below(other->len + 1);
        tail = other->len - oq;
        /* growth room is MAXLEN - len (the tail part [cut..len) is moved up
           by `tail`, so the final total is len + tail <= MAXLEN). */
        avail = MAXLEN - len;
        if (tail > avail)
            tail = avail;
        memmove(b + cut + tail, b + cut, len - cut);
        memcpy(b + cut, other->data + oq, tail);
        w->len = len + tail;
        mt_add(w->mt, sizeof(w->mt), "splice(cut%llu,o%llu,n%llu)", (unsigned long long)cut, (unsigned long long)oq, (unsigned long long)tail);
        break;
    }
    case 5: { /* duplicate a header-ish line */
        size_t i, lstart = 0, dlen;
        if (len == 0) {
            mt_add(w->mt, sizeof(w->mt), "dup?empty");
            break;
        }
        i = rnd_below(len);
        while (i < len && b[i] != '\n')
            i++;
        if (i < len) {
            size_t j = i;
            while (j > 0 && b[j - 1] != '\n')
                j--;
            lstart = j;
            dlen = i + 1 - j; /* includes trailing LF */
        } else {
            /* no LF anywhere: duplicate a small chunk */
            size_t maxc = len < 32 ? len : 32;
            lstart = rnd_below(len);
            if (lstart + maxc > len)
                maxc = len - lstart;
            dlen = 1 + rnd_below(maxc);
        }
        {
            size_t pos = rnd_below(len + 1);
            w->len = insert_at(b, len, pos, b + lstart, dlen);
        }
        mt_add(w->mt, sizeof(w->mt), "dup(%llu..%llu)", (unsigned long long)lstart, (unsigned long long)(lstart + dlen));
        break;
    }
    default:
        break;
    }
}

/* ---- full-state comparison (mirrors difftest per-entry logic) ------------ */
static size_t total_cases = 0;
static size_t mismatch_count = 0;

static int slots_equal(const struct phr_header *rh, size_t rn, const struct phr_header *ch,
                       size_t cn)
{
    size_t i, n = rn > cn ? rn : cn;
    for (i = 0; i <= n && i < MAXH; i++) {
        if (rh[i].name != ch[i].name || rh[i].name_len != ch[i].name_len ||
            rh[i].value != ch[i].value || rh[i].value_len != ch[i].value_len) {
            return 0;
        }
    }
    return 1;
}

static void dump_head(const unsigned char *b, size_t len)
{
    size_t i, n = len < 64 ? len : 64;
    printf("    input[%zu] hex:", n);
    for (i = 0; i < n; i++)
        printf(" %02x", b[i]);
    if (len > n)
        printf(" ...(%zu bytes total)", len);
    printf("\n");
}

static void write_case(int ordinal, const unsigned char *b, size_t len)
{
    const char *dir = getenv("FUZZ_CASE_DIR");
    char path[512];
    FILE *f;
    if (dir == NULL || *dir == '\0')
        dir = "target";
    snprintf(path, sizeof(path), "%s/fuzz-parse-case-%d.bin", dir, ordinal);
    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "could not write %s\n", path);
        return;
    }
    if (len > 0)
        fwrite(b, 1, len, f);
    fclose(f);
    printf("    repro written: %s\n", path);
}

/* Per-entry comparison helpers. Return 1 on mismatch. */
static int cmp_request(size_t iter, const char *seedname, size_t cap, size_t last_len,
                       const char *mt, const unsigned char *data, size_t len,
                       struct phr_header *rh, size_t rn, const char *rm, size_t rml, const char *rp,
                       size_t rpl, int rv, int rc, struct phr_header *ch, size_t cn, const char *cm,
                       size_t cml, const char *cp, size_t cpl, int cv, int cc)
{
    if (rc != cc || rv != cv || rn != cn || rml != cml || rpl != cpl || rm != cm || rp != cp ||
        !slots_equal(rh, rn, ch, cn)) {
        mismatch_count++;
        printf("MISMATCH entry=request iter=%" PRIu64 " seed=%s cap=%zu last_len=%zu mut=%s\n",
               (uint64_t)iter, seedname, cap, last_len, mt);
        printf("  rust: ret=%d ver=%d nhdr=%zu mlen=%zu plen=%zu\n", rc, rv, rn, rml, rpl);
        printf("  c:    ret=%d ver=%d nhdr=%zu mlen=%zu plen=%zu\n", cc, cv, cn, cml, cpl);
        dump_head(data, len);
        write_case((int)mismatch_count, data, len);
        return 1;
    }
    (void)len;
    return 0;
}

static int cmp_response(size_t iter, const char *seedname, size_t cap, size_t last_len,
                        const char *mt, const unsigned char *data, size_t len,
                        struct phr_header *rh, size_t rn, const char *rm, size_t rml, int rv,
                        int rs, int rc, struct phr_header *ch, size_t cn, const char *cm,
                        size_t cml, int cv, int cs, int cc)
{
    if (rc != cc || rv != cv || rs != cs || rml != cml || rm != cm || rn != cn ||
        !slots_equal(rh, rn, ch, cn)) {
        mismatch_count++;
        printf("MISMATCH entry=response iter=%" PRIu64 " seed=%s cap=%zu last_len=%zu mut=%s\n",
               (uint64_t)iter, seedname, cap, last_len, mt);
        printf("  rust: ret=%d ver=%d status=%d msglen=%zu nhdr=%zu\n", rc, rv, rs, rml, rn);
        printf("  c:    ret=%d ver=%d status=%d msglen=%zu nhdr=%zu\n", cc, cv, cs, cml, cn);
        dump_head(data, len);
        write_case((int)mismatch_count, data, len);
        return 1;
    }
    (void)len;
    return 0;
}

static int cmp_headers(size_t iter, const char *seedname, size_t cap, size_t last_len,
                       const char *mt, const unsigned char *data, size_t len,
                       struct phr_header *rh, size_t rn, int rc, struct phr_header *ch, size_t cn,
                       int cc)
{
    if (rc != cc || rn != cn || !slots_equal(rh, rn, ch, cn)) {
        mismatch_count++;
        printf("MISMATCH entry=headers iter=%" PRIu64 " seed=%s cap=%zu last_len=%zu mut=%s\n",
               (uint64_t)iter, seedname, cap, last_len, mt);
        printf("  rust: ret=%d nhdr=%zu\n", rc, rn);
        printf("  c:    ret=%d nhdr=%zu\n", cc, cn);
        dump_head(data, len);
        write_case((int)mismatch_count, data, len);
        return 1;
    }
    (void)len;
    return 0;
}

/* Run one entry pair on the given buffer with cap/last_len. */
static void run_entry(int entry, size_t iter, const char *seedname, size_t cap, size_t last_len,
                      const char *mt, const unsigned char *data, size_t len)
{
    struct phr_header rh[MAXH], ch[MAXH];
    memset(rh, 0xAA, sizeof(rh));
    memset(ch, 0xAA, sizeof(ch));
    if (entry == 0) {
        const char *rm = NULL, *rp = NULL, *cm = NULL, *cp = NULL;
        size_t rml = 0, rpl = 0, rnh = cap, cml = 0, cpl = 0, cnh = cap;
        int rv = -99, cv = -99, rc, cc;
        rc = phr_parse_request((const char *)data, len, &rm, &rml, &rp, &rpl, &rv, rh, &rnh,
                               last_len);
        cc = c_phr_parse_request((const char *)data, len, &cm, &cml, &cp, &cpl, &cv, ch, &cnh,
                                 last_len);
        total_cases++;
        cmp_request(iter, seedname, cap, last_len, mt, data, len, rh, rnh, rm, rml, rp, rpl, rv,
                    rc, ch, cnh, cm, cml, cp, cpl, cv, cc);
    } else if (entry == 1) {
        const char *rm = NULL, *cm = NULL;
        size_t rml = 0, rnh = cap, cml = 0, cnh = cap;
        int rv = -99, rs = -99, cv = -99, cs = -99, rc, cc;
        rc = phr_parse_response((const char *)data, len, &rv, &rs, &rm, &rml, rh, &rnh, last_len);
        cc = c_phr_parse_response((const char *)data, len, &cv, &cs, &cm, &cml, ch, &cnh, last_len);
        total_cases++;
        cmp_response(iter, seedname, cap, last_len, mt, data, len, rh, rnh, rm, rml, rv, rs, rc, ch,
                     cnh, cm, cml, cv, cs, cc);
    } else {
        size_t rnh = cap, cnh = cap;
        int rc, cc;
        rc = phr_parse_headers((const char *)data, len, rh, &rnh, last_len);
        cc = c_phr_parse_headers((const char *)data, len, ch, &cnh, last_len);
        total_cases++;
        cmp_headers(iter, seedname, cap, last_len, mt, data, len, rh, rnh, rc, ch, cnh, cc);
    }
}

int main(int argc, char **argv)
{
    uint64_t seed, iters;
    int entry = 3; /* 0 request, 1 response, 2 headers, 3 all */
    size_t i;
    int nloaded;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s SEED ITERATIONS [request|response|headers|all] [SEED_FILE ...]\n",
                argv[0]);
        return 2;
    }
    seed = strtoull(argv[1], NULL, 10);
    iters = strtoull(argv[2], NULL, 10);
    if (argc > 3) {
        if (strcmp(argv[3], "request") == 0)
            entry = 0;
        else if (strcmp(argv[3], "response") == 0)
            entry = 1;
        else if (strcmp(argv[3], "headers") == 0)
            entry = 2;
        else if (strcmp(argv[3], "all") == 0)
            entry = 3;
        else {
            fprintf(stderr, "unknown entry selector '%s'\n", argv[3]);
            return 2;
        }
    }
    xs_seed(seed);
    printf("PRNG seed=%" PRIu64 "\n", seed);
    printf("iterations=%" PRIu64 " entry=%s\n", iters,
           entry == 0 ? "request" : entry == 1 ? "response" : entry == 2 ? "headers" : "all");

    nloaded = load_seeds(argv + 4, argc - 4);
    if (nloaded < 0)
        return 2;
    if (nloaded == 0) {
        fprintf(stderr, "no seed files loaded (pass corpus files after the selector)\n");
        return 2;
    }
    printf("seeds_loaded=%d (request=%zu response=%zu headers=%zu)\n", nloaded, pool_n[0],
           pool_n[1], pool_n[2]);

    for (i = 0; i < (size_t)iters; i++) {
        Work w;
        int selpool;
        const Seed *base;
        int entry_here;
        size_t nmut, m, cap, last_len;
        const char *mt;

        /* choose entry and matching seed pool */
        if (entry == 3) {
            int order[3];
            size_t cnt = 0, k;
            for (k = 0; k < 3; k++) {
                if (pool_n[k] > 0)
                    order[cnt++] = (int)k;
            }
            if (cnt == 0) {
                fprintf(stderr, "no seeds for any entry\n");
                return 2;
            }
            selpool = order[rnd_below(cnt)];
        } else {
            selpool = entry;
        }
        if (pool_n[selpool] == 0) {
            fprintf(stderr, "no seeds for entry pool %d\n", selpool);
            return 2;
        }
        entry_here = selpool;
        base = &pool[selpool][rnd_below(pool_n[selpool])];

        memcpy(w.buf, base->data, base->len);
        w.len = base->len;
        w.mt[0] = '\0';

        /* 1-4 random mutations */
        nmut = 1 + rnd_below(4);
        for (m = 0; m < nmut; m++)
            mutate(&w, selpool);

        cap = CAPS[rnd_below(NCAPS)];
        last_len = rnd_below(w.len + 1);
        mt = w.mt[0] != '\0' ? w.mt : "none";
        run_entry(entry_here, i, base->name, cap, last_len, mt, w.buf, w.len);

        if (mismatch_count >= MAX_CASES)
            break;
    }

    printf("cases=%zu mismatches=%zu\n", total_cases, mismatch_count);
    return mismatch_count == 0 ? 0 : 1;
}
