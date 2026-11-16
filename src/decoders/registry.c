/*
 * registry.c — Tabla de decodificadores disponibles.
 *
 * Cada decodificador exporta un descriptor DEC_<ID> en src/decoders/<id>.c.
 * Para añadir uno nuevo basta crear su fichero y añadir una línea aquí (el
 * Makefile compila todo src/ automáticamente).
 */
#include "decoder.h"

#include <string.h>

extern const decoder_desc_t DEC_ADSB;
extern const decoder_desc_t DEC_RDS;
extern const decoder_desc_t DEC_DAB;
extern const decoder_desc_t DEC_AIS;
extern const decoder_desc_t DEC_DSC;
extern const decoder_desc_t DEC_APRS;
extern const decoder_desc_t DEC_POCSAG;
extern const decoder_desc_t DEC_FLEX;
extern const decoder_desc_t DEC_DMR;
extern const decoder_desc_t DEC_ACARS;
extern const decoder_desc_t DEC_VDL2;
extern const decoder_desc_t DEC_ISM;
extern const decoder_desc_t DEC_RS41;
extern const decoder_desc_t DEC_LORA;
extern const decoder_desc_t DEC_MESHTASTIC;
extern const decoder_desc_t DEC_LTE;
extern const decoder_desc_t DEC_NR;
extern const decoder_desc_t DEC_GSM;
extern const decoder_desc_t DEC_WIFI;
extern const decoder_desc_t DEC_DVBT;
extern const decoder_desc_t DEC_TDT;

const decoder_desc_t *const DECODERS[] = {
    &DEC_ADSB,
    &DEC_RDS,
    &DEC_DAB,
    &DEC_AIS,
    &DEC_DSC,
    &DEC_APRS,
    &DEC_POCSAG,
    &DEC_FLEX,
    &DEC_DMR,
    &DEC_ACARS,
    &DEC_VDL2,
    &DEC_ISM,
    &DEC_RS41,
    &DEC_LORA,
    &DEC_MESHTASTIC,
    &DEC_LTE,
    &DEC_NR,
    &DEC_GSM,
    &DEC_WIFI,
    &DEC_DVBT,
    &DEC_TDT,
    NULL,
};

const decoder_desc_t *decoder_find(const char *id)
{
    if (!id)
        return NULL;
    for (int i = 0; DECODERS[i]; i++)
        if (strcmp(DECODERS[i]->id, id) == 0)
            return DECODERS[i];
    return NULL;
}
