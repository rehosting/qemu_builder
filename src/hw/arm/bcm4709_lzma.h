/*
 * Minimal LZMA ("LZMA alone", .lzma) decoder for the BCM47094 board's
 * CFE-style flash boot: CFE stores the kernel in TRX partition 0 as an LZMA
 * stream and decompresses it to 0x8000. Written from the LZMA format
 * description; decodes into one flat buffer (the whole output is the
 * dictionary), so there is no window wraparound.
 */
#ifndef HW_ARM_BCM4709_LZMA_H
#define HW_ARM_BCM4709_LZMA_H

#define NS_LZ_PROB_BITS     11
#define NS_LZ_PROB_INIT     (1u << (NS_LZ_PROB_BITS - 1))
#define NS_LZ_STATES        12
#define NS_LZ_POS_MAX       16              /* 1 << kNumPosBitsMax */
#define NS_LZ_END_POS_MODEL 14
#define NS_LZ_FULL_DIST     128

typedef struct NSLzLen {
    uint16_t choice, choice2;
    uint16_t low[NS_LZ_POS_MAX][8], mid[NS_LZ_POS_MAX][8], high[256];
} NSLzLen;

typedef struct NSLz {
    const uint8_t *in;
    size_t in_pos, in_len;
    uint32_t range, code;
    bool bad;
    uint16_t is_match[NS_LZ_STATES * NS_LZ_POS_MAX];
    uint16_t is_rep[NS_LZ_STATES], is_rep_g0[NS_LZ_STATES];
    uint16_t is_rep_g1[NS_LZ_STATES], is_rep_g2[NS_LZ_STATES];
    uint16_t is_rep0_long[NS_LZ_STATES * NS_LZ_POS_MAX];
    uint16_t pos_slot[4][64];
    uint16_t pos[1 + NS_LZ_FULL_DIST - NS_LZ_END_POS_MODEL];
    uint16_t align[16];
    NSLzLen len, rep_len;
    uint16_t *lit;
} NSLz;

static inline uint8_t ns_lz_byte(NSLz *z)
{
    if (z->in_pos >= z->in_len) {
        z->bad = true;
        return 0;
    }
    return z->in[z->in_pos++];
}

static inline void ns_lz_norm(NSLz *z)
{
    if (z->range < (1u << 24)) {
        z->range <<= 8;
        z->code = (z->code << 8) | ns_lz_byte(z);
    }
}

static inline unsigned ns_lz_bit(NSLz *z, uint16_t *p)
{
    uint32_t bound = (z->range >> NS_LZ_PROB_BITS) * *p;
    unsigned b;

    if (z->code < bound) {
        z->range = bound;
        *p += ((1u << NS_LZ_PROB_BITS) - *p) >> 5;
        b = 0;
    } else {
        z->range -= bound;
        z->code -= bound;
        *p -= *p >> 5;
        b = 1;
    }
    ns_lz_norm(z);
    return b;
}

static unsigned ns_lz_direct(NSLz *z, unsigned n)
{
    unsigned r = 0;

    while (n--) {
        uint32_t t;

        z->range >>= 1;
        z->code -= z->range;
        t = 0 - (z->code >> 31);
        z->code += z->range & t;
        if (z->code == z->range) {
            z->bad = true;
        }
        ns_lz_norm(z);
        r = (r << 1) + (t + 1);
    }
    return r;
}

static unsigned ns_lz_tree(NSLz *z, uint16_t *p, unsigned bits)
{
    unsigned m = 1, i;

    for (i = 0; i < bits; i++) {
        m = (m << 1) + ns_lz_bit(z, &p[m]);
    }
    return m - (1u << bits);
}

static unsigned ns_lz_rtree(NSLz *z, uint16_t *p, unsigned bits)
{
    unsigned m = 1, sym = 0, i;

    for (i = 0; i < bits; i++) {
        unsigned b = ns_lz_bit(z, &p[m]);

        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

static unsigned ns_lz_len(NSLz *z, NSLzLen *l, unsigned pos_state)
{
    if (!ns_lz_bit(z, &l->choice)) {
        return ns_lz_tree(z, l->low[pos_state], 3);
    }
    if (!ns_lz_bit(z, &l->choice2)) {
        return 8 + ns_lz_tree(z, l->mid[pos_state], 3);
    }
    return 16 + ns_lz_tree(z, l->high, 8);
}

static uint32_t ns_lz_dist(NSLz *z, unsigned len)
{
    unsigned st = len < 3 ? len : 3;
    unsigned slot = ns_lz_tree(z, z->pos_slot[st], 6);
    unsigned nbits;
    uint32_t dist;

    if (slot < 4) {
        return slot;
    }
    nbits = (slot >> 1) - 1;
    dist = (2 | (slot & 1)) << nbits;
    if (slot < NS_LZ_END_POS_MODEL) {
        return dist + ns_lz_rtree(z, z->pos + dist - slot, nbits);
    }
    dist += ns_lz_direct(z, nbits - 4) << 4;
    return dist + ns_lz_rtree(z, z->align, 4);
}

static void ns_lz_init_probs(uint16_t *p, size_t n)
{
    while (n--) {
        *p++ = NS_LZ_PROB_INIT;
    }
}

/*
 * Decode an .lzma stream (13-byte header: properties, dictionary size,
 * 64-bit uncompressed size). The size must be known and at most max_out;
 * an end marker, if present, ends the stream early. On success returns a
 * g_malloc'd buffer of *out_len bytes; on failure NULL and *err.
 */
static uint8_t *ns_lzma_alone_decode(const uint8_t *in, size_t in_len,
                                     size_t max_out, size_t *out_len,
                                     const char **err)
{
    NSLz *z;
    uint8_t *out;
    uint64_t size = 0;
    size_t op = 0;
    unsigned lc, lp, pb, d, state = 0, i;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;

    if (in_len < 13 + 5) {
        *err = "stream too short";
        return NULL;
    }
    d = in[0];
    if (d >= 9 * 5 * 5) {
        *err = "bad properties byte";
        return NULL;
    }
    lc = d % 9;
    d /= 9;
    lp = d % 5;
    pb = d / 5;
    for (i = 0; i < 8; i++) {
        size |= (uint64_t)in[5 + i] << (8 * i);
    }
    if (size == UINT64_MAX || size > max_out) {
        *err = "uncompressed size unknown or too large";
        return NULL;
    }

    z = g_new0(NSLz, 1);
    z->in = in;
    z->in_pos = 13;
    z->in_len = in_len;
    z->lit = g_new(uint16_t, 0x300u << (lc + lp));
    ns_lz_init_probs(z->lit, 0x300u << (lc + lp));
    ns_lz_init_probs(z->is_match, sizeof(z->is_match) / 2);
    ns_lz_init_probs(z->is_rep, NS_LZ_STATES);
    ns_lz_init_probs(z->is_rep_g0, NS_LZ_STATES);
    ns_lz_init_probs(z->is_rep_g1, NS_LZ_STATES);
    ns_lz_init_probs(z->is_rep_g2, NS_LZ_STATES);
    ns_lz_init_probs(z->is_rep0_long, sizeof(z->is_rep0_long) / 2);
    ns_lz_init_probs(&z->pos_slot[0][0], sizeof(z->pos_slot) / 2);
    ns_lz_init_probs(z->pos, sizeof(z->pos) / 2);
    ns_lz_init_probs(z->align, 16);
    ns_lz_init_probs(&z->len.choice, sizeof(NSLzLen) / 2);
    ns_lz_init_probs(&z->rep_len.choice, sizeof(NSLzLen) / 2);

    if (ns_lz_byte(z) != 0) {
        z->bad = true;
    }
    z->range = 0xffffffff;
    for (i = 0; i < 4; i++) {
        z->code = (z->code << 8) | ns_lz_byte(z);
    }

    out = g_malloc(size ? size : 1);
    while (op < size && !z->bad) {
        unsigned ps = op & ((1u << pb) - 1);
        unsigned len;

        if (!ns_lz_bit(z, &z->is_match[(state << 4) + ps])) {
            unsigned prev = op ? out[op - 1] : 0;
            unsigned ls = ((op & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc));
            uint16_t *p = z->lit + 0x300 * ls;
            unsigned sym = 1;

            if (state >= 7) {
                unsigned mb = out[op - rep0 - 1];

                do {
                    unsigned mbit = (mb >> 7) & 1, b;

                    mb <<= 1;
                    b = ns_lz_bit(z, &p[((1 + mbit) << 8) + sym]);
                    sym = (sym << 1) | b;
                    if (mbit != b) {
                        break;
                    }
                } while (sym < 0x100);
            }
            while (sym < 0x100) {
                sym = (sym << 1) | ns_lz_bit(z, &p[sym]);
            }
            out[op++] = sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }

        if (ns_lz_bit(z, &z->is_rep[state])) {
            if (op == 0) {
                z->bad = true;
                break;
            }
            if (!ns_lz_bit(z, &z->is_rep_g0[state])) {
                if (!ns_lz_bit(z, &z->is_rep0_long[(state << 4) + ps])) {
                    state = state < 7 ? 9 : 11;
                    out[op] = out[op - rep0 - 1];
                    op++;
                    continue;
                }
            } else {
                uint32_t dist;

                if (!ns_lz_bit(z, &z->is_rep_g1[state])) {
                    dist = rep1;
                } else {
                    if (!ns_lz_bit(z, &z->is_rep_g2[state])) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = ns_lz_len(z, &z->rep_len, ps);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = ns_lz_len(z, &z->len, ps);
            state = state < 7 ? 7 : 10;
            rep0 = ns_lz_dist(z, len);
            if (rep0 == 0xffffffff) {
                break;                          /* end marker */
            }
        }
        if (rep0 >= op) {
            z->bad = true;
            break;
        }
        for (len += 2; len && op < size; len--, op++) {
            out[op] = out[op - rep0 - 1];
        }
    }

    if (z->bad || op != size) {
        *err = z->bad ? "corrupt LZMA data" : "stream ended before its size";
        g_free(z->lit);
        g_free(z);
        g_free(out);
        return NULL;
    }
    g_free(z->lit);
    g_free(z);
    *out_len = op;
    return out;
}

#endif
