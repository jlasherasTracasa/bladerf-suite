/*
 * meshtastic.c — Decodificador Meshtastic (malla LoRa) con la clave pública por
 * defecto del canal primario.
 *
 * Cadena: LoRa PHY (lora_phy) con el preset de módem (EU_868 LongFast por
 * defecto: SF11, BW 250 kHz, CR 4/5, sync 0x2B, preámbulo 16) → cabecera de 16
 * bytes en claro (meshtastic.h) → AES-128-CTR con la clave por defecto
 * (aes.c) → protobuf "Data" (meshtastic/mesh.proto):
 *     Data { PortNum portnum = 1; bytes payload = 2; bool want_response = 3;
 *            fixed32 dest = 4; fixed32 source = 5; fixed32 request_id = 6;
 *            fixed32 reply_id = 7; fixed32 emoji = 8; uint32 bitfield = 9; }
 * y, según portnum, el mensaje interno:
 *     1  TEXT_MESSAGE_APP: payload = texto UTF-8.
 *     3  POSITION_APP:     Position { sfixed32 latitude_i = 1 (1e-7°);
 *                          sfixed32 longitude_i = 2; int32 altitude = 3;
 *                          fixed32 time = 4; … uint32 sats_in_view = 19; }
 *     4  NODEINFO_APP:     User { string id = 1; string long_name = 2;
 *                          string short_name = 3; bytes macaddr = 4;
 *                          HardwareModel hw_model = 5; … }
 *     67 TELEMETRY_APP:    Telemetry { fixed32 time = 1; oneof { DeviceMetrics
 *                          device_metrics = 2 {battery_level 1, voltage 2,
 *                          channel_utilization 3, air_util_tx 4, uptime 5};
 *                          EnvironmentMetrics environment_metrics = 3
 *                          {temperature 1, relative_humidity 2,
 *                          barometric_pressure 3} } }
 *   (Números de campo tomados de meshtastic/protobufs de memoria; los que no
 *   se han podido contrastar en línea se limitan a los de arriba.)
 * Un paquete solo se considera descifrado si el protobuf resultante es
 * estructuralmente válido (tipos de cable correctos, consumo exacto, portnum
 * conocido); si no, se muestra como cifrado (otra clave, canal privado…).
 * AGENTS.md §0.12: no se admite ninguna otra clave.
 *
 * opts: {"preset":"LONG_FAST"|"LONG_MODERATE"|"LONG_SLOW"|"MEDIUM_FAST"|
 *        "MEDIUM_SLOW"|"SHORT_FAST"|"SHORT_SLOW"} o {"sf":..,"bw":..}.
 *
 * state_json: {"nodes":[{id,short_name,long_name,lat,lon,alt,…,age}],…} con
 * caducidad de 2 h; los nodos con posición aparecen en el mapa.
 */
#include "decoder.h"
#include "aes.h"
#include "lora_phy.h"
#include "meshtastic.h"

#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const uint8_t MESH_DEFAULT_KEY[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                      0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

/* Presets (https://meshtastic.org/docs/overview/radio-settings/). LONG_SLOW
 * (SF12, BW 125, CR 4/8) no figura en esa página: es el valor histórico del
 * firmware, no verificado en línea. Los Turbo (BW 500) no caben en 500 kS/s. */
static const mesh_preset_t PRESETS[] = {
    {"LONG_FAST", 11, 250000, 1},   {"LONG_MODERATE", 11, 125000, 4}, {"LONG_SLOW", 12, 125000, 4},
    {"MEDIUM_FAST", 9, 250000, 1},  {"MEDIUM_SLOW", 10, 250000, 1},   {"SHORT_FAST", 7, 250000, 1},
    {"SHORT_SLOW", 8, 250000, 1},   {NULL, 0, 0, 0},
};

const mesh_preset_t *mesh_preset_find(const char *name)
{
    for (const mesh_preset_t *p = PRESETS; p->name; p++)
        if (!strcasecmp(p->name, name))
            return p;
    return NULL;
}

uint8_t mesh_channel_hash(const char *name, const uint8_t *key, int keylen)
{
    uint8_t h = 0;
    for (; *name; name++)
        h ^= (uint8_t)*name;
    for (int i = 0; i < keylen; i++)
        h ^= key[i];
    return h;
}

void mesh_crypt(const uint8_t *key, uint32_t from, uint32_t id, uint8_t *data, int n)
{
    aes_ctx_t c;
    uint8_t nonce[16] = {0};
    aes_set_key(&c, key, 16);
    for (int i = 0; i < 4; i++) {
        nonce[i] = (uint8_t)(id >> (8 * i)); /* packetId como u64 little-endian: los 4 bytes altos son 0 */
        nonce[8 + i] = (uint8_t)(from >> (8 * i));
    }
    aes_ctr_xor(&c, nonce, 4, data, data, (size_t)n);
}

/* ---- protobuf mínimo ---- */
typedef struct {
    const uint8_t *p;
    size_t n, i;
} pb_t;

static bool pb_varint(pb_t *r, uint64_t *v)
{
    uint64_t x = 0;
    for (int s = 0; s < 70 && r->i < r->n; s += 7) {
        uint8_t b = r->p[r->i++];
        x |= (uint64_t)(b & 0x7F) << s;
        if (!(b & 0x80)) {
            *v = x;
            return true;
        }
    }
    return false;
}

typedef struct {
    int field, wt;
    uint64_t val;       /* varint, fixed32 o fixed64 */
    const uint8_t *ptr; /* wt 2 */
    size_t len;
} pb_field_t;

static bool pb_next(pb_t *r, pb_field_t *f)
{
    uint64_t key;
    if (r->i >= r->n || !pb_varint(r, &key))
        return false;
    f->field = (int)(key >> 3);
    f->wt = (int)(key & 7);
    f->ptr = NULL;
    f->len = 0;
    switch (f->wt) {
    case 0:
        return pb_varint(r, &f->val);
    case 1:
        if (r->n - r->i < 8)
            return false;
        f->val = 0;
        for (int i = 7; i >= 0; i--)
            f->val = (f->val << 8) | r->p[r->i + (size_t)i];
        r->i += 8;
        return true;
    case 5:
        if (r->n - r->i < 4)
            return false;
        f->val = (uint64_t)r->p[r->i] | ((uint64_t)r->p[r->i + 1] << 8) | ((uint64_t)r->p[r->i + 2] << 16) |
                 ((uint64_t)r->p[r->i + 3] << 24);
        r->i += 4;
        return true;
    case 2: {
        uint64_t l;
        if (!pb_varint(r, &l) || l > r->n - r->i)
            return false;
        f->ptr = r->p + r->i;
        f->len = (size_t)l;
        r->i += (size_t)l;
        return true;
    }
    default:
        return false;
    }
}

static float f32(uint64_t v)
{
    uint32_t u = (uint32_t)v;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static const char *port_name(int p)
{
    switch (p) {
    case 1: return "TEXT_MESSAGE_APP";
    case 2: return "REMOTE_HARDWARE_APP";
    case 3: return "POSITION_APP";
    case 4: return "NODEINFO_APP";
    case 5: return "ROUTING_APP";
    case 6: return "ADMIN_APP";
    case 7: return "TEXT_MESSAGE_COMPRESSED_APP";
    case 8: return "WAYPOINT_APP";
    case 10: return "DETECTION_SENSOR_APP";
    case 32: return "REPLY_APP";
    case 34: return "PAXCOUNTER_APP";
    case 64: return "SERIAL_APP";
    case 65: return "STORE_FORWARD_APP";
    case 66: return "RANGE_TEST_APP";
    case 67: return "TELEMETRY_APP";
    case 70: return "TRACEROUTE_APP";
    case 71: return "NEIGHBORINFO_APP";
    case 73: return "MAP_REPORT_APP";
    default: return NULL;
    }
}

/* ¿Es buf un mensaje Data plausible? Devuelve portnum o −1. */
static int data_validate(const uint8_t *buf, int n)
{
    pb_t r = {buf, (size_t)n, 0};
    pb_field_t f;
    int port = -1;
    while (r.i < r.n) {
        if (!pb_next(&r, &f))
            return -1;
        switch (f.field) {
        case 1:
            if (f.wt != 0)
                return -1;
            port = (int)f.val;
            break;
        case 2:
            if (f.wt != 2)
                return -1;
            break;
        case 3:
        case 9:
            if (f.wt != 0)
                return -1;
            break;
        case 4: case 5: case 6: case 7: case 8:
            if (f.wt != 5)
                return -1;
            break;
        default:
            return -1;
        }
    }
    return (port > 0 && port_name(port)) ? port : -1;
}

/* Copia saneada UTF-8 (reemplaza bytes inválidos y de control por '?'). */
static void utf8_clean(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < cap;) {
        uint8_t c = src[i];
        int l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = l > 0 && i + (size_t)l <= n;
        for (int k = 1; ok && k < l; k++)
            ok = (src[i + (size_t)k] & 0xC0) == 0x80;
        if (l == 1 && (c < 0x20 && c != '\n'))
            ok = false;
        if (!ok) {
            dst[o++] = '?';
            i++;
            continue;
        }
        memcpy(dst + o, src + i, (size_t)l);
        o += (size_t)l;
        i += (size_t)l;
    }
    dst[o] = 0;
}

/* ---- tabla de nodos ---- */
#define MAX_NODES 256
#define NODE_TTL 7200.0
#define DUP_RING 64

typedef struct {
    bool used;
    uint32_t id;
    char short_name[16], long_name[48];
    bool has_pos, has_batt, has_volt, has_temp;
    double lat, lon, temp;
    int alt, hw_model, battery, sats;
    double voltage;
    double last;
    unsigned count;
    double rssi, snr;
    int hop_limit, hop_start;
} node_t;

typedef struct {
    lora_rx_t *rx;
    int sf, bw_hz;
    dec_emit_fn emit;
    void *ectx;
    double now;
    sb_t sb;
    unsigned long packets, decrypted, encrypted, dups;
    node_t nodes[MAX_NODES];
    struct {
        uint32_t from, id;
        double t;
    } dup[DUP_RING];
    int dup_pos;
} mesh_t;

static node_t *node_get(mesh_t *m, uint32_t id, double now)
{
    node_t *freen = NULL, *old = &m->nodes[0];
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &m->nodes[i];
        if (n->used && now - n->last > NODE_TTL)
            n->used = false;
        if (n->used && n->id == id)
            return n;
        if (!n->used && !freen)
            freen = n;
        if (n->last < old->last)
            old = n;
    }
    node_t *n = freen ? freen : old;
    memset(n, 0, sizeof *n);
    n->used = true;
    n->id = id;
    return n;
}

static void *mesh_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    const mesh_preset_t *pr = mesh_preset_find("LONG_FAST");
    int sf = pr->sf, bw = pr->bw_hz;
    if (cfg->opts && cfg->opts[0]) {
        jtok_t t[32];
        int nt = json_parse(cfg->opts, strlen(cfg->opts), t, 32);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "opts no es un objeto JSON válido");
            return NULL;
        }
        int i;
        double v;
        char s[40];
        if ((i = json_obj_get(cfg->opts, t, 0, "preset")) >= 0 && json_str(cfg->opts, t, i, s, sizeof s)) {
            pr = mesh_preset_find(s);
            if (!pr) {
                snprintf(err, errlen, "preset desconocido (LONG_FAST, LONG_MODERATE, MEDIUM_FAST, MEDIUM_SLOW, SHORT_FAST, SHORT_SLOW)");
                return NULL;
            }
            sf = pr->sf;
            bw = pr->bw_hz;
        }
        if ((i = json_obj_get(cfg->opts, t, 0, "sf")) >= 0 && json_num(cfg->opts, t, i, &v) && v >= 7 && v <= 12)
            sf = (int)v;
        if ((i = json_obj_get(cfg->opts, t, 0, "bw")) >= 0 && json_num(cfg->opts, t, i, &v)) {
            if (v != 125000 && v != 250000) {
                snprintf(err, errlen, "bw debe ser 125000 o 250000 (los presets Turbo de 500 kHz no caben en 500 kS/s)");
                return NULL;
            }
            bw = (int)v;
        }
    }
    lora_rx_cfg_t c;
    memset(&c, 0, sizeof c);
    c.fs = cfg->rate;
    c.bw_hz = bw;
    c.sf_mask = 1u << sf;
    c.nsync = 1;
    c.sync[0] = 0x2B;
    c.ldro = -1;
    mesh_t *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    m->sf = sf;
    m->bw_hz = bw;
    m->rx = lora_rx_create(&c, err, errlen);
    if (!m->rx) {
        free(m);
        return NULL;
    }
    return m;
}

static void mesh_destroy(void *st)
{
    mesh_t *m = st;
    if (!m)
        return;
    lora_rx_destroy(m->rx);
    sb_free(&m->sb);
    free(m);
}

static void node_label(const mesh_t *m, uint32_t id, char *out, size_t cap)
{
    if (id == 0xFFFFFFFFu) {
        snprintf(out, cap, "^all");
        return;
    }
    for (int i = 0; i < MAX_NODES; i++)
        if (m->nodes[i].used && m->nodes[i].id == id && m->nodes[i].short_name[0]) {
            snprintf(out, cap, "%s", m->nodes[i].short_name);
            return;
        }
    snprintf(out, cap, "!%08x", id);
}

/* Interpreta el mensaje interno; añade campos al evento y actualiza el nodo. */
static void decode_inner(mesh_t *m, node_t *nd, int port, const uint8_t *pl, size_t pn, sb_t *sb, char *summary,
                         size_t sl)
{
    pb_t r = {pl, pn, 0};
    pb_field_t f;
    char buf[300];
    switch (port) {
    case MESH_PORT_TEXT:
        utf8_clean(buf, sizeof buf, pl, pn);
        sb_puts(sb, ",\"msg\":");
        sb_json_str(sb, buf);
        snprintf(summary, sl, "“%.150s”", buf);
        break;
    case MESH_PORT_POSITION: {
        int32_t lat = 0, lon = 0;
        bool hl = false, ho = false;
        int alt = 0, sats = 0;
        bool ha = false;
        uint32_t tm = 0;
        while (pb_next(&r, &f)) {
            if (f.field == 1 && f.wt == 5) {
                lat = (int32_t)(uint32_t)f.val;
                hl = true;
            } else if (f.field == 2 && f.wt == 5) {
                lon = (int32_t)(uint32_t)f.val;
                ho = true;
            } else if (f.field == 3 && f.wt == 0) {
                alt = (int)(int32_t)(uint32_t)f.val;
                ha = true;
            } else if (f.field == 4 && f.wt == 5)
                tm = (uint32_t)f.val;
            else if (f.field == 19 && f.wt == 0)
                sats = (int)f.val;
        }
        if (hl && ho) {
            nd->has_pos = true;
            nd->lat = lat * 1e-7;
            nd->lon = lon * 1e-7;
            sb_printf(sb, ",\"lat\":%.7f,\"lon\":%.7f", nd->lat, nd->lon);
            snprintf(summary, sl, "posición %.5f, %.5f", nd->lat, nd->lon);
        } else
            snprintf(summary, sl, "posición (sin fix)");
        if (ha) {
            nd->alt = alt;
            sb_printf(sb, ",\"alt\":%d", alt);
        }
        if (sats) {
            nd->sats = sats;
            sb_printf(sb, ",\"sats\":%d", sats);
        }
        if (tm)
            sb_printf(sb, ",\"gps_time\":%u", tm);
        break;
    }
    case MESH_PORT_NODEINFO:
        while (pb_next(&r, &f)) {
            if (f.wt == 2 && f.field == 2) {
                utf8_clean(nd->long_name, sizeof nd->long_name, f.ptr, MIN(f.len, sizeof nd->long_name - 1));
            } else if (f.wt == 2 && f.field == 3) {
                utf8_clean(nd->short_name, sizeof nd->short_name, f.ptr, MIN(f.len, sizeof nd->short_name - 1));
            } else if (f.wt == 0 && f.field == 5)
                nd->hw_model = (int)f.val;
        }
        sb_puts(sb, ",\"long_name\":");
        sb_json_str(sb, nd->long_name);
        sb_puts(sb, ",\"short_name\":");
        sb_json_str(sb, nd->short_name);
        sb_printf(sb, ",\"hw_model\":%d", nd->hw_model);
        snprintf(summary, sl, "nodeinfo «%s» (%s)", nd->long_name, nd->short_name);
        break;
    case MESH_PORT_TELEMETRY: {
        size_t k = 0;
        while (pb_next(&r, &f)) {
            if (f.field == 2 && f.wt == 2) { /* DeviceMetrics */
                pb_t d = {f.ptr, f.len, 0};
                pb_field_t g;
                while (pb_next(&d, &g)) {
                    if (g.field == 1 && g.wt == 0) {
                        nd->battery = (int)g.val;
                        nd->has_batt = true;
                        sb_printf(sb, ",\"battery\":%d", nd->battery);
                    } else if (g.field == 2 && g.wt == 5) {
                        nd->voltage = f32(g.val);
                        nd->has_volt = true;
                        sb_puts(sb, ",\"voltage\":");
                        sb_json_num(sb, nd->voltage, 2);
                    } else if (g.field == 3 && g.wt == 5) {
                        sb_puts(sb, ",\"channel_util\":");
                        sb_json_num(sb, f32(g.val), 1);
                    } else if (g.field == 4 && g.wt == 5) {
                        sb_puts(sb, ",\"air_util_tx\":");
                        sb_json_num(sb, f32(g.val), 1);
                    } else if (g.field == 5 && g.wt == 0)
                        sb_printf(sb, ",\"uptime\":%llu", (unsigned long long)g.val);
                }
                k += (size_t)snprintf(summary + k, sl - k, "telemetría batería %d%% %.2f V", nd->battery, nd->voltage);
            } else if (f.field == 3 && f.wt == 2) { /* EnvironmentMetrics */
                pb_t d = {f.ptr, f.len, 0};
                pb_field_t g;
                while (pb_next(&d, &g)) {
                    if (g.field == 1 && g.wt == 5) {
                        nd->temp = f32(g.val);
                        nd->has_temp = true;
                        sb_puts(sb, ",\"temperature\":");
                        sb_json_num(sb, nd->temp, 1);
                    } else if (g.field == 2 && g.wt == 5) {
                        sb_puts(sb, ",\"humidity\":");
                        sb_json_num(sb, f32(g.val), 1);
                    } else if (g.field == 3 && g.wt == 5) {
                        sb_puts(sb, ",\"pressure\":");
                        sb_json_num(sb, f32(g.val), 1);
                    }
                }
                k += (size_t)snprintf(summary + k, sl - k, "telemetría %.1f °C", nd->temp);
            }
        }
        if (!k)
            snprintf(summary, sl, "telemetría");
        break;
    }
    default:
        snprintf(summary, sl, "%s (%zu B)", port_name(port), pn);
        break;
    }
    (void)m;
}

static void on_frame(void *ctx, const lora_frame_t *f)
{
    mesh_t *m = ctx;
    if (!f->crc_ok || f->len < MESH_HDR_LEN)
        return;
    const uint8_t *p = f->data;
    uint32_t to = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t from = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
    uint32_t id = p[8] | (p[9] << 8) | (p[10] << 16) | ((uint32_t)p[11] << 24);
    unsigned flags = p[12];
    int chash = p[13];
    m->packets++;
    bool dup = false;
    for (int i = 0; i < DUP_RING; i++)
        if (m->dup[i].from == from && m->dup[i].id == id && m->now - m->dup[i].t < 600.0 && m->dup[i].t > 0)
            dup = true;
    if (dup)
        m->dups++;
    m->dup[m->dup_pos].from = from;
    m->dup[m->dup_pos].id = id;
    m->dup[m->dup_pos].t = m->now > 0 ? m->now : 1e-3;
    m->dup_pos = (m->dup_pos + 1) % DUP_RING;

    uint8_t dec[256];
    int n = f->len - MESH_HDR_LEN;
    memcpy(dec, p + MESH_HDR_LEN, (size_t)n);
    mesh_crypt(MESH_DEFAULT_KEY, from, id, dec, n);
    int port = data_validate(dec, n);

    node_t *nd = node_get(m, from, m->now);
    nd->last = m->now;
    nd->count++;
    nd->rssi = f->rssi_dbfs;
    nd->snr = f->snr_db;
    nd->hop_limit = (int)(flags & 7);
    nd->hop_start = (int)(flags >> 5);

    if (!m->emit)
        return;
    sb_t *sb = &m->sb;
    sb_clear(sb);
    char fl[48], tl[48], summary[260] = "", text[400];
    snprintf(fl, sizeof fl, "!%08x", from);
    node_label(m, to, tl, sizeof tl);
    sb_printf(sb, "{\"from\":\"!%08x\",\"to\":\"%s\",\"to_id\":%u,\"id\":%u,\"hop_limit\":%u,\"hop_start\":%u,"
                  "\"want_ack\":%s,\"via_mqtt\":%s,\"channel\":%d,\"next_hop\":%d,\"relay\":%d,\"dup\":%s,"
                  "\"sf\":%d,\"bw\":%d,\"snr\":%.1f,\"rssi\":%.1f,\"len\":%d",
              from, to == 0xFFFFFFFFu ? "^all" : "dm", to, id, flags & 7, flags >> 5, (flags & 8) ? "true" : "false",
              (flags & 16) ? "true" : "false", chash, p[14], p[15], dup ? "true" : "false", f->sf, f->bw_hz, f->snr_db,
              f->rssi_dbfs, f->len);
    if (port > 0) {
        m->decrypted++;
        sb_printf(sb, ",\"encrypted\":false,\"portnum\":%d,\"port\":\"%s\"", port, port_name(port));
        pb_t r = {dec, (size_t)n, 0};
        pb_field_t g;
        const uint8_t *inner = NULL;
        size_t il = 0;
        while (pb_next(&r, &g))
            if (g.field == 2) {
                inner = g.ptr;
                il = g.len;
            }
        decode_inner(m, nd, port, inner ? inner : dec, inner ? il : 0, sb, summary, sizeof summary);
        node_label(m, from, fl, sizeof fl);
        snprintf(text, sizeof text, "%s → %s: %s", fl, tl, summary);
        if (!inner)
            snprintf(text, sizeof text, "%s → %s: %s", fl, tl, port_name(port));
    } else {
        m->encrypted++;
        sb_puts(sb, ",\"encrypted\":true");
        snprintf(text, sizeof text, "!%08x → %s: cifrado (hash de canal 0x%02X, %d B)", from, tl, chash, n);
    }
    sb_puts(sb, ",\"text\":");
    sb_json_str(sb, text);
    sb_puts(sb, "}");
    m->emit(m->ectx, sb->data, sb->len);
}

static void mesh_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    mesh_t *m = st;
    m->emit = emit;
    m->ectx = ctx;
    m->now = now;
    lora_rx_process(m->rx, iq, n, on_frame, m);
}

static void mesh_state(void *st, sb_t *sb, double now)
{
    mesh_t *m = st;
    sb_printf(sb, "{\"sf\":%d,\"bw\":%d,\"packets\":%lu,\"decrypted\":%lu,\"encrypted\":%lu,\"dups\":%lu,\"nodes\":[",
              m->sf, m->bw_hz, m->packets, m->decrypted, m->encrypted, m->dups);
    int cnt = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &m->nodes[i];
        if (!n->used || now - n->last > NODE_TTL)
            continue;
        if (cnt++)
            sb_puts(sb, ",");
        sb_printf(sb, "{\"id\":\"!%08x\",\"short_name\":", n->id);
        sb_json_str(sb, n->short_name);
        sb_puts(sb, ",\"long_name\":");
        sb_json_str(sb, n->long_name);
        if (n->has_pos) {
            sb_printf(sb, ",\"lat\":%.6f,\"lon\":%.6f,\"alt\":%d", n->lat, n->lon, n->alt);
        }
        if (n->has_batt)
            sb_printf(sb, ",\"battery\":%d", n->battery);
        if (n->has_volt) {
            sb_puts(sb, ",\"voltage\":");
            sb_json_num(sb, n->voltage, 2);
        }
        if (n->has_temp) {
            sb_puts(sb, ",\"temperature\":");
            sb_json_num(sb, n->temp, 1);
        }
        sb_printf(sb, ",\"packets\":%u,\"rssi\":%.1f,\"snr\":%.1f,\"hops\":%d,\"age\":%.0f}", n->count, n->rssi,
                  n->snr, n->hop_start - n->hop_limit, now - n->last);
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_MESHTASTIC = {
    .id = "meshtastic",
    .name = "Meshtastic",
    .group = "IoT",
    .desc = "Malla Meshtastic sobre LoRa (clave pública por defecto).",
    .kind = DEC_NARROW,
    .rate = 500000,
    .bw_hz = 300000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {869525000LL, 0LL, 0LL, 0LL},
    .create = mesh_create,
    .destroy = mesh_destroy,
    .process = mesh_process,
    .state_json = mesh_state,
};
