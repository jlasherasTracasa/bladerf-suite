/*
 * crc.c — CRC genérico por tabla. Ver crc.h.
 *
 * Dominio normal (refin=0), anchura w ≥ 8: reg = (reg << 8) ^ T[(reg >> (w-8)) ^ b].
 * Dominio reflejado (refin=1), cualquier w: reg = (reg >> 8) ^ T[(reg ^ b) & 255]
 * con T construida con el polinomio reflejado. Para w < 8 sin reflejo se usa
 * el camino bit a bit (poco frecuente y de mensajes cortos).
 */
#include "crc.h"
#include "bits.h"

#include <string.h>
#include <strings.h>

static uint32_t reflect(uint32_t v, int w) { return bits_reverse(v, w); }

int crc_init(crc_t *c, const crc_params_t *p)
{
    if (p->width < 1 || p->width > 32)
        return -1;
    c->p = *p;
    c->mask = p->width == 32 ? 0xFFFFFFFFu : ((1u << p->width) - 1);
    c->rpoly = reflect(p->poly & c->mask, p->width);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r;
        if (p->refin) {
            r = i;
            for (int b = 0; b < 8; b++)
                r = (r & 1) ? (r >> 1) ^ c->rpoly : r >> 1;
        } else if (p->width >= 8) {
            r = i << (p->width - 8);
            for (int b = 0; b < 8; b++)
                r = ((r & (1u << (p->width - 1))) ? (r << 1) ^ p->poly : r << 1) & c->mask;
        } else {
            r = 0;
        }
        c->table[i] = r & c->mask;
    }
    return 0;
}

uint32_t crc_start(const crc_t *c)
{
    return c->p.refin ? reflect(c->p.init & c->mask, c->p.width) : (c->p.init & c->mask);
}

uint32_t crc_update_bit(const crc_t *c, uint32_t reg, int bit)
{
    bit &= 1;
    if (c->p.refin) {
        uint32_t fb = (reg ^ (uint32_t)bit) & 1;
        reg >>= 1;
        return fb ? reg ^ c->rpoly : reg;
    }
    uint32_t fb = ((reg >> (c->p.width - 1)) & 1) ^ (uint32_t)bit;
    reg = (reg << 1) & c->mask;
    return fb ? reg ^ c->p.poly : reg;
}

uint32_t crc_update(const crc_t *c, uint32_t reg, const uint8_t *data, size_t n)
{
    if (c->p.refin) {
        for (size_t i = 0; i < n; i++)
            reg = (reg >> 8) ^ c->table[(reg ^ data[i]) & 0xFF];
    } else if (c->p.width >= 8) {
        int sh = c->p.width - 8;
        for (size_t i = 0; i < n; i++)
            reg = ((reg << 8) ^ c->table[((reg >> sh) ^ data[i]) & 0xFF]) & c->mask;
    } else {
        for (size_t i = 0; i < n; i++)
            for (int b = 7; b >= 0; b--)
                reg = crc_update_bit(c, reg, (data[i] >> b) & 1);
    }
    return reg;
}

uint32_t crc_finish(const crc_t *c, uint32_t reg)
{
    if (c->p.refin != c->p.refout)
        reg = reflect(reg, c->p.width);
    return (reg ^ c->p.xorout) & c->mask;
}

uint32_t crc_compute(const crc_t *c, const uint8_t *data, size_t n)
{
    return crc_finish(c, crc_update(c, crc_start(c), data, n));
}

uint32_t crc_compute_bits(const crc_t *c, const uint8_t *bits, size_t nbits)
{
    uint32_t reg = crc_start(c);
    for (size_t i = 0; i < nbits; i++)
        reg = crc_update_bit(c, reg, bits[i]);
    return crc_finish(c, reg);
}

/* ======================================================================= */
/*  Catálogo. Valores 'check' publicados en reveng.sourceforge.io/crc-catalogue */
/* ======================================================================= */
#define T true
#define F false
static const crc_params_t PRESETS[] = {
    /* name, alias, width, poly, init, refin, refout, xorout, check */
    {"CRC-3/GSM", NULL, 3, 0x3, 0x0, F, F, 0x7, 0x4},
    {"CRC-4/G-704", "CRC-4/ITU", 4, 0x3, 0x0, T, T, 0x0, 0x7},
    {"CRC-5/EPC-C1G2", "CRC-5/EPC", 5, 0x09, 0x09, F, F, 0x00, 0x00},
    {"CRC-5/USB", NULL, 5, 0x05, 0x1F, T, T, 0x1F, 0x19},
    {"CRC-6/GSM", NULL, 6, 0x2F, 0x00, F, F, 0x3F, 0x13},
    {"CRC-7/MMC", NULL, 7, 0x09, 0x00, F, F, 0x00, 0x75},
    {"CRC-8/SMBUS", "CRC-8", 8, 0x07, 0x00, F, F, 0x00, 0xF4},
    {"CRC-8/CDMA2000", NULL, 8, 0x9B, 0xFF, F, F, 0x00, 0xDA},
    {"CRC-8/DARC", NULL, 8, 0x39, 0x00, T, T, 0x00, 0x15},
    {"CRC-8/GSM-B", NULL, 8, 0x49, 0x00, F, F, 0xFF, 0x94},
    {"CRC-8/I-432-1", "CRC-8/ITU", 8, 0x07, 0x00, F, F, 0x55, 0xA1},
    {"CRC-8/MAXIM-DOW", "CRC-8/MAXIM", 8, 0x31, 0x00, T, T, 0x00, 0xA1},
    {"CRC-8/ROHC", NULL, 8, 0x07, 0xFF, T, T, 0x00, 0xD0},
    {"CRC-8/AUTOSAR", NULL, 8, 0x2F, 0xFF, F, F, 0xFF, 0xDF},
    {"CRC-8/WCDMA", NULL, 8, 0x9B, 0x00, T, T, 0x00, 0x25},
    {"CRC-8/BLUETOOTH", NULL, 8, 0xA7, 0x00, T, T, 0x00, 0x26},
    {"CRC-8/I-CODE", NULL, 8, 0x1D, 0xFD, F, F, 0x00, 0x7E},
    {"CRC-12/DECT", NULL, 12, 0x80F, 0x000, F, F, 0x000, 0xF5B},
    {"CRC-15/CAN", NULL, 15, 0x4599, 0x0000, F, F, 0x0000, 0x059E},
    {"CRC-16/IBM-SDLC", "CRC-16/X-25", 16, 0x1021, 0xFFFF, T, T, 0xFFFF, 0x906E},
    {"CRC-16/IBM-3740", "CRC-16/CCITT-FALSE", 16, 0x1021, 0xFFFF, F, F, 0x0000, 0x29B1},
    {"CRC-16/KERMIT", "CRC-16/CCITT", 16, 0x1021, 0x0000, T, T, 0x0000, 0x2189},
    {"CRC-16/XMODEM", "CRC-16/ACORN", 16, 0x1021, 0x0000, F, F, 0x0000, 0x31C3},
    {"CRC-16/GSM", NULL, 16, 0x1021, 0x0000, F, F, 0xFFFF, 0xCE3C},
    {"CRC-16/SPI-FUJITSU", "CRC-16/AUG-CCITT", 16, 0x1021, 0x1D0F, F, F, 0x0000, 0xE5CC},
    {"CRC-16/GENIBUS", "CRC-16/EPC", 16, 0x1021, 0xFFFF, F, F, 0xFFFF, 0xD64E},
    {"CRC-16/MCRF4XX", NULL, 16, 0x1021, 0xFFFF, T, T, 0x0000, 0x6F91},
    {"CRC-16/ISO-IEC-14443-3-A", "CRC-16/A", 16, 0x1021, 0xC6C6, T, T, 0x0000, 0xBF05},
    {"CRC-16/ARC", "CRC-16/IBM", 16, 0x8005, 0x0000, T, T, 0x0000, 0xBB3D},
    {"CRC-16/MODBUS", NULL, 16, 0x8005, 0xFFFF, T, T, 0x0000, 0x4B37},
    {"CRC-16/USB", NULL, 16, 0x8005, 0xFFFF, T, T, 0xFFFF, 0xB4C8},
    {"CRC-16/MAXIM-DOW", "CRC-16/MAXIM", 16, 0x8005, 0x0000, T, T, 0xFFFF, 0x44C2},
    {"CRC-16/BUYPASS", "CRC-16/UMTS", 16, 0x8005, 0x0000, F, F, 0x0000, 0xFEE8},
    {"CRC-16/DNP", NULL, 16, 0x3D65, 0x0000, T, T, 0xFFFF, 0xEA82},
    {"CRC-16/EN-13757", NULL, 16, 0x3D65, 0x0000, F, F, 0xFFFF, 0xC2B7},
    {"CRC-16/DECT-R", NULL, 16, 0x0589, 0x0000, F, F, 0x0001, 0x007E},
    {"CRC-16/DECT-X", NULL, 16, 0x0589, 0x0000, F, F, 0x0000, 0x007F},
    {"CRC-16/CDMA2000", NULL, 16, 0xC867, 0xFFFF, F, F, 0x0000, 0x4C06},
    {"CRC-16/T10-DIF", NULL, 16, 0x8BB7, 0x0000, F, F, 0x0000, 0xD0DB},
    {"CRC-16/TELEDISK", NULL, 16, 0xA097, 0x0000, F, F, 0x0000, 0x0FB3},
    {"CRC-16/DDS-110", NULL, 16, 0x8005, 0x800D, F, F, 0x0000, 0x9ECF},
    {"CRC-24/OPENPGP", NULL, 24, 0x864CFB, 0xB704CE, F, F, 0x000000, 0x21CF02},
    {"CRC-24/LTE-A", "CRC-24A", 24, 0x864CFB, 0x000000, F, F, 0x000000, 0xCDE703},
    {"CRC-24/LTE-B", "CRC-24B", 24, 0x800063, 0x000000, F, F, 0x000000, 0x23EF52},
    {"CRC-24/BLE", NULL, 24, 0x00065B, 0x555555, T, T, 0x000000, 0xC25A56},
    {"CRC-24/FLEXRAY-A", NULL, 24, 0x5D6DCB, 0xFEDCBA, F, F, 0x000000, 0x7979BD},
    {"CRC-24/FLEXRAY-B", NULL, 24, 0x5D6DCB, 0xABCDEF, F, F, 0x000000, 0x1F23B8},
    /* Mode S / ADS-B (ICAO Anexo 10 vol. IV): G(x) = x^24 + 0xFFF409, init 0.
     * 'check' (no está en reveng) calculado con la referencia bit a bit del test; validado con el mensaje DF17 de ejemplo. */
    {"CRC-24/MODES", "CRC-24/ADSB", 24, 0xFFF409, 0x000000, F, F, 0x000000, 0x054268},
    {"CRC-32/ISO-HDLC", "CRC-32", 32, 0x04C11DB7, 0xFFFFFFFF, T, T, 0xFFFFFFFF, 0xCBF43926},
    {"CRC-32/ISCSI", "CRC-32C", 32, 0x1EDC6F41, 0xFFFFFFFF, T, T, 0xFFFFFFFF, 0xE3069283},
    {"CRC-32/BZIP2", "CRC-32/AAL5", 32, 0x04C11DB7, 0xFFFFFFFF, F, F, 0xFFFFFFFF, 0xFC891918},
    {"CRC-32/MPEG-2", NULL, 32, 0x04C11DB7, 0xFFFFFFFF, F, F, 0x00000000, 0x0376E6E7},
    {"CRC-32/CKSUM", "CRC-32/POSIX", 32, 0x04C11DB7, 0x00000000, F, F, 0xFFFFFFFF, 0x765E7680},
    {"CRC-32/JAMCRC", NULL, 32, 0x04C11DB7, 0xFFFFFFFF, T, T, 0x00000000, 0x340BC6D9},
    {"CRC-32/AIXM", "CRC-32Q", 32, 0x814141AB, 0x00000000, F, F, 0x00000000, 0x3010BF7F},
};
#undef T
#undef F

const crc_params_t *crc_preset_at(int index)
{
    return (index >= 0 && index < (int)ARRAY_LEN(PRESETS)) ? &PRESETS[index] : NULL;
}

const crc_params_t *crc_preset(const char *name)
{
    static const struct { const char *a, *b; } extra[] = {
        {"HDLC", "CRC-16/IBM-SDLC"}, {"AX.25", "CRC-16/IBM-SDLC"}, {"CRC-16/HDLC", "CRC-16/IBM-SDLC"},
        {"CRC-24/MODE-S", "CRC-24/MODES"}, {"CRC-16", "CRC-16/ARC"},
    };
    for (size_t i = 0; i < ARRAY_LEN(PRESETS); i++)
        if (!strcasecmp(name, PRESETS[i].name) || (PRESETS[i].alias && !strcasecmp(name, PRESETS[i].alias)))
            return &PRESETS[i];
    for (size_t i = 0; i < ARRAY_LEN(extra); i++)
        if (!strcasecmp(name, extra[i].a))
            return crc_preset(extra[i].b);
    return NULL;
}

uint32_t crc24_modes(const uint8_t *msg, int nbytes)
{
    crc_t c;
    crc_init(&c, crc_preset("CRC-24/MODES"));
    return crc_compute(&c, msg, (size_t)nbytes);
}
