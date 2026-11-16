/*
 * bits.c — Utilidades de bits. Ver bits.h.
 */
#include "bits.h"

void bits_pack_msb(const uint8_t *bits, int n, uint8_t *out)
{
    for (int i = 0; i < (n + 7) / 8; i++)
        out[i] = 0;
    for (int i = 0; i < n; i++)
        if (bits[i])
            out[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
}

void bits_pack_lsb(const uint8_t *bits, int n, uint8_t *out)
{
    for (int i = 0; i < (n + 7) / 8; i++)
        out[i] = 0;
    for (int i = 0; i < n; i++)
        if (bits[i])
            out[i >> 3] |= (uint8_t)(1 << (i & 7));
}

void bits_unpack_msb(const uint8_t *in, int n, uint8_t *bits)
{
    for (int i = 0; i < n; i++)
        bits[i] = (in[i >> 3] >> (7 - (i & 7))) & 1;
}

void bits_unpack_lsb(const uint8_t *in, int n, uint8_t *bits)
{
    for (int i = 0; i < n; i++)
        bits[i] = (in[i >> 3] >> (i & 7)) & 1;
}

uint32_t bits_get(const uint8_t *m, int first, int last)
{
    uint32_t v = 0;
    for (int b = first; b <= last; b++) {
        int i = b - 1;
        v = (v << 1) | ((m[i >> 3] >> (7 - (i & 7))) & 1);
    }
    return v;
}

void bits_set(uint8_t *m, int first, int last, uint32_t value)
{
    for (int b = last; b >= first; b--, value >>= 1) {
        int i = b - 1;
        uint8_t mask = (uint8_t)(0x80 >> (i & 7));
        if (value & 1)
            m[i >> 3] |= mask;
        else
            m[i >> 3] &= (uint8_t)~mask;
    }
}

void bits_flip(uint8_t *m, int i) { m[i >> 3] ^= (uint8_t)(0x80 >> (i & 7)); }

uint8_t bits_reverse8(uint8_t v)
{
    v = (uint8_t)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
    v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
    v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
    return v;
}

uint32_t bits_reverse(uint32_t v, int nbits)
{
    uint32_t r = 0;
    for (int i = 0; i < nbits; i++, v >>= 1)
        r = (r << 1) | (v & 1);
    return r;
}

int bits_popcount32(uint32_t v) { return __builtin_popcount(v); }
int bits_popcount64(uint64_t v) { return __builtin_popcountll(v); }
int bits_parity32(uint32_t v) { return __builtin_parity(v); }

int bits_popcount_buf(const uint8_t *buf, int nbytes)
{
    int c = 0;
    for (int i = 0; i < nbytes; i++)
        c += __builtin_popcount(buf[i]);
    return c;
}

int bits_hamming_dist(const uint8_t *a, const uint8_t *b, int nbytes)
{
    int c = 0;
    for (int i = 0; i < nbytes; i++)
        c += __builtin_popcount((unsigned)(a[i] ^ b[i]));
    return c;
}

int bits_find_sync(const uint8_t *bits, int n, uint64_t pattern, int pat_len, int max_err, int start)
{
    if (pat_len < 1 || pat_len > 64 || start < 0)
        return -1;
    uint64_t mask = pat_len == 64 ? ~0ULL : ((1ULL << pat_len) - 1);
    uint64_t w = 0;
    /* ventana deslizante de pat_len bits; el bit más antiguo es el MSB */
    for (int i = start; i < n; i++) {
        w = ((w << 1) | (bits[i] & 1)) & mask;
        if (i - start + 1 >= pat_len && __builtin_popcountll((w ^ pattern) & mask) <= max_err)
            return i - pat_len + 1;
    }
    return -1;
}

int nrzi_decode(const uint8_t *in, int n, uint8_t *out, int prev, int one_changes)
{
    for (int i = 0; i < n; i++) {
        int cur = in[i] & 1;
        int changed = cur != prev;
        out[i] = (uint8_t)(one_changes ? changed : !changed);
        prev = cur;
    }
    return prev;
}

int nrzi_encode(const uint8_t *in, int n, uint8_t *out, int prev, int one_changes)
{
    for (int i = 0; i < n; i++) {
        int change = one_changes ? (in[i] & 1) : !(in[i] & 1);
        prev ^= change;
        out[i] = (uint8_t)prev;
    }
    return prev;
}

/* ======================================================================= */
/*  LFSR                                                                   */
/* ======================================================================= */
void lfsr_init(lfsr_t *l, int order, uint32_t taps, uint32_t init_state, lfsr_mode_t mode)
{
    uint32_t mask = order >= 32 ? 0xFFFFFFFFu : ((1u << order) - 1);
    l->order = order;
    l->taps = taps & mask;
    l->state = init_state & mask;
    l->mode = mode;
}

void lfsr_init_wifi(lfsr_t *l, uint32_t seed) { lfsr_init(l, 7, 0x48, seed ? seed : 0x7F, LFSR_ADDITIVE); }
void lfsr_init_dvb(lfsr_t *l)
{
    /* Etapas 1..15 = 1,0,0,1,0,1,0,1,0,0,0,0,0,0,0; la etapa k es el bit k-1. */
    lfsr_init(l, 15, 0x6000, 0x0A9, LFSR_ADDITIVE);
}
void lfsr_init_dab(lfsr_t *l) { lfsr_init(l, 9, 0x110, 0x1FF, LFSR_ADDITIVE); }
void lfsr_init_g3ruh(lfsr_t *l) { lfsr_init(l, 17, 0x10800, 0, LFSR_MULTIPLICATIVE); }

static inline void lfsr_shift(lfsr_t *l, int bit)
{
    l->state = (l->state << 1) | (uint32_t)bit;
    if (l->order < 32)
        l->state &= (1u << l->order) - 1;
}

int lfsr_prbs_bit(lfsr_t *l)
{
    int p = __builtin_parity(l->state & l->taps);
    lfsr_shift(l, p);
    return p;
}

int lfsr_bit(lfsr_t *l, int in, int descramble)
{
    in &= 1;
    int fb = __builtin_parity(l->state & l->taps);
    if (l->mode == LFSR_ADDITIVE) {
        lfsr_shift(l, fb);
        return in ^ fb;
    }
    int out = in ^ fb;
    lfsr_shift(l, descramble ? in : out);
    return out;
}

void lfsr_process(lfsr_t *l, const uint8_t *in, int n, uint8_t *out, int descramble)
{
    for (int i = 0; i < n; i++)
        out[i] = (uint8_t)lfsr_bit(l, in[i], descramble);
}

void lfsr_process_bytes(lfsr_t *l, const uint8_t *in, int nbytes, uint8_t *out, int descramble, int msb_first)
{
    for (int i = 0; i < nbytes; i++) {
        uint8_t v = in[i], o = 0;
        for (int b = 0; b < 8; b++) {
            int sh = msb_first ? 7 - b : b;
            o |= (uint8_t)(lfsr_bit(l, (v >> sh) & 1, descramble) << sh);
        }
        out[i] = o;
    }
}
