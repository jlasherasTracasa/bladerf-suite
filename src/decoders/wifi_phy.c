/*
 * wifi_phy.c — Tablas de la capa física 802.11 (ver wifi_phy.h).
 */
#include "wifi_phy.h"

#include "../dsp/bits.h"

static const wifi_rate_t RATES[] = {
    {6, 0xD, 1, 48, 24, 0, 0},    {9, 0xF, 1, 48, 36, 1, 0},    {12, 0x5, 2, 96, 48, 0, 0},   {18, 0x7, 2, 96, 72, 1, 0},
    {24, 0x9, 4, 192, 96, 0, 0},  {36, 0xB, 4, 192, 144, 1, 0}, {48, 0x1, 6, 288, 192, 0, 1}, {54, 0x3, 6, 288, 216, 1, 0},
};

const wifi_rate_t *wifi_rate_by_code(int code)
{
    for (size_t i = 0; i < ARRAY_LEN(RATES); i++)
        if (RATES[i].code == code)
            return &RATES[i];
    return NULL;
}

const wifi_rate_t *wifi_rate_by_mbps(int mbps)
{
    for (size_t i = 0; i < ARRAY_LEN(RATES); i++)
        if (RATES[i].mbps == mbps)
            return &RATES[i];
    return NULL;
}

int wifi_data_carrier(int i)
{
    int n = 0;
    for (int k = -26; k <= 26; k++) {
        if (k == 0 || k == -21 || k == -7 || k == 7 || k == 21)
            continue;
        if (n++ == i)
            return k;
    }
    return 0;
}

int wifi_pilot_polarity(int n)
{
    static int8_t tab[127];
    static int init;
    if (!init) {
        lfsr_t l;
        lfsr_init_wifi(&l, 0x7F);
        for (int i = 0; i < 127; i++)
            tab[i] = lfsr_prbs_bit(&l) ? -1 : 1;
        init = 1;
    }
    return tab[((n % 127) + 127) % 127];
}

void wifi_stf_bins(cf32 out[64])
{
    /* S(−26…26) = √(13/6)·{…}: solo cada 4.º subportador es no nulo (periodo 16). */
    static const int8_t sgn[13] = {1, -1, 1, -1, -1, 1, 0, -1, -1, 1, 1, 1, 1}; /* k = −24, −20, … 24 */
    const float a = sqrtf(13.0f / 6.0f);
    for (int i = 0; i < 64; i++)
        out[i] = (cf32){0, 0};
    for (int i = 0; i < 13; i++) {
        int k = -24 + 4 * i;
        out[wifi_bin(k)] = (cf32){a * sgn[i], a * sgn[i]};
    }
}

void wifi_ltf_bins(float out[64])
{
    static const int8_t L[53] = {1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1,
                                 0, 1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1};
    for (int i = 0; i < 64; i++)
        out[i] = 0;
    for (int k = -26; k <= 26; k++)
        out[wifi_bin(k)] = L[k + 26];
}

void wifi_interleave_table(const wifi_rate_t *r, int *j_of_k)
{
    int n = r->ncbps, s = r->nbpsc / 2 > 1 ? r->nbpsc / 2 : 1;
    for (int k = 0; k < n; k++) {
        int i = (n / 16) * (k % 16) + k / 16;                  /* 1.ª permutación */
        j_of_k[k] = s * (i / s) + (i + n - (16 * i) / n) % s;  /* 2.ª permutación */
    }
}

conv_code_t wifi_conv_code(void)
{
    conv_code_t c = conv_nasa_k7(); /* (171,133): 802.11 usa A=133 primero, B=171 */
    uint32_t t = c.poly[0];
    c.poly[0] = c.poly[1];
    c.poly[1] = t;
    return c;
}

const conv_punct_t *wifi_punct(const wifi_rate_t *r)
{
    static const uint8_t keep34[6] = {1, 1, 1, 0, 0, 1}; /* [t][A,B]: A0 B0 A1 · · B2 */
    static const conv_punct_t p34 = {3, 2, keep34};
    static const uint8_t keep23[4] = {1, 1, 1, 0};       /* A0 B0 A1 · */
    static const conv_punct_t p23 = {2, 2, keep23};
    return r->r34 ? &p34 : r->r23 ? &p23 : NULL;
}

int wifi_coded_bits(int ninfo, const conv_punct_t *p)
{
    if (!p)
        return 2 * ninfo;
    int n = 0;
    for (int t = 0; t < ninfo; t++)
        n += p->keep[(t % p->period) * 2] + p->keep[(t % p->period) * 2 + 1];
    return n;
}

const int8_t WIFI_BARKER[11] = {1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1};

uint16_t wifi_plcp_crc16(const uint8_t *bits, int n)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        int fb = ((crc >> 15) ^ bits[i]) & 1;
        crc = (uint16_t)(crc << 1);
        if (fb)
            crc ^= 0x1021;
    }
    return (uint16_t)~crc;
}
