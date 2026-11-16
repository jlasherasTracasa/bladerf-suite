/*
 * mod_ism.c — Modulador ISM: sensores meteorológicos y trama de prueba propia.
 *
 * Construye el tren de pulsos del sensor pedido (ism_protocols.c) y lo emite
 * como OOK (portadora a amplitud 0,8 o apagada) a 250 kHz, centrado en 0 Hz.
 *
 * Política (AGENTS.md §6, air_ok = true): SOLO puede generar tramas de sensores
 * meteorológicos (Oregon Scientific, LaCrosse, Acurite, Nexus, Fine Offset,
 * Ambient) y la trama de prueba "BladeRFLab-test". Cualquier otro modelo
 * (TPMS, mandos, llaveros, puertas de garaje, alarmas…) se rechaza con error:
 * no existe codificador para ellos en el árbol.
 *
 * Parámetros JSON (todos opcionales; sin params se emite cada modelo una vez):
 *   {"model":"Oregon-THGR122N","id":167,"channel":2,"temp":21.4,"hum":56,
 *    "batt_low":false,"wind":3.4,"gust":6.8,"rain":4.5,"dir":90,"repeat":2}
 * Modelos: ver ISM_TX_MODELS. Para "BladeRFLab-test", id = contador.
 */
#include "mod.h"

#include "../decoders/ism_protocols.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 250000.0

static int default_repeat(const char *model)
{
    if (!strcmp(model, "Nexus-TH"))
        return 12;
    if (!strncmp(model, "Oregon", 6) || !strcmp(model, "FineOffset-WH1080") || !strcmp(model, "Ambient-F007TH"))
        return 2;
    return 3;
}

static int emit_one(const ism_msg_t *m, int repeat, iqbuf_t *out, char *err, size_t errlen)
{
    ism_train_t *t = malloc(sizeof *t);
    if (!t) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    ism_train_clear(t);
    int rc = ism_encode(m, repeat > 0 ? repeat : default_repeat(m->model), t);
    if (rc < 0) {
        snprintf(err, errlen, "modelo '%s' no permitido: solo sensores meteorológicos y 'BladeRFLab-test'",
                 m->model ? m->model : "?");
        free(t);
        return -1;
    }
    cf32 *buf = NULL;
    int n = 0, cap = 0;
    ism_render_silence(&buf, &n, &cap, 5000);
    ism_render(&buf, &n, &cap, t, false, RATE, 0.8, 0, NULL);
    ism_render_silence(&buf, &n, &cap, 12500);
    iqbuf_reserve(out, n);
    memcpy(out->s + out->n, buf, sizeof(cf32) * (size_t)n);
    out->n += n;
    free(buf);
    free(t);
    return 0;
}

static int ism_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    out->rate = RATE;
    ism_msg_t m;
    memset(&m, 0, sizeof m);
    int repeat = 0;
    if (!params || !params[0]) {
        /* ejemplo: un paquete de cada modelo permitido */
        for (int i = 0; ISM_TX_MODELS[i]; i++) {
            memset(&m, 0, sizeof m);
            m.model = ISM_TX_MODELS[i];
            m.id = 0x40u + (uint32_t)i;
            m.channel = 1 + i % 3;
            m.temp_c = 15.0 + i * 1.3;
            m.humidity = 40 + i * 3;
            m.wind_avg_ms = 3.4;
            m.wind_gust_ms = 5.1;
            m.rain_mm = 1.2;
            m.wind_dir_deg = 135;
            if (emit_one(&m, 0, out, err, errlen) < 0)
                return -1;
        }
        return 0;
    }
    jtok_t t[64];
    int nt = json_parse(params, strlen(params), t, 64);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "params no es un objeto JSON válido");
        return -1;
    }
    char model[40] = "Oregon-THGR122N";
    double v;
    bool b;
    int i;
    if ((i = json_obj_get(params, t, 0, "model")) >= 0)
        json_str(params, t, i, model, sizeof model);
    m.model = NULL;
    for (int k = 0; ISM_TX_MODELS[k]; k++)
        if (!strcmp(model, ISM_TX_MODELS[k]))
            m.model = ISM_TX_MODELS[k];
    if (!m.model) {
        snprintf(err, errlen, "modelo '%s' no permitido: solo sensores meteorológicos y 'BladeRFLab-test'", model);
        return -1;
    }
    m.id = 0xA7;
    m.channel = 1;
    m.temp_c = 21.4;
    m.humidity = 56;
    if ((i = json_obj_get(params, t, 0, "id")) >= 0 && json_num(params, t, i, &v))
        m.id = (uint32_t)v;
    if ((i = json_obj_get(params, t, 0, "channel")) >= 0 && json_num(params, t, i, &v))
        m.channel = (int)CLAMP(v, 1, 3);
    if ((i = json_obj_get(params, t, 0, "temp")) >= 0 && json_num(params, t, i, &v))
        m.temp_c = CLAMP(v, -40, 85);
    if ((i = json_obj_get(params, t, 0, "hum")) >= 0 && json_num(params, t, i, &v))
        m.humidity = CLAMP(v, 0, 99);
    if ((i = json_obj_get(params, t, 0, "batt_low")) >= 0 && json_bool(params, t, i, &b))
        m.batt_low = b;
    if ((i = json_obj_get(params, t, 0, "wind")) >= 0 && json_num(params, t, i, &v))
        m.wind_avg_ms = CLAMP(v, 0, 80);
    if ((i = json_obj_get(params, t, 0, "gust")) >= 0 && json_num(params, t, i, &v))
        m.wind_gust_ms = CLAMP(v, 0, 80);
    if ((i = json_obj_get(params, t, 0, "rain")) >= 0 && json_num(params, t, i, &v))
        m.rain_mm = CLAMP(v, 0, 19000);
    if ((i = json_obj_get(params, t, 0, "dir")) >= 0 && json_num(params, t, i, &v))
        m.wind_dir_deg = (int)CLAMP(v, 0, 359);
    if ((i = json_obj_get(params, t, 0, "repeat")) >= 0 && json_num(params, t, i, &v))
        repeat = (int)CLAMP(v, 1, 20);
    return emit_one(&m, repeat, out, err, errlen);
}

const modulator_desc_t MOD_ISM = {
    .id = "ism",
    .name = "ISM: sensores meteo + prueba",
    .air_ok = true,
    .modulate = ism_modulate,
};
