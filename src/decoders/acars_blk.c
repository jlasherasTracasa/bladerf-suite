/*
 * acars_blk.c — Análisis y construcción de bloques ACARS. Ver acars_blk.h.
 */
#include "acars_blk.h"
#include "../dsp/bits.h"
#include "../dsp/crc.h"

#include <ctype.h>
#include <string.h>

uint8_t acars_add_parity(uint8_t c)
{
    c &= 0x7F;
    /* paridad impar: nº total de unos (incluido el bit 7) impar */
    if (!bits_parity32(c))
        c |= 0x80;
    return c;
}

uint16_t acars_bcs(const uint8_t *c, int n)
{
    static crc_t crc;
    static bool ready = false;
    if (!ready) {
        crc_init(&crc, crc_preset("CRC-16/KERMIT"));
        ready = true;
    }
    return (uint16_t)crc_compute(&crc, c, (size_t)n);
}

int acars_blk_parse(const uint8_t *c, int n, acars_blk_t *b)
{
    memset(b, 0, sizeof *b);
    if (n < 13 || (c[n - 1] != ACARS_ETX && c[n - 1] != ACARS_ETB))
        return -1;
    if (n > 13 && c[12] != ACARS_STX)
        return -1;
    if (n == 13 && c[12] != c[n - 1])
        return -1;
    b->mode = (char)c[0];
    int k = 0;
    while (k < 7 && c[1 + k] == '.')
        k++; /* relleno por la izquierda */
    memcpy(b->reg, c + 1 + k, (size_t)(7 - k));
    b->tak = (char)c[8];
    b->label[0] = (char)c[9];
    b->label[1] = c[10] == ACARS_DEL ? 'd' : (char)c[10];
    b->bid = (char)c[11];
    b->etb = c[n - 1] == ACARS_ETB;
    b->downlink = isdigit(c[11]) != 0;
    const uint8_t *t = c + 13;
    int tl = n > 13 ? n - 14 : 0;
    if (b->downlink && tl >= 10 && isupper(t[0]) && isdigit(t[1]) && isdigit(t[2]) && isupper(t[3])) {
        memcpy(b->msgno, t, 4);
        memcpy(b->flight, t + 4, 6);
        for (int i = 5; i >= 0 && b->flight[i] == ' '; i--)
            b->flight[i] = 0;
        t += 10;
        tl -= 10;
    }
    if (tl > (int)sizeof b->text - 1)
        tl = (int)sizeof b->text - 1;
    memcpy(b->text, t, (size_t)tl);
    b->text[tl] = 0;
    b->text_len = tl;
    return 0;
}

int acars_blk_build(const acars_blk_t *b, uint8_t *out, int cap)
{
    int n = 0;
    uint8_t tmp[300];
    if (cap < 300)
        return -1;
    tmp[n++] = (uint8_t)b->mode;
    int rl = (int)strlen(b->reg);
    if (rl > 7)
        rl = 7;
    for (int i = 0; i < 7 - rl; i++)
        tmp[n++] = '.';
    memcpy(tmp + n, b->reg, (size_t)rl);
    n += rl;
    tmp[n++] = (uint8_t)b->tak;
    tmp[n++] = (uint8_t)b->label[0];
    tmp[n++] = b->label[1] == 'd' ? ACARS_DEL : (uint8_t)b->label[1];
    tmp[n++] = (uint8_t)b->bid;
    bool dl = isdigit((unsigned char)b->bid) != 0;
    if (b->text_len > 0 || (dl && b->msgno[0])) {
        tmp[n++] = ACARS_STX;
        if (dl && b->msgno[0]) {
            memcpy(tmp + n, b->msgno, 4);
            n += 4;
            char fl[7];
            memset(fl, ' ', 6);
            fl[6] = 0;
            memcpy(fl, b->flight, MIN(strlen(b->flight), 6));
            memcpy(tmp + n, fl, 6);
            n += 6;
        }
        int tl = MIN(b->text_len, 220);
        memcpy(tmp + n, b->text, (size_t)tl);
        n += tl;
    }
    tmp[n++] = b->etb ? ACARS_ETB : ACARS_ETX;
    memcpy(out, tmp, (size_t)n);
    return n;
}
