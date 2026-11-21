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
        u[i] = (uint8_t)((l3[i >> 3] >> (i & 7)) & 1);       /* LSB del octeto primero */
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
        l3[i >> 3] |= (uint8_t)(u[i] << (i & 7));
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Nivel 3: System Information (44.018 §9.1.31–9.1.36)                      */
/* ------------------------------------------------------------------------ */
/* Todas las SI de la ráfaga BCCH normal son «mensajes con pseudo-longitud L2» de 23 octetos
 * (44.006 §5.5): octeto 1 = L2 pseudo length (6 bits de longitud + «01»), octeto 2 = skip
 * indicator|PD (0x06 = RR), octeto 3 = tipo de mensaje y después los IE. Los octetos sobrantes
 * («rest octets» o relleno 0x2B) no se interpretan salvo en la pseudo-longitud.
 *
 *   SI1  (0x19): CCD(16) RACH(3) rest(1)             §9.1.31, IE 10.5.2.1b y 10.5.2.29
 *   SI2  (0x1A): NCD(16) NCC permitidos(1) RACH(3)   §9.1.32, IE 10.5.2.22 y 10.5.2.27
 *   SI2bis(0x02): EBCCH list(16) RACH(3) rest(1)     §9.1.33
 *   SI2ter(0x03): EBCCH list(16) rest(4)             §9.1.34
 *   SI3  (0x1B): CI(2) LAI(5) CCD(3) opciones(1) selección(2) RACH(3) rest(4)   §9.1.35
 *   SI4  (0x1C): LAI(5) selección(2) RACH(3) [CBCH] rest                         §9.1.36
 * LAI (10.5.1.3): MCC2|MCC1, MNC3|MCC3, MNC2|MNC1, LAC(2). */

static int bit(int v, int hi) { return (v >> (hi - 1)) & 1; }    /* bit 'hi' (1..8) de un octeto */

/* Lista de frecuencias «bit map 0» (10.5.2.1b.2 / 10.5.2.22): octeto 1 con bits 8,7 = 00 (y en la
 * NCD: bit 6 = EXT-IND, bit 5 = BA-IND) y bits 4..1 = ARFCN 124..121; octeto i (2..16):
 * ARFCN 128−8i+8 … 128−8i+1; es decir, el ARFCN n (1..124) está en el octeto 16−(n−1)/8, bit (n−1)%8+1. */
static void parse_arfcn_list(const uint8_t *o, gsm_si_t *si, bool ba)
{
    si->arfcn_fmt = o[0];
    si->n_arfcn = 0;
    si->arfcn_ok = false;
    if ((o[0] & 0xC0) != 0)
        return;                         /* rangos, 1024, bit map variable: no implementado */
    si->arfcn_ok = true;
    if (ba)
        si->ba_ind = bit(o[0], 5);
    for (int n = 1; n <= 124; n++)
        if ((o[15 - (n - 1) / 8] >> ((n - 1) % 8)) & 1)
            si->arfcn[si->n_arfcn++] = (uint16_t)n;
}

static void parse_rach(const uint8_t *o, gsm_si_t *si)
{
    static const int retr[4] = {1, 2, 4, 7};
    si->rach_ok = true;
    si->max_retrans = retr[o[0] >> 6];
    si->tx_integer = (o[0] >> 2) & 15;       /* ranura de dispersión: 3..12, 14, 16, 20, 25, 32, 50 */
    si->cell_bar = bit(o[0], 2);
    si->reest_allowed = !bit(o[0], 1);       /* RE = 1: re-establecimiento NO permitido */
    si->ac_barred = (unsigned)(o[1] << 8 | o[2]);   /* octeto 2: C15..C8, octeto 3: C7..C0 */
    /* 10.5.2.29: C10 (bit 3 del octeto 2) es la «emergencia»; aquí se devuelve la máscara tal cual */
}

static void parse_sel(const uint8_t *o, gsm_si_t *si)
{
    si->sel_ok = true;
    si->hyst_db = 2 * (o[0] >> 5);                 /* CELL-RESELECT-HYSTERESIS: 2 dB por paso */
    si->txpwr_max_cch = o[0] & 31;
    si->acs = bit(o[1], 8);
    si->neci = bit(o[1], 7);
    si->rxlev_min_dbm = (o[1] & 63) - 110;          /* RXLEV-ACCESS-MIN: −110 + n dBm */
}

static int parse_lai(const uint8_t *o, gsm_si_t *si)
{
    int mcc1 = o[0] & 15, mcc2 = o[0] >> 4, mcc3 = o[1] & 15;
    int mnc3 = o[1] >> 4, mnc1 = o[2] & 15, mnc2 = o[2] >> 4;
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
    si->lac = (o[3] << 8) | o[4];
    si->lai_ok = true;
    return 0;
}

int gsm_si_parse(const uint8_t l3[23], gsm_si_t *si)
{
    memset(si, 0, sizeof *si);
    si->ci = -1;
    si->ba_ind = -1;
    si->ncc_permitted = -1;
    if ((l3[0] & 3) != 1 || (l3[1] & 0x0F) != 0x06)   /* pseudo-longitud L2 y PD de RR */
        return -1;
    si->type = l3[2];
    switch (si->type) {
    case GSM_SI1:
        parse_arfcn_list(l3 + 3, si, false);
        parse_rach(l3 + 19, si);
        break;
    case GSM_SI2:
        parse_arfcn_list(l3 + 3, si, true);
        si->ncc_permitted = l3[19];
        parse_rach(l3 + 20, si);
        break;
    case GSM_SI2BIS:
        parse_arfcn_list(l3 + 3, si, true);
        parse_rach(l3 + 19, si);
        break;
    case GSM_SI2TER:
        parse_arfcn_list(l3 + 3, si, true);
        break;
    case GSM_SI3:
        si->ci = (l3[3] << 8) | l3[4];
        if (parse_lai(l3 + 5, si) < 0)
            return -1;
        si->cc_ok = true;                                 /* 10.5.2.11 */
        si->att = bit(l3[10], 7);
        si->bs_ag_blks_res = (l3[10] >> 3) & 7;
        si->ccch_conf = l3[10] & 7;
        si->bs_pa_mfrms = (l3[11] & 7) + 2;
        si->t3212_decih = l3[12];                         /* 0 = periódica desactivada */
        si->opt_ok = true;                                /* 10.5.2.3 */
        si->pwrc = bit(l3[13], 7);
        si->dtx = (l3[13] >> 4) & 3;
        si->radio_link_timeout = ((l3[13] & 15) + 1) * 4;
        parse_sel(l3 + 14, si);
        parse_rach(l3 + 16, si);
        break;
    case GSM_SI4:
        if (parse_lai(l3 + 3, si) < 0)
            return -1;
        parse_sel(l3 + 8, si);
        parse_rach(l3 + 10, si);
        break;
    default:
        break;
    }
    return 0;
}

int gsm_arfcn_from_hz(double hz)
{
    double m = hz / 1e6;
    int n;
    if (m >= 869.0 && m <= 894.0 + 1e-6) {                 /* GSM 850: 128–251 */
        n = (int)lround((m - 869.2) / 0.2) + 128;
        return n >= 128 && n <= 251 ? n : -1;
    }
    if (m >= 920.0 && m <= 960.0 + 1e-6) {                 /* E/R-GSM 900: 0–124, 955–1023 */
        n = (int)lround((m - 935.0) / 0.2);
        if (n >= 0 && n <= 124)
            return n;
        n += 1024;                                          /* DL = 935 + 0,2·(n−1024) */
        return n >= 955 && n <= 1023 ? n : -1;
    }
    if (m >= 1805.0 && m <= 1880.0 + 1e-6) {               /* DCS 1800: 512–885 */
        n = (int)lround((m - 1805.2) / 0.2) + 512;
        return n >= 512 && n <= 885 ? n : -1;
    }
    if (m >= 1930.0 && m <= 1990.0 + 1e-6) {               /* PCS 1900: 512–810 */
        n = (int)lround((m - 1930.2) / 0.2) + 512;
        return n >= 512 && n <= 810 ? n : -1;
    }
    return -1;
}

/* ---- Constructor (para el modulador y los tests) ---- */
static void put_lai(uint8_t *o, int mcc, int mnc, int mnc_digits, int lac)
{
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
    o[0] = (uint8_t)((m2 << 4) | m1);
    o[1] = (uint8_t)((n3 << 4) | m3);
    o[2] = (uint8_t)((n2 << 4) | n1);
    o[3] = (uint8_t)(lac >> 8);
    o[4] = (uint8_t)lac;
}

/* RACH: 7 reintentos (bits 8,7 = 11), TX-integer 9, sin CELL-BAR, RE = 1, clases 0–9, 11–15 libres. */
static void put_rach(uint8_t *o)
{
    o[0] = (uint8_t)((3 << 6) | (9 << 2) | 1);
    o[1] = 0;
    o[2] = 0;
}

void gsm_si_build_arfcn(int type, const int *arfcn, int n, uint8_t l3[23])
{
    memset(l3, 0x2B, 23);
    l3[1] = 0x06;
    l3[2] = (uint8_t)type;
    memset(l3 + 3, 0, 16);
    for (int i = 0; i < n; i++)
        if (arfcn[i] >= 1 && arfcn[i] <= 124)
            l3[3 + 15 - (arfcn[i] - 1) / 8] |= (uint8_t)(1 << ((arfcn[i] - 1) % 8));
    if (type == GSM_SI2)
        l3[3] |= 0x10;                       /* BA-IND = 1 */
    if (type == GSM_SI1) {
        l3[0] = (uint8_t)((19 << 2) | 1);   /* 2 + 16 + 3 octetos antes de los rest octets */
        put_rach(l3 + 19);
    } else if (type == GSM_SI2) {
        l3[0] = (uint8_t)((21 << 2) | 1);
        l3[19] = 0xFF;                       /* NCC permitidos: todos */
        put_rach(l3 + 20);
    } else if (type == GSM_SI2BIS) {
        l3[0] = (uint8_t)((21 << 2) | 1);
        put_rach(l3 + 19);
    } else {
        l3[0] = (uint8_t)((18 << 2) | 1);
    }
}

void gsm_si_build(int type, int mcc, int mnc, int mnc_digits, int lac, int ci, uint8_t l3[23])
{
    if (type != GSM_SI3 && type != GSM_SI4) {
        static const int cell[] = {1, 5, 9, 17, 33, 60, 124};
        gsm_si_build_arfcn(type, cell, 7, l3);
        return;
    }
    memset(l3, 0x2B, 23);              /* relleno ("L2 fill bits" 0x2B) */
    l3[1] = 0x06;
    l3[2] = (uint8_t)type;
    uint8_t *sel, *rach;
    if (type == GSM_SI3) {
        l3[0] = (uint8_t)((18 << 2) | 1);   /* pseudo-longitud */
        l3[3] = (uint8_t)(ci >> 8);
        l3[4] = (uint8_t)ci;
        put_lai(l3 + 5, mcc, mnc, mnc_digits, lac);
        l3[10] = (uint8_t)((1 << 6) | (1 << 3) | 0);     /* ATT, 1 bloque AGCH reservado, CCCH_CONF 0 */
        l3[11] = 0;                                       /* BS-PA-MFRMS = 2 */
        l3[12] = GSM_SI_DEF_T3212;
        l3[13] = (uint8_t)((2 << 4) | 7);                 /* DTX 2, radio link timeout (7+1)·4 = 32 */
        sel = l3 + 14;
        rach = l3 + 16;
    } else {
        l3[0] = (uint8_t)((12 << 2) | 1);
        put_lai(l3 + 3, mcc, mnc, mnc_digits, lac);
        sel = l3 + 8;
        rach = l3 + 10;
    }
    sel[0] = (uint8_t)(((GSM_SI_DEF_HYST_DB / 2) << 5) | GSM_SI_DEF_TXPWR);
    sel[1] = (uint8_t)((1 << 6) | (GSM_SI_DEF_RXLEV_MIN_DBM + 110));   /* NECI = 1 */
    put_rach(rach);
}
