/*
 * gsm_phy.c — Tablas y codificación de canal de GSM. Ver gsm_phy.h.
 *
 * Procedencia de las constantes (TS 45.002 §5.2.3: secuencias de entrenamiento; TS
 * 45.003 §4.1 y §4.7: codificación). Las 8 secuencias de entrenamiento normales y la
 * extendida del SCH están transcritas de memoria de la norma / gr-gsm: no se han
 * podido contrastar con un documento offline (si alguna estuviera mal, el modulador y
 * el decodificador seguirían siendo coherentes entre sí pero no con una red real; ver
 * docs/protocolos/gsm.md).
 */
#include "gsm_phy.h"

#include "../dsp/fec.h"

#include <string.h>

const uint8_t GSM_TSC[8][26] = {
    {0,0,1,0,0,1,0,1,1,1,0,0,0,0,1,0,0,0,1,0,0,1,0,1,1,1},
    {0,0,1,0,1,1,0,1,1,1,0,1,1,1,1,0,0,0,1,0,1,1,0,1,1,1},
    {0,1,0,0,0,0,1,1,1,0,1,1,1,0,1,0,0,1,0,0,0,0,1,1,1,0},
    {0,1,0,0,0,1,1,1,1,0,1,1,0,1,0,0,0,1,0,0,0,1,1,1,1,0},
    {0,0,0,1,1,0,1,0,1,1,1,0,0,1,0,0,0,0,0,1,1,0,1,0,1,1},
    {0,1,0,0,1,1,1,0,1,0,1,1,0,0,0,0,0,1,0,0,1,1,1,0,1,0},
    {1,0,1,0,0,1,1,1,1,1,0,1,1,0,0,0,1,0,1,0,0,1,1,1,1,1},
    {1,1,1,0,1,1,1,1,0,0,0,1,0,0,1,0,1,1,1,0,1,1,1,1,0,0},
};

const uint8_t GSM_SCH_TRAIN[64] = {
    1,0,1,1,1,0,0,1,0,1,1,0,0,0,1,0, 0,0,0,0,0,1,0,0,0,0,0,0,1,1,1,1,
    0,0,1,0,1,1,0,1,0,1,0,0,0,1,0,1, 0,1,1,1,0,1,1,0,0,0,0,1,1,0,1,1};

/* Resto de d(D)·D^w / g(D): registro de w bits, el bit de entrada se suma al más
 * significativo (división binaria). poly_low = g(D) sin el término D^w. */
static uint64_t crc_rem(const uint8_t *bits, int n, int w, uint64_t poly_low)
{
    uint64_t r = 0, top = 1ULL << (w - 1), mask = (w == 64) ? ~0ULL : ((1ULL << w) - 1);
    for (int i = 0; i < n; i++) {
        uint64_t fb = ((r & top) ? 1 : 0) ^ (bits[i] & 1);
        r = (r << 1) & mask;
        if (fb)
            r ^= poly_low;
    }
    return r;
}

/* g_SCH(D) = D^10 + D^8 + D^6 + D^5 + D^4 + D^2 + 1 */
#define SCH_POLY 0x175ULL
/* g_Fire(D) = (D^23 + 1)(D^17 + D^3 + 1) = D^40 + D^26 + D^23 + D^17 + D^3 + 1 */
#define FIRE_POLY ((1ULL << 26) | (1ULL << 23) | (1ULL << 17) | (1ULL << 3) | 1ULL)

/* ------------------------------------------------------------------------ */
/*  SCH                                                                     */
/* ------------------------------------------------------------------------ */
/* Información del SCH, d(0..24). T1 = FN div 1326 (11 bits), T2 = FN mod 26 (5), T3' = (T3−1)/10 (3), con
 * T3 = FN mod 51 (45.002 §3.3.2.2.1 y 4.3.3).
 * ORDEN DE BITS (contrastado con una grabación real de sigidwiki.com, 211 SCH con CRC correcto de una
 * celda BSIC 23 cuyo FN debe avanzar 10-11 tramas entre ráfagas; la búsqueda exhaustiva de
 * ordenaciones solo da FN coherentes con esta, que además coincide con los índices 16, 17 y 24 de T3'
 * del decodificador de airprobe):
 *   d0..d5   BSIC (NCC = d0..d2, BCC = d3..d5, MSB primero)
 *   d6..d15  T1 bits 10..1 (MSB primero)
 *   d16      T3' bit 1      d17  T3' bit 2
 *   d18..d22 T2 bits 0..4   (¡el LSB va primero!)
 *   d23      T1 bit 0       d24  T3' bit 0
 * (la versión anterior suponía los campos contiguos y MSB primero: el BSIC salía bien y el FN, mal). */
static void sch_pack(int bsic, uint32_t t1, uint32_t t2, uint32_t t3p, uint8_t d[25])
{
    for (int i = 0; i < 6; i++)
        d[i] = (uint8_t)((bsic >> (5 - i)) & 1);
    for (int i = 0; i < 10; i++)
        d[6 + i] = (uint8_t)((t1 >> (10 - i)) & 1);
    d[16] = (uint8_t)((t3p >> 1) & 1);
    d[17] = (uint8_t)((t3p >> 2) & 1);
    for (int i = 0; i < 5; i++)
        d[18 + i] = (uint8_t)((t2 >> i) & 1);
    d[23] = (uint8_t)(t1 & 1);
    d[24] = (uint8_t)(t3p & 1);
}

void gsm_sch_encode(int bsic, uint32_t fn, uint8_t bits[78])
{
    uint32_t t1 = fn / 1326, t2 = fn % 26, t3 = fn % 51, t3p = t3 >= 1 ? (t3 - 1) / 10 : 0;
    uint8_t u[35];
    sch_pack(bsic, t1 & 0x7FF, t2, t3p, u);
    uint64_t p = crc_rem(u, 25, 10, SCH_POLY) ^ 0x3FF;     /* paridad invertida */
    for (int i = 0; i < 10; i++)
        u[25 + i] = (uint8_t)((p >> (9 - i)) & 1);
    conv_code_t code = conv_gsm_k5();
    conv_encode(&code, u, 35, bits, CONV_TERM_ZERO, NULL);   /* 2·(35+4) = 78 */
}

int gsm_sch_decode(const float *soft, int *bsic, uint32_t *fn)
{
    conv_code_t code = conv_gsm_k5();
    viterbi_t *v = viterbi_create(&code, 35);
    if (!v)
        return -1;
    uint8_t u[35];
    int r = viterbi_decode_f(v, soft, 78, 35, CONV_TERM_ZERO, NULL, u);
    viterbi_destroy(v);
    if (r < 0)
        return -1;
    uint64_t p = crc_rem(u, 25, 10, SCH_POLY) ^ 0x3FF, rx = 0;
    for (int i = 0; i < 10; i++)
        rx = (rx << 1) | u[25 + i];
    if (p != rx)
        return -1;
    uint32_t t1 = 0, t2 = 0, t3p = (uint32_t)(u[24] | (u[16] << 1) | (u[17] << 2));
    for (int i = 0; i < 10; i++)
        t1 = (t1 << 1) | u[6 + i];
    t1 = (t1 << 1) | u[23];
    for (int i = 0; i < 5; i++)
        t2 |= (uint32_t)u[18 + i] << i;
    uint32_t t3 = 10 * t3p + 1;
    *bsic = 0;
    for (int i = 0; i < 6; i++)
        *bsic = (*bsic << 1) | u[i];
    /* FN = 51·((T3 − T2) mod 26) + T3 + 1326·T1 (45.002 §4.3.3) */
    *fn = 51 * ((t3 + 26 - t2 % 26) % 26) + t3 + 1326 * t1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  BCCH                                                                    */
/* ------------------------------------------------------------------------ */
void gsm_bcch_encode(const uint8_t l3[23], uint8_t bits[456])
{
    uint8_t u[224];
    for (int i = 0; i < 184; i++)
        u[i] = (uint8_t)((l3[i >> 3] >> (7 - (i & 7))) & 1);
    uint64_t p = crc_rem(u, 184, 40, FIRE_POLY) ^ 0xFFFFFFFFFFULL;
    for (int i = 0; i < 40; i++)
        u[184 + i] = (uint8_t)((p >> (39 - i)) & 1);
    conv_code_t code = conv_gsm_k5();
    conv_encode(&code, u, 224, bits, CONV_TERM_ZERO, NULL);   /* 2·(224+4) = 456 */
}

int gsm_bcch_decode(const float *soft, uint8_t l3[23])
{
    conv_code_t code = conv_gsm_k5();
    viterbi_t *v = viterbi_create(&code, 224);
    if (!v)
        return -1;
    uint8_t u[224];
    int r = viterbi_decode_f(v, soft, 456, 224, CONV_TERM_ZERO, NULL, u);
    viterbi_destroy(v);
    if (r < 0)
        return -1;
    uint64_t p = crc_rem(u, 184, 40, FIRE_POLY) ^ 0xFFFFFFFFFFULL, rx = 0;
    for (int i = 0; i < 40; i++)
        rx = (rx << 1) | u[184 + i];
    if (p != rx)
        return -1;
    memset(l3, 0, 23);
    for (int i = 0; i < 184; i++)
        l3[i >> 3] |= (uint8_t)(u[i] << (7 - (i & 7)));
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Nivel 3: System Information 3 y 4 (44.018 §9.1.35 y §9.1.36)             */
/* ------------------------------------------------------------------------ */
/* SI3: L2 pseudo length(1), PD/skip(1)=0x06, tipo(1)=0x1B, Cell Identity(2), LAI(5),
 * Control Channel Description(3), Cell Options(1), Cell Selection(2), RACH Control(3),
 * rest octets(4) = 23. LAI (10.5.1.3): MCC2|MCC1, MNC3|MCC3, MNC2|MNC1, LAC(2). */
int gsm_si_parse(const uint8_t l3[23], gsm_si_t *si)
{
    memset(si, 0, sizeof *si);
    si->ci = -1;
    if ((l3[1] & 0x0F) != 0x06)        /* discriminador de protocolo RR */
        return -1;
    si->type = l3[2];
    int off = -1;
    if (si->type == 0x1B) {            /* SI3: con Cell Identity */
        si->ci = (l3[3] << 8) | l3[4];
        off = 5;
    } else if (si->type == 0x1C) {     /* SI4: LAI directamente */
        off = 3;
    } else {
        return 0;
    }
    int mcc1 = l3[off] & 15, mcc2 = l3[off] >> 4, mcc3 = l3[off + 1] & 15;
    int mnc3 = l3[off + 1] >> 4, mnc1 = l3[off + 2] & 15, mnc2 = l3[off + 2] >> 4;
    if (mcc1 > 9 || mcc2 > 9 || mcc3 > 9 || mnc1 > 9 || mnc2 > 9 || (mnc3 > 9 && mnc3 != 15))
        return -1;
    si->mcc = mcc1 * 100 + mcc2 * 10 + mcc3;
    if (mnc3 == 15) {
        si->mnc = mnc1 * 10 + mnc2;
        si->mnc_digits = 2;
    } else {
        si->mnc = mnc1 * 100 + mnc2 * 10 + mnc3;
        si->mnc_digits = 3;
    }
    si->lac = (l3[off + 3] << 8) | l3[off + 4];
    si->lai_ok = true;
    return 0;
}

void gsm_si_build(int type, int mcc, int mnc, int mnc_digits, int lac, int ci, uint8_t l3[23])
{
    memset(l3, 0x2B, 23);              /* relleno ("L2 fill bits" 0x2B) */
    l3[1] = 0x06;
    l3[2] = (uint8_t)type;
    int off = type == 0x1B ? 5 : 3;
    if (type == 0x1B) {
        l3[0] = (uint8_t)((18 << 2) | 1);   /* pseudo-longitud */
        l3[3] = (uint8_t)(ci >> 8);
        l3[4] = (uint8_t)ci;
    } else {
        l3[0] = (uint8_t)((16 << 2) | 1);
    }
    int m1 = mcc / 100, m2 = (mcc / 10) % 10, m3 = mcc % 10, n1, n2, n3;
    if (mnc_digits == 3) {
        n1 = mnc / 100;
        n2 = (mnc / 10) % 10;
        n3 = mnc % 10;
    } else {
        n1 = mnc / 10;
        n2 = mnc % 10;
        n3 = 15;
    }
    l3[off] = (uint8_t)((m2 << 4) | m1);
    l3[off + 1] = (uint8_t)((n3 << 4) | m3);
    l3[off + 2] = (uint8_t)((n2 << 4) | n1);
    l3[off + 3] = (uint8_t)(lac >> 8);
    l3[off + 4] = (uint8_t)lac;
}
