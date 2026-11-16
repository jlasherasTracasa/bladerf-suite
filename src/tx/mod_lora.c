/*
 * mod_lora.c — Modulador LoRa / LoRaWAN (capa física CSS) a 500 kS/s.
 *
 * Genera tramas LoRa con cabecera explícita y las modula como chirps
 * (lora_phy.c: preámbulo, palabra de sincronía, 2,25 downchirps, símbolos).
 *
 * Parámetros JSON (todos opcionales):
 *   {"sf":7..12, "bw":125000|250000, "cr":1..4, "crc":true, "sync":52,
 *    "preamble":8, "repeat":1, "gap_ms":50,
 *    "payload":"hex"            carga LoRa en bruto, o
 *    "text":"cadena"            carga = texto ASCII, o (por defecto)
 *    "lorawan":{"devaddr":"26011BDA","fcnt":1,"fport":1,"confirmed":false,
 *               "data":"hex"}   trama LoRaWAN de subida con MHDR/DevAddr/FCtrl/
 *                               FCnt/FPort; MIC ficticio (no se tienen claves)}
 *
 * air_ok = true: LoRa en SRD 868 MHz (868,0–868,6 y 869,4–869,65 MHz) es
 * legal con las limitaciones de ciclo de trabajo que impone tx.c. El ancho
 * ocupado (125/250 kHz) lo comprueba la política de tx.c.
 */
#include "mod.h"

#include "../decoders/lora_phy.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 500000.0

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    while (s[0] && s[1] && n < cap) {
        int a = hexval(s[0]), b = hexval(s[1]);
        if (a < 0 || b < 0)
            return -1;
        out[n++] = (uint8_t)(a * 16 + b);
        s += 2;
    }
    return n;
}

static int lora_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    int sf = 7, bw = 125000, cr = 1, sync = 0x34, pre = 8, repeat = 1, gap_ms = 50;
    bool crc = true;
    uint8_t pl[LORA_MAX_PAYLOAD];
    int len = 0;
    char s[600];
    double v;
    bool have_payload = false, lw = true;
    uint32_t devaddr = 0x26011BDA;
    int fcnt = 1, fport = 1;
    bool confirmed = false;
    uint8_t lwdata[64] = {0x48, 0x6F, 0x6C, 0x61};
    int lwlen = 4;
    if (params && params[0]) {
        jtok_t t[64];
        int nt = json_parse(params, strlen(params), t, 64);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "params no es un objeto JSON válido");
            return -1;
        }
        int i;
        if ((i = json_obj_get(params, t, 0, "sf")) >= 0 && json_num(params, t, i, &v))
            sf = (int)v;
        if ((i = json_obj_get(params, t, 0, "bw")) >= 0 && json_num(params, t, i, &v))
            bw = (int)v;
        if ((i = json_obj_get(params, t, 0, "cr")) >= 0 && json_num(params, t, i, &v))
            cr = (int)v;
        if ((i = json_obj_get(params, t, 0, "sync")) >= 0 && json_num(params, t, i, &v))
            sync = (int)v & 0xFF;
        if ((i = json_obj_get(params, t, 0, "preamble")) >= 0 && json_num(params, t, i, &v))
            pre = (int)CLAMP(v, 6, 64);
        if ((i = json_obj_get(params, t, 0, "repeat")) >= 0 && json_num(params, t, i, &v))
            repeat = (int)CLAMP(v, 1, 20);
        if ((i = json_obj_get(params, t, 0, "gap_ms")) >= 0 && json_num(params, t, i, &v))
            gap_ms = (int)CLAMP(v, 1, 5000);
        bool b;
        if ((i = json_obj_get(params, t, 0, "crc")) >= 0 && json_bool(params, t, i, &b))
            crc = b;
        if ((i = json_obj_get(params, t, 0, "payload")) >= 0 && json_str(params, t, i, s, sizeof s)) {
            len = parse_hex(s, pl, LORA_MAX_PAYLOAD);
            if (len < 1) {
                snprintf(err, errlen, "payload no es hexadecimal válido");
                return -1;
            }
            have_payload = true;
        } else if ((i = json_obj_get(params, t, 0, "text")) >= 0 && json_str(params, t, i, s, sizeof s)) {
            len = (int)MIN(strlen(s), (size_t)LORA_MAX_PAYLOAD);
            memcpy(pl, s, (size_t)len);
            have_payload = true;
        }
        int lj = json_obj_get(params, t, 0, "lorawan");
        if (lj >= 0 && t[lj].type == J_OBJ) {
            int k;
            if ((k = json_obj_get(params, t, lj, "devaddr")) >= 0 && json_str(params, t, k, s, sizeof s))
                devaddr = (uint32_t)strtoul(s, NULL, 16);
            if ((k = json_obj_get(params, t, lj, "fcnt")) >= 0 && json_num(params, t, k, &v))
                fcnt = (int)v & 0xFFFF;
            if ((k = json_obj_get(params, t, lj, "fport")) >= 0 && json_num(params, t, k, &v))
                fport = (int)CLAMP(v, 1, 223);
            if ((k = json_obj_get(params, t, lj, "confirmed")) >= 0 && json_bool(params, t, k, &b))
                confirmed = b;
            if ((k = json_obj_get(params, t, lj, "data")) >= 0 && json_str(params, t, k, s, sizeof s)) {
                lwlen = parse_hex(s, lwdata, (int)sizeof lwdata);
                if (lwlen < 0) {
                    snprintf(err, errlen, "lorawan.data no es hexadecimal válido");
                    return -1;
                }
            }
        }
        if (have_payload && lj < 0)
            lw = false;
    }
    if (have_payload)
        lw = false;
    if (sf < 7 || sf > 12 || (bw != 125000 && bw != 250000) || cr < 1 || cr > 4) {
        snprintf(err, errlen, "parámetros fuera de rango (sf 7..12, bw 125000/250000, cr 1..4)");
        return -1;
    }
    if (lw) { /* MHDR | DevAddr | FCtrl | FCnt | FPort | FRMPayload | MIC */
        int n = 0;
        pl[n++] = confirmed ? 0x80 : 0x40;
        for (int i = 0; i < 4; i++)
            pl[n++] = (uint8_t)(devaddr >> (8 * i));
        pl[n++] = 0x80; /* ADR, sin opciones */
        pl[n++] = (uint8_t)fcnt;
        pl[n++] = (uint8_t)(fcnt >> 8);
        pl[n++] = (uint8_t)fport;
        for (int i = 0; i < lwlen; i++)
            pl[n++] = lwdata[i];
        for (int i = 0; i < 4; i++) /* MIC ficticio: sin AppSKey/NwkSKey no hay CMAC */
            pl[n++] = (uint8_t)(0xA5 ^ (i * 0x3C));
        len = n;
    }
    bool ldro = lora_ldro_auto(sf, bw);
    uint16_t syms[1100];
    int ns = lora_encode_symbols(pl, len, sf, cr, crc, ldro, syms, 1100);
    if (ns < 0) {
        snprintf(err, errlen, "no se pudo codificar la trama");
        return -1;
    }
    out->rate = FS;
    lora_push_silence(out, (int)(0.02 * FS));
    for (int r = 0; r < repeat; r++) {
        lora_modulate_frame(out, FS, bw, sf, sync, pre, syms, ns, 0.8f);
        lora_push_silence(out, (int)(gap_ms * 1e-3 * FS));
    }
    return 0;
}

const modulator_desc_t MOD_LORA = {
    .id = "lora",
    .name = "LoRa / LoRaWAN",
    .air_ok = true,
    .modulate = lora_modulate,
};
