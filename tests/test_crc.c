/*
 * test_crc.c — Pruebas de src/dsp/crc.* y src/dsp/bits.*
 *
 * Valores de comprobación ("123456789"): catálogo de Greg Cook,
 * https://reveng.sourceforge.io/crc-catalogue/ (cada preset lleva el suyo).
 * Ejemplo Mode S: mensaje DF17 8D4840D6202CC371C32CE0576098 (J. Sun,
 * "The 1090MHz Riddle"), cuyo CRC-24 sobre los 11 primeros bytes es 0x576098.
 */
#include "dsp/bits.h"
#include "dsp/crc.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_n;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Referencia bit a bit del modelo de Rocksoft (lenta, independiente de la tabla). */
static uint32_t ref_crc(const crc_params_t *p, const uint8_t *d, int n)
{
    uint32_t mask = p->width == 32 ? 0xFFFFFFFFu : (1u << p->width) - 1;
    uint32_t reg = p->init & mask;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) {
            int bit = p->refin ? (d[i] >> b) & 1 : (d[i] >> (7 - b)) & 1;
            int top = (reg >> (p->width - 1)) & 1;
            reg = (reg << 1) & mask;
            if (top ^ bit)
                reg ^= p->poly;
        }
    if (p->refout)
        reg = bits_reverse(reg, p->width);
    return (reg ^ p->xorout) & mask;
}

static void test_crc(void)
{
    const uint8_t *s = (const uint8_t *)"123456789";
    int np = 0;
    for (int i = 0; crc_preset_at(i); i++, np++) {
        const crc_params_t *p = crc_preset_at(i);
        crc_t c;
        CHECK(crc_init(&c, p) == 0);
        uint32_t got = crc_compute(&c, s, 9);
        if (got != p->check)
            printf("  %s: obtenido %X, check %X\n", p->name, got, p->check);
        CHECK(got == p->check);
        CHECK(ref_crc(p, s, 9) == got);
        /* incremental == de golpe, y por bits (orden de transmisión) == por bytes */
        uint32_t r = crc_start(&c);
        r = crc_update(&c, r, s, 4);
        r = crc_update(&c, r, s + 4, 5);
        CHECK(crc_finish(&c, r) == got);
        uint8_t bits[72];
        for (int k = 0; k < 9; k++)
            for (int b = 0; b < 8; b++)
                bits[k * 8 + b] = p->refin ? (s[k] >> b) & 1 : (s[k] >> (7 - b)) & 1;
        CHECK(crc_compute_bits(&c, bits, 72) == got);
    }
    printf("CRC: %d presets\n", np);
    CHECK(crc_preset("hdlc") == crc_preset("CRC-16/X-25"));
    CHECK(crc_preset("CRC-16/CCITT-FALSE") && crc_preset("CRC-16/KERMIT") && crc_preset("CRC-16/GSM"));
    CHECK(crc_preset("nope") == NULL);
}

static void test_crc_misc(void)
{
    /* Mode S */
    static const uint8_t m[14] = {0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98};
    CHECK(crc24_modes(m, 11) == 0x576098);
    CHECK(crc24_modes(m, 14) == 0);  /* con la paridad incluida el resto es 0 */
    /* Residuo HDLC: datos + FCS (LSB primero) -> registro 0xF0B8 */
    crc_t x;
    crc_init(&x, crc_preset("HDLC"));
    uint8_t f[12] = "ABCDEFGHxx";
    uint32_t fcs = crc_compute(&x, f, 8);
    f[8] = (uint8_t)fcs;
    f[9] = (uint8_t)(fcs >> 8);
    uint32_t reg = crc_update(&x, crc_start(&x), f, 10);
    CHECK(reg == 0xF0B8);
    CHECK(crc_finish(&x, reg) == (0xF0B8u ^ 0xFFFF));
    /* LTE: CRC-24A de un bloque + su CRC => 0 */
    crc_t l;
    crc_init(&l, crc_preset("CRC-24A"));
    uint8_t g[8] = {1, 2, 3, 4, 5};
    uint32_t v = crc_compute(&l, g, 5);
    g[5] = (uint8_t)(v >> 16); g[6] = (uint8_t)(v >> 8); g[7] = (uint8_t)v;
    CHECK(crc_compute(&l, g, 8) == 0);
}

static void test_bits(void)
{
    uint8_t bits[19], out[3], back[19];
    for (int i = 0; i < 19; i++)
        bits[i] = (uint8_t)((i * 7 + 3) % 5 < 2);
    bits_pack_msb(bits, 19, out);
    bits_unpack_msb(out, 19, back);
    CHECK(!memcmp(bits, back, 19));
    bits_pack_lsb(bits, 19, out);
    bits_unpack_lsb(out, 19, back);
    CHECK(!memcmp(bits, back, 19));
    uint8_t one[2] = {1, 0};
    bits_pack_msb(one, 2, out);
    CHECK(out[0] == 0x80);
    bits_pack_lsb(one, 2, out);
    CHECK(out[0] == 0x01);

    uint8_t m[4] = {0xA5, 0x3C, 0xFF, 0x01};
    CHECK(bits_get(m, 1, 8) == 0xA5);
    CHECK(bits_get(m, 5, 12) == 0x53);
    CHECK(bits_get(m, 1, 32) == 0xA53CFF01u);
    bits_set(m, 5, 12, 0xAB);
    CHECK(bits_get(m, 5, 12) == 0xAB && bits_get(m, 1, 4) == 0xA && bits_get(m, 13, 16) == 0xC);
    bits_flip(m, 0);
    CHECK(bits_get(m, 1, 1) == 0);

    CHECK(bits_reverse8(0x01) == 0x80 && bits_reverse8(0x2C) == 0x34);
    CHECK(bits_reverse(0x1, 24) == 0x800000 && bits_reverse(0xF0F0F0F0u, 32) == 0x0F0F0F0Fu);
    CHECK(bits_popcount32(0xF0F0) == 8 && bits_popcount64(~0ULL) == 64);
    CHECK(bits_popcount_buf(m, 4) > 0 && bits_parity32(7) == 1);
    CHECK(bits_hamming_dist(m, m, 4) == 0);

    /* búsqueda de sincronía con tolerancia */
    uint8_t s[40] = {0};
    uint64_t pat = 0x7E; /* 01111110 */
    for (int i = 0; i < 8; i++)
        s[13 + i] = (uint8_t)((pat >> (7 - i)) & 1);
    CHECK(bits_find_sync(s, 40, pat, 8, 0, 0) == 13);
    s[15] ^= 1;
    CHECK(bits_find_sync(s, 40, pat, 8, 0, 0) == -1);
    CHECK(bits_find_sync(s, 40, pat, 8, 1, 0) == 13);

    /* NRZI ida y vuelta, y convención HDLC (0 = transición) */
    uint8_t d[32], e[32], r[32];
    for (int i = 0; i < 32; i++)
        d[i] = (uint8_t)((i * 5 + 1) % 3 == 0);
    for (int oc = 0; oc < 2; oc++) {
        int last = nrzi_encode(d, 32, e, 1, oc);
        CHECK(last == e[31]);
        nrzi_decode(e, 32, r, 1, oc);
        CHECK(!memcmp(d, r, 32));
    }
    uint8_t z[3] = {0, 0, 1}, ze[3];
    nrzi_encode(z, 3, ze, 0, 0);
    CHECK(ze[0] == 1 && ze[1] == 0 && ze[2] == 0);
}

static void test_lfsr(void)
{
    /* WiFi 802.11-2012 §17.3.5.5: con estado 1111111 la secuencia de 127 bits
     * empieza 00001110 11110010 11001001 ... y se repite con periodo 127. */
    lfsr_t l;
    lfsr_init_wifi(&l, 0x7F);
    uint8_t seq[254];
    for (int i = 0; i < 254; i++)
        seq[i] = (uint8_t)lfsr_prbs_bit(&l);
    uint8_t packed[4];
    bits_pack_msb(seq, 24, packed);
    CHECK(packed[0] == 0x0E && packed[1] == 0xF2 && packed[2] == 0xC9);
    CHECK(!memcmp(seq, seq + 127, 127));
    int period_ok = 1;
    for (int p = 1; p < 127; p++)
        if (!memcmp(seq, seq + p, 127))
            period_ok = 0;
    CHECK(period_ok);

    /* DVB: primer byte de la PRBS 0x03 (init 100101010000000), periodo 32767 */
    lfsr_init_dvb(&l);
    uint8_t first[8];
    for (int i = 0; i < 8; i++)
        first[i] = (uint8_t)lfsr_prbs_bit(&l);
    bits_pack_msb(first, 8, packed);
    CHECK(packed[0] == 0x03);
    lfsr_init_dvb(&l);
    int n = 0;
    do { lfsr_prbs_bit(&l); n++; } while (l.state != 0x0A9 && n < 40000);
    CHECK(n == 32767);

    /* DAB: periodo 511 */
    lfsr_init_dab(&l);
    n = 0;
    do { lfsr_prbs_bit(&l); n++; } while (l.state != 0x1FF && n < 1000);
    CHECK(n == 511);

    /* aditivo: aplicar dos veces = identidad; bytes == bits */
    uint8_t data[16], t1[16], t2[16], bb[128], bo[128], bp[16];
    for (int i = 0; i < 16; i++)
        data[i] = (uint8_t)(i * 37 + 11);
    lfsr_init_dab(&l); lfsr_process_bytes(&l, data, 16, t1, 0, 1);
    lfsr_init_dab(&l); lfsr_process_bytes(&l, t1, 16, t2, 0, 1);
    CHECK(!memcmp(data, t2, 16) && memcmp(data, t1, 16));
    bits_unpack_msb(data, 128, bb);
    lfsr_init_dab(&l); lfsr_process(&l, bb, 128, bo, 0);
    bits_pack_msb(bo, 128, bp);
    CHECK(!memcmp(bp, t1, 16));

    /* multiplicativo G3RUH: descrambler(scrambler(x)) = x y autosincroniza */
    lfsr_t s, d;
    lfsr_init_g3ruh(&s);
    lfsr_init_g3ruh(&d);
    d.state = 0x1ABCD; /* estado inicial erróneo a propósito */
    uint8_t x[200], sc[200], dc[200];
    for (int i = 0; i < 200; i++)
        x[i] = (uint8_t)(((i * i) >> 3) & 1);
    lfsr_process(&s, x, 200, sc, 0);
    lfsr_process(&d, sc, 200, dc, 1);
    CHECK(!memcmp(x + 17, dc + 17, 183));
}

int main(void)
{
    test_crc();
    test_crc_misc();
    test_bits();
    test_lfsr();
    printf("test_crc: %d comprobaciones, %d fallos\n", g_n, g_fail);
    return g_fail != 0;
}
