/*
 * lte_phy.c — Capa física LTE común al decodificador y al modulador.
 * Ver lte_phy.h. Referencias: 3GPP TS 36.211 (secuencias, mapeo) y TS 36.212
 * (codificación del PBCH).
 */
#include "lte_phy.h"

#include "../dsp/fec.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/*  Geometría de la trama                                                   */
/* ------------------------------------------------------------------------ */
int lte_nsym(bool ext) { return ext ? 6 : 7; }

/* CP normal: 160·Ts (10 muestras a 1,92 Msps) el primer símbolo de la ranura y
 * 144·Ts (9) el resto; CP extendido: 512·Ts = 32 muestras (36.211 tabla 6.12-1). */
int lte_cp_len(bool ext, int l) { return ext ? 32 : (l == 0 ? 10 : 9); }

int lte_sym_start(bool ext, int l)
{
    if (ext)
        return l * 160;
    return l == 0 ? 0 : 138 + (l - 1) * 137;
}

/* Inicio útil de la PSS dentro de la trama. FDD: último símbolo de la ranura 0
 * (la parte útil empieza en 832 con CP normal y extendido). TDD: tercer símbolo
 * (l = 2) de la ranura 2 (subtrama 1). */
int lte_pss_pos(int grid)
{
    switch (grid) {
    case LTE_FDD_N:
    case LTE_FDD_E: return 832;
    case LTE_TDD_N: return 2 * LTE_SLOT + lte_sym_start(false, 2) + 9;
    default: return 2 * LTE_SLOT + lte_sym_start(true, 2) + 32;
    }
}

/* FDD: la SSS es el símbolo anterior a la PSS; TDD: la SSS es el último símbolo
 * de la subtrama 0 y la PSS el tercero de la subtrama 1. */
int lte_sss_delta(int grid)
{
    switch (grid) {
    case LTE_FDD_N: return 128 + 9;
    case LTE_FDD_E: return 128 + 32;
    default: return lte_pss_pos(grid) - (LTE_SLOT + 832);   /* SSS: ranura 1, último símbolo */
    }
}

int lte_win_to_f(int j) { return j < 36 ? j - 36 : j - 35; }
int lte_f_to_bin(int f) { return f >= 0 ? f : LTE_N + f; }

/* ------------------------------------------------------------------------ */
/*  Secuencias                                                              */
/* ------------------------------------------------------------------------ */
/* Gold de 31 bits: c(n) = x1(n+1600) ⊕ x2(n+1600); x1(n+31) = x1(n+3)+x1(n);
 * x2(n+31) = x2(n+3)+x2(n+2)+x2(n+1)+x2(n); x1 = 1,0,0…; x2 = c_init (§7.2). */
void lte_gold(uint32_t cinit, int n, uint8_t *out)
{
    uint32_t x1 = 1, x2 = cinit;
    for (int i = 0; i < 1600 + n; i++) {
        if (i >= 1600)
            out[i - 1600] = (uint8_t)((x1 ^ x2) & 1);
        uint32_t f1 = ((x1 >> 3) ^ x1) & 1;
        uint32_t f2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
        x1 = (x1 >> 1) | (f1 << 30);
        x2 = (x2 >> 1) | (f2 << 30);
    }
}

int lte_pss_root(int n_id2) { return n_id2 == 0 ? 25 : (n_id2 == 1 ? 29 : 34); }

/* Zadoff-Chu de longitud 63 con el elemento central (DC) eliminado (§6.11.1.1). */
void lte_pss_seq(int n_id2, cf32 d[62])
{
    int u = lte_pss_root(n_id2);
    for (int n = 0; n < 62; n++) {
        double a = n < 31 ? (double)n * (n + 1) : (double)(n + 1) * (n + 2);
        double ph = -M_PI * u * a / 63.0;
        d[n].re = (float)cos(ph);
        d[n].im = (float)sin(ph);
    }
}

/* m-secuencia de 31 con realimentación 'fb' (bits de x a sumar) y x(0..4) = 0,0,0,0,1. */
static void msec31(int8_t *s, int kind)
{
    uint8_t x[31 + 5];
    memset(x, 0, sizeof x);
    x[4] = 1;
    for (int i = 0; i + 5 < 31 + 5 && i + 5 < 36; i++) {
        if (kind == 0)
            x[i + 5] = (x[i + 2] + x[i]) & 1;                       /* s̃ */
        else if (kind == 1)
            x[i + 5] = (x[i + 3] + x[i]) & 1;                       /* c̃ */
        else
            x[i + 5] = (x[i + 4] + x[i + 2] + x[i + 1] + x[i]) & 1;  /* z̃ */
    }
    for (int i = 0; i < 31; i++)
        s[i] = (int8_t)(1 - 2 * x[i]);
}

void lte_sss_seq(int n_id1, int n_id2, int half, int8_t d[62])
{
    int8_t st[31], ct[31], zt[31];
    msec31(st, 0);
    msec31(ct, 1);
    msec31(zt, 2);
    int qp = n_id1 / 30;
    int q = (n_id1 + qp * (qp + 1) / 2) / 30;
    int mp = n_id1 + q * (q + 1) / 2;
    int m0 = mp % 31;
    int m1 = (m0 + mp / 31 + 1) % 31;
    for (int n = 0; n < 31; n++) {
        int s0 = st[(n + m0) % 31], s1 = st[(n + m1) % 31];
        int c0 = ct[(n + n_id2) % 31], c1 = ct[(n + n_id2 + 3) % 31];
        int z0 = zt[(n + (m0 % 8)) % 31], z1 = zt[(n + (m1 % 8)) % 31];
        if (half == 0) {
            d[2 * n] = (int8_t)(s0 * c0);
            d[2 * n + 1] = (int8_t)(s1 * c1 * z0);
        } else {
            d[2 * n] = (int8_t)(s1 * c0);
            d[2 * n + 1] = (int8_t)(s0 * c1 * z1);
        }
    }
}

/* r(m) = [ (1−2c(2m)) + j(1−2c(2m+1)) ]/√2, c_init = 2^10 (7(ns+1)+l+1)(2N+1) + 2N + N_CP.
 * Para el centro de la banda m = 104..115 → hay que generar hasta c(231). */
void lte_crs_seq(int pci, int ns, int l, bool ext, cf32 r[12])
{
    uint32_t cinit = (1u << 10) * (uint32_t)(7 * (ns + 1) + l + 1) * (uint32_t)(2 * pci + 1) +
                     (uint32_t)(2 * pci) + (ext ? 0u : 1u);
    uint8_t c[232];
    lte_gold(cinit, 232, c);
    const float a = 0.70710678f;
    for (int m = 0; m < 12; m++) {
        r[m].re = a * (float)(1 - 2 * c[2 * (104 + m)]);
        r[m].im = a * (float)(1 - 2 * c[2 * (104 + m) + 1]);
    }
}

/* Puertos 0/1: símbolos l = 0 y l = Nsym−3 (v = 0/3 y 3/0). Puertos 2/3: l = 1,
 * v = 3·(ns mod 2) (+3 para el puerto 3). k = 6m + (v + N_ID mod 6) mod 6. */
int lte_crs_j0(int pci, int port, int ns, int l, bool ext)
{
    int vs = pci % 6, v;
    int l2 = lte_nsym(ext) - 3;
    if (port < 2) {
        if (l != 0 && l != l2)
            return -1;
        v = (l == 0) ? (port == 0 ? 0 : 3) : (port == 0 ? 3 : 0);
    } else {
        if (l != 1)
            return -1;
        v = 3 * (ns & 1) + (port == 3 ? 3 : 0);
    }
    return (v + vs) % 6;
}

/* ------------------------------------------------------------------------ */
/*  PBCH (36.211 §6.6, 36.212 §5.3.1)                                       */
/* ------------------------------------------------------------------------ */
int lte_pbch_res(int pci, bool ext, int *lsym, int *jj)
{
    int n = 0, vs3 = pci % 3;
    for (int l = 0; l < 4; l++)
        for (int j = 0; j < 72; j++) {
            bool rsv = ((l == 0 || l == 1) && j % 3 == vs3) || (ext && l == 3 && j % 3 == vs3);
            if (rsv)
                continue;
            lsym[n] = l;
            jj[n] = j;
            n++;
        }
    return n;
}

static const int BW_TAB[6] = {6, 15, 25, 50, 75, 100};

/* MIB (36.331): dl-Bandwidth(3) phich-Duration(1) phich-Resource(2) SFN(8) spare(10). */
void lte_mib_pack(const lte_mib_t *m, uint8_t a[24])
{
    memset(a, 0, 24);
    int bi = 0;
    for (int i = 0; i < 6; i++)
        if (BW_TAB[i] == m->bw_rb)
            bi = i;
    for (int i = 0; i < 3; i++)
        a[i] = (uint8_t)((bi >> (2 - i)) & 1);
    a[3] = (uint8_t)(m->phich_ext & 1);
    a[4] = (uint8_t)((m->phich_res >> 1) & 1);
    a[5] = (uint8_t)(m->phich_res & 1);
    for (int i = 0; i < 8; i++)
        a[6 + i] = (uint8_t)((m->sfn8 >> (7 - i)) & 1);
}

int lte_mib_unpack(const uint8_t a[24], lte_mib_t *m)
{
    int bi = (a[0] << 2) | (a[1] << 1) | a[2];
    if (bi > 5)
        return -1;
    m->bw_rb = BW_TAB[bi];
    m->phich_ext = a[3];
    m->phich_res = (a[4] << 1) | a[5];
    int s = 0;
    for (int i = 0; i < 8; i++)
        s = (s << 1) | a[6 + i];
    m->sfn8 = s;
    return 0;
}

/* gCRC16 = D^16 + D^12 + D^5 + 1, registro a cero, MSB primero. */
uint16_t lte_crc16(const uint8_t *bits, int n)
{
    uint16_t r = 0;
    for (int i = 0; i < n; i++) {
        int fb = ((r >> 15) & 1) ^ (bits[i] & 1);
        r = (uint16_t)(r << 1);
        if (fb)
            r ^= 0x1021;
    }
    return r;
}

static uint16_t ant_mask(int ports) { return ports == 1 ? 0x0000 : (ports == 2 ? 0xFFFF : 0x5555); }

/* Mapa de adaptación de velocidad (36.212 §5.1.4.2): cada una de las 3 corrientes
 * (D = 40) pasa por el entrelazador de subbloques de 32 columnas (patrón del
 * código convolucional), se concatenan en el búfer circular y se leen saltando los
 * <NULL>. Resultado: map[k] = corriente·64 + posición de la k-ésima salida
 * (k = 0..119 sin nulos; luego se repite). */
static int rm_map(int map[120])
{
    static const int P[32] = {1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31,
                              0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30};
    const int D = 40, R = 2, ND = R * 32 - D;
    int n = 0;
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < R * 32; k++) {
            int y = P[k / R] + 32 * (k % R);
            if (y >= ND)
                map[n++] = s * 64 + (y - ND);
        }
    return n; /* 120 */
}

int lte_pbch_encode(const lte_mib_t *m, int ports, int pci, bool ext, uint8_t *bits)
{
    uint8_t c[40], coded[120 + 8];
    lte_mib_pack(m, c);
    uint16_t crc = (uint16_t)(lte_crc16(c, 24) ^ ant_mask(ports));
    for (int i = 0; i < 16; i++)
        c[24 + i] = (uint8_t)((crc >> (15 - i)) & 1);
    conv_code_t code = conv_lte_k7_r3();
    conv_encode(&code, c, 40, coded, CONV_TAILBITING, NULL);
    int map[120];
    rm_map(map);
    int eseg = ext ? 432 : 480, E = 4 * eseg;
    uint8_t scr[1920];
    lte_gold((uint32_t)pci, E, scr);
    for (int k = 0; k < E; k++) {
        int mm = map[k % 120];
        int s = mm / 64, t = mm % 64;
        bits[k] = (uint8_t)(coded[3 * t + s] ^ scr[k]);
    }
    return E;
}

int lte_pbch_decode(const float *soft, int seg, int pci, bool ext, lte_mib_t *mib)
{
    int map[120];
    rm_map(map);
    int eseg = ext ? 432 : 480;
    uint8_t scr[1920];
    lte_gold((uint32_t)pci, 4 * eseg, scr);
    float acc[3 * 64];
    memset(acc, 0, sizeof acc);
    for (int i = 0; i < eseg; i++) {
        int k = seg * eseg + i;
        float v = soft[i] * (scr[k] ? -1.0f : 1.0f);   /* positivo = bit 0 tras desaleatorizar */
        acc[map[k % 120]] += -v;                       /* convención de fec.h: positivo = bit 1 */
    }
    float mx = 1e-9f;
    for (int i = 0; i < 192; i++)
        mx = fmaxf(mx, fabsf(acc[i]));
    int8_t sv[120];
    for (int t = 0; t < 40; t++)
        for (int s = 0; s < 3; s++)
            sv[3 * t + s] = (int8_t)lrintf(CLAMP(acc[s * 64 + t] / mx * 127.0f, -127.0f, 127.0f));
    conv_code_t code = conv_lte_k7_r3();
    viterbi_t *v = viterbi_create(&code, 40);
    if (!v)
        return 0;
    uint8_t out[40];
    int r = viterbi_decode_i8(v, sv, 120, 40, CONV_TAILBITING, NULL, out);
    viterbi_destroy(v);
    if (r < 0)
        return 0;
    uint16_t crc = lte_crc16(out, 24), rx = 0;
    for (int i = 0; i < 16; i++)
        rx = (uint16_t)((rx << 1) | out[24 + i]);
    static const int P3[3] = {1, 2, 4};
    for (int i = 0; i < 3; i++)
        if ((uint16_t)(crc ^ ant_mask(P3[i])) == rx) {
            if (lte_mib_unpack(out, mib) < 0)
                return 0;
            return P3[i];
        }
    return 0;
}
