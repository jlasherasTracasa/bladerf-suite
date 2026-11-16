/*
 * dvbt_phy.c — Tablas DVB-T (ver dvbt_phy.h). ETSI EN 300 744, tablas 7 y 9.
 */
#include "dvbt_phy.h"

/* Pilotos continuos de 2k (45) y portadoras TPS de 2k (17). */
static const int CP2K[45] = {0,   48,  54,  87,  141, 156, 192, 201,  255,  279,  282,  333,
                             432, 450, 483, 525, 531, 618, 636, 714,  759,  765,  780,  804,
                             873, 888, 918, 939, 942, 969, 984, 1050, 1101, 1107, 1110, 1137,
                             1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704};
static const int TPS2K[17] = {34, 50, 209, 346, 413, 569, 595, 688, 790, 901, 1073, 1219, 1262, 1286, 1469, 1594, 1687};

int dvbt_cp_list(int n, int *out)
{
    if (n == 2048) {
        for (int i = 0; i < 45; i++)
            out[i] = CP2K[i];
        return 45;
    }
    /* 8k = 2k repetido en k + m·1704 (m = 0…3); 1704, 3408 y 5112 coinciden. */
    int c = 0;
    for (int m = 0; m < 4; m++)
        for (int i = (m ? 1 : 0); i < 45; i++)
            out[c++] = CP2K[i] + 1704 * m;
    return c;
}

int dvbt_tps_list(int n, int *out)
{
    int reps = n == 2048 ? 1 : 4, c = 0;
    for (int m = 0; m < reps; m++)
        for (int i = 0; i < 17; i++)
            out[c++] = TPS2K[i] + 1704 * m;
    return c;
}

int dvbt_prbs(int k)
{
    static uint8_t w[6817];
    static int init;
    if (!init) {
        /* s[n] = s[n−11] ⊕ s[n−9], semilla de 11 unos (polinomio x^11 + x^2 + 1). */
        for (int i = 0; i < 6817; i++)
            w[i] = i < 11 ? 1 : (uint8_t)(w[i - 11] ^ w[i - 9]);
        init = 1;
    }
    return w[k];
}

const uint8_t DVBT_SYNC_EVEN[16] = {0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0};
const uint8_t DVBT_SYNC_ODD[16] = {1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1};

/* ---- BCH(67,53): división por g(x) = x^14+x^9+x^8+x^6+x^5+x^4+x^2+x+1 ---- */
#define GPOLY 0x377u /* g sin el término x^14 */

static uint32_t bch_rem(const uint8_t *w, int n)
{
    uint32_t reg = 0;
    for (int i = 0; i < n; i++) {
        uint32_t fb = ((reg >> 13) ^ w[i]) & 1;
        reg = (reg << 1) & 0x3FFF;
        if (fb)
            reg ^= GPOLY;
    }
    return reg;
}

static void put_field(uint8_t *s, int first, int nbits, int v)
{
    for (int i = 0; i < nbits; i++)
        s[first + i] = (uint8_t)((v >> (nbits - 1 - i)) & 1);
}

static int get_field(const uint8_t *s, int first, int nbits)
{
    int v = 0;
    for (int i = 0; i < nbits; i++)
        v = v << 1 | (s[first + i] & 1);
    return v;
}

void dvbt_tps_build(const dvbt_tps_t *t, uint8_t s[68])
{
    for (int i = 0; i < 68; i++)
        s[i] = 0;
    const uint8_t *sync = (t->frame & 1) ? DVBT_SYNC_ODD : DVBT_SYNC_EVEN;
    for (int i = 0; i < 16; i++)
        s[1 + i] = sync[i];
    put_field(s, 17, 6, t->length);
    put_field(s, 23, 2, t->frame);
    put_field(s, 25, 2, t->constellation);
    put_field(s, 27, 3, t->hierarchy);
    put_field(s, 30, 3, t->hp);
    put_field(s, 33, 3, t->lp);
    put_field(s, 36, 2, t->guard);
    put_field(s, 38, 2, t->mode);
    put_field(s, 40, 8, t->length == 31 ? t->cell_byte : 0);
    uint32_t par = bch_rem(s + 1, 53); /* resto de m(x)·x^14: s54 = coeficiente de x^13 */
    put_field(s, 54, 14, (int)par);
}

int dvbt_tps_check(uint8_t s[68], int *alt)
{
    static uint32_t syn[67];
    static int init;
    if (!init) {
        for (int i = 0; i < 67; i++) {
            uint8_t u[67] = {0};
            u[i] = 1;
            syn[i] = bch_rem(u, 67);
        }
        init = 1;
    }
    *alt = 0;
    uint8_t w[67];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 53; i++)
            w[i] = s[1 + i];
        for (int i = 0; i < 14; i++)
            w[53 + i] = s[pass ? 67 - i : 54 + i]; /* pasada 2: paridad en orden inverso */
        uint32_t r = bch_rem(w, 67);
        int fixed = -1, a = -1, b = -1;
        if (r == 0) {
            fixed = 0;
        } else {
            for (int i = 0; i < 67 && fixed < 0; i++)
                if (syn[i] == r) {
                    fixed = 1;
                    a = i;
                }
            for (int i = 0; i < 67 && fixed < 0; i++)
                for (int j = i + 1; j < 67; j++)
                    if ((syn[i] ^ syn[j]) == r) {
                        fixed = 2;
                        a = i;
                        b = j;
                        break;
                    }
        }
        if (fixed >= 0) {
            if (a >= 0)
                w[a] ^= 1;
            if (b >= 0)
                w[b] ^= 1;
            for (int i = 0; i < 53; i++)
                s[1 + i] = w[i];
            for (int i = 0; i < 14; i++)
                s[pass ? 67 - i : 54 + i] = w[53 + i];
            *alt = pass;
            return fixed;
        }
    }
    return -1;
}

void dvbt_tps_parse(const uint8_t s[68], dvbt_tps_t *t)
{
    t->length = get_field(s, 17, 6);
    t->frame = get_field(s, 23, 2);
    t->constellation = get_field(s, 25, 2);
    t->hierarchy = get_field(s, 27, 3);
    t->hp = get_field(s, 30, 3);
    t->lp = get_field(s, 33, 3);
    t->guard = get_field(s, 36, 2);
    t->mode = get_field(s, 38, 2);
    t->cell_byte = get_field(s, 40, 8);
    t->bch_alt = 0;
}

const char *dvbt_rate_name(int r)
{
    static const char *n[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
    return r >= 0 && r < 5 ? n[r] : "?";
}
const char *dvbt_guard_name(int g)
{
    static const char *n[] = {"1/32", "1/16", "1/8", "1/4"};
    return g >= 0 && g < 4 ? n[g] : "?";
}
const char *dvbt_const_name(int c)
{
    static const char *n[] = {"QPSK", "16-QAM", "64-QAM"};
    return c >= 0 && c < 3 ? n[c] : "?";
}
int dvbt_guard_div(int g)
{
    static const int d[] = {32, 16, 8, 4};
    return g >= 0 && g < 4 ? d[g] : 0;
}
