/*
 * mod_acars.c — Modulador ACARS (AM + MSK 2400 bit/s) a 48 kHz.
 *
 * Construye un bloque ARINC 618 (ver decoders/acars_blk.h): pre-key de unos,
 * "+*", SYN SYN, SOH, Modo, matrícula, TAK, etiqueta, bloque, STX, texto, ETX,
 * BCS (CRC-16/KERMIT, byte bajo primero) y DEL; cada carácter con paridad impar
 * y transmitido con el bit menos significativo primero.
 *
 * Modulación: señal moduladora MSK de fase continua (1 → 1200 Hz, 0 → 2400 Hz,
 * 20 muestras por bit) que modula en amplitud la portadora:
 *     x[n] = A·(1 + m·cos φ[n]),   φ' = 2π·f_bit/fs,   m = profundidad (0,5)
 * La salida es real pura (Q = 0) y centrada en 0 Hz; el decodificador usa la
 * envolvente, así que no importa el desvío de portadora.
 *
 * Parámetros JSON (todos opcionales):
 *   {"reg":"EC-MXY","flight":"IB3456","label":"H1","msgno":"M12A","block":"3",
 *    "text":"…","mode":"2","depth":0.5,"repeat":2,"up":false}
 * air_ok = false: banda aeronáutica protegida (AGENTS §6).
 */
#include "mod.h"

#include "../decoders/acars_blk.h"
#include "../util/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define SPB 20

/* Codificación diferencial validada con una grabación real: el tono es 2400 Hz si
 * el bit de datos repite el anterior y 1200 Hz si cambia (el pre-key de unos es un
 * tono fijo de 2400 Hz). 'prev' es el último bit de datos enviado. */
static int prev_bit = 1;

static void put_bit(iqbuf_t *o, int bit, double *ph, double depth)
{
    double dph = 2.0 * M_PI * (bit == prev_bit ? 2400.0 : 1200.0) / FS;
    prev_bit = bit;
    iqbuf_reserve(o, SPB);
    for (int i = 0; i < SPB; i++) {
        float v = (float)(0.9 / (1.0 + depth) * (1.0 + depth * cos(*ph)));
        iqbuf_push(o, (cf32){v, 0});
        *ph += dph;
    }
    if (*ph > 2 * M_PI)
        *ph = fmod(*ph, 2 * M_PI);
}

static void put_char(iqbuf_t *o, uint8_t c, double *ph, double depth)
{
    for (int b = 0; b < 8; b++)
        put_bit(o, (c >> b) & 1, ph, depth);
}

static int acars_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
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
    double depth = 0.5;
    int repeat = 2;
    bool up = false;
    if (params && params[0]) {
        jtok_t t[64];
        int nt = json_parse(params, strlen(params), t, 64);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "params no es un objeto JSON válido");
            return -1;
        }
        char s[240];
        double v;
        int i;
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
        if ((i = json_obj_get(params, t, 0, "mode")) >= 0 && json_str(params, t, i, s, sizeof s) && s[0])
            b.mode = s[0];
        if ((i = json_obj_get(params, t, 0, "text")) >= 0 && json_str(params, t, i, s, sizeof s))
            snprintf(b.text, sizeof b.text, "%.200s", s);
        if ((i = json_obj_get(params, t, 0, "depth")) >= 0 && json_num(params, t, i, &v))
            depth = CLAMP(v, 0.1, 0.9);
        if ((i = json_obj_get(params, t, 0, "repeat")) >= 0 && json_num(params, t, i, &v))
            repeat = (int)CLAMP(v, 1, 50);
        bool bv;
        if ((i = json_obj_get(params, t, 0, "up")) >= 0 && json_bool(params, t, i, &bv))
            up = bv;
    }
    if (up) { /* enlace ascendente: bloque con letra y sin nº de mensaje/vuelo */
        if (isdigit((unsigned char)b.bid))
            b.bid = 'A';
        b.msgno[0] = 0;
        b.flight[0] = 0;
    }
    b.text_len = (int)strlen(b.text);

    uint8_t blk[320];
    int n = acars_blk_build(&b, blk, sizeof blk);
    if (n < 0) {
        snprintf(err, errlen, "bloque demasiado largo");
        return -1;
    }
    uint8_t chars[340];
    int nc = 0;
    chars[nc++] = acars_add_parity(0x2B);
    chars[nc++] = acars_add_parity(0x2A);
    chars[nc++] = acars_add_parity(ACARS_SYN);
    chars[nc++] = acars_add_parity(ACARS_SYN);
    chars[nc++] = acars_add_parity(ACARS_SOH);
    int body0 = nc;
    for (int i = 0; i < n; i++)
        chars[nc++] = acars_add_parity(blk[i]);
    /* BCS sobre los bytes tal como van por el aire (con paridad), byte bajo primero */
    uint16_t bcs = acars_bcs(chars + body0, nc - body0);
    chars[nc++] = (uint8_t)(bcs & 0xFF);
    chars[nc++] = (uint8_t)(bcs >> 8);
    chars[nc++] = acars_add_parity(ACARS_DEL);

    out->rate = FS;
    double ph = 0;
    prev_bit = 1;
    iqbuf_reserve(out, 4800);
    for (int i = 0; i < 4800; i++) /* 0,1 s de silencio con portadora AM sin modular */
        iqbuf_push(out, (cf32){(float)(0.9 / (1.0 + depth)), 0});
    for (int r = 0; r < repeat; r++) {
        for (int i = 0; i < 48; i++) /* pre-key: unos */
            put_bit(out, 1, &ph, depth);
        for (int i = 0; i < nc; i++)
            put_char(out, chars[i], &ph, depth);
        for (int i = 0; i < 24; i++)
            put_bit(out, 1, &ph, depth);
        for (int i = 0; i < 9600; i++)
            iqbuf_push(out, (cf32){(float)(0.9 / (1.0 + depth)), 0});
    }
    return 0;
}

const modulator_desc_t MOD_ACARS = {
    .id = "acars",
    .name = "ACARS",
    .air_ok = false,
    .modulate = acars_modulate,
};
