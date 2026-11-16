/*
 * dvbt_vit.c — Viterbi K=7 rápido (ver dvbt_vit.h).
 */
#include "dvbt_vit.h"

#include <stdlib.h>
#include <string.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

#define TB 128                 /* profundidad de recorrido (≥ 5·K y suficiente para 7/8) */
#define BLK 128                /* bits decididos por recorrido */
#define CAP (TB + BLK)

struct dvbt_vit {
    int16_t pm[64] __attribute__((aligned(32)));
    uint64_t dec[CAP];
    int n;                     /* decisiones almacenadas */
    int since_norm;
    int16_t sg0[32] __attribute__((aligned(32))), sg1[32] __attribute__((aligned(32)));
};

dvbt_vit_t *dvbt_vit_create(void)
{
    dvbt_vit_t *v = aligned_alloc(64, (sizeof *v + 63) & ~(size_t)63);
    if (!v)
        return NULL;
    memset(v, 0, sizeof *v);
    for (int j = 0; j < 32; j++) {
        v->sg0[j] = __builtin_parity((unsigned)(2 * j) & 0x79u) ? -1 : 1;
        v->sg1[j] = __builtin_parity((unsigned)(2 * j) & 0x5Bu) ? -1 : 1;
    }
    return v;
}

void dvbt_vit_destroy(dvbt_vit_t *v) { free(v); }

void dvbt_vit_reset(dvbt_vit_t *v)
{
    memset(v->pm, 0, sizeof v->pm);
    v->n = 0;
    v->since_norm = 0;
}

/* Recorrido hacia atrás desde el mejor estado final; emite los BLK bits más antiguos. */
static void traceback(dvbt_vit_t *v, uint8_t *out)
{
    int best = 0;
    int16_t bm = v->pm[0];
    for (int s = 1; s < 64; s++)
        if (v->pm[s] > bm) {
            bm = v->pm[s];
            best = s;
        }
    unsigned s = (unsigned)best;
    uint64_t w[BLK / 64] = {0, 0};
    for (int t = CAP - 1; t >= 0; t--) {
        if (t < BLK) {
            /* bit decodificado = bit 5 del estado alcanzado en el paso t */
            uint64_t b = (s >> 5) & 1;
            w[t >> 6] |= b << (63 - (t & 63));
        }
        s = ((s << 1) & 63u) | (unsigned)((v->dec[t] >> s) & 1u);
    }
    for (int i = 0; i < BLK / 64; i++)
        for (int k = 0; k < 8; k++)
            out[i * 8 + k] = (uint8_t)(w[i] >> (56 - 8 * k));
    memmove(v->dec, v->dec + BLK, sizeof(uint64_t) * TB);
    v->n = TB;
}

#ifdef __AVX2__
/* Elementos pares / impares de (a, b) concatenados, en orden. */
static inline __m256i even16(__m256i a, __m256i b)
{
    __m256i ea = _mm256_srai_epi32(_mm256_slli_epi32(a, 16), 16);
    __m256i eb = _mm256_srai_epi32(_mm256_slli_epi32(b, 16), 16);
    return _mm256_permute4x64_epi64(_mm256_packs_epi32(ea, eb), 0xD8);
}
static inline __m256i odd16(__m256i a, __m256i b)
{
    return _mm256_permute4x64_epi64(_mm256_packs_epi32(_mm256_srai_epi32(a, 16), _mm256_srai_epi32(b, 16)), 0xD8);
}
static inline uint32_t mask16(__m256i a, __m256i b)
{
    return (uint32_t)_mm256_movemask_epi8(_mm256_permute4x64_epi64(_mm256_packs_epi16(a, b), 0xD8));
}

int dvbt_vit_push(dvbt_vit_t *v, const int8_t *soft, int nsteps, uint8_t *out)
{
    int nout = 0;
    /* estados pares/impares: E = pm[2j], O = pm[2j+1] */
    __m256i pm0 = _mm256_load_si256((const __m256i *)(v->pm)), pm1 = _mm256_load_si256((const __m256i *)(v->pm + 16)),
            pm2 = _mm256_load_si256((const __m256i *)(v->pm + 32)), pm3 = _mm256_load_si256((const __m256i *)(v->pm + 48));
    const __m256i g00 = _mm256_load_si256((const __m256i *)v->sg0), g01 = _mm256_load_si256((const __m256i *)(v->sg0 + 16)),
                  g10 = _mm256_load_si256((const __m256i *)v->sg1), g11 = _mm256_load_si256((const __m256i *)(v->sg1 + 16));
    __m256i E0 = even16(pm0, pm1), E1 = even16(pm2, pm3), O0 = odd16(pm0, pm1), O1 = odd16(pm2, pm3);
    for (int i = 0; i < nsteps; i++) {
        __m256i s0 = _mm256_set1_epi16(soft[2 * i]), s1 = _mm256_set1_epi16(soft[2 * i + 1]);
        __m256i M0 = _mm256_add_epi16(_mm256_sign_epi16(s0, g00), _mm256_sign_epi16(s1, g10));
        __m256i M1 = _mm256_add_epi16(_mm256_sign_epi16(s0, g01), _mm256_sign_epi16(s1, g11));
        __m256i aE0 = _mm256_adds_epi16(E0, M0), aE1 = _mm256_adds_epi16(E1, M1);
        __m256i sE0 = _mm256_subs_epi16(E0, M0), sE1 = _mm256_subs_epi16(E1, M1);
        __m256i aO0 = _mm256_adds_epi16(O0, M0), aO1 = _mm256_adds_epi16(O1, M1);
        __m256i sO0 = _mm256_subs_epi16(O0, M0), sO1 = _mm256_subs_epi16(O1, M1);
        /* estados nuevos j (b = 0): máx(E+M, O−M); j+32 (b = 1): máx(E−M, O+M) */
        __m256i lo0 = _mm256_max_epi16(aE0, sO0), lo1 = _mm256_max_epi16(aE1, sO1);
        __m256i hi0 = _mm256_max_epi16(sE0, aO0), hi1 = _mm256_max_epi16(sE1, aO1);
        uint32_t dl = mask16(_mm256_cmpgt_epi16(sO0, aE0), _mm256_cmpgt_epi16(sO1, aE1));
        uint32_t dh = mask16(_mm256_cmpgt_epi16(aO0, sE0), _mm256_cmpgt_epi16(aO1, sE1));
        v->dec[v->n++] = (uint64_t)dl | ((uint64_t)dh << 32);
        if (++v->since_norm == 16) {
            v->since_norm = 0;
            __m256i m = _mm256_max_epi16(_mm256_max_epi16(lo0, lo1), _mm256_max_epi16(hi0, hi1));
            m = _mm256_max_epi16(m, _mm256_permute2x128_si256(m, m, 1));
            m = _mm256_max_epi16(m, _mm256_srli_si256(m, 8));
            m = _mm256_max_epi16(m, _mm256_srli_si256(m, 4));
            m = _mm256_max_epi16(m, _mm256_srli_si256(m, 2));
            m = _mm256_broadcastw_epi16(_mm256_castsi256_si128(m));
            lo0 = _mm256_subs_epi16(lo0, m);
            lo1 = _mm256_subs_epi16(lo1, m);
            hi0 = _mm256_subs_epi16(hi0, m);
            hi1 = _mm256_subs_epi16(hi1, m);
        }
        E0 = even16(lo0, lo1);
        E1 = even16(hi0, hi1);
        O0 = odd16(lo0, lo1);
        O1 = odd16(hi0, hi1);
        if (v->n == CAP) {
            _mm256_store_si256((__m256i *)(v->pm), lo0);
            _mm256_store_si256((__m256i *)(v->pm + 16), lo1);
            _mm256_store_si256((__m256i *)(v->pm + 32), hi0);
            _mm256_store_si256((__m256i *)(v->pm + 48), hi1);
            traceback(v, out + nout);
            nout += BLK / 8;
        }
        pm0 = lo0; pm1 = lo1; pm2 = hi0; pm3 = hi1;
    }
    _mm256_store_si256((__m256i *)(v->pm), pm0);
    _mm256_store_si256((__m256i *)(v->pm + 16), pm1);
    _mm256_store_si256((__m256i *)(v->pm + 32), pm2);
    _mm256_store_si256((__m256i *)(v->pm + 48), pm3);
    return nout;
}
#else
/* Versión escalar de reserva (misma aritmética). */
int dvbt_vit_push(dvbt_vit_t *v, const int8_t *soft, int nsteps, uint8_t *out)
{
    int nout = 0;
    int16_t nw[64];
    for (int i = 0; i < nsteps; i++) {
        int s0 = soft[2 * i], s1 = soft[2 * i + 1];
        uint64_t d = 0;
        for (int j = 0; j < 32; j++) {
            int M = v->sg0[j] * s0 + v->sg1[j] * s1;
            int e = v->pm[2 * j], o = v->pm[2 * j + 1];
            int a = e + M, b = o - M;
            nw[j] = (int16_t)(a >= b ? a : b);
            d |= (uint64_t)(b > a) << j;
            a = e - M;
            b = o + M;
            nw[j + 32] = (int16_t)(a >= b ? a : b);
            d |= (uint64_t)(b > a) << (j + 32);
        }
        memcpy(v->pm, nw, sizeof nw);
        v->dec[v->n++] = d;
        if (++v->since_norm == 16) {
            v->since_norm = 0;
            int16_t m = v->pm[0];
            for (int s = 1; s < 64; s++)
                if (v->pm[s] > m)
                    m = v->pm[s];
            for (int s = 0; s < 64; s++)
                v->pm[s] = (int16_t)(v->pm[s] - m);
        }
        if (v->n == CAP) {
            traceback(v, out + nout);
            nout += BLK / 8;
        }
    }
    return nout;
}
#endif
