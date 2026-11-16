/*
 * modreg.c — Tabla de moduladores disponibles (uno por decodificador).
 */
#include "mod.h"

#include <string.h>

extern const modulator_desc_t MOD_ADSB;
extern const modulator_desc_t MOD_RDS;
extern const modulator_desc_t MOD_DAB;
extern const modulator_desc_t MOD_AIS;
extern const modulator_desc_t MOD_DSC;
extern const modulator_desc_t MOD_APRS;
extern const modulator_desc_t MOD_POCSAG;
extern const modulator_desc_t MOD_FLEX;
extern const modulator_desc_t MOD_DMR;
extern const modulator_desc_t MOD_ACARS;
extern const modulator_desc_t MOD_VDL2;
extern const modulator_desc_t MOD_ISM;
extern const modulator_desc_t MOD_RS41;
extern const modulator_desc_t MOD_LORA;
extern const modulator_desc_t MOD_MESHTASTIC;
extern const modulator_desc_t MOD_LTE;
extern const modulator_desc_t MOD_NR;
extern const modulator_desc_t MOD_GSM;
extern const modulator_desc_t MOD_WIFI;
extern const modulator_desc_t MOD_DVBT;

const modulator_desc_t *const MODULATORS[] = {
    &MOD_ADSB,
    &MOD_RDS,
    &MOD_DAB,
    &MOD_AIS,
    &MOD_DSC,
    &MOD_APRS,
    &MOD_POCSAG,
    &MOD_FLEX,
    &MOD_DMR,
    &MOD_ACARS,
    &MOD_VDL2,
    &MOD_ISM,
    &MOD_RS41,
    &MOD_LORA,
    &MOD_MESHTASTIC,
    &MOD_LTE,
    &MOD_NR,
    &MOD_GSM,
    &MOD_WIFI,
    &MOD_DVBT,
    NULL,
};

const modulator_desc_t *modulator_find(const char *id)
{
    if (!id)
        return NULL;
    for (int i = 0; MODULATORS[i]; i++)
        if (strcmp(MODULATORS[i]->id, id) == 0)
            return MODULATORS[i];
    return NULL;
}
