/*
 * vdl2_avlc.c — Análisis de tramas AVLC. Ver vdl2_avlc.h.
 */
#include "vdl2_avlc.h"

#include <stdio.h>
#include <string.h>

static uint32_t rev28(uint32_t v)
{
    uint32_t r = 0;
    for (int i = 0; i < 28; i++)
        r |= ((v >> i) & 1u) << (27 - i);
    return r;
}

/* Los 4 octetos llevan 7 bits útiles cada uno (el bit 0 es la extensión de dirección). Los 28 bits se
 * transmiten en este orden: 1 bit de estado, 3 bits de tipo (MSB primero) y la dirección ICAO de 24
 * bits (MSB primero). Como cada octeto sale LSB primero, tras juntar los 7 bits de cada octeto como
 * un número hay que invertir los 28 bits. Contrastado con 11 tramas reales (47806D → 10981A, etc.). */
static int addr_get(const uint8_t *o, avlc_addr_t *a)
{
    uint32_t v = (uint32_t)(o[0] >> 1) | (uint32_t)(o[1] >> 1) << 7 | (uint32_t)(o[2] >> 1) << 14 |
                 (uint32_t)(o[3] >> 1) << 21;
    uint32_t r = rev28(v);
    a->flag = (int)((r >> 27) & 1);
    a->type = (int)((r >> 24) & 7);
    a->icao = r & 0xFFFFFF;
    return 0;
}

void avlc_addr_put(uint8_t *o, uint32_t icao, int type, int flag)
{
    uint32_t r = (uint32_t)(flag & 1) << 27 | (uint32_t)(type & 7) << 24 | (icao & 0xFFFFFF);
    uint32_t v = rev28(r);
    for (int i = 0; i < 4; i++)
        o[i] = (uint8_t)(((v >> (7 * i)) & 0x7F) << 1); /* el bit 0 (extensión) lo pone quien llama */
}

const char *avlc_type_name(int t)
{
    switch (t) {
    case 1: return "aircraft";
    case 4:
    case 5: return "ground";
    case 7: return "broadcast";
    default: return "reserved";
    }
}

int avlc_parse(const uint8_t *f, int n, avlc_t *a)
{
    memset(a, 0, sizeof *a);
    if (n < 9)
        return -1;
    if (addr_get(f, &a->dst) < 0 || addr_get(f + 4, &a->src) < 0)
        return -1;
    a->ctrl = f[8];
    a->info = f + 9;
    a->info_len = n - 9;
    uint8_t c = a->ctrl;
    a->poll = (c >> 4) & 1;
    if (!(c & 1)) {
        snprintf(a->ftype, sizeof a->ftype, "I");
        a->ns = (c >> 1) & 7;
        a->nr = (c >> 5) & 7;
    } else if ((c & 3) == 1) {
        static const char *s[4] = {"RR", "RNR", "REJ", "SREJ"};
        snprintf(a->ftype, sizeof a->ftype, "%s", s[(c >> 2) & 3]);
        a->nr = (c >> 5) & 7;
    } else {
        switch (c & 0xEF) {
        case 0x03: snprintf(a->ftype, sizeof a->ftype, "UI"); break;
        case 0xAF: snprintf(a->ftype, sizeof a->ftype, "XID"); break;
        case 0xE3: snprintf(a->ftype, sizeof a->ftype, "TEST"); break;
        case 0x2F: snprintf(a->ftype, sizeof a->ftype, "SABM"); break;
        case 0x43: snprintf(a->ftype, sizeof a->ftype, "DISC"); break;
        case 0x63: snprintf(a->ftype, sizeof a->ftype, "UA"); break;
        case 0x0F: snprintf(a->ftype, sizeof a->ftype, "DM"); break;
        case 0x87: snprintf(a->ftype, sizeof a->ftype, "FRMR"); break;
        default: snprintf(a->ftype, sizeof a->ftype, "U?"); break;
        }
    }
    return 0;
}
