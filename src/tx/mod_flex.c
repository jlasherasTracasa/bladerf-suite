/*
 * mod_flex.c — Modulador FLEX a 48 ksps (banda base compleja).
 *
 * Genera UNA trama FLEX con la estructura de decoders/flex.h (Sync 1 = dotting
 * + palabra A, FIW, Sync 2 = 16 símbolos de dotting a la velocidad de datos, y
 * 11 bloques por fase con entrelazado 8×32). Las constantes no confirmadas se
 * listan en flex.h y docs/protocolos/flex.md; el decodificador y este modulador
 * comparten esa interpretación, así que el test sintético NO valida la
 * concordancia con transmisores reales.
 *
 * Modulación: FSK con filtro de un polo (τ = 0,15 símbolo); 2 niveles ±4800 Hz;
 * 4 niveles ±1600 y ±4800 Hz (Gray: 01 → +3, 00 → +1, 10 → −1, 11 → −3).
 *
 * Parámetros JSON (opcionales):
 *   {"mode":"1600/2"|"3200/2"|"3200/4"|"6400/4", "cycle":3, "frame":42,
 *    "messages":[{"capcode":123456,"phase":"A","type":"alpha","text":"HOLA"},
 *                {"capcode":777,"type":"numeric","text":"0123-45"},
 *                {"capcode":9,"type":"tone"}]}
 * capcode: dirección corta 1..1933312. "phase" por omisión A.
 *
 * air_ok = false.
 */
#include "mod.h"

#include "../decoders/flex.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define MAXMSG 24

typedef struct {
    uint32_t cap;
    int phase;
    int type; /* 2 tono, 3 numérico, 5 alfanumérico */
    char text[300];
} fmsg_t;

static uint32_t fix_nibble(uint32_t d)
{
    d &= ~0xFu;
    return d | ((0xFu - flex_nib_sum(d)) & 0xF);
}

/* Palabras de mensaje de un texto; devuelve cuántas. */
static int msg_words(const fmsg_t *m, uint32_t *w)
{
    int n = 0;
    if (m->type == 5) {
        uint8_t ch[400];
        int nc = 0;
        for (const char *p = m->text; *p && nc < 380; p++)
            ch[nc++] = (uint8_t)*p;
        ch[nc++] = 0x03; /* ETX */
        int i = 0;
        w[n++] = ((uint32_t)ch[i] << 7) | ((uint32_t)(i + 1 < nc ? ch[i + 1] : 0) << 14); /* cabecera 0 */
        i = 2;
        while (i < nc) {
            uint32_t v = 0;
            for (int c = 0; c < 3; c++)
                v |= (uint32_t)(i + c < nc ? ch[i + c] : 0) << (7 * c);
            w[n++] = v;
            i += 3;
        }
    } else if (m->type == 3) {
        static const char dig[] = "0123456789 U -][";
        uint8_t bits[1200];
        int nb = 0;
        for (const char *p = m->text; *p && nb < 1000; p++) {
            int v = 10;
            for (int i = 0; i < 16; i++)
                if (dig[i] == *p)
                    v = i;
            for (int k = 0; k < 4; k++)
                bits[nb++] = (uint8_t)((v >> k) & 1);
        }
        /* relleno con espacios (0xA) hasta completar palabras: 11 bits en la
         * primera y 21 en las siguientes */
        while (nb < 11 || (nb - 11) % 21)
            for (int q = 0; q < 4; q++)
                bits[nb++] = (uint8_t)((10 >> q) & 1);
        int k = 0;
        w[n] = 0;
        for (int b = 10; b < 21 && k < nb; b++)
            w[n] |= (uint32_t)bits[k++] << b;
        n++;
        while (k < nb) {
            uint32_t v = 0;
            for (int b = 0; b < 21 && k < nb; b++)
                v |= (uint32_t)bits[k++] << b;
            w[n++] = v;
        }
    }
    return n;
}

static int build_phase(const fmsg_t *msgs, int nmsg, int phase, uint32_t *W)
{
    int idx[MAXMSG], n = 0;
    for (int i = 0; i < nmsg; i++)
        if (msgs[i].phase == phase)
            idx[n++] = i;
    memset(W, 0, sizeof(uint32_t) * FLEX_WORDS_PER_PHASE);
    if (n == 0)
        return 0;
    int voff = 1 + n, next = 1 + 2 * n;
    W[0] = (uint32_t)voff << 10;
    for (int k = 0; k < n; k++) {
        const fmsg_t *m = &msgs[idx[k]];
        uint32_t mw[200];
        int nw = m->type == 2 ? 0 : msg_words(m, mw);
        if (next + nw > FLEX_WORDS_PER_PHASE)
            return -1;
        W[1 + k] = m->cap + 0x8000;
        uint32_t v = ((uint32_t)m->type << 4) | ((uint32_t)(nw ? next : 0) << 7) | ((uint32_t)nw << 14);
        W[voff + k] = fix_nibble(v);
        for (int i = 0; i < nw; i++)
            W[next++] = mw[i];
    }
    return n;
}

static int get_msg(const char *js, const jtok_t *t, int o, fmsg_t *m)
{
    double v;
    char s[32];
    int k;
    memset(m, 0, sizeof *m);
    m->type = 5;
    if ((k = json_obj_get(js, t, o, "capcode")) >= 0 && json_num(js, t, k, &v))
        m->cap = (uint32_t)v;
    if (m->cap < 1 || m->cap > 1933312)
        return -1;
    if ((k = json_obj_get(js, t, o, "phase")) >= 0 && json_str(js, t, k, s, sizeof s) && s[0] >= 'A' && s[0] <= 'D')
        m->phase = s[0] - 'A';
    if ((k = json_obj_get(js, t, o, "type")) >= 0 && json_str(js, t, k, s, sizeof s))
        m->type = !strcmp(s, "tone") ? 2 : !strcmp(s, "numeric") ? 3 : 5;
    if ((k = json_obj_get(js, t, o, "text")) >= 0)
        json_str(js, t, k, m->text, sizeof m->text);
    return 0;
}

/* Añade nsym símbolos FM a 'sps' muestras/símbolo; levels[i] ∈ {±1, ±3}. */
typedef struct {
    double ph, f;
} fmst_t;

static void emit_syms(iqbuf_t *o, fmst_t *s, const int8_t *lv, int n, int sps)
{
    double alpha = 1.0 - exp(-1.0 / (0.15 * sps));
    iqbuf_reserve(o, n * sps);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < sps; k++) {
            s->f += alpha * (lv[i] * 1600.0 - s->f);
            s->ph += 2 * M_PI * s->f / FS;
            if (s->ph > M_PI)
                s->ph -= 2 * M_PI;
            else if (s->ph < -M_PI)
                s->ph += 2 * M_PI;
            iqbuf_push(o, (cf32){(float)(0.8 * cos(s->ph)), (float)(0.8 * sin(s->ph))});
        }
}

static int flex_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    fmsg_t msgs[MAXMSG];
    int nmsg = 0, mode = 0, cycle = 3, frame = 42;
    const char *js = params ? params : "{}";
    jtok_t t[512];
    int nt = json_parse(js, strlen(js), t, 512);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "parámetros JSON no válidos");
        return -1;
    }
    double v;
    char s[32];
    int k;
    if ((k = json_obj_get(js, t, 0, "mode")) >= 0 && json_str(js, t, k, s, sizeof s)) {
        mode = -1;
        for (int i = 0; i < 4; i++)
            if (!strcmp(s, FLEX_MODES[i].name))
                mode = i;
        if (mode < 0) {
            snprintf(err, errlen, "modo debe ser 1600/2, 3200/2, 3200/4 o 6400/4");
            return -1;
        }
    }
    if ((k = json_obj_get(js, t, 0, "cycle")) >= 0 && json_num(js, t, k, &v))
        cycle = (int)v & 0xF;
    if (cycle > 14)
        cycle = 14;
    if ((k = json_obj_get(js, t, 0, "frame")) >= 0 && json_num(js, t, k, &v))
        frame = (int)v & 0x7F;
    int arr = json_obj_get(js, t, 0, "messages");
    if (arr >= 0 && t[arr].type == J_ARR) {
        for (int i = 0; i < t[arr].size && nmsg < MAXMSG; i++) {
            int e = json_arr_get(t, arr, i);
            if (e < 0 || t[e].type != J_OBJ || get_msg(js, t, e, &msgs[nmsg]) < 0) {
                snprintf(err, errlen, "mensaje %d no válido (capcode 1..1933312)", i);
                return -1;
            }
            nmsg++;
        }
    } else {
        memset(msgs, 0, sizeof msgs);
        msgs[0] = (fmsg_t){.cap = 123456, .phase = 0, .type = 5};
        snprintf(msgs[0].text, sizeof msgs[0].text, "BLADERF LAB: PRUEBA FLEX 0123456789");
        msgs[1] = (fmsg_t){.cap = 1600000, .phase = 0, .type = 3};
        snprintf(msgs[1].text, sizeof msgs[1].text, "0123456789 -[]");
        msgs[2] = (fmsg_t){.cap = 4242, .phase = 0, .type = 2};
        nmsg = 3;
        if (FLEX_MODES[mode].phases > 1) { /* una segunda fase con otro mensaje */
            msgs[3] = (fmsg_t){.cap = 777777, .phase = 1, .type = 5};
            snprintf(msgs[3].text, sizeof msgs[3].text, "FASE B: SEGUNDO BUSCA");
            nmsg = 4;
        }
        if (FLEX_MODES[mode].phases > 2) {
            msgs[4] = (fmsg_t){.cap = 31337, .phase = 2, .type = 5};
            snprintf(msgs[4].text, sizeof msgs[4].text, "FASE C");
            msgs[5] = (fmsg_t){.cap = 31338, .phase = 3, .type = 5};
            snprintf(msgs[5].text, sizeof msgs[5].text, "FASE D");
            nmsg = 6;
        }
    }
    const flex_mode_t *fm = &FLEX_MODES[mode];
    for (int i = 0; i < nmsg; i++)
        if (msgs[i].phase >= fm->phases) {
            snprintf(err, errlen, "la fase %c no existe en el modo %s", 'A' + msgs[i].phase, fm->name);
            return -1;
        }

    bch_t b;
    bch_init_pocsag(&b);
    /* Bits por fase */
    int ph = fm->phases, nbk = 11 * 256;
    uint8_t *pb = calloc((size_t)ph * nbk, 1);
    for (int p = 0; p < ph; p++) {
        uint32_t W[FLEX_WORDS_PER_PHASE];
        if (build_phase(msgs, nmsg, p, W) < 0) {
            free(pb);
            snprintf(err, errlen, "demasiado texto para una fase (88 palabras)");
            return -1;
        }
        for (int blk = 0; blk < FLEX_BLOCKS; blk++)
            for (int w = 0; w < 8; w++) {
                uint8_t bits[32];
                flex_word_bits(&b, W[8 * blk + w], bits);
                for (int j = 0; j < 32; j++)
                    pb[(size_t)p * nbk + blk * 256 + j * 8 + w] = bits[j];
            }
    }
    /* Flujo global de bits: g = k·fases + p */
    int ng = ph * nbk;
    uint8_t *g = malloc((size_t)ng);
    for (int k2 = 0; k2 < nbk; k2++)
        for (int p = 0; p < ph; p++)
            g[k2 * ph + p] = pb[(size_t)p * nbk + k2];
    free(pb);

    out->rate = FS;
    fmst_t st = {0, 0};
    int lead = (int)(0.05 * FS);
    iqbuf_reserve(out, lead);
    for (int i = 0; i < lead; i++)
        iqbuf_push(out, (cf32){0.8f, 0});

    /* Sync 1 + A + FIW a 1600 Bd, 2 niveles */
    int8_t hdr[96];
    int nh = 0;
    for (int i = 31; i >= 0; i--)
        hdr[nh++] = (FLEX_BS1 >> i) & 1 ? 3 : -3;
    uint32_t aw = ((uint32_t)fm->code << 16) | FLEX_SYNC_B;
    for (int i = 31; i >= 0; i--)
        hdr[nh++] = (aw >> i) & 1 ? 3 : -3;
    emit_syms(out, &st, hdr, nh, 30);
    uint32_t fiw = fix_nibble(((uint32_t)cycle << 4) | ((uint32_t)frame << 8));
    uint8_t fb[32];
    flex_word_bits(&b, fiw, fb);
    int8_t fl[32];
    for (int i = 0; i < 32; i++)
        fl[i] = fb[i] ? 3 : -3;
    emit_syms(out, &st, fl, 32, 30);

    /* Sync 2: dotting a la velocidad de datos */
    int sps = (int)(FS / fm->baud);
    int8_t s2[16];
    for (int i = 0; i < 16; i++)
        s2[i] = (i & 1) ? -3 : 3;
    emit_syms(out, &st, s2, 16, sps);

    /* Datos */
    int nsym = fm->levels == 4 ? ng / 2 : ng;
    int8_t *lv = malloc((size_t)nsym);
    for (int i = 0; i < nsym; i++) {
        if (fm->levels == 2)
            lv[i] = g[i] ? 3 : -3;
        else {
            int b1 = g[2 * i], b0 = g[2 * i + 1];
            lv[i] = (int8_t)(b1 ? (b0 ? -3 : -1) : (b0 ? 3 : 1));
        }
    }
    emit_syms(out, &st, lv, nsym, sps);
    free(lv);
    free(g);

    /* cola: portadora sin modular */
    int tail = (int)(0.2 * FS);
    iqbuf_reserve(out, tail);
    for (int i = 0; i < tail; i++) {
        st.f *= 0.9;
        st.ph += 2 * M_PI * st.f / FS;
        iqbuf_push(out, (cf32){(float)(0.8 * cos(st.ph)), (float)(0.8 * sin(st.ph))});
    }
    return 0;
}

const modulator_desc_t MOD_FLEX = {
    .id = "flex",
    .name = "FLEX",
    .air_ok = false,
    .modulate = flex_modulate,
};
