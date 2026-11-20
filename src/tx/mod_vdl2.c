/*
 * mod_vdl2.c — Modulador VDL Modo 2 (D8PSK, 10 500 símbolos/s) a 105 kHz.
 *
 * Construye una trama AVLC (por defecto un bloque ACARS sobre AVLC de una
 * aeronave a una estación de tierra) y la envuelve como la capa física de
 * Doc 9776 (resumen en decoders/vdl2_frame.h):
 *
 *   AVLC → HDLC (flags, relleno de ceros, FCS CRC-16/X-25) → octetos (LSB primero) →
 *   RS(255,249) entrelazado → [cabecera + datos] aleatorizados juntos → bits
 *   flujo = UW(48) | cabecera(25) | datos, agrupado en tripletes
 *   cada triplete → incremento de fase k·π/4 (Gray → binario) → símbolo D8PSK
 *   conformación con raíz de coseno alzado, α = 0,6, 10 muestras/símbolo.
 *
 * La fase absoluta es arbitraria (la modulación es diferencial). Se añaden 4
 * símbolos nulos de rampa antes de la UW y 8 de cola.
 *
 * Parámetros JSON (opcionales):
 *  {"src":"4840D6","dst":"2B4F10","ftype":"I|UI|RR|XID|SABM|UA|DISC",
 *   "ns":0,"nr":0,"poll":false,"acars":true,"reg":"EC-MXY","flight":"IB3456",
 *   "label":"H1","msgno":"M12A","block":"3","text":"…","info":"hex","repeat":2}
 * air_ok = false (banda aeronáutica).
 */
#include "mod.h"

#include "../decoders/acars_blk.h"
#include "../decoders/vdl2_avlc.h"
#include "../decoders/vdl2_frame.h"
#include "../dsp/pulse.h"
#include "../util/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10); }

static int append_burst(iqbuf_t *o, const uint8_t *avlc, int navlc, const float *h, int nh, double *ph)
{
    uint8_t hb[16 + 10 * 460];
    int nbits = vdl2_hdlc_encode(avlc, navlc, hb, sizeof hb);
    if (nbits < 0)
        return -1;
    int D = (nbits + 7) / 8;
    uint8_t *data = calloc((size_t)D, 1);
    for (int i = 0; i < nbits; i++)
        if (hb[i])
            data[i >> 3] |= (uint8_t)(1u << (i & 7)); /* octetos LSB primero (orden de línea) */
    int E = vdl2_encoded_bytes(D);
    uint8_t *enc = malloc((size_t)E);
    vdl2_fec_encode(data, D, enc);
    int total = 48 + VDL2_HDR_BITS + 8 * E;
    int nsym = (total + 2) / 3;
    uint8_t *bits = calloc((size_t)nsym * 3, 1);
    memcpy(bits, VDL2_UW_BITS, 48);
    vdl2_hdr_encode((uint32_t)nbits, bits + 48);
    uint8_t *db = bits + 48 + VDL2_HDR_BITS;
    for (int i = 0; i < 8 * E; i++)
        db[i] = (uint8_t)((enc[i >> 3] >> (i & 7)) & 1);
    vdl2_scramble(bits + 48, VDL2_HDR_BITS + 8 * E); /* cabecera y datos en un solo flujo */

    int tot_syms = 4 + nsym + 8;
    cf32 *sy = malloc(sizeof(cf32) * (size_t)tot_syms);
    int k = 0;
    for (int i = 0; i < 4; i++, k++)
        sy[k] = (cf32){(float)cos(*ph), (float)sin(*ph)};
    for (int i = 0; i < nsym; i++, k++) {
        int v = bits[3 * i] << 2 | bits[3 * i + 1] << 1 | bits[3 * i + 2];
        *ph += VDL2_GRAY_TO_INC[v] * M_PI / 4.0;
        sy[k] = (cf32){(float)cos(*ph), (float)sin(*ph)};
    }
    for (int i = 0; i < 8; i++, k++)
        sy[k] = (cf32){(float)cos(*ph), (float)sin(*ph)};
    int nout = tot_syms * VDL2_SPS + nh - 1;
    cf32 *w = malloc(sizeof(cf32) * (size_t)nout);
    pulse_shape_cf(sy, tot_syms, h, nh, VDL2_SPS, w);
    iqbuf_reserve(o, nout);
    for (int i = 0; i < nout; i++)
        iqbuf_push(o, w[i]);
    free(w);
    free(sy);
    free(bits);
    free(enc);
    free(data);
    return 0;
}

static int vdl2_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    uint32_t src = 0x4840D6, dst = 0x2B4F10;
    char ftype[8] = "I";
    int ns = 0, nr = 0, poll = 0, repeat = 2;
    bool acars = true;
    acars_blk_t b;
    memset(&b, 0, sizeof b);
    b.mode = '2';
    snprintf(b.reg, sizeof b.reg, "EC-MXY");
    b.tak = ACARS_NAK;
    snprintf(b.label, sizeof b.label, "H1");
    b.bid = '3';
    snprintf(b.msgno, sizeof b.msgno, "M12A");
    snprintf(b.flight, sizeof b.flight, "IB3456");
    snprintf(b.text, sizeof b.text, "#DFB/WX LEMD LEBL 151200Z 27008KT CAVOK");
    uint8_t rawinfo[420];
    int nraw = -1;
    if (params && params[0]) {
        jtok_t t[96];
        int nt = json_parse(params, strlen(params), t, 96);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "params no es un objeto JSON válido");
            return -1;
        }
        char s[900];
        double v;
        bool bv;
        int i;
        if ((i = json_obj_get(params, t, 0, "src")) >= 0 && json_str(params, t, i, s, sizeof s))
            src = (uint32_t)strtoul(s, NULL, 16) & 0xFFFFFF;
        if ((i = json_obj_get(params, t, 0, "dst")) >= 0 && json_str(params, t, i, s, sizeof s))
            dst = (uint32_t)strtoul(s, NULL, 16) & 0xFFFFFF;
        if ((i = json_obj_get(params, t, 0, "ftype")) >= 0 && json_str(params, t, i, s, sizeof s))
            snprintf(ftype, sizeof ftype, "%.7s", s);
        if ((i = json_obj_get(params, t, 0, "ns")) >= 0 && json_num(params, t, i, &v))
            ns = (int)v & 7;
        if ((i = json_obj_get(params, t, 0, "nr")) >= 0 && json_num(params, t, i, &v))
            nr = (int)v & 7;
        if ((i = json_obj_get(params, t, 0, "poll")) >= 0 && json_bool(params, t, i, &bv))
            poll = bv;
        if ((i = json_obj_get(params, t, 0, "acars")) >= 0 && json_bool(params, t, i, &bv))
            acars = bv;
        if ((i = json_obj_get(params, t, 0, "reg")) >= 0 && json_str(params, t, i, s, sizeof s))
            snprintf(b.reg, sizeof b.reg, "%.7s", s);
        if ((i = json_obj_get(params, t, 0, "flight")) >= 0 && json_str(params, t, i, s, sizeof s))
            snprintf(b.flight, sizeof b.flight, "%.6s", s);
        if ((i = json_obj_get(params, t, 0, "label")) >= 0 && json_str(params, t, i, s, sizeof s) && strlen(s) >= 2) {
            b.label[0] = s[0];
            b.label[1] = s[1];
        }
        if ((i = json_obj_get(params, t, 0, "msgno")) >= 0 && json_str(params, t, i, s, sizeof s))
            snprintf(b.msgno, sizeof b.msgno, "%.4s", s);
        if ((i = json_obj_get(params, t, 0, "block")) >= 0 && json_str(params, t, i, s, sizeof s) && s[0])
            b.bid = s[0];
        if ((i = json_obj_get(params, t, 0, "text")) >= 0 && json_str(params, t, i, s, sizeof s))
            snprintf(b.text, sizeof b.text, "%.200s", s);
        if ((i = json_obj_get(params, t, 0, "repeat")) >= 0 && json_num(params, t, i, &v))
            repeat = (int)CLAMP(v, 1, 20);
        if ((i = json_obj_get(params, t, 0, "info")) >= 0 && json_str(params, t, i, s, sizeof s)) {
            nraw = 0;
            for (int k = 0; s[k] && s[k + 1] && nraw < (int)sizeof rawinfo; k += 2)
                rawinfo[nraw++] = (uint8_t)(hexval(s[k]) << 4 | hexval(s[k + 1]));
            acars = false;
        }
    }
    b.text_len = (int)strlen(b.text);

    uint8_t f[460];
    int n = 0;
    avlc_addr_put(f, dst, 5, 0); /* tipo 5 = estación de tierra, como en la grabación real */
    avlc_addr_put(f + 4, src, 1, 0);
    f[7] |= 1;                    /* extensión: último octeto del campo de direcciones */
    n = 8;
    uint8_t c;
    if (!strcmp(ftype, "I"))
        c = (uint8_t)(ns << 1 | poll << 4 | nr << 5);
    else if (!strcmp(ftype, "RR"))
        c = (uint8_t)(0x01 | poll << 4 | nr << 5);
    else if (!strcmp(ftype, "UI"))
        c = (uint8_t)(0x03 | poll << 4);
    else if (!strcmp(ftype, "XID"))
        c = (uint8_t)(0xAF | poll << 4);
    else if (!strcmp(ftype, "SABM"))
        c = (uint8_t)(0x2F | poll << 4);
    else if (!strcmp(ftype, "UA"))
        c = (uint8_t)(0x63 | poll << 4);
    else if (!strcmp(ftype, "DISC"))
        c = (uint8_t)(0x43 | poll << 4);
    else {
        snprintf(err, errlen, "ftype desconocido: %s", ftype);
        return -1;
    }
    f[n++] = c;
    if (nraw >= 0) {
        memcpy(f + n, rawinfo, (size_t)nraw);
        n += nraw;
    } else if (acars && (c & 1) == 0) {
        /* ACARS sobre AVLC: FF FF 01 + bloque (7 bits) + BCS + DEL */
        uint8_t blk[300];
        int nb = acars_blk_build(&b, blk, sizeof blk);
        if (nb < 0) {
            snprintf(err, errlen, "bloque ACARS demasiado largo");
            return -1;
        }
        f[n++] = 0xFF; /* cabecera de ACARS sobre AVLC: FF FF 01 */
        f[n++] = 0xFF;
        f[n++] = ACARS_SOH;
        memcpy(f + n, blk, (size_t)nb);
        uint16_t bcs = acars_bcs(blk, nb);
        n += nb;
        f[n++] = (uint8_t)(bcs & 0xFF);
        f[n++] = (uint8_t)(bcs >> 8);
        f[n++] = ACARS_DEL;
    }
    n = vdl2_add_fcs(f, n);

    float h[81];
    int nh = pulse_rrc(h, 0.6, VDL2_SPS, 8, PULSE_NORM_ENERGY);
    out->rate = VDL2_RATE;
    iqbuf_reserve(out, 8000);
    for (int i = 0; i < 4000; i++)
        iqbuf_push(out, (cf32){0, 0});
    double ph = 0.7;
    for (int r = 0; r < repeat; r++) {
        if (append_burst(out, f, n, h, nh, &ph) < 0) {
            snprintf(err, errlen, "trama demasiado larga");
            return -1;
        }
        for (int i = 0; i < 4000; i++)
            iqbuf_push(out, (cf32){0, 0});
    }
    float pk = 0;
    for (int i = 0; i < out->n; i++)
        pk = fmaxf(pk, hypotf(out->s[i].re, out->s[i].im));
    float g = pk > 0 ? 0.85f / pk : 1.0f;
    for (int i = 0; i < out->n; i++) {
        out->s[i].re *= g;
        out->s[i].im *= g;
    }
    return 0;
}

const modulator_desc_t MOD_VDL2 = {
    .id = "vdl2",
    .name = "VDL Modo 2",
    .air_ok = false,
    .modulate = vdl2_modulate,
};
