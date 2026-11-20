/*
 * flex.c — Decodificador FLEX (1600 / 3200 / 6400 bit/s, 2 y 4 niveles).
 * Ver docs/protocolos/flex.md y la advertencia de flex.h: la especificación de
 * FLEX no es pública y varias constantes están SIN CONFIRMAR.
 *
 * Cadena: IQ 48 kHz → FIR de canal → discriminador FM → dos receptores con
 * reloj propio (1600 Bd, sps = 30, y 3200 Bd, sps = 15; pocsag_fsk.h).
 *
 * Máquina de estados:
 *   SEARCH   el receptor de 1600 Bd (Sync 1 y FIW van siempre a 1600 Bd, 2
 *            niveles) busca la palabra A (código de 16 bits + complemento, ≤ 3
 *            errores, en ambas polaridades). El código dice el modo.
 *   FIW      siguientes 32 bits: BCH(31,21) + paridad + suma de nibbles = 0xF
 *            + ciclo ≤ 14. Da ciclo y nº de trama.
 *   DATA     se acumulan los símbolos blandos a la velocidad de datos (del
 *            receptor de 1600 o de 3200 Bd según el modo): 11 bloques por fase
 *            más FLEX_SYNC2_MAX símbolos de margen para el Sync 2.
 *   decode   se busca el desplazamiento del Sync 2 (0..96 símbolos) en el que
 *            el primer bloque de la fase A tenga ≥ 6 de 8 palabras BCH válidas.
 *            Los bits globales se reparten entre fases por g mod nfases (para 4
 *            niveles cada símbolo da 2 bits: MSB primero).
 *
 * Bloque de una fase = 8 palabras de 32 bits entrelazadas: el bit j de la
 * palabra w está en la posición 8j + w. La palabra 0 del bloque 0 es la BIW
 * (información de bloque): bits 8-9 = nº de palabras BIW adicionales
 * (aoffset), bits 10-15 = índice de la primera palabra de vectores (voffset).
 * Las direcciones ocupan las palabras 1+aoffset … voffset−1 y a cada una le
 * corresponde el vector de la palabra voffset+i. Dirección corta: 0x8001 ≤ w ≤
 * 0x1E0000, capcode = w − 0x8000 (las largas se cuentan y se ignoran).
 * Vector (21 bits): bits 0-3 suma de nibbles, 4-6 tipo (2 tono, 3/4 numérico,
 * 5 alfanumérico, 6 binario), 7-13 primera palabra del mensaje, 14-20 nº de
 * palabras. [Todo SIN CONFIRMAR frente a la norma; ver docs.]
 * Alfanumérico: la primera palabra del mensaje lleva 7 bits de cabecera y 2
 * caracteres de 7 bits; las demás, 3 caracteres (bits 0-6, 7-13, 14-20).
 * Numérico: nibbles BCD LSB primero tras 10 bits de cabecera.
 */
#include "decoder.h"
#include "flex.h"
#include "pocsag_fsk.h"

#include "../util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RECENT 32
#define RECENT_TTL 600.0
#define TEXT_MAX 400
#define MAX_SYMS (FLEX_WORDS_PER_PHASE * 32 * 4 / 2 + FLEX_SYNC2_MAX + 8) /* cota holgada */

typedef enum { ST_SEARCH = 0, ST_FIW, ST_DATA } fstate_t;

typedef struct {
    uint32_t cap;
    char phase, kind[8];
    int cycle, frame;
    char msg[TEXT_MAX];
    const char *mode;
    double t;
    int count;
} recent_t;

typedef struct {
    fskfe_t fe;
    fskch_t c16, c32;
    bch_t bch;
    double rate, now;
    fstate_t st;
    uint32_t sr;
    int nsr, mode;
    bool inv;
    uint8_t fiw[32];
    int nfiw, cycle, frame;
    float *buf;
    int bufn, need;
    unsigned long n_frames, n_bad_frames, n_msgs, n_long;
    recent_t rec[MAX_RECENT];
    int nrec;
    int last_mode;
    sb_t sb;
    dec_emit_fn emit;
    void *emit_ctx;
    float *fm;
    int fm_cap;
} flex_t;

/* ------------------------------------------------------------------------- */
static void recent_add(flex_t *d, const recent_t *n)
{
    for (int i = 0; i < d->nrec; i++)
        if (d->rec[i].cap == n->cap && d->rec[i].phase == n->phase && !strcmp(d->rec[i].msg, n->msg) &&
            d->rec[i].cycle == n->cycle && d->rec[i].frame == n->frame) {
            d->rec[i].count++;
            d->rec[i].t = n->t;
            return;
        }
    int idx = d->nrec;
    if (idx == MAX_RECENT) {
        idx = 0;
        for (int i = 1; i < d->nrec; i++)
            if (d->rec[i].t < d->rec[idx].t)
                idx = i;
    } else
        d->nrec++;
    d->rec[idx] = *n;
    d->rec[idx].count = 1;
}

static void emit_msg(flex_t *d, char phase, uint32_t cap, const char *kind, const char *msg, int corrected)
{
    const flex_mode_t *m = &FLEX_MODES[d->mode];
    d->n_msgs++;
    recent_t r = {.cap = cap, .phase = phase, .cycle = d->cycle, .frame = d->frame, .mode = m->name, .t = d->now};
    snprintf(r.kind, sizeof r.kind, "%s", kind);
    snprintf(r.msg, sizeof r.msg, "%s", msg);
    recent_add(d, &r);
    if (!d->emit)
        return;
    sb_t *s = &d->sb;
    sb_clear(s);
    sb_printf(s, "{\"capcode\":%u,\"phase\":\"%c\",\"cycle\":%d,\"frame\":%d,\"kind\":\"%s\",\"msg\":", cap, phase,
              d->cycle, d->frame, kind);
    sb_json_str(s, msg);
    sb_printf(s, ",\"mode\":\"%s\",\"baud\":%d,\"levels\":%d,\"corrected\":%d,\"text\":", m->name, m->baud * (m->levels / 2),
              m->levels, corrected);
    char line[TEXT_MAX + 100];
    if (!strcmp(kind, "tone"))
        snprintf(line, sizeof line, "FLEX %s capcode %u fase %c tono", m->name, cap, phase);
    else
        snprintf(line, sizeof line, "FLEX %s capcode %u fase %c %s: %s", m->name, cap, phase, kind, msg);
    sb_json_str(s, line);
    sb_puts(s, "}");
    d->emit(d->emit_ctx, s->data, s->len);
}

/* ------------------------------------------------------------------------- */
/*  Decodificación de la trama                                               */
/* ------------------------------------------------------------------------- */
/* Símbolo blando → bits globales (2 por símbolo de 4 niveles, MSB primero). */
static int syms_to_bits(const float *y, int nsym, int levels, bool inv, uint8_t *g, int cap)
{
    int n = 0;
    for (int i = 0; i < nsym; i++) {
        float v = inv ? -y[i] : y[i];
        if (levels == 2) {
            if (n >= cap)
                break;
            g[n++] = v > 0;
        } else {
            if (n + 2 > cap)
                break;
            /* Gray: +3 → 01, +1 → 00, −1 → 10, −3 → 11 */
            g[n++] = v < 0;                       /* b1 */
            g[n++] = fabsf(v) > (2.0f / 3.0f);    /* b0 */
        }
    }
    return n;
}

/* Palabra w del bloque b de una fase a partir de los bits de esa fase. */
static int get_word(const flex_t *d, const uint8_t *pb, int b, int w, uint32_t *d21)
{
    uint8_t bits[32];
    for (int j = 0; j < 32; j++)
        bits[j] = pb[b * 256 + j * 8 + w];
    return flex_word_fix(&d->bch, bits, d21);
}

static void phase_bits(const uint8_t *g, int ng, int phases, int p, uint8_t *pb, int cap)
{
    int k = 0;
    for (int i = p; i < ng && k < cap; i += phases)
        pb[k++] = g[i];
}

/* Texto alfanumérico: 3 caracteres de 7 bits por palabra, LSB primero.
 * Estructura contrastada con tráfico real (P2000, 1600/2 y 3200/4):
 *  - Mensajes largos: la primera palabra es una cabecera pequeña (bits 14-20 a 0:
 *    fragmento en 11-12, continuación en 10) y el texto empieza en la palabra
 *    siguiente.
 *  - En ambos casos los primeros 7 bits de la primera palabra de datos son
 *    cabecera (no ASCII) y el texto empieza en el segundo carácter.
 * (Medido con multimon-ng como referencia: coincide en todos los mensajes.) */
static void render_alpha(const uint32_t *w, int n, char *out, size_t cap)
{
    size_t k = 0;
    int i0 = (n > 1 && w[0] < (1u << 13)) ? 1 : 0;
    for (int i = i0; i < n; i++) {
        int first = i == i0;
        for (int c = first ? 1 : 0; c < 3; c++) {
            int ch = (int)((w[i] >> (7 * c)) & 0x7F);
            if (ch == 0x03)
                goto done;
            if (ch == 0)
                continue;
            if (k + 1 < cap)
                out[k++] = ch < 0x20 ? ' ' : (char)ch;
        }
    }
done:
    out[k] = 0;
    while (k > 0 && out[k - 1] == ' ')
        out[--k] = 0;
}

static void render_numeric(const uint32_t *w, int n, char *out, size_t cap)
{
    static const char dig[] = "0123456789 U -][";
    size_t k = 0;
    int nib = 0, acc = 0;
    for (int i = 0; i < n; i++)
        for (int b = (i == 0 ? 10 : 0); b < 21; b++) {
            acc |= (int)((w[i] >> b) & 1) << nib;
            if (++nib == 4) {
                if (k + 1 < cap)
                    out[k++] = dig[acc];
                nib = acc = 0;
            }
        }
    out[k] = 0;
    while (k > 0 && out[k - 1] == ' ')
        out[--k] = 0;
}

static void decode_phase(flex_t *d, int p, const uint8_t *pb)
{
    uint32_t W[FLEX_WORDS_PER_PHASE];
    uint8_t ok[FLEX_WORDS_PER_PHASE], er[FLEX_WORDS_PER_PHASE];
    for (int b = 0; b < FLEX_BLOCKS; b++)
        for (int w = 0; w < 8; w++) {
            int e = get_word(d, pb, b, w, &W[8 * b + w]);
            ok[8 * b + w] = e >= 0;
            er[8 * b + w] = (uint8_t)(e > 0 ? e : 0);
        }
    if (!ok[0])
        return;
    int aoff = (int)((W[0] >> 8) & 3), voff = (int)((W[0] >> 10) & 0x3F);
    if (voff < 1 + aoff || voff > FLEX_WORDS_PER_PHASE)
        return;
    int naddr = voff - (1 + aoff);
    for (int i = 0; i < naddr && voff + i < FLEX_WORDS_PER_PHASE; i++) {
        int ia = 1 + aoff + i, iv = voff + i;
        if (!ok[ia] || !ok[iv])
            continue;
        uint32_t a = W[ia], v = W[iv];
        if (a < 0x8001 || a > 0x1E0000) {
            d->n_long++;
            continue;
        }
        if (flex_nib_sum(v) != 0xF)
            continue;
        int type = (int)((v >> 4) & 7), mw1 = (int)((v >> 7) & 0x7F), len = (int)((v >> 14) & 0x7F);
        uint32_t cap = a - 0x8000;
        int corr = er[ia] + er[iv];
        char msg[TEXT_MAX] = "";
        char ph = (char)('A' + p);
        if (type == 2) {
            emit_msg(d, ph, cap, "tone", "", corr);
        } else if (type == 3 || type == 4 || type == 5) {
            if (len < 1 || mw1 < voff || mw1 + len > FLEX_WORDS_PER_PHASE)
                continue;
            bool good = true;
            uint32_t mw[FLEX_WORDS_PER_PHASE];
            for (int k = 0; k < len; k++) {
                if (!ok[mw1 + k]) {
                    good = false;
                    break;
                }
                mw[k] = W[mw1 + k];
                corr += er[mw1 + k];
            }
            if (!good)
                continue;
            if (type == 5)
                render_alpha(mw, len, msg, sizeof msg);
            else
                render_numeric(mw, len, msg, sizeof msg);
            emit_msg(d, ph, cap, type == 5 ? "alpha" : "numeric", msg, corr);
        }
    }
}

static void decode_frame(flex_t *d)
{
    const flex_mode_t *m = &FLEX_MODES[d->mode];
    int ph = m->phases;
    int nbits_all = 11 * 256 * ph;
    uint8_t *g = malloc((size_t)nbits_all + 8);
    uint8_t *pb = malloc(11 * 256);
    if (!g || !pb) {
        free(g);
        free(pb);
        return;
    }
    int best = -1, bests = 0;
    for (int s = 0; s <= FLEX_SYNC2_MAX; s++) {
        int ng = syms_to_bits(d->buf + s, d->bufn - s, m->levels, d->inv, g, 256 * ph);
        if (ng < 256 * ph)
            break;
        phase_bits(g, ng, ph, 0, pb, 256);
        /* puntuación: cada palabra válida suma 10 − errores corregidos. Con
         * solo "válida" un desplazamiento de 8 símbolos (una palabra entera
         * corrida, casi un desplazamiento cíclico del BCH) empata con el bueno. */
        int score = 0, good = 0;
        for (int w = 0; w < 8; w++) {
            uint32_t x;
            int e = get_word(d, pb, 0, w, &x);
            if (e >= 0) {
                score += 10 - e;
                good++;
            }
        }
        if (good < 6)
            score = 0;
        if (score > best) {
            best = score;
            bests = s;
        }
    }
    if (best <= 0) {
        d->n_bad_frames++;
    } else {
        int ng = syms_to_bits(d->buf + bests, d->bufn - bests, m->levels, d->inv, g, nbits_all);
        d->n_frames++;
        d->last_mode = d->mode;
        for (int p = 0; p < ph; p++) {
            phase_bits(g, ng, ph, p, pb, 11 * 256);
            decode_phase(d, p, pb);
        }
    }
    free(g);
    free(pb);
}

/* ------------------------------------------------------------------------- */
/*  Máquina de estados                                                       */
/* ------------------------------------------------------------------------- */
static void start_data(flex_t *d)
{
    const flex_mode_t *m = &FLEX_MODES[d->mode];
    int nbits = 11 * 256 * m->phases;
    d->need = nbits / (m->levels == 4 ? 2 : 1) + FLEX_SYNC2_MAX;
    d->bufn = 0;
    d->st = ST_DATA;
}

static void data_sym(flex_t *d, float y)
{
    d->buf[d->bufn++] = y;
    if (d->bufn >= d->need) {
        decode_frame(d);
        d->st = ST_SEARCH;
        d->nsr = 0;
        d->sr = 0;
    }
}

static void sym16(flex_t *d, float y)
{
    int bit = y > 0;
    switch (d->st) {
    case ST_SEARCH:
        d->sr = (d->sr << 1) | (uint32_t)bit;
        if (d->nsr < 32)
            d->nsr++;
        if (d->nsr >= 32)
            for (int m = 0; m < 4; m++) {
                uint32_t ref = ((uint32_t)FLEX_MODES[m].code << 16) | FLEX_SYNC_B; /* real: código + 0xA6C6 */
                int e0 = bits_popcount32(d->sr ^ ref), e1 = bits_popcount32(d->sr ^ ~ref);
                if (e0 <= 3 || e1 <= 3) {
                    d->mode = m;
                    d->inv = e1 < e0;
                    d->st = ST_FIW;
                    d->nfiw = 0;
                    break;
                }
            }
        break;
    case ST_FIW:
        d->fiw[d->nfiw++] = (uint8_t)(bit ^ d->inv);
        d->sr = (d->sr << 1) | (uint32_t)bit; /* se sigue buscando por si era la 1ª copia */
        if (d->nfiw == 32) {
            uint32_t w;
            int e = flex_word_fix(&d->bch, d->fiw, &w);
            if (e >= 0 && flex_nib_sum(w) == 0xF && ((w >> 4) & 0xF) <= 14) {
                d->cycle = (int)((w >> 4) & 0xF);
                d->frame = (int)((w >> 8) & 0x7F);
                start_data(d);
            } else {
                /* La palabra A se repite: [~A][puntos][A]. Con la polaridad normal la 1ª copia
                 * (~A) casa como "polaridad invertida" y el FIW que le sigue es basura; la 2ª
                 * copia llega 16 bits después, así que se conserva el registro y se sigue. */
                d->n_bad_frames++;
                d->st = ST_SEARCH;
                d->nsr = 32;
            }
        }
        break;
    case ST_DATA:
        if (FLEX_MODES[d->mode].baud == 1600)
            data_sym(d, y);
        break;
    }
}

static void sym32(flex_t *d, float y)
{
    if (d->st == ST_DATA && FLEX_MODES[d->mode].baud == 3200)
        data_sym(d, y);
}

/* ------------------------------------------------------------------------- */
static void *flex_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    flex_t *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    d->rate = cfg->rate;
    d->buf = malloc(sizeof(float) * MAX_SYMS);
    if (!d->buf || bch_init_pocsag(&d->bch) < 0 || fskch_init(&d->c16, cfg->rate / 1600.0, NULL, 0, 0.03) < 0 ||
        fskch_init(&d->c32, cfg->rate / 3200.0, NULL, 0, 0.03) < 0) {
        snprintf(err, errlen, "sin memoria o BCH no inicializado");
        free(d->buf);
        free(d);
        return NULL;
    }
    fskfe_init(&d->fe, cfg->rate, 10500.0, 4800.0);
    d->last_mode = -1;
    return d;
}

static void flex_destroy(void *st)
{
    flex_t *d = st;
    if (!d)
        return;
    fskch_free(&d->c16);
    fskch_free(&d->c32);
    sb_free(&d->sb);
    free(d->buf);
    free(d->fm);
    free(d);
}

static void flex_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    flex_t *d = st;
    d->emit = emit;
    d->emit_ctx = ctx;
    if (n > d->fm_cap) {
        d->fm_cap = n;
        d->fm = realloc(d->fm, sizeof(float) * (size_t)n);
        if (!d->fm)
            return;
    }
    fskfe_process(&d->fe, iq, n, d->fm);
    for (int k = 0; k < n; k++) {
        d->now = now + (double)k / d->rate;
        float y;
        if (fskch_push(&d->c16, d->fm[k], &y))
            sym16(d, y);
        if (fskch_push(&d->c32, d->fm[k], &y))
            sym32(d, y);
    }
}

static void flex_state(void *st, sb_t *sb, double now)
{
    flex_t *d = st;
    sb_printf(sb, "{\"frames\":%lu,\"bad_frames\":%lu,\"msgs\":%lu,\"long_addr_skipped\":%lu,\"mode\":", d->n_frames,
              d->n_bad_frames, d->n_msgs, d->n_long);
    if (d->last_mode >= 0)
        sb_printf(sb, "\"%s\",\"cycle\":%d,\"frame\":%d", FLEX_MODES[d->last_mode].name, d->cycle, d->frame);
    else
        sb_puts(sb, "null");
    sb_puts(sb, ",\"pages\":[");
    int w = 0;
    for (int i = 0; i < d->nrec; i++)
        if (now - d->rec[i].t <= RECENT_TTL)
            d->rec[w++] = d->rec[i];
    d->nrec = w;
    for (int i = 0; i < d->nrec; i++) {
        const recent_t *r = &d->rec[d->nrec - 1 - i];
        if (i)
            sb_puts(sb, ",");
        sb_printf(sb, "{\"capcode\":%u,\"phase\":\"%c\",\"kind\":\"%s\",\"mode\":\"%s\",\"count\":%d,\"seen\":%.1f,\"msg\":",
                  r->cap, r->phase, r->kind, r->mode, r->count, fmax(0.0, now - r->t));
        sb_json_str(sb, r->msg);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_FLEX = {
    .id = "flex",
    .name = "FLEX",
    .group = "Buscapersonas",
    .desc = "Buscapersonas FLEX 1600/3200/6400 (2 y 4 niveles, BCH).",
    .kind = DEC_NARROW,
    .rate = 48000,
    .bw_hz = 12500,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {466075000LL, 0LL, 0LL, 0LL},
    .create = flex_create,
    .destroy = flex_destroy,
    .process = flex_process,
    .state_json = flex_state,
};
