/*
 * mod_pocsag.c — Modulador POCSAG (ITU-R M.584) a 48 ksps en banda base.
 *
 * Construye: preámbulo de 576 bits alternos, lotes (sincronía 0x7CD215D8 + 16
 * palabras) con direcciones colocadas en la trama RIC&7, mensajes numéricos
 * (BCD de 4 bits, LSB primero) o alfanuméricos (ASCII de 7 bits, LSB primero,
 * terminado con EOT), relleno con la palabra de reposo 0x7A89C197. Un mensaje
 * puede desbordar un lote: la sincronía del siguiente se intercala.
 * Cada palabra: 21 bits + BCH(31,21) + paridad par (bch_encode_u32).
 *
 * Modulación: FSK con desviación ±4,5 kHz y transiciones suavizadas con un
 * filtro de un polo (τ = 0,15 bit) para parecerse a un transmisor real; el
 * bit 1 es la desviación negativa. El decodificador detecta la polaridad.
 *
 * Parámetros JSON (todos opcionales):
 *   {"baud":1200, "invert":false,
 *    "messages":[{"ric":1234567,"func":3,"text":"HOLA MUNDO"},
 *                {"ric":42,"func":0,"text":"0123-45","numeric":true},
 *                {"ric":99,"func":1}]}              (sin texto = solo tono)
 * o un único mensaje con "ric", "func", "text", "numeric" en la raíz.
 *
 * air_ok = false (AGENTS.md §6: las bandas de buscapersonas no están en la
 * lista blanca).
 */
#include "mod.h"

#include "../decoders/pocsag.h"
#include "../dsp/bits.h"
#include "../dsp/fec.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define MAX_WORDS 4096

typedef struct {
    uint32_t ric;
    int func;
    bool numeric;
    char text[600];
    bool has_text;
} pmsg_t;

static uint32_t make_word(const bch_t *b, uint32_t data21)
{
    uint32_t c31 = bch_encode_u32(b, data21, 21);
    return (c31 << 1) | (uint32_t)bits_parity32(c31);
}

/* Bits del cuerpo del mensaje (antes de repartirlos en palabras de 20 bits). */
static int body_bits(const pmsg_t *m, uint8_t *bits, int cap)
{
    int n = 0;
    if (m->numeric) {
        static const char dig[] = "0123456789*U -][";
        for (const char *p = m->text; *p && n + 4 <= cap; p++) {
            int v = 12; /* espacio si no se reconoce */
            for (int i = 0; i < 16; i++)
                if (dig[i] == *p)
                    v = i;
            for (int k = 0; k < 4; k++)
                bits[n++] = (uint8_t)((v >> k) & 1);
        }
        while (n % 20) { /* relleno con espacios */
            for (int k = 0; k < 4; k++)
                bits[n++] = (uint8_t)((12 >> k) & 1);
        }
    } else {
        for (const char *p = m->text; *p && n + 7 <= cap; p++)
            for (int k = 0; k < 7; k++)
                bits[n++] = (uint8_t)((*p >> k) & 1);
        for (int k = 0; k < 7; k++)
            bits[n++] = (uint8_t)((0x04 >> k) & 1); /* EOT */
        while (n % 20)
            bits[n++] = 0;
    }
    return n;
}

static int build_words(const bch_t *b, const pmsg_t *msgs, int nmsg, uint32_t *words)
{
    int gi = 0; /* índice global de palabra de datos (sin contar sincronías) */
    for (int i = 0; i < MAX_WORDS; i++)
        words[i] = POCSAG_IDLE;
    for (int m = 0; m < nmsg; m++) {
        int frame = (int)(msgs[m].ric & 7);
        /* siguiente posición ≥ gi cuya trama sea 'frame' */
        while ((gi % 16) / 2 != frame)
            gi++;
        if (gi + 2 >= MAX_WORDS)
            return -1;
        words[gi++] = make_word(b, (((msgs[m].ric >> 3) & 0x3FFFFu) << 2) | (uint32_t)(msgs[m].func & 3));
        if (msgs[m].has_text) {
            uint8_t bits[4200];
            int nb = body_bits(&msgs[m], bits, 4000);
            for (int o = 0; o < nb; o += 20) {
                uint32_t d = 0;
                for (int k = 0; k < 20; k++)
                    d = (d << 1) | bits[o + k];
                if (gi >= MAX_WORDS)
                    return -1;
                words[gi++] = make_word(b, (1u << 20) | d);
            }
        }
    }
    return gi;
}

static int get_msg(const char *js, const jtok_t *t, int obj, pmsg_t *m)
{
    double v;
    int k;
    memset(m, 0, sizeof *m);
    if ((k = json_obj_get(js, t, obj, "ric")) >= 0 && json_num(js, t, k, &v))
        m->ric = (uint32_t)v & 0x1FFFFFu;
    if ((k = json_obj_get(js, t, obj, "func")) >= 0 && json_num(js, t, k, &v))
        m->func = (int)v & 3;
    if ((k = json_obj_get(js, t, obj, "text")) >= 0 && json_str(js, t, k, m->text, sizeof m->text))
        m->has_text = true;
    bool nb;
    m->numeric = false;
    if ((k = json_obj_get(js, t, obj, "numeric")) >= 0 && json_bool(js, t, k, &nb))
        m->numeric = nb;
    else
        m->numeric = m->has_text && m->func == 0;
    return 0;
}

static int pocsag_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    pmsg_t msgs[16];
    int nmsg = 0, baud = 1200;
    bool invert = false;
    jtok_t t[256];
    int n = 0;
    const char *js = params ? params : "{}";
    n = json_parse(js, strlen(js), t, 256);
    if (n < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "parámetros JSON no válidos");
        return -1;
    }
    double v;
    int k;
    if ((k = json_obj_get(js, t, 0, "baud")) >= 0 && json_num(js, t, k, &v))
        baud = (int)v;
    if (baud != 512 && baud != 1200 && baud != 2400) {
        snprintf(err, errlen, "baud debe ser 512, 1200 o 2400");
        return -1;
    }
    bool bv;
    if ((k = json_obj_get(js, t, 0, "invert")) >= 0 && json_bool(js, t, k, &bv))
        invert = bv;
    int arr = json_obj_get(js, t, 0, "messages");
    if (arr >= 0 && t[arr].type == J_ARR) {
        for (int i = 0; i < t[arr].size && nmsg < 16; i++) {
            int e = json_arr_get(t, arr, i);
            if (e >= 0 && t[e].type == J_OBJ)
                get_msg(js, t, e, &msgs[nmsg++]);
        }
    } else if (json_obj_get(js, t, 0, "ric") >= 0) {
        get_msg(js, t, 0, &msgs[nmsg++]);
    } else { /* ejemplo: alfanumérico + numérico + tono */
        memset(msgs, 0, sizeof msgs);
        msgs[0] = (pmsg_t){.ric = 1234567, .func = 3, .has_text = true};
        snprintf(msgs[0].text, sizeof msgs[0].text, "BLADERF LAB: PRUEBA POCSAG 1234567890");
        msgs[1] = (pmsg_t){.ric = 2001002, .func = 0, .numeric = true, .has_text = true};
        snprintf(msgs[1].text, sizeof msgs[1].text, "0123456789 -[]");
        msgs[2] = (pmsg_t){.ric = 424242, .func = 1};
        nmsg = 3;
    }
    if (nmsg == 0) {
        snprintf(err, errlen, "sin mensajes");
        return -1;
    }

    bch_t b;
    bch_init_pocsag(&b);
    uint32_t *words = malloc(sizeof(uint32_t) * MAX_WORDS);
    int nw = words ? build_words(&b, msgs, nmsg, words) : -1;
    if (nw < 0) {
        free(words);
        snprintf(err, errlen, "mensajes demasiado largos");
        return -1;
    }
    int nbatch = (nw + 15) / 16;

    /* Cadena de bits: preámbulo, y por lote: sincronía + 16 palabras */
    int total = POCSAG_PREAMBLE_BITS + nbatch * 17 * 32;
    uint8_t *bits = malloc((size_t)total);
    int nb = 0;
    for (int i = 0; i < POCSAG_PREAMBLE_BITS; i++)
        bits[nb++] = (uint8_t)((i & 1) ^ 1); /* 1010… */
    for (int bt = 0; bt < nbatch; bt++) {
        for (int i = 31; i >= 0; i--)
            bits[nb++] = (uint8_t)((POCSAG_SYNC >> i) & 1);
        for (int w = 0; w < 16; w++)
            for (int i = 31; i >= 0; i--)
                bits[nb++] = (uint8_t)((words[bt * 16 + w] >> i) & 1);
    }

    out->rate = FS;
    double sps = FS / baud, ph = 0, f = 0, alpha = 1.0 - exp(-1.0 / (0.15 * sps));
    int lead = (int)(0.05 * FS), tail = (int)(0.15 * FS);
    iqbuf_reserve(out, lead + tail + (int)(nb * sps) + 16);
    for (int i = 0; i < lead; i++)
        iqbuf_push(out, (cf32){0.8f, 0});
    int ns = (int)(nb * sps);
    for (int i = 0; i < ns; i++) {
        int bit = bits[(int)(i / sps)] ^ invert;
        double target = bit ? -POCSAG_DEV_HZ : POCSAG_DEV_HZ;
        f += alpha * (target - f);
        ph += 2 * M_PI * f / FS;
        if (ph > M_PI)
            ph -= 2 * M_PI;
        else if (ph < -M_PI)
            ph += 2 * M_PI;
        iqbuf_push(out, (cf32){(float)(0.8 * cos(ph)), (float)(0.8 * sin(ph))});
    }
    for (int i = 0; i < tail; i++) {
        f *= 0.9;
        ph += 2 * M_PI * f / FS;
        iqbuf_push(out, (cf32){(float)(0.8 * cos(ph)), (float)(0.8 * sin(ph))});
    }
    free(bits);
    free(words);
    return 0;
}

const modulator_desc_t MOD_POCSAG = {
    .id = "pocsag",
    .name = "POCSAG",
    .air_ok = false,
    .modulate = pocsag_modulate,
};
