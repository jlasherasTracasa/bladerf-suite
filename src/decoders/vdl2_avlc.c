/*
 * vdl2_avlc.c — Análisis de tramas AVLC. Ver vdl2_avlc.h.
 */
#include "vdl2_avlc.h"

#include <stdio.h>
#include <string.h>

static int addr_get(const uint8_t *o, avlc_addr_t *a)
{
    /* El bit 0 de cada octeto es la extensión de dirección: no se exige un valor
     * concreto para no rechazar tramas por una convención no contrastada. */
    uint32_t v = (uint32_t)(o[0] >> 1) | (uint32_t)(o[1] >> 1) << 7 | (uint32_t)(o[2] >> 1) << 14 |
                 (uint32_t)(o[3] >> 1) << 21;
    a->type = (int)(v & 7);
    a->flag = (int)((v >> 3) & 1);
    a->icao = v >> 4;
    return 0;
}

void avlc_addr_put(uint8_t *o, uint32_t icao, int type, int flag)
{
    uint32_t v = (icao & 0xFFFFFF) << 4 | (uint32_t)(flag & 1) << 3 | (uint32_t)(type & 7);
    for (int i = 0; i < 4; i++)
        o[i] = (uint8_t)(((v >> (7 * i)) & 0x7F) << 1);
    o[3] |= 1; /* último octeto de la dirección */
}

const char *avlc_type_name(int t)
{
    switch (t) {
    case 1: return "aircraft";
    case 4: return "ground";
    case 6: return "ground-any";
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
