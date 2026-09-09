/* Deterministic differential FUZZ driver: C oracle vs Rust chunked decoder.
 *
 * Reuses the proven comparison pattern of scripts/difftest_chunked.c: after
 * every call on both sides compare ret, *bufsz, is_in_data, the FULL decoder
 * struct (repr(C), memcmp) and the entire working buffer byte-for-byte
 * (fresh identical copies per call; the C oracle is linked with -D renames as
 * c_phr_decode_chunked*, the Rust cdylib provides the real phr_* symbols).
 *
 * Randomness: xorshift64star, seeded from argv[1] — fully deterministic, the
 * seed is printed at startup. argv[2] = iteration count.
 *
 * Seed pool: every file given on argv[3..] (the .sh passes the chunked
 * corpus). Per iteration a logical body is built from the pool:
 *   - 80%: a window of one random seed (whole seed when it fits MAX_BODY,
 *     random mid-buffer window otherwise — huge corpus files become
 *     mid-stream slices),
 *   - 20%: a splice of windows of two random seeds (concatenation),
 * then 1..12 random byte mutations (hex digits, ';', CR, LF, ' ', '\t',
 * NUL, DEL, high bytes, printables, controls, insertions, deletions,
 * truncations).
 *
 * The body is then decoded EITHER as a single call with a fresh decoder, OR
 * as a 2..4-call split at random (possibly duplicate -> zero-length) cut
 * points with the decoder carried across calls (exactly the streaming shape
 * difftest covers exhaustively for small files). consume_trailer is random
 * per iteration; ~1/8 of iterations start from a dirty-but-contract-valid
 * decoder (_state in 0..3 with plausible bytes_left/_hex_count; forged
 * states 4..7 as *initial* values are excluded because C aborts on corrupt
 * state — documented out of scope in docs/divergences.md row 2).
 *
 * On mismatch: prints seed/iteration/operation trace, dumps the body to
 * target/fuzz-chunked-case-N.bin, keeps going up to 5 mismatches, then exits
 * nonzero (exit 1 whenever mismatches > 0, 0 only when clean).
 *
 * Usage: fuzz_chunked <seed> <iters> <corpus-file>...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picohttpparser.h"

/* Oracle entry points (renamed at compile time, declared here by hand). */
ssize_t c_phr_decode_chunked(struct phr_chunked_decoder *decoder, char *buf, size_t *bufsz);
int c_phr_decode_chunked_is_in_data(struct phr_chunked_decoder *decoder);

#define MAX_BODY 4096
#define MAX_CALLS 4
#define MAX_SEEDS 64

typedef struct {
    const char *name;
    unsigned char *data;
    long len;
} Seed;

static Seed pool[MAX_SEEDS];
static int nseeds = 0;

static unsigned long long rng_state;
static unsigned long long rng_next(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}
/* Uniform-ish pick in [0, n). n == 0 is never passed. */
static unsigned long long rng_less(unsigned long long n)
{
    return rng_next() % n;
}

static unsigned char interesting_byte(void)
{
    /* The byte classes chunked grammar cares about + chaos. */
    static const unsigned char set[] = {
        '0', '5', 'a', 'f', 'A', 'F', ';', '\r', '\n', ' ', '\t', 'x', '=', ',', '\0', 0x7f,
    };
    return set[rng_less(sizeof(set))];
}

static unsigned char body[MAX_BODY];
static size_t body_len;

/* Copy a window of one seed into body[]. */
static void window_seed(int s)
{
    long len = pool[s].len;
    long off = 0;
    if (len > (long)MAX_BODY) {
        len = (long)MAX_BODY;
        off = (long)rng_less((unsigned long)(pool[s].len - len + 1));
    }
    memcpy(body, pool[s].data + off, (size_t)len);
    body_len = (size_t)len;
}

/* Copy a window of one seed into body at *dst, advancing *dst; returns bytes
 * added (capped so *dst never exceeds MAX_BODY). */
static size_t window_seed_at(int s, size_t *dst, size_t budget)
{
    long len = pool[s].len;
    long off = 0;
    if (len > (long)budget) {
        len = (long)budget;
        off = (long)rng_less((unsigned long)(pool[s].len - len + 1));
    }
    memcpy(body + *dst, pool[s].data + off, (size_t)len);
    *dst += (size_t)len;
    return (size_t)len;
}

/* 80% one-seed window, 20% splice of two seeds' windows. */
static void build_body(void)
{
    if (nseeds <= 0) {
        fprintf(stderr, "no seed files\n");
        exit(2);
    }
    if (rng_less(10) < 2 && nseeds >= 2) {
        size_t dst = 0;
        int a = (int)rng_less((unsigned)nseeds);
        int b = (int)rng_less((unsigned)nseeds);
        size_t budget = MAX_BODY / 2;
        window_seed_at(a, &dst, budget);
        window_seed_at(b, &dst, MAX_BODY - dst);
        body_len = dst;
    } else {
        int s = (int)rng_less((unsigned)nseeds);
        window_seed(s);
    }
    /* 1..12 random mutations over the working body. */
    unsigned long long k, nmut = 1 + rng_less(12);
    for (k = 0; k < nmut; k++) {
        if (body_len == 0) {
            /* Nothing to mutate in place; an insert is always legal. */
            if (body_len >= MAX_BODY)
                break;
            memmove(body + 1, body, body_len);
            body[0] = interesting_byte();
            body_len += 1;
            continue;
        }
        switch (rng_less(15)) {
        case 0: /* set to grammar-relevant byte */
        case 1:
            body[rng_less(body_len)] = interesting_byte();
            break;
        case 2: /* random printable */
            body[rng_less(body_len)] = (unsigned char)(0x20 + rng_less(0x5f));
            break;
        case 3: /* random control */
            body[rng_less(body_len)] = (unsigned char)rng_less(0x20);
            break;
        case 4: /* high byte */
            body[rng_less(body_len)] = (unsigned char)(0x80 + rng_less(0x80));
            break;
        case 5: /* hex digit */
            body[rng_less(body_len)] =
                (unsigned char)"0123456789abcdefABCDEF"[rng_less(22)];
            break;
        case 6: /* CR */
            body[rng_less(body_len)] = '\r';
            break;
        case 7: /* LF */
            body[rng_less(body_len)] = '\n';
            break;
        case 8: /* BWS */
            body[rng_less(body_len)] = rng_less(2) ? ' ' : '\t';
            break;
        case 9: /* NUL or DEL */
            body[rng_less(body_len)] = rng_less(2) ? '\0' : 0x7f;
            break;
        case 10: /* delete a byte */
            if (body_len > 1) {
                size_t at = (size_t)rng_less(body_len);
                memmove(body + at, body + at + 1, body_len - at - 1);
                body_len -= 1;
            }
            break;
        case 11: /* insert a grammar byte */
            if (body_len < MAX_BODY) {
                size_t at = (size_t)rng_less(body_len + 1);
                memmove(body + at + 1, body + at, body_len - at);
                body[at] = interesting_byte();
                body_len += 1;
            }
            break;
        case 12: { /* insert a hex run */
            size_t run = 1 + (size_t)rng_less(8);
            if (run > MAX_BODY - body_len)
                run = MAX_BODY - body_len;
            size_t at = (size_t)rng_less(body_len + 1);
            memmove(body + at + run, body + at, body_len - at);
            size_t i;
            for (i = 0; i < run; i++)
                body[at + i] = (unsigned char)"0123456789abcdef"[rng_less(16)];
            body_len += run;
            break;
        }
        case 13: /* truncate */
            if (body_len > 1)
                body_len = 1 + (size_t)rng_less(body_len);
            break;
        default: /* 14: splice a fresh random window onto the tail */
            if (body_len < MAX_BODY) {
                int s = (int)rng_less((unsigned)nseeds);
                size_t budget = MAX_BODY - body_len;
                size_t before = body_len;
                window_seed_at(s, &body_len, budget);
                if (body_len == before && body_len < MAX_BODY && pool[s].len > 0) {
                    /* zero-length window (empty seed): force one byte */
                    body[body_len++] = interesting_byte();
                }
            }
            break;
        }
    }
}

typedef struct {
    ssize_t ret;
    size_t out_len;
    int in_data;
    struct phr_chunked_decoder dec;
} CallCmp;

static void fresh_dec(struct phr_chunked_decoder *d, int trailer)
{
    memset(d, 0, sizeof(*d));
    d->consume_trailer = (char)trailer;
}

/* Apply a dirty-but-valid initial decoder state identically to both sides.
 * _state is forced into 0..3 (the machine states a zero-init decoder can
 * legitimately be in); bytes_left/_hex_count are only read in specific
 * states by both implementations, so arbitrary values there are "defined"
 * input, exactly as in difftest_chunked.c's dirty matrix. */
static void dirty_dec(struct phr_chunked_decoder *dr, struct phr_chunked_decoder *dc)
{
    int st = (int)rng_less(4); /* 0..3 */
    unsigned char left = 0;
    unsigned char hex = 0;
    switch (st) {
    case 0: /* IN_CHUNK_SIZE: bytes_left accumulates; hex_count gates at 16 */
        left = (unsigned char)rng_less(256);
        hex = (unsigned char)rng_less(17);
        break;
    case 1: /* IN_CHUNK_EXT / case 2 EXPECT_LF: fields unused */
    case 2:
        left = 0;
        hex = 0;
        break;
    case 3: /* IN_CHUNK_DATA: bytes_left drives the data path */
        left = (unsigned char)rng_less(256);
        hex = 0;
        break;
    }
    dr->_state = dc->_state = (char)st;
    dr->bytes_left_in_chunk = dc->bytes_left_in_chunk = (size_t)left;
    dr->_hex_count = dc->_hex_count = (char)hex;
}

static size_t mismatch_count = 0;
static size_t call_count = 0;
static unsigned long long iteration = 0;
static unsigned long long total_iters = 0;
static unsigned long long seed_value = 0;
static unsigned char *body_off[MAX_CALLS];
static size_t body_part[MAX_CALLS];
static int ncalls_of_iter = 0;

static void print_dec(const char *side, const CallCmp *o)
{
    printf("    %s ret=%d out=%zu in_data=%d left=%zu hex=%d st=%d rd=%llu oh=%llu\n",
           side, (int)o->ret, o->out_len, o->in_data, o->dec.bytes_left_in_chunk,
           (int)o->dec._hex_count, (int)o->dec._state, (unsigned long long)o->dec._total_read,
           (unsigned long long)o->dec._total_overhead);
}

static void report_mismatch(int call_index, int trailer, int dirty, CallCmp *r, CallCmp *c,
                            const unsigned char *buf, size_t len)
{
    size_t i;
    mismatch_count++;
    printf("MISMATCH seed=%llu iteration=%llu calls=%d call=%d trailer=%d dirty=%d "
           "body_len=%zu\n",
           seed_value, iteration, ncalls_of_iter, call_index, trailer, dirty, body_len);
    printf("    seed_pool=%d files", nseeds);
    for (i = 0; i < (size_t)nseeds; i++)
        printf(" %s", pool[i].name);
    printf("\n    parts:");
    for (i = 0; i < (size_t)ncalls_of_iter; i++)
        printf(" [%td,+%zu]", (ptrdiff_t)(body_off[i] - body), body_part[i]);
    printf("\n    this_call_slice=[%td,+%zu]\n", (ptrdiff_t)(body_off[call_index] - body), len);
    print_dec("rust", r);
    print_dec("c   ", c);
    /* Dump the FULL logical body of this iteration so the case can be
     * replayed (same seed + iteration reruns deterministically; the file is
     * the reproduction). */
    {
        char path[256];
        FILE *f;
        (void)buf;
        (void)len;
        snprintf(path, sizeof(path), "target/fuzz-chunked-case-%zu.bin", mismatch_count);
        f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(body, 1, body_len, f);
            fclose(f);
            printf("    dumped=%s (%zu bytes)\n", path, body_len);
        } else {
            printf("    dump FAILED (%s)\n", path);
        }
    }
}

/* One streaming step over a slice of the body. Fresh buffers for both sides,
 * carried decoder state in/out, identical to difftest_chunked.c::step plus
 * call-context reporting. Returns 1 on mismatch. */
static int step(int call_index, int trailer, int dirty, const unsigned char *data, size_t len,
                struct phr_chunked_decoder *dec_r, struct phr_chunked_decoder *dec_c)
{
    char *br = malloc(len + 1);
    char *bc = malloc(len + 1);
    CallCmp r, c;
    int bad = 0;
    size_t out_len_r, out_len_c;
    if (!br || !bc) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    memcpy(br, data, len);
    memcpy(bc, data, len);
    (void)trailer;
    (void)dirty;
    call_count++;
    out_len_r = len;
    out_len_c = len;
    r.ret = phr_decode_chunked(dec_r, br, &out_len_r);
    r.in_data = phr_decode_chunked_is_in_data(dec_r);
    r.out_len = out_len_r;
    r.dec = *dec_r;
    c.ret = c_phr_decode_chunked(dec_c, bc, &out_len_c);
    c.in_data = c_phr_decode_chunked_is_in_data(dec_c);
    c.out_len = out_len_c;
    c.dec = *dec_c;
    if (r.ret != c.ret || r.out_len != c.out_len || r.in_data != c.in_data ||
        memcmp(&r.dec, &c.dec, sizeof(r.dec)) != 0 || memcmp(br, bc, len) != 0) {
        report_mismatch(call_index, trailer, dirty, &r, &c, data, len);
        bad = 1;
    }
    free(br);
    free(bc);
    return bad;
}

int main(int argc, char **argv)
{
    int i;
    unsigned long long iter;
    int trailer;
    if (argc < 4) {
        fprintf(stderr, "usage: %s <seed> <iters> <corpus-file>...\n", argv[0]);
        return 2;
    }
    seed_value = strtoull(argv[1], NULL, 0);
    total_iters = strtoull(argv[2], NULL, 0);
    rng_state = seed_value ? seed_value : 0x9E3779B97F4A7C15ULL; /* never seed 0 */
    if (rng_state == 0)
        rng_state = 1;
    nseeds = 0;
    for (i = 3; i < argc && nseeds < MAX_SEEDS; i++) {
        FILE *fp = fopen(argv[i], "rb");
        long len;
        unsigned char *data;
        if (!fp) {
            fprintf(stderr, "cannot open seed %s\n", argv[i]);
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
            fprintf(stderr, "short read %s\n", argv[i]);
            return 2;
        }
        fclose(fp);
        pool[nseeds].name = argv[i];
        pool[nseeds].data = data;
        pool[nseeds].len = len;
        nseeds++;
    }
    printf("SEED %llu (0x%llx) ITERS %llu SEED_FILES %d\n", seed_value, seed_value,
           total_iters, nseeds);
    for (iter = 0; iter < total_iters; iter++) {
        struct phr_chunked_decoder dr, dc;
        size_t cuts[MAX_CALLS - 1];
        unsigned long long split;
        int ncall, j, dirty;
        size_t prev;
        iteration = iter;
        build_body();
        trailer = (int)rng_less(2);
        dirty = (int)rng_less(8) == 0; /* ~1/8 */
        /* Decide single call vs split; both reuse the same body. */
        split = rng_less(10);
        if (split < 5 || body_len == 0) {
            /* single call (fresh decoder; body_len == 0 forces this path) */
            fresh_dec(&dr, trailer);
            fresh_dec(&dc, trailer);
            if (dirty) {
                dirty_dec(&dr, &dc);
            }
            ncalls_of_iter = 1;
            body_off[0] = body;
            body_part[0] = body_len;
            if (step(0, trailer, dirty, body, body_len, &dr, &dc)) {
                if (mismatch_count >= 5)
                    goto done;
            }
        } else {
            /* 2..4 calls at random cut points (duplicates -> zero-length
             * parts are allowed and valuable: EOF-in-state coverage). */
            ncall = 2 + (int)rng_less(3);
            for (j = 0; j < ncall - 1; j++)
                cuts[j] = (size_t)rng_less(body_len + 1);
            /* insertion sort */
            for (j = 1; j < ncall - 1; j++) {
                size_t key = cuts[j];
                int k = j - 1;
                while (k >= 0 && cuts[k] > key) {
                    cuts[k + 1] = cuts[k];
                    k--;
                }
                cuts[k + 1] = key;
            }
            fresh_dec(&dr, trailer);
            fresh_dec(&dc, trailer);
            if (dirty)
                dirty_dec(&dr, &dc);
            ncalls_of_iter = ncall;
            prev = 0;
            for (j = 0; j < ncall; j++) {
                size_t end = (j < ncall - 1) ? cuts[j] : body_len;
                body_off[j] = body + prev;
                body_part[j] = end - prev;
                if (step(j, trailer, dirty, body + prev, end - prev, &dr, &dc)) {
                    if (mismatch_count >= 5)
                        goto done;
                }
                prev = end;
            }
        }
    }
done:
    printf("fuzz done seed=%llu iters=%llu calls=%zu mismatches=%zu\n", seed_value,
           total_iters, call_count, mismatch_count);
    if (mismatch_count > 0)
        return 1;
    return 0;
}
