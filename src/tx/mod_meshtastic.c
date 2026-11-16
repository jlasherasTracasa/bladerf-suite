/*
 * mod_meshtastic.c — Modulador Meshtastic: LoRa (preset EU_868 LongFast por
 * defecto: SF11, BW 250 kHz, CR 4/5, sync 0x2B, preámbulo 16) con paquetes
 * cifrados con la clave pública por defecto (AES-128-CTR, ver
 * decoders/meshtastic.h para el formato de cabecera y nonce).
 *
 * Genera una secuencia de paquetes (por defecto nodeinfo, posición, texto y
 * telemetría) con el nodo de origen fijado por params. Los protobuf se
 * codifican a mano: varint (clave = campo<<3 | tipo de cable), fixed32 y
 * campos con prefijo de longitud.
 *
 * Parámetros JSON (todos opcionales):
 *   {"src":"!a1b2c3d4"|número, "dst":"ffffffff", "id":número, "hops":3,
 *    "kinds":["nodeinfo","position","text","telemetry"], "text":"Hola mesh",
 *    "lat":40.4168,"lon":-3.7038,"alt":667,"long_name":"BladeRF Lab",
 *    "short_name":"BLDR","hw_model":255,"battery":87,"voltage":4.01,
 *    "channel":"LongFast","preset":"LONG_FAST","gap_ms":150}
 *
 * air_ok = true: LoRa en SRD 868 (869,4–869,65 MHz para LongFast EU) es legal
 * con las restricciones de potencia y ciclo de trabajo de tx.c.
 */
#include "mod.h"

#include "../decoders/lora_phy.h"
#include "../decoders/meshtastic.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 500000.0

typedef struct {
    uint8_t b[256];
    int n;
} buf_t;

static void put(buf_t *o, uint8_t v)
{
    if (o->n < (int)sizeof o->b)
        o->b[o->n++] = v;
}

static void put_varint(buf_t *o, uint64_t v)
{
    while (v >= 0x80) {
        put(o, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    put(o, (uint8_t)v);
}

static void put_key(buf_t *o, int field, int wt) { put_varint(o, (uint64_t)(field << 3 | wt)); }

static void put_u(buf_t *o, int field, uint64_t v)
{
    put_key(o, field, 0);
    put_varint(o, v);
}

static void put_f32(buf_t *o, int field, uint32_t v)
{
    put_key(o, field, 5);
    for (int i = 0; i < 4; i++)
        put(o, (uint8_t)(v >> (8 * i)));
}

static void put_float(buf_t *o, int field, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    put_f32(o, field, u);
}

static void put_bytes(buf_t *o, int field, const uint8_t *p, int n)
{
    put_key(o, field, 2);
    put_varint(o, (uint64_t)n);
    for (int i = 0; i < n; i++)
        put(o, p[i]);
}

typedef struct {
    uint32_t src, dst, id;
    int hops, battery, hw_model, alt;
    double lat, lon, voltage;
    char text[200], long_name[40], short_name[8], channel[24];
    int gap_ms;
    char kinds[4][12];
    int nkinds;
} params_t;

/* Mensaje interno según el tipo. Devuelve el portnum. */
static int build_inner(const params_t *p, const char *kind, buf_t *in)
{
    in->n = 0;
    if (!strcmp(kind, "text")) {
        memcpy(in->b, p->text, strlen(p->text));
        in->n = (int)strlen(p->text);
        return MESH_PORT_TEXT;
    }
    if (!strcmp(kind, "position")) {
        put_f32(in, 1, (uint32_t)(int32_t)lrint(p->lat * 1e7));
        put_f32(in, 2, (uint32_t)(int32_t)lrint(p->lon * 1e7));
        put_u(in, 3, (uint64_t)(int64_t)p->alt);
        put_f32(in, 4, 1766000000u);
        put_u(in, 19, 9);
        return MESH_PORT_POSITION;
    }
    if (!strcmp(kind, "nodeinfo")) {
        char id[16];
        snprintf(id, sizeof id, "!%08x", p->src);
        put_bytes(in, 1, (const uint8_t *)id, (int)strlen(id));
        put_bytes(in, 2, (const uint8_t *)p->long_name, (int)strlen(p->long_name));
        put_bytes(in, 3, (const uint8_t *)p->short_name, (int)strlen(p->short_name));
        put_u(in, 5, (uint64_t)p->hw_model);
        return MESH_PORT_NODEINFO;
    }
    /* telemetry: Telemetry{time=1, device_metrics=2{battery 1, voltage 2, chan_util 3, air_util 4, uptime 5}} */
    buf_t dm = {{0}, 0};
    put_u(&dm, 1, (uint64_t)p->battery);
    put_float(&dm, 2, (float)p->voltage);
    put_float(&dm, 3, 3.5f);
    put_float(&dm, 4, 0.8f);
    put_u(&dm, 5, 86400);
    put_f32(in, 1, 1766000000u);
    put_bytes(in, 2, dm.b, dm.n);
    return MESH_PORT_TELEMETRY;
}

static uint32_t parse_node(const char *s)
{
    if (s[0] == '!')
        s++;
    return (uint32_t)strtoul(s, NULL, 16);
}

static int parse_params(const char *js, params_t *p, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    p->src = 0xA1B2C3D4;
    p->dst = 0xFFFFFFFFu;
    p->id = 0x1A2B3C00;
    p->hops = 3;
    p->battery = 87;
    p->voltage = 4.01;
    p->hw_model = 255;
    p->alt = 667;
    p->lat = 40.4168;
    p->lon = -3.7038;
    p->gap_ms = 150;
    snprintf(p->text, sizeof p->text, "Hola mesh desde BladeRF Lab");
    snprintf(p->long_name, sizeof p->long_name, "BladeRF Lab");
    snprintf(p->short_name, sizeof p->short_name, "BLDR");
    snprintf(p->channel, sizeof p->channel, "LongFast");
    const char *def[4] = {"nodeinfo", "position", "text", "telemetry"};
    for (int i = 0; i < 4; i++)
        snprintf(p->kinds[i], sizeof p->kinds[i], "%s", def[i]);
    p->nkinds = 4;
    if (!js || !js[0])
        return 0;
    jtok_t t[48];
    int nt = json_parse(js, strlen(js), t, 48);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "params no es un objeto JSON válido");
        return -1;
    }
    int i;
    double v;
    char s[64];
    if ((i = json_obj_get(js, t, 0, "src")) >= 0) {
        if (json_str(js, t, i, s, sizeof s))
            p->src = parse_node(s);
        else if (json_num(js, t, i, &v))
            p->src = (uint32_t)v;
    }
    if ((i = json_obj_get(js, t, 0, "dst")) >= 0) {
        if (json_str(js, t, i, s, sizeof s))
            p->dst = parse_node(s);
        else if (json_num(js, t, i, &v))
            p->dst = (uint32_t)v;
    }
    if ((i = json_obj_get(js, t, 0, "id")) >= 0 && json_num(js, t, i, &v))
        p->id = (uint32_t)v;
    if ((i = json_obj_get(js, t, 0, "hops")) >= 0 && json_num(js, t, i, &v))
        p->hops = (int)CLAMP(v, 0, 7);
    if ((i = json_obj_get(js, t, 0, "battery")) >= 0 && json_num(js, t, i, &v))
        p->battery = (int)CLAMP(v, 0, 101);
    if ((i = json_obj_get(js, t, 0, "voltage")) >= 0 && json_num(js, t, i, &v))
        p->voltage = v;
    if ((i = json_obj_get(js, t, 0, "hw_model")) >= 0 && json_num(js, t, i, &v))
        p->hw_model = (int)v;
    if ((i = json_obj_get(js, t, 0, "alt")) >= 0 && json_num(js, t, i, &v))
        p->alt = (int)v;
    if ((i = json_obj_get(js, t, 0, "lat")) >= 0 && json_num(js, t, i, &v))
        p->lat = CLAMP(v, -90, 90);
    if ((i = json_obj_get(js, t, 0, "lon")) >= 0 && json_num(js, t, i, &v))
        p->lon = CLAMP(v, -180, 180);
    if ((i = json_obj_get(js, t, 0, "gap_ms")) >= 0 && json_num(js, t, i, &v))
        p->gap_ms = (int)CLAMP(v, 1, 5000);
    if ((i = json_obj_get(js, t, 0, "text")) >= 0 && json_str(js, t, i, p->text, sizeof p->text)) {
        /* un texto explícito sin "kinds" → solo ese mensaje */
        if (json_obj_get(js, t, 0, "kinds") < 0) {
            snprintf(p->kinds[0], sizeof p->kinds[0], "text");
            p->nkinds = 1;
        }
    }
    if ((i = json_obj_get(js, t, 0, "long_name")) >= 0)
        json_str(js, t, i, p->long_name, sizeof p->long_name);
    if ((i = json_obj_get(js, t, 0, "short_name")) >= 0)
        json_str(js, t, i, p->short_name, sizeof p->short_name);
    if ((i = json_obj_get(js, t, 0, "channel")) >= 0)
        json_str(js, t, i, p->channel, sizeof p->channel);
    if ((i = json_obj_get(js, t, 0, "kinds")) >= 0 && t[i].type == J_ARR) {
        p->nkinds = 0;
        for (int k = 0; k < t[i].size && p->nkinds < 4; k++) {
            int e = json_arr_get(t, i, k);
            if (e >= 0 && json_str(js, t, e, s, sizeof s)) {
                if (strcmp(s, "text") && strcmp(s, "position") && strcmp(s, "nodeinfo") && strcmp(s, "telemetry")) {
                    snprintf(err, errlen, "kind desconocido: %s (text, position, nodeinfo, telemetry)", s);
                    return -1;
                }
                snprintf(p->kinds[p->nkinds++], sizeof p->kinds[0], "%.11s", s);
            }
        }
        if (!p->nkinds) {
            snprintf(err, errlen, "kinds vacío");
            return -1;
        }
    }
    return 0;
}

static int mesh_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse_params(params, &p, err, errlen) < 0)
        return -1;
    const mesh_preset_t *pr = mesh_preset_find("LONG_FAST");
    if (params && params[0]) {
        jtok_t t[48];
        char s[40];
        int nt = json_parse(params, strlen(params), t, 48), i;
        if (nt > 0 && (i = json_obj_get(params, t, 0, "preset")) >= 0 && json_str(params, t, i, s, sizeof s)) {
            pr = mesh_preset_find(s);
            if (!pr) {
                snprintf(err, errlen, "preset desconocido");
                return -1;
            }
        }
    }
    uint8_t hash = mesh_channel_hash(p.channel, MESH_DEFAULT_KEY, 16);
    out->rate = FS;
    lora_push_silence(out, (int)(0.05 * FS));
    for (int k = 0; k < p.nkinds; k++) {
        buf_t inner, data = {{0}, 0};
        int port = build_inner(&p, p.kinds[k], &inner);
        put_u(&data, 1, (uint64_t)port);
        put_bytes(&data, 2, inner.b, inner.n);
        if (data.n > 200) {
            snprintf(err, errlen, "mensaje demasiado largo");
            return -1;
        }
        uint32_t id = p.id + (uint32_t)k;
        uint8_t pkt[MESH_HDR_LEN + 256];
        for (int i = 0; i < 4; i++) {
            pkt[i] = (uint8_t)(p.dst >> (8 * i));
            pkt[4 + i] = (uint8_t)(p.src >> (8 * i));
            pkt[8 + i] = (uint8_t)(id >> (8 * i));
        }
        pkt[12] = (uint8_t)((p.hops & 7) | ((p.hops & 7) << 5)); /* hop_limit = hop_start */
        pkt[13] = hash;
        pkt[14] = 0;
        pkt[15] = (uint8_t)(p.src & 0xFF); /* relay_node: último byte del origen */
        memcpy(pkt + MESH_HDR_LEN, data.b, (size_t)data.n);
        mesh_crypt(MESH_DEFAULT_KEY, p.src, id, pkt + MESH_HDR_LEN, data.n);
        uint16_t syms[1100];
        int ns = lora_encode_symbols(pkt, MESH_HDR_LEN + data.n, pr->sf, pr->cr, true, lora_ldro_auto(pr->sf, pr->bw_hz),
                                     syms, 1100);
        if (ns < 0) {
            snprintf(err, errlen, "no se pudo codificar la trama");
            return -1;
        }
        lora_modulate_frame(out, FS, pr->bw_hz, pr->sf, 0x2B, 16, syms, ns, 0.8f);
        lora_push_silence(out, (int)(p.gap_ms * 1e-3 * FS));
    }
    return 0;
}

const modulator_desc_t MOD_MESHTASTIC = {
    .id = "meshtastic",
    .name = "Meshtastic",
    .air_ok = true,
    .modulate = mesh_modulate,
};
