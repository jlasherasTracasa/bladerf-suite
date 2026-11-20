/*
 * mod_flex.c — Modulador FLEX a 48 ksps (banda base compleja).
 *
 * Genera UNA trama FLEX con la estructura de decoders/flex.h:
 *   Sync 1 (1600 Bd, 2 niveles) = BS1 (32 bits 1010…) + A + 0xA6C6 + 0xAAAA + ~A
 *     + 16 bits de puntos; FIW; Sync 2 = 25 ms de puntos a la velocidad de símbolo
 *   de los datos; y 11 bloques por fase con entrelazado 8×32. La palabra A
 *   (0x870C, 0x7B18, 0xB068, 0xDEA0) fija el modo. Las constantes salen de la
 *   tesis de McCulley (1997) y de grabaciones reales (ver docs/protocolos/flex.md).
 *
 * Modulación: FSK con filtro de un polo (τ = 0,15 símbolo); 2 niveles ±4800 Hz;
 * 4 niveles ±1600 y ±4800 Hz, Gray de −4,8 a +4,8 kHz: 00, 01, 11, 10 (primer bit
 * = signo, fases A y C; segundo bit = nivel interno, fases B y D).
 *
 * Parámetros JSON (opcionales):
 *   {"mode":"1600/2"|"3200/2"|"3200/4"|"6400/4", "cycle":3, "frame":42,
 *    "messages":[{"capcode":123456,"phase":"A","type":"alpha","text":"HOLA"},
 *                {"capcode":777,"type":"numeric","text":"0123-45"},
 *                {"capcode":9,"type":"tone"}]}
 * capcode: dirección corta 1..1933312; hasta 4297068542 con dirección larga (2 palabras). "phase" por omisión A. Fases por modo:
 * 1600/2 A · 3200/2 A,C · 3200/4 A,B · 6400/4 A,B,C,D. Un "tone" con "text" de
 * hasta 3 dígitos es el "tono con dígitos" del vector de 21 bits.
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
    int64_t cap;
    int phase;
    int type; /* 2 tono, 3 numérico, 5 alfanumérico */
    char text[300];
} fmsg_t;

static uint32_t fix_nibble(uint32_t d)
{
    d &= ~0xFu;
    return d | ((0xFu - flex_nib_sum(d)) & 0xF);
}

/* Palabras de mensaje de un texto; devuelve cuántas.
 * Alfa: palabra de cabecera (fragmento único: bits 11-12 = 3) y a continuación
 * 7 bits de firma + 2 caracteres, y 3 caracteres por palabra. Numérico: 2 bits de
 * cabecera y nibbles BCD LSB primero, relleno 0xC. */
static int msg_words(const fmsg_t *m, uint32_t *w)
{
    int n = 0;
    if (m->type == 5) {
        uint8_t ch[400];
        int nc = 0;
        for (const char *p = m->text; *p && nc < 380; p++)
            ch[nc++] = (uint8_t)*p;
        ch[nc++] = 0x03; /* ETX */
        w[n++] = 3u << 11;
        int i = 0;
        w[n++] = ((uint32_t)ch[i] << 7) | ((uint32_t)(i + 1 < nc ? ch[i + 1] : 0) << 14);
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
        int nb = 2; /* cabecera de 2 bits */
        memset(bits, 0, sizeof bits);
        for (const char *p = m->text; *p && nb < 160; p++) {
            int v = 10;
            for (int i = 15; i >= 0; i--) /* el primer índice de cada carácter (el espacio es 10; 12 es relleno) */
                if (dig[i] == *p)
                    v = i;
            for (int k = 0; k < 4; k++)
                bits[nb++] = (uint8_t)((v >> k) & 1);
        }
        for (int pos = 0; nb % 21; nb++, pos = (pos + 1) & 3) /* relleno: nibbles 0xC */
            bits[nb] = (uint8_t)((0xC >> pos) & 1);
        int k = 0;
        while (k < nb) {
            uint32_t v = 0;
            for (int b = 0; b < 21 && k < nb; b++)
                v |= (uint32_t)bits[k++] << b;
            w[n++] = v;
        }
    }
    return n;
}

/* Palabras de dirección de un capcode: corto = 1 palabra (cap + 0x8000); largo (> 1 933 312) = 2
 * palabras (a1, a2) con a1 ∈ [1, 0x8000] y cap = a1 + ((a2 ^ 0x1FFFFF) << 15) + 0x1F9000 (flex.h). */
static int addr_words(int64_t cap, uint32_t *a)
{
    if (cap <= 1933312) {
        a[0] = (uint32_t)(cap + 0x8000);
        return 1;
    }
    int64_t r = cap - 0x1F9000;
    uint32_t a1 = (uint32_t)((r - 1) % 32768) + 1;
    a[0] = a1;
    a[1] = (uint32_t)(((r - a1) / 32768) ^ 0x1FFFFF);
    return 2;
}

static int build_phase(const fmsg_t *msgs, int nmsg, int phase, uint32_t *W)
{
    int idx[MAXMSG], n = 0, naw = 0;
    for (int i = 0; i < nmsg; i++)
        if (msgs[i].phase == phase) {
            idx[n++] = i;
            naw += msgs[i].cap > 1933312 ? 2 : 1;
        }
    memset(W, 0, sizeof(uint32_t) * FLEX_WORDS_PER_PHASE);
    if (n == 0)
        return 0;
    int voff = 1 + naw, next = 1 + 2 * naw, ia = 1;
    if (next > FLEX_WORDS_PER_PHASE)
        return -1;
    W[0] = (uint32_t)voff << 10;
    for (int k = 0; k < n; k++) {
        const fmsg_t *m = &msgs[idx[k]];
        uint32_t mw[200], aw[2];
        int nw = m->type == 2 ? 0 : msg_words(m, mw);
        int na = addr_words(m->cap, aw), lng = na == 2;
        if (next + nw > FLEX_WORDS_PER_PHASE || (m->type == 3 && nw > 8))
            return -1;
        int iv = voff + (ia - 1);
        W[ia] = aw[0];
        if (lng)
            W[ia + 1] = aw[1];
        ia += na;
        /* Con dirección larga el vector son dos palabras y la segunda es la primera del mensaje
         * (cabecera en alfa, primeros datos en numérico): cuenta en la longitud pero no se repite
         * en el área de mensajes. */
        int first = (lng && nw) ? 1 : 0;
        uint32_t v = (uint32_t)m->type << 4;
        if (m->type == 5)
            v |= ((uint32_t)next << 7) | ((uint32_t)nw << 14);
        else if (m->type == 3)
            v |= ((uint32_t)next << 7) | ((uint32_t)(nw - 1) << 14);
        else if (m->text[0]) { /* tono con hasta 3 dígitos (bits 7-8 = 0, dígitos en 9, 13, 17) */
            static const char dig[] = "0123456789 U -][";
            for (int q = 0; q < 3 && m->text[q]; q++) {
                uint32_t dv = 10;
                for (int i = 15; i >= 0; i--)
                    if (dig[i] == m->text[q])
                        dv = (uint32_t)i;
                v |= dv << (9 + 4 * q);
            }
        } else
            v |= 1u << 7; /* tono puro */
        W[iv] = fix_nibble(v);
        if (lng && m->type == 2)
            W[iv + 1] = 0xAAAAA; /* 5 dígitos adicionales de la 2ª palabra: espacios */
        if (first)
            W[iv + 1] = mw[0];
        for (int i = first; i < nw; i++)
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
        m->cap = (int64_t)v;
    if (m->cap < 1 || m->cap > 4297068542LL)
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
        for (int i = FLEX_NMODES_TX - 1; i >= 0; i--)
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
                snprintf(err, errlen, "mensaje %d no válido (capcode 1..4297068542)", i);
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
        static const struct {
            int64_t cap;
            const char *txt;
        } extra[4] = {{0, ""}, {777777, "FASE B: SEGUNDO BUSCA"}, {31337, "FASE C"}, {31338, "FASE D"}};
        for (int p = 1; p < FLEX_MODES[mode].phases; p++) { /* un mensaje en cada fase adicional */
            int L = FLEX_MODES[mode].ph[p];
            msgs[nmsg] = (fmsg_t){.cap = extra[L].cap, .phase = L, .type = 5};
            snprintf(msgs[nmsg].text, sizeof msgs[nmsg].text, "%s", extra[L].txt);
            nmsg++;
        }
    }
    const flex_mode_t *fm = &FLEX_MODES[mode];
    for (int i = 0; i < nmsg; i++) {
        bool in_mode = false;
        for (int p = 0; p < fm->phases; p++)
            in_mode |= fm->ph[p] == msgs[i].phase;
        if (!in_mode) {
            snprintf(err, errlen, "la fase %c no existe en el modo %s", 'A' + msgs[i].phase, fm->name);
            return -1;
        }
    }

    bch_t b;
    bch_init_pocsag(&b);
    /* Bits de cada fase (índice = fase de letra L, aunque solo se usen algunas) */
    int nbk = FLEX_BITS_PER_PHASE;
    int nsym = flex_data_syms(fm);
    uint8_t *pb = calloc((size_t)nbk, 1);
    uint8_t *sa = calloc((size_t)nsym, 1), *sb2 = calloc((size_t)nsym, 1);
    int8_t *lv = malloc((size_t)nsym);
    if (!pb || !sa || !sb2 || !lv) {
        free(pb);
        free(sa);
        free(sb2);
        free(lv);
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    for (int p = 0; p < fm->phases; p++) {
        int L = fm->ph[p];
        uint32_t W[FLEX_WORDS_PER_PHASE];
        if (build_phase(msgs, nmsg, L, W) < 0) {
            free(pb);
            free(sa);
            free(sb2);
            free(lv);
            snprintf(err, errlen, "demasiado texto para una fase (88 palabras)");
            return -1;
        }
        for (int blk = 0; blk < FLEX_BLOCKS; blk++)
            for (int w = 0; w < 8; w++) {
                uint8_t bits[32];
                flex_word_bits(&b, W[8 * blk + w], bits);
                for (int j = 0; j < 32; j++)
                    pb[blk * 256 + j * 8 + w] = bits[j];
            }
        /* reparto en símbolos: signo (A, C) o nivel interno (B, D) */
        for (int c = 0; c < nbk; c++) {
            int k = flex_sym_index(fm, L, c);
            if (L & 1)
                sb2[k] = pb[c];
            else
                sa[k] = pb[c];
        }
    }
    for (int k = 0; k < nsym; k++)
        lv[k] = (int8_t)flex_level_of(fm->levels, sa[k], sb2[k]);
    free(pb);
    free(sa);
    free(sb2);

    out->rate = FS;
    fmst_t st = {0, 0};
    int lead = (int)(0.05 * FS);
    iqbuf_reserve(out, lead);
    for (int i = 0; i < lead; i++)
        iqbuf_push(out, (cf32){0.8f, 0});

    /* Sync 1 (BS1, [A, A6C6], 0xAAAA, [~A, ~A6C6]) + FIW a 1600 Bd, 2 niveles. Los bits de
     * sincronía son los complementarios de los de datos (flex.h): un 1 canónico es −4,8 kHz. */
    int8_t hdr[32 + 64 + 16 + 32];
    int nh = 0;
    uint32_t sync1[3] = {FLEX_BS1, ((uint32_t)fm->code << 16) | FLEX_SYNC_B,
                         ((uint32_t)FLEX_SYNC_C << 16) | (uint16_t)~fm->code};
    for (int wd = 0; wd < 3; wd++)
        for (int i = 31; i >= 0; i--)
            hdr[nh++] = (sync1[wd] >> i) & 1 ? -3 : 3;
    for (int i = 15; i >= 0; i--)
        hdr[nh++] = ((uint16_t)~FLEX_SYNC_B >> i) & 1 ? -3 : 3;
    uint32_t fiw = fix_nibble(((uint32_t)cycle << 4) | ((uint32_t)frame << 8));
    uint8_t fb[32];
    flex_word_bits(&b, fiw, fb);
    for (int i = 0; i < 32; i++)
        hdr[nh++] = fb[i] ? 3 : -3;
    emit_syms(out, &st, hdr, nh, 30);

    /* Sync 2: 25 ms de puntos a la velocidad de símbolo de los datos */
    int sps = (int)(FS / fm->baud), n2 = flex_sync2_syms(fm);
    int8_t *s2 = malloc((size_t)n2);
    if (!s2) {
        free(lv);
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    for (int i = 0; i < n2; i++)
        s2[i] = (i & 1) ? -3 : 3;
    if (fm->baud == 3200) /* final de Sync 2 como en las grabaciones reales (v canónico = −físico) */
        for (int i = 0; i < 8; i++)
            s2[n2 - 8 + i] = (int8_t)(fm->levels == 2 && FLEX_SYNC2_TAIL[i] == 1 ? -3 : -FLEX_SYNC2_TAIL[i]);
    emit_syms(out, &st, s2, n2, sps);
    free(s2);

    /* Datos */
    emit_syms(out, &st, lv, nsym, sps);
    free(lv);

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
