/*
 * profiles.h — Perfiles: configuraciones listas para un servicio de radio.
 *
 * Un perfil fija el centro y la tasa del hardware, opcionalmente el canal de
 * audio del cliente que lo pide y un conjunto de decodificadores. Los centros
 * se desplazan a propósito de las frecuencias de interés para que ningún
 * canal caiga sobre el pico de continua (DC) del receptor.
 *
 * La orden {"cmd":"profile","id":"ais"} (main.c) llama a profile_apply():
 * retira las instancias creadas por el perfil anterior (owner = 1), cambia el
 * hardware y crea las instancias del nuevo. Las que el usuario añadió a mano
 * (owner = 0) no se tocan.
 */
#ifndef BLADERF_LAB_PROFILES_H
#define BLADERF_LAB_PROFILES_H

#include "decoders/host.h"
#include "engine.h"

#define PROFILE_MAX_DEC 8

typedef struct {
    const char *proto;
    int64_t freq;      /* 0 = en el centro (decodificadores de banda ancha) */
    bool follow;       /* la interfaz lo mueve cuando cambia la frecuencia de audio */
    bool autodec;      /* el host lo crea sobre las señales detectadas (opcional) */
} prof_dec_t;

typedef struct {
    const char *id, *name, *group;
    int64_t center;
    uint32_t rate;
    const char *mode;  /* "wfm", "nfm", "am"… o NULL si no hay audio */
    int64_t chan_freq;
    int bw;            /* anchura del canal de audio (0 = la habitual del modo) */
    prof_dec_t dec[PROFILE_MAX_DEC];
    int ndec;
    bool spectrum_only;
} profile_t;

extern const profile_t PROFILES[];
extern const int PROFILE_COUNT;

const profile_t *profile_find(const char *id);
/* Mensaje "profiles". */
void profiles_json(sb_t *sb);
/* Aplica el perfil. client: cliente WebSocket cuyo canal de audio se ajusta
 * (−1 = ninguno). Devuelve 0 o <0 con err (los fallos de instancias sueltas
 * solo se registran como aviso). */
int profile_apply(const profile_t *p, engine_t *e, dec_host_t *h, int client, char *err, size_t errlen);

#endif
