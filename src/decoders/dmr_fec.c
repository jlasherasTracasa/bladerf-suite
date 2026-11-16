/*
 * dmr_fec.c — Códigos de canal de DMR (ETSI TS 102 361-1 V2.5.1, anexo B).
 * Ver dmr_fec.h. Las matrices son las impresas en la norma (tablas B.11 a B.17),
 * guardadas como la parte P de [I | P] en cadenas de '0'/'1' para poder
 * contrastarlas a simple vista con el PDF.
 */
#include "dmr_fec.h"

#include "../dsp/fec.h"

#include <pthread.h>
#include <string.h>

static linblock_t lb_golay, lb_qr, lb_h139, lb_h1511, lb_h1611, lb_h74;
static rs_t rs129;
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* Construye el código [I_k | P] con P dado fila a fila (k cadenas de n−k bits). */
static void make_code(linblock_t *lb, int n, int k, const char *const *tails)
{
    uint32_t rows[12];
    for (int i = 0; i < k; i++) {
        uint32_t v = 1u << (n - 1 - i);
        for (int j = 0; j < n - k; j++)
            if (tails[i][j] == '1')
                v |= 1u << (n - k - 1 - j);
        rows[i] = v;
    }
    linblock_init(lb, n, k, rows);
}

static void init_all(void)
{
    /* Tabla B.11: Golay (20,8) */
    static const char *const golay[8] = {"001111011010", "110110011001", "011011001101", "001101100111",
                                         "110111000110", "101010010111", "100100111110", "100011101011"};
    /* Tabla B.12: QR (16,7,6) */
    static const char *const qr[7] = {"001001111", "100011110", "110110111", "111100010",
                                      "111001001", "011100101", "001110011"};
    /* Tabla B.14: Hamming (13,9,3) */
    static const char *const h139[9] = {"1111", "1110", "0111", "1010", "0101", "1011", "1100", "0110", "0011"};
    /* Tabla B.15: Hamming (15,11,3) */
    static const char *const h1511[11] = {"1001", "1101", "1111", "1110", "0111", "1010",
                                          "0101", "1011", "1100", "0110", "0011"};
    /* Tabla B.16: Hamming (16,11,4) */
    static const char *const h1611[11] = {"10011", "11010", "11111", "11100", "01110", "10101",
                                          "01011", "10110", "11001", "01101", "00111"};
    /* Tabla B.17: Hamming (7,4,3) */
    static const char *const h74[4] = {"101", "111", "110", "011"};
    make_code(&lb_golay, 20, 8, golay);
    make_code(&lb_qr, 16, 7, qr);
    make_code(&lb_h139, 13, 9, h139);
    make_code(&lb_h1511, 15, 11, h1511);
    make_code(&lb_h1611, 16, 11, h1611);
    make_code(&lb_h74, 7, 4, h74);
    rs_init(&rs129, 0x11D, 1, 1, 3); /* g(x) = (x+α)(x+α²)(x+α³) */
}

void dmr_fec_init(void) { pthread_once(&once, init_all); }

int dmr_fec_dmin(int code)
{
    dmr_fec_init();
    const linblock_t *l[6] = {&lb_golay, &lb_qr, &lb_h139, &lb_h1511, &lb_h1611, &lb_h74};
    return code >= 0 && code < 6 ? l[code]->dmin : -1;
}

/* ------------------------------------------------------------------------- */
uint32_t dmr_slottype_encode(int cc, int dt)
{
    dmr_fec_init();
    return linblock_encode(&lb_golay, (uint32_t)((cc & 15) << 4 | (dt & 15)));
}

int dmr_slottype_decode(uint32_t w20, int *cc, int *dt)
{
    dmr_fec_init();
    uint32_t cw = w20, data = 0;
    int r = linblock_decode(&lb_golay, &cw, &data);
    if (r < 0)
        return -1;
    *cc = (int)(data >> 4);
    *dt = (int)(data & 15);
    return r;
}

uint32_t dmr_emb_encode(int cc, int pi, int lcss)
{
    dmr_fec_init();
    return linblock_encode(&lb_qr, (uint32_t)((cc & 15) << 3 | (pi & 1) << 2 | (lcss & 3)));
}

int dmr_emb_decode(uint32_t w16, int *cc, int *pi, int *lcss)
{
    dmr_fec_init();
    uint32_t cw = w16, data = 0;
    int r = linblock_decode(&lb_qr, &cw, &data);
    if (r < 0)
        return -1;
    *cc = (int)(data >> 3);
    *pi = (int)((data >> 2) & 1);
    *lcss = (int)(data & 3);
    return r;
}

uint32_t dmr_tact_encode(int at, int tc, int lcss)
{
    dmr_fec_init();
    return linblock_encode(&lb_h74, (uint32_t)((at & 1) << 3 | (tc & 1) << 2 | (lcss & 3)));
}

int dmr_tact_decode(uint32_t w7, int *at, int *tc, int *lcss)
{
    dmr_fec_init();
    uint32_t cw = w7, data = 0;
    int r = linblock_decode(&lb_h74, &cw, &data);
    if (r < 0)
        return -1;
    *at = (int)(data >> 3);
    *tc = (int)((data >> 2) & 1);
    *lcss = (int)(data & 3);
    return 0;
}

/* ------------------------------------------------------------------------- */
/*  CACH (figura B.9): posiciones temporales                                 */
/* ------------------------------------------------------------------------- */
static const uint8_t TACT_POS[7] = {0, 4, 8, 12, 14, 18, 22};          /* AT TC LS1 LS0 H2 H1 H0 */
static const uint8_t PAY_POS[17] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 15, 16, 17, 19, 20, 21, 23}; /* P16..P0 */

void dmr_cach_build(uint32_t tact7, const uint8_t *payload17, uint8_t *bits24)
{
    for (int i = 0; i < 7; i++)
        bits24[TACT_POS[i]] = (uint8_t)((tact7 >> (6 - i)) & 1);
    for (int i = 0; i < 17; i++)
        bits24[PAY_POS[i]] = payload17[i];
}

void dmr_cach_split(const uint8_t *bits24, uint32_t *tact7, uint8_t *payload17)
{
    uint32_t t = 0;
    for (int i = 0; i < 7; i++)
        t = (t << 1) | bits24[TACT_POS[i]];
    *tact7 = t;
    for (int i = 0; i < 17; i++)
        payload17[i] = bits24[PAY_POS[i]];
}

/* ------------------------------------------------------------------------- */
/*  BPTC (196,96)                                                            */
/* ------------------------------------------------------------------------- */
#define BR 13
#define BC 15

static uint32_t row_word(uint8_t m[BR][BC], int r)
{
    uint32_t v = 0;
    for (int c = 0; c < BC; c++)
        v = (v << 1) | m[r][c];
    return v;
}

static uint32_t col_word(uint8_t m[BR][BC], int c)
{
    uint32_t v = 0;
    for (int r = 0; r < BR; r++)
        v = (v << 1) | m[r][c];
    return v;
}

void dmr_bptc_encode(const uint8_t *info96, uint8_t *out196)
{
    dmr_fec_init();
    uint8_t m[BR][BC];
    memset(m, 0, sizeof m);
    int pos = 0;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 11; c++)
            m[r][c] = (r == 0 && c < 3) ? 0 : info96[pos++]; /* R(2..0) = 0 */
    for (int r = 0; r < 9; r++) {
        uint32_t w = linblock_encode(&lb_h1511, row_word(m, r) >> 4);
        for (int c = 0; c < BC; c++)
            m[r][c] = (uint8_t)((w >> (BC - 1 - c)) & 1);
    }
    for (int c = 0; c < BC; c++) {
        uint32_t w = linblock_encode(&lb_h139, col_word(m, c) >> 4);
        for (int r = 0; r < BR; r++)
            m[r][c] = (uint8_t)((w >> (BR - 1 - r)) & 1);
    }
    out196[0] = 0; /* R(3): índice 0 → posición 0 */
    for (int i = 1; i < 196; i++) {
        int r = (i - 1) / BC, c = (i - 1) % BC;
        out196[(i * 181) % 196] = m[r][c];
    }
}

int dmr_bptc_decode(const uint8_t *in196, uint8_t *info96)
{
    dmr_fec_init();
    uint8_t m[BR][BC];
    for (int i = 1; i < 196; i++)
        m[(i - 1) / BC][(i - 1) % BC] = in196[(i * 181) % 196];
    int total = 0;
    for (int pass = 0; pass < 4; pass++) {
        bool bad = false, changed = false;
        for (int r = 0; r < 9; r++) {
            uint32_t cw = row_word(m, r), d;
            int e = linblock_decode(&lb_h1511, &cw, &d);
            if (e < 0)
                bad = true;
            else if (e > 0) {
                total += e;
                changed = true;
                for (int c = 0; c < BC; c++)
                    m[r][c] = (uint8_t)((cw >> (BC - 1 - c)) & 1);
            }
        }
        for (int c = 0; c < BC; c++) {
            uint32_t cw = col_word(m, c), d;
            int e = linblock_decode(&lb_h139, &cw, &d);
            if (e < 0)
                bad = true;
            else if (e > 0) {
                total += e;
                changed = true;
                for (int r = 0; r < BR; r++)
                    m[r][c] = (uint8_t)((cw >> (BR - 1 - r)) & 1);
            }
        }
        if (!bad && !changed) {
            int pos = 0;
            for (int r = 0; r < 9; r++)
                for (int c = 0; c < 11; c++)
                    if (!(r == 0 && c < 3))
                        info96[pos++] = m[r][c];
            return total;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
void dmr_rs129_parity(const uint8_t *d9, uint8_t *p3)
{
    dmr_fec_init();
    rs_encode(&rs129, d9, 9, p3);
}

int dmr_rs129_check(uint8_t *block12)
{
    dmr_fec_init();
    return rs_decode(&rs129, block12, 12, NULL, 0);
}

uint16_t dmr_crc_ccitt(const uint8_t *bytes, int n)
{
    uint16_t reg = 0;
    for (int i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            int fb = ((reg >> 15) & 1) ^ ((bytes[i] >> b) & 1);
            reg = (uint16_t)(reg << 1);
            if (fb)
                reg ^= 0x1021;
        }
    return (uint16_t)(reg ^ 0xFFFF);
}

/* ------------------------------------------------------------------------- */
/*  LC embebido (figura B.3)                                                 */
/* ------------------------------------------------------------------------- */
static int lc_cs(const uint8_t *lc9)
{
    int s = 0;
    for (int i = 0; i < 9; i++)
        s += lc9[i];
    return s % 31;
}

void dmr_emblc_encode(const uint8_t *lc9, uint32_t *frag4)
{
    dmr_fec_init();
    uint8_t lc[72], m[8][16];
    for (int i = 0; i < 72; i++)
        lc[i] = (uint8_t)((lc9[i / 8] >> (7 - i % 8)) & 1);
    int cs = lc_cs(lc9), pos = 0;
    for (int r = 0; r < 7; r++) {
        uint32_t d = 0;
        int nlc = r < 2 ? 11 : 10;
        for (int j = 0; j < nlc; j++)
            d = (d << 1) | lc[pos++];
        if (r >= 2)
            d = (d << 1) | (uint32_t)((cs >> (6 - r)) & 1); /* CS(4) en la fila 2 … CS(0) en la 6 */
        uint32_t w = linblock_encode(&lb_h1611, d);
        for (int c = 0; c < 16; c++)
            m[r][c] = (uint8_t)((w >> (15 - c)) & 1);
    }
    for (int c = 0; c < 16; c++) {
        uint8_t p = 0;
        for (int r = 0; r < 7; r++)
            p ^= m[r][c];
        m[7][c] = p;
    }
    memset(frag4, 0, 4 * sizeof(uint32_t));
    for (int n = 0; n < 128; n++)
        if (m[n % 8][n / 8])
            frag4[n / 32] |= 1u << (31 - n % 32);
}

int dmr_emblc_decode(const uint32_t *frag4, uint8_t *lc9)
{
    dmr_fec_init();
    uint8_t m[8][16];
    for (int n = 0; n < 128; n++)
        m[n % 8][n / 8] = (uint8_t)((frag4[n / 32] >> (31 - n % 32)) & 1);
    int total = 0;
    uint8_t lc[72];
    int pos = 0, cs = 0;
    for (int r = 0; r < 7; r++) {
        uint32_t cw = 0, d = 0;
        for (int c = 0; c < 16; c++)
            cw = (cw << 1) | m[r][c];
        int e = linblock_decode(&lb_h1611, &cw, &d);
        if (e < 0)
            return -1;
        total += e;
        for (int c = 0; c < 16; c++)
            m[r][c] = (uint8_t)((cw >> (15 - c)) & 1);
        int nlc = r < 2 ? 11 : 10;
        for (int j = 0; j < nlc; j++)
            lc[pos++] = (uint8_t)((d >> (10 - j)) & 1);
        if (r >= 2)
            cs |= (int)(d & 1) << (6 - r);
    }
    /* Paridad par de columna: un único fallo es un error en la propia fila de
     * paridad (inofensivo: los datos ya están corregidos por el Hamming); dos o
     * más indican filas mal corregidas. */
    int bad_cols = 0;
    for (int c = 0; c < 16; c++) {
        uint8_t p = 0;
        for (int r = 0; r < 8; r++)
            p ^= m[r][c];
        bad_cols += p;
    }
    if (bad_cols > 1)
        return -1;
    total += bad_cols;
    memset(lc9, 0, 9);
    for (int i = 0; i < 72; i++)
        lc9[i / 8] |= (uint8_t)(lc[i] << (7 - i % 8));
    if (lc_cs(lc9) != cs)
        return -1;
    return total;
}
