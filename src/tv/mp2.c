/*
 * mp2.c — Decodificador MPEG-1/2 Layer II (ver mp2.h).
 *
 * Las tablas de asignación de bits (ISO 11172-3 B.2a–d y ISO 13818-3 B.1) se
 * describen por grupos de subbandas: (nº de subbandas, bits de nbal, lista de
 * cuantificadores por índice). La ventana D[] (Tabla 3-B.3) son enteros/65536
 * simétricos, por eso solo se guardan 257 valores.
 */
#include "mp2.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---- Cuantificadores: niveles → (agrupado, bits por código) ---- */
typedef struct { int nlev; int grouped; int bits; } qclass_t;

static qclass_t qclass(int nlev)
{
    switch (nlev) {
    case 3: return (qclass_t){3, 1, 5};
    case 5: return (qclass_t){5, 1, 7};
    case 9: return (qclass_t){9, 1, 10};
    default: {
        int b = 0;
        while ((1 << b) - 1 < nlev)
            b++;
        return (qclass_t){nlev, 0, b}; /* 7 → 3, 15 → 4 … 65535 → 16 */
    }
    }
}

/* ---- Tablas de asignación ---- */
typedef struct { int nsb; int nbal; int nl; int lev[16]; } alloc_group_t;

#define L3 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191
static const alloc_group_t tab_a[] = {
    {3, 4, 15, {3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535}},
    {8, 4, 15, {L3, 65535}},
    {12, 3, 7, {3, 5, 7, 9, 15, 31, 65535}},
    {4, 2, 3, {3, 5, 65535}},
    {0, 0, 0, {0}},
};
static const alloc_group_t tab_b[] = {
    {3, 4, 15, {3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535}},
    {8, 4, 15, {L3, 65535}},
    {12, 3, 7, {3, 5, 7, 9, 15, 31, 65535}},
    {7, 2, 3, {3, 5, 65535}},
    {0, 0, 0, {0}},
};
static const alloc_group_t tab_c[] = {
    {2, 4, 15, {3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767}},
    {6, 3, 7, {3, 5, 9, 15, 31, 63, 127}},
    {0, 0, 0, {0}},
};
static const alloc_group_t tab_d[] = {
    {2, 4, 15, {3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767}},
    {10, 3, 7, {3, 5, 9, 15, 31, 63, 127}},
    {0, 0, 0, {0}},
};
static const alloc_group_t tab_lsf[] = {
    {4, 4, 15, {3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383}},
    {7, 3, 7, {3, 5, 9, 15, 31, 63, 127}},
    {19, 2, 3, {3, 5, 9}},
    {0, 0, 0, {0}},
};

/* Ventana de síntesis D[i]·65536, i = 0..256 (Tabla 3-B.3); D[512−i] = ±D[i]. */
static const int32_t win_half[257] = {
    0, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3, -3, -4, -4, -5,
    -5, -6, -7, -7, -8, -9, -10, -11, -13, -14, -16, -17, -19, -21, -24, -26,
    -29, -31, -35, -38, -41, -45, -49, -53, -58, -63, -68, -73, -79, -85, -91, -97,
    -104, -111, -117, -125, -132, -139, -147, -154, -161, -169, -176, -183, -190, -196, -202, -208,
    213, 218, 222, 225, 227, 228, 228, 227, 224, 221, 215, 208, 200, 189, 177, 163,
    146, 127, 106, 83, 57, 29, -2, -36, -72, -111, -153, -197, -244, -294, -347, -401,
    -459, -519, -581, -645, -711, -779, -848, -919, -991, -1064, -1137, -1210, -1283, -1356, -1428, -1498,
    -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962, -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063,
    2037, 2000, 1952, 1893, 1822, 1739, 1644, 1535, 1414, 1280, 1131, 970, 794, 605, 402, 185,
    -45, -288, -545, -814, -1095, -1388, -1692, -2006, -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
    -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597, -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585,
    -9727, -9838, -9916, -9959, -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
    6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082, 70, -998, -2122, -3300, -4533, -5818, -7154, -8540,
    -9975, -11455, -12980, -14548, -16155, -17799, -19478, -21189, -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
    -37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137, -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420, -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
    75038,
};

static const int kbps_mpeg1[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
static const int kbps_lsf[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
static const int rate_mpeg1[3] = {44100, 48000, 32000};

/* ---- Tablas calculadas una vez ---- */
static float g_win[512];
static float g_cos[64][32];
static float g_sf[64];
static bool g_init;

static void init_tables(void)
{
    if (g_init)
        return;
    for (int i = 0; i <= 256; i++) {
        float v = (float)win_half[i] / 65536.0f;
        g_win[i] = v;
        if (i != 0)
            g_win[512 - i] = (i & 63) ? -v : v;
    }
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++)
            g_cos[i][k] = (float)cos((16 + i) * (2 * k + 1) * M_PI / 64.0);
    for (int i = 0; i < 63; i++)
        g_sf[i] = (float)pow(2.0, 1.0 - i / 3.0);
    g_sf[63] = 0;
    g_init = true;
}

typedef struct {
    float v[1024];
    int off;
} synth_t;

struct mp2 {
    synth_t sy[2];
    uint8_t *buf;
    size_t len, cap;
    unsigned long bad;
};

mp2_t *mp2_create(void)
{
    init_tables();
    return calloc(1, sizeof(mp2_t));
}
void mp2_destroy(mp2_t *d)
{
    if (d) {
        free(d->buf);
        free(d);
    }
}
void mp2_reset(mp2_t *d)
{
    memset(d->sy, 0, sizeof d->sy);
    d->len = 0;
}
unsigned long mp2_bad_frames(const mp2_t *d) { return d->bad; }

/* ---- Lector de bits ---- */
typedef struct { const uint8_t *d; size_t nbits, pos; bool over; } bits_t;
static uint32_t getbits(bits_t *b, int n)
{
    uint32_t v = 0;
    if (b->pos + (size_t)n > b->nbits) {
        b->over = true;
        b->pos = b->nbits;
        return 0;
    }
    for (int i = 0; i < n; i++, b->pos++)
        v = (v << 1) | ((b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
    return v;
}

/* ---- Síntesis polifásica: 32 subbandas → 32 muestras PCM ---- */
static void synth32(synth_t *s, const float *sb, float *out)
{
    s->off = (s->off - 64) & 1023;
    for (int i = 0; i < 64; i++) {
        float a = 0;
        for (int k = 0; k < 32; k++)
            a += g_cos[i][k] * sb[k];
        s->v[(s->off + i) & 1023] = a;
    }
    for (int j = 0; j < 32; j++) {
        float a = 0;
        for (int i = 0; i < 16; i++)
            a += g_win[32 * i + j] * s->v[(s->off + 64 * i + j + 32 * (i & 1)) & 1023];
        out[j] = a;
    }
}

typedef struct {
    int id;       /* 1 = MPEG-1, 0 = MPEG-2 LSF */
    int br_idx, rate, kbps, pad, mode, mode_ext, prot;
    int ch, frame_bytes;
} hdr_t;

static int parse_header(const uint8_t *p, hdr_t *h)
{
    uint32_t v = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    if ((v >> 20) != 0xFFF)
        return -1;
    h->id = (v >> 19) & 1;
    if (((v >> 17) & 3) != 2) /* solo Layer II */
        return -1;
    h->prot = (v >> 16) & 1;
    h->br_idx = (v >> 12) & 15;
    int fi = (v >> 10) & 3;
    if (h->br_idx == 0 || h->br_idx == 15 || fi == 3)
        return -1; /* tasa libre/prohibida: no se usan en TDT */
    h->pad = (v >> 9) & 1;
    h->mode = (v >> 6) & 3;
    h->mode_ext = (v >> 4) & 3;
    h->rate = h->id ? rate_mpeg1[fi] : rate_mpeg1[fi] / 2;
    h->kbps = h->id ? kbps_mpeg1[h->br_idx] : kbps_lsf[h->br_idx];
    h->ch = h->mode == 3 ? 1 : 2;
    h->frame_bytes = 144000 * h->kbps / h->rate + h->pad;
    /* MPEG-1 y estéreo: ciertas combinaciones tasa/modo están prohibidas (ISO 11172-3 §2.4.2.3) */
    if (h->id) {
        if (h->mode == 3 && h->kbps > 192)
            return -1;
        if (h->mode != 3 && (h->kbps == 32 || h->kbps == 48 || h->kbps == 56 || h->kbps == 80))
            return -1;
    }
    return 0;
}

/* Selección de tabla de asignación (ISO 11172-3 §2.4.2.3 y B.2) */
static const alloc_group_t *select_table(const hdr_t *h)
{
    if (!h->id)
        return tab_lsf;
    int per_ch = h->kbps / h->ch; /* bits por canal */
    if (h->ch == 2 && h->mode != 3)
        per_ch = h->kbps / 2;
    if ((h->rate == 48000 && per_ch >= 56) || (per_ch >= 56 && per_ch <= 80))
        return tab_a;
    if (h->rate != 48000 && per_ch >= 96)
        return tab_b;
    if (h->rate != 32000 && per_ch <= 48)
        return tab_c;
    return tab_d;
}

/* Lee un código de nlev niveles; escribe 3 valores normalizados (2c−(n−1))/n. */
static void read_samples(bits_t *b, const qclass_t *q, float *o)
{
    float n = (float)q->nlev, inv = 1.0f / n;
    if (q->grouped) {
        uint32_t v = getbits(b, q->bits);
        uint32_t nl = (uint32_t)q->nlev;
        for (int i = 0; i < 3; i++) {
            uint32_t c = v % nl;
            v /= nl;
            o[i] = (2.0f * (float)c - (n - 1.0f)) * inv;
        }
    } else {
        for (int i = 0; i < 3; i++) {
            uint32_t c = getbits(b, q->bits);
            o[i] = (2.0f * (float)c - (n - 1.0f)) * inv;
        }
    }
}

static int decode_frame(mp2_t *d, const uint8_t *f, const hdr_t *h, int16_t *pcm)
{
    bits_t b = {f, (size_t)h->frame_bytes * 8, 32, false};
    if (!h->prot)
        getbits(&b, 16); /* CRC: no se verifica (el RS ya filtró) */
    const alloc_group_t *tab = select_table(h);
    int nch = h->ch;
    /* sblimit y nbal/niveles por subbanda */
    int sblimit = 0, nbal[32], nlev_list[32][16], nl[32];
    for (const alloc_group_t *g = tab; g->nsb; g++)
        for (int i = 0; i < g->nsb; i++, sblimit++) {
            nbal[sblimit] = g->nbal;
            nl[sblimit] = g->nl;
            memcpy(nlev_list[sblimit], g->lev, sizeof g->lev);
        }
    int bound = sblimit;
    if (h->mode == 1) {
        bound = 4 + 4 * h->mode_ext;
        if (bound > sblimit)
            bound = sblimit;
    }
    int alloc[2][32] = {{0}};
    for (int sb = 0; sb < sblimit; sb++) {
        if (sb < bound) {
            for (int c = 0; c < nch; c++)
                alloc[c][sb] = (int)getbits(&b, nbal[sb]);
        } else {
            alloc[0][sb] = alloc[1][sb] = (int)getbits(&b, nbal[sb]);
        }
        for (int c = 0; c < nch; c++)
            if (alloc[c][sb] > nl[sb])
                alloc[c][sb] = 0; /* índice no definido */
    }
    int scfsi[2][32] = {{0}};
    for (int sb = 0; sb < sblimit; sb++)
        for (int c = 0; c < nch; c++)
            if (alloc[c][sb])
                scfsi[c][sb] = (int)getbits(&b, 2);
    float sf[2][32][3];
    memset(sf, 0, sizeof sf);
    for (int sb = 0; sb < sblimit; sb++)
        for (int c = 0; c < nch; c++) {
            if (!alloc[c][sb])
                continue;
            int s0, s1, s2;
            switch (scfsi[c][sb]) {
            case 0: s0 = getbits(&b, 6); s1 = getbits(&b, 6); s2 = getbits(&b, 6); break;
            case 1: s0 = s1 = getbits(&b, 6); s2 = getbits(&b, 6); break;
            case 2: s0 = s1 = s2 = getbits(&b, 6); break;
            default: s0 = getbits(&b, 6); s1 = s2 = getbits(&b, 6); break;
            }
            sf[c][sb][0] = g_sf[s0];
            sf[c][sb][1] = g_sf[s1];
            sf[c][sb][2] = g_sf[s2];
        }
    /* muestras: 12 grupos de 3 por subbanda */
    static float samp[2][32][36];
    memset(samp, 0, sizeof samp);
    for (int gr = 0; gr < 12; gr++) {
        for (int sb = 0; sb < sblimit; sb++) {
            if (sb < bound || nch == 1) {
                for (int c = 0; c < nch; c++) {
                    int a = alloc[c][sb];
                    if (!a)
                        continue;
                    qclass_t q = qclass(nlev_list[sb][a - 1]);
                    float t[3];
                    read_samples(&b, &q, t);
                    float s = sf[c][sb][gr / 4];
                    for (int i = 0; i < 3; i++)
                        samp[c][sb][gr * 3 + i] = t[i] * s;
                }
            } else {
                int a = alloc[0][sb];
                if (!a)
                    continue;
                qclass_t q = qclass(nlev_list[sb][a - 1]);
                float t[3];
                read_samples(&b, &q, t);
                for (int c = 0; c < 2; c++) {
                    float s = sf[c][sb][gr / 4];
                    for (int i = 0; i < 3; i++)
                        samp[c][sb][gr * 3 + i] = t[i] * s;
                }
            }
        }
    }
    bool bad = b.over;
    /* síntesis */
    for (int c = 0; c < nch; c++)
        for (int t = 0; t < 36; t++) {
            float s[32], o[32];
            for (int sb = 0; sb < 32; sb++)
                s[sb] = bad ? 0.0f : samp[c][sb][t];
            synth32(&d->sy[c], s, o);
            for (int j = 0; j < 32; j++) {
                float x = o[j] * 32768.0f;
                int v = (int)lrintf(x);
                pcm[(t * 32 + j) * nch + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
    return bad ? -1 : 0;
}

int mp2_decode_one(mp2_t *d, const uint8_t *data, size_t len, int16_t *pcm, int *ch, int *rate)
{
    hdr_t h;
    if (len < 4 || parse_header(data, &h) < 0 || (size_t)h.frame_bytes > len)
        return -1;
    if (decode_frame(d, data, &h, pcm) < 0)
        d->bad++;
    *ch = h.ch;
    *rate = h.rate;
    return h.frame_bytes;
}

int mp2_decode(mp2_t *d, const uint8_t *data, size_t len, mp2_frame_fn fn, void *ctx)
{
    if (d->len + len > d->cap) {
        size_t nc = (d->len + len) * 2 + 4096;
        uint8_t *nb = realloc(d->buf, nc);
        if (!nb)
            return 0;
        d->buf = nb;
        d->cap = nc;
    }
    memcpy(d->buf + d->len, data, len);
    d->len += len;
    int frames = 0;
    size_t pos = 0;
    static int16_t pcm[1152 * 2];
    while (d->len - pos >= 4) {
        hdr_t h;
        if (d->buf[pos] != 0xFF || parse_header(d->buf + pos, &h) < 0) {
            pos++;
            continue;
        }
        if (d->len - pos < (size_t)h.frame_bytes + 2)
            break; /* trama incompleta (se mira 2 bytes más para validar la siguiente sincronía) */
        /* la trama siguiente debe empezar por 0xFFF si hay datos: evita falsos positivos */
        if (d->len - pos >= (size_t)h.frame_bytes + 2 && d->buf[pos + h.frame_bytes] != 0xFF) {
            pos++;
            d->bad++;
            continue;
        }
        if (decode_frame(d, d->buf + pos, &h, pcm) < 0)
            d->bad++;
        if (fn)
            fn(ctx, pcm, 1152, h.ch, h.rate);
        frames++;
        pos += h.frame_bytes;
    }
    memmove(d->buf, d->buf + pos, d->len - pos);
    d->len -= pos;
    return frames;
}
