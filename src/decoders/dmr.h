/*
 * dmr.h — Constantes de DMR (ETSI TS 102 361-1 V2.5.1) comunes al decodificador
 * y al modulador. SOLO METADATOS: este proyecto no extrae ni decodifica voz.
 *
 * Ráfaga (burst) = 264 bits = 132 símbolos (27,5 ms) + 12 símbolos de CACH
 * delante = ranura de 30 ms (144 símbolos). Dos ranuras alternan (TDMA).
 *   datos : info(98) | tipo de ranura(10) | SYNC(48) | tipo de ranura(10) | info(98)
 *   voz   : voz(108) | EMB(8) + incrustado(32) + EMB(8)  o  SYNC(48) | voz(108)
 * 4FSK: dibit 01 → +3 (+1944 Hz), 00 → +1 (+648), 10 → −1, 11 → −3 (tabla 10.3).
 */
#ifndef BLADERF_LAB_DMR_H
#define BLADERF_LAB_DMR_H

#include <stdint.h>

#define DMR_BAUD 4800
#define DMR_DEV_HZ 1944.0 /* desviación del símbolo ±3 */

typedef enum {
    DMR_SYNC_BS_VOICE = 0,
    DMR_SYNC_BS_DATA,
    DMR_SYNC_MS_VOICE,
    DMR_SYNC_MS_DATA,
    DMR_SYNC_DM1_VOICE,
    DMR_SYNC_DM1_DATA,
    DMR_SYNC_DM2_VOICE,
    DMR_SYNC_DM2_DATA,
    DMR_SYNC_COUNT
} dmr_sync_kind_t;

/* Tabla 9.2 (patrones de sincronía de 48 bits). */
static const uint64_t DMR_SYNC[DMR_SYNC_COUNT] = {
    0x755FD7DF75F7ull, /* BS voz */
    0xDFF57D75DF5Dull, /* BS datos */
    0x7F7D5DD57DFDull, /* MS voz */
    0xD5D7F77FD757ull, /* MS datos */
    0x5D577F7757FFull, /* modo directo ranura 1 voz */
    0xF7FDD5DDFD55ull, /* modo directo ranura 1 datos */
    0x7DFFD5F55D5Full, /* modo directo ranura 2 voz */
    0xD7557F5FF7F5ull, /* modo directo ranura 2 datos */
};

static inline int dmr_sync_is_voice(int k) { return !(k & 1); }
static inline const char *dmr_sync_name(int k)
{
    static const char *n[DMR_SYNC_COUNT] = {"BS voz", "BS datos", "MS voz", "MS datos",
                                            "directo S1 voz", "directo S1 datos", "directo S2 voz", "directo S2 datos"};
    return n[k];
}

/* Tabla 9.22 (tipo de dato). */
enum {
    DMR_DT_PI_HEADER = 0,
    DMR_DT_VOICE_LC_HEADER = 1,
    DMR_DT_TERMINATOR_LC = 2,
    DMR_DT_CSBK = 3,
    DMR_DT_MBC_HEADER = 4,
    DMR_DT_MBC_CONT = 5,
    DMR_DT_DATA_HEADER = 6,
    DMR_DT_RATE_12 = 7,
    DMR_DT_RATE_34 = 8,
    DMR_DT_IDLE = 9,
    DMR_DT_RATE_1 = 10,
    DMR_DT_USBD = 11
};

#endif
