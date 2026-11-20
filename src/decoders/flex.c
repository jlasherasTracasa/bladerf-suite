/*
 * flex.c — Decodificador FLEX (1600 / 3200 / 6400 bit/s, 2 y 4 niveles).
 * Ver docs/protocolos/flex.md y la cabecera flex.h (fuentes públicas y qué se
 * ha contrastado con grabaciones reales).
 *
 * Cadena: IQ 48 kHz → FIR de canal → discriminador FM → dos receptores con
 * reloj propio (1600 Bd, sps = 30, y 3200 Bd, sps = 15; pocsag_fsk.h).
 *
 * Máquina de estados:
 *   SEARCH   el receptor de 1600 Bd (Sync 1 y FIW van siempre a 1600 Bd, 2
 *            niveles) mantiene un registro de 64 bits y busca la estructura
 *            [A:16][0xA6C6:16][0xAAAA:16][~A:16] con ≤ 3 errores por campo, en
 *            ambas polaridades. A (0x870C, 0xB068, 0x7B18, 0xDEA0…) da el modo.
 *   FIW      16 bits de puntos y 32 bits de FIW: BCH(31,21) + paridad + suma
 *            de nibbles = 0xF + ciclo ≤ 14. Da ciclo y nº de trama.
 *   DATA     se acumulan los símbolos blandos a la velocidad de símbolo del
 *            modo (receptor de 1600 o de 3200 Bd): 25 ms de Sync 2 + 1760 ms de
 *            datos, con ±FLEX_SYNC2_SLACK símbolos de margen.
 *   decode   se busca el desplazamiento de Sync 2 (±24 símbolos alrededor de
 *            los 25 ms nominales) en el que los bloques 0 de las fases tengan
 *            más palabras BCH válidas; se extrae cada fase (flex.h: reparto de
 *            bits por símbolo), se desentrelaza y se recorren BIW, direcciones
 *            y vectores.
 *
 * Bloque de una fase = 8 palabras de 32 bits entrelazadas: el bit j de la
 * palabra w está en la posición 8j + w. La palabra 0 del bloque 0 es la BIW
 * (información de bloque): bits 8-9 = nº de palabras BIW adicionales
 * (aoffset), bits 10-15 = índice de la primera palabra de vectores (voffset).
 * Las direcciones ocupan las palabras 1+aoffset … voffset−1 y a cada una le
 * corresponde el vector de la palabra voffset+i (una dirección larga ocupa dos
 * palabras en ambos campos).
 * Vector (21 bits): bits 0-3 suma de nibbles, 4-6 tipo (1 instrucción corta,
 * 2 tono, 3/4/7 numérico, 5 alfanumérico, 6 binario), 7-13 primera palabra del
 * mensaje; 14-20 nº de palabras (alfa) o bits 14-16 nº de palabras − 1 (numérico).
 * Con dirección larga el vector ocupa dos palabras y la segunda es la primera palabra
 * del mensaje (cabecera en alfa), que cuenta en la longitud.
 * Alfanumérico: la primera palabra del mensaje es una cabecera (bits 10-12:
 * continuación y fragmento); la segunda empieza con 7 bits de firma si el
 * mensaje es un fragmento único (bits 11-12 = 3) y los caracteres de 7 bits van
 * LSB primero (2 en esa palabra, 3 en las demás).
 * Numérico: nibbles BCD LSB primero tras 2 bits de cabecera (10 en el tipo 7),
 * tabla "0123456789 U -][" y el nibble 0xC es relleno.
 * Tono: si los bits 7-8 del vector son 0, lleva 3 dígitos en los bits 9, 13, 17.
 */
#include "decoder.h"
#include "flex.h"
#include "pocsag_fsk.h"

#include "../util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ganancia del lazo de reloj (Gardner): alta en la búsqueda de Sync 1 para engancharse con
 * los 32 bits de puntos de BS1 (entre tramas el lazo vaga con ruido), baja al seguir la trama. */
#define KP_ACQ 0.10
#define KP_TRACK 0.03

#define MAX_RECENT 32
#define RECENT_TTL 600.0
#define TEXT_MAX 400
/* 5632 símbolos de datos a 3200 Bd + Sync 2 de 80 + holgura a cada lado */
#define MAX_SYMS (5632 + 80 + 2 * FLEX_SYNC2_SLACK + 8)

typedef enum { ST_SEARCH = 0, ST_FIW, ST_DATA } fstate_t;

typedef struct {
    int64_t cap;
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
    uint64_t sr;
    int nsr, mode;
    bool inv;
    uint8_t fiw[32];
    int nfiw, cycle, frame;
    float *buf;
    int bufn, need;
    float hp_hold, ln_hold; /* seguidores de pico congelados durante los datos de 4 niveles */
    int64_t samples, cur;          /* muestras de llamadas anteriores / índice de la actual */
    int64_t f_smp0, f_smp1;        /* posición de la trama en curso (índice absoluto) */
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

static void emit_msg(flex_t *d, char phase, int64_t cap, const char *kind, const char *msg, int corrected, bool long_addr)
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
    sb_printf(s, "{\"capcode\":%lld,\"phase\":\"%c\",\"cycle\":%d,\"frame\":%d,\"kind\":\"%s\",\"msg\":", (long long)cap,
              phase, d->cycle, d->frame, kind);
    sb_json_str(s, msg);
    sb_printf(s, ",\"mode\":\"%s\",\"baud\":%d,\"levels\":%d,\"long\":%s,\"corrected\":%d,\"text\":", m->name, m->bps,
              m->levels, long_addr ? "true" : "false", corrected);
    char line[TEXT_MAX + 100];
    if (!strcmp(kind, "tone") && !msg[0])
        snprintf(line, sizeof line, "FLEX %s capcode %lld fase %c tono", m->name, (long long)cap, phase);
    else
        snprintf(line, sizeof line, "FLEX %s capcode %lld fase %c %s: %s", m->name, (long long)cap, phase, kind, msg);
    sb_json_str(s, line);
    sb_puts(s, "}");
    dec_emit_span(d->emit, d->emit_ctx, d->f_smp0 - d->samples, d->f_smp1 - d->samples, s->data, s->len);
}

/* ------------------------------------------------------------------------- */
/*  Decodificación de la trama                                               */
/* ------------------------------------------------------------------------- */
/* Bit c de la fase de letra L (0 = A … 3 = D) a partir de los símbolos y[]
 * (desplazados s símbolos): signo para A y C, nivel interno para B y D. */
static inline uint8_t phase_bit(const float *y, const flex_mode_t *m, bool inv, float thr, int L, int c, int s)
{
    float v = y[s + flex_sym_index(m, L, c)];
    if (inv)
        v = -v;
    /* v > 0 es el bit "canónico" 1 de la sincronía; el dato es su complemento */
    if (m->levels == 2 || !(L & 1))
        return v < 0;
    return fabsf(v) < thr;
}

static void phase_bits(const float *y, const flex_mode_t *m, bool inv, float thr, int L, int s, int nbits, uint8_t *pb)
{
    for (int c = 0; c < nbits; c++)
        pb[c] = phase_bit(y, m, inv, thr, L, c, s);
}

/* Palabra w del bloque b de una fase a partir de los bits de esa fase. */
static int get_word(const flex_t *d, const uint8_t *pb, int b, int w, uint32_t *d21)
{
    uint8_t bits[32];
    for (int j = 0; j < 32; j++)
        bits[j] = pb[b * 256 + j * 8 + w];
    return flex_word_fix(&d->bch, bits, d21);
}

/* Texto alfanumérico (ver cabecera): w[0] es la cabecera del mensaje; en un
 * mensaje de fragmento único los 7 primeros bits de w[1] son una firma. */
static void render_alpha(const uint32_t *w, int n, char *out, size_t cap)
{
    size_t k = 0;
    int frag = n > 0 ? (int)((w[0] >> 11) & 3) : 3;
    for (int i = 1; i < n; i++) {
        for (int c = (i == 1 && frag == 3) ? 1 : 0; c < 3; c++) {
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

static void render_numeric(const uint32_t *w, int n, int skip, char *out, size_t cap)
{
    static const char dig[] = "0123456789 U -][";
    size_t k = 0;
    int nib = 0, acc = 0, sk = skip;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 21; b++) {
            if (sk > 0) {
                sk--;
                continue;
            }
            acc |= (int)((w[i] >> b) & 1) << nib;
            if (++nib == 4) {
                if (acc != 0xC && k + 1 < cap) /* 0xC = relleno */
                    out[k++] = dig[acc];
                nib = acc = 0;
            }
        }
    out[k] = 0;
    while (k > 0 && out[k - 1] == ' ')
        out[--k] = 0;
}

static void decode_phase(flex_t *d, int L, const uint8_t *pb)
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
    char ph = (char)('A' + L);
    for (int ia = 1 + aoff; ia < voff;) {
        int iv = voff + (ia - (1 + aoff));
        if (iv >= FLEX_WORDS_PER_PHASE)
            break;
        if (!ok[ia] || W[ia] == 0 || W[ia] == 0x1FFFFF) { /* relleno */
            ia++;
            continue;
        }
        uint32_t a = W[ia];
        bool lng = false;
        int64_t cap;
        int corr = er[ia] + (ok[iv] ? er[iv] : 0), step = 1;
        if (flex_addr_is_short(a)) {
            cap = (int64_t)a - 0x8000;
        } else if (flex_addr_is_long1(a) && ia + 1 < voff && ok[ia + 1] && flex_addr_is_long2(W[ia + 1]) &&
                   iv + 1 < FLEX_WORDS_PER_PHASE && ok[iv + 1]) {
            lng = true;
            step = 2;
            cap = flex_long_capcode(a, W[ia + 1]);
            corr += er[ia + 1] + er[iv + 1];
            d->n_long++;
        } else {
            ia++;
            continue;
        }
        int adv = step;
        if (!ok[iv] || flex_nib_sum(W[iv]) != 0xF) {
            ia += adv;
            continue;
        }
        uint32_t v = W[iv];
        int type = (int)((v >> 4) & 7), mw1 = (int)((v >> 7) & 0x7F);
        char msg[TEXT_MAX] = "";
        if (type == 2) { /* tono (o 3 dígitos cortos) */
            if (((v >> 7) & 3) == 0) {
                static const char dig[] = "0123456789 U -][";
                int k = 0;
                for (int s = 9; s <= 17; s += 4)
                    msg[k++] = dig[(v >> s) & 0xF];
                if (lng && ok[iv + 1]) /* 5 dígitos más en la segunda palabra del vector */
                    for (int sft = 0; sft <= 16; sft += 4)
                        msg[k++] = dig[(W[iv + 1] >> sft) & 0xF];
                msg[k] = 0;
                while (k > 0 && msg[k - 1] == ' ')
                    msg[--k] = 0;
            }
            emit_msg(d, ph, cap, "tone", msg, corr, lng);
        } else if (type == 3 || type == 4 || type == 7 || type == 5) {
            int len = type == 5 ? (int)((v >> 14) & 0x7F) : (int)((v >> 14) & 7) + 1;
            uint32_t mw[FLEX_WORDS_PER_PHASE];
            int nmw = 0;
            bool good = true;
            if (lng) { /* dirección larga: el 2º vector es la 1ª palabra del mensaje (la cabecera en alfa) */
                mw[nmw++] = W[iv + 1];
                len--;
            }
            if (len > 0 && (mw1 < voff || mw1 + len > FLEX_WORDS_PER_PHASE))
                good = false;
            for (int k = 0; good && k < len; k++) {
                if (!ok[mw1 + k])
                    good = false;
                else {
                    mw[nmw++] = W[mw1 + k];
                    corr += er[mw1 + k];
                }
            }
            if (good && nmw > 0) {
                if (type == 5)
                    render_alpha(mw, nmw, msg, sizeof msg);
                else
                    render_numeric(mw, nmw, type == 7 ? 10 : 2, msg, sizeof msg);
                emit_msg(d, ph, cap, type == 5 ? "alpha" : "numeric", msg, corr, lng);
            }
        }
        ia += adv;
    }
}

/* Palabras válidas (BCH + paridad) de las 88 de una fase. */
static int count_valid(const flex_t *d, const uint8_t *pb)
{
    int ok = 0;
    for (int b = 0; b < FLEX_BLOCKS; b++)
        for (int w = 0; w < 8; w++) {
            uint32_t x;
            ok += get_word(d, pb, b, w, &x) >= 0;
        }
    return ok;
}

/* Final de Sync 2 observado en grabaciones reales de 6400 bit/s (sigidwiki; en 3200/2 se
 * supone igual, sin grabación que lo confirme):
 * los 8 símbolos anteriores a los datos tienen estos signos en el dominio
 * canónico (v > 0 = bit 1 de la sincronía). Sirve para resolver la paridad de
 * símbolo: a 3200 Bd las fases A y C (B y D) alternan y un símbolo de desfase las
 * intercambia sin invalidar ninguna palabra BCH. */
static const int8_t SYNC2_TAIL[8] = {1, 1, 1, -1, 1, -1, -1, -1};

static int tail_score(const float *y, int s, bool inv)
{
    int agree = 0;
    if (s < 8)
        return 0;
    for (int i = 0; i < 8; i++) {
        float v = inv ? -y[s - 8 + i] : y[s - 8 + i];
        agree += (v > 0) == (SYNC2_TAIL[i] > 0);
    }
    return agree;
}

static void decode_frame(flex_t *d)
{
    const flex_mode_t *m = &FLEX_MODES[d->mode];
    int nsym = flex_data_syms(m), sync2 = flex_sync2_syms(m);
    uint8_t *pb = malloc(FLEX_BITS_PER_PHASE);
    if (!pb)
        return;
    int best = -1, bests = 0;
    int s0 = sync2 - FLEX_SYNC2_SLACK;
    if (s0 < 0)
        s0 = 0;
    for (int s = s0; s <= sync2 + FLEX_SYNC2_SLACK; s++) {
        if (s + nsym > d->bufn)
            break;
        /* Puntuación solo con las fases de signo (A, C): cada palabra válida del
         * bloque 0 suma 10 − errores; una fase cuenta si tiene ≥ 6 de 8 válidas.
         * Las fases B y D (nivel interno) dependen de un umbral y, si están en
         * reposo, dan palabras válidas con cualquier desfase. Desempates: el final
         * de Sync 2 conocido (4 niveles, 3200 Bd) y la cercanía al desfase nominal. */
        int score = 0;
        for (int p = 0; p < m->phases; p++) {
            if (m->levels == 4 && (m->ph[p] & 1))
                continue;
            phase_bits(d->buf, m, d->inv, 0.0f, m->ph[p], s, 256, pb);
            int sc = 0, good = 0;
            for (int w = 0; w < 8; w++) {
                uint32_t x;
                int e = get_word(d, pb, 0, w, &x);
                if (e >= 0) {
                    sc += 10 - e;
                    good++;
                }
            }
            if (good >= 6)
                score += sc * 100;
        }
        if (score == 0)
            continue;
        if (m->baud == 3200)
            score += tail_score(d->buf, s, d->inv) * 10;
        score -= abs(s - sync2);
        if (score > best) {
            best = score;
            bests = s;
        }
    }
    /* posición: el fin de la trama es el último símbolo de datos */
    if (best <= 0)
        bests = sync2;
    d->f_smp1 = d->cur - (int64_t)((d->bufn - (bests + nsym)) * d->rate / m->baud);
    if (best <= 0) {
        d->n_bad_frames++;
    } else {
        d->n_frames++;
        d->last_mode = d->mode;
        for (int p = 0; p < m->phases; p++) {
            int L = m->ph[p];
            if (m->levels == 4 && (L & 1)) {
                /* Fase de nivel interno: con audio real el nivel interno no cae en 1/3 del
                 * externo (se midieron 0,3-0,6), así que el umbral se elige por datos: el
                 * que deja más palabras BCH válidas. */
                static const float cand[] = {0.667f, 0.55f, 0.45f, 0.8f, 0.35f, 0.25f};
                float bt = cand[0];
                int bv = -1;
                for (unsigned q = 0; q < sizeof cand / sizeof *cand; q++) {
                    phase_bits(d->buf, m, d->inv, cand[q], L, bests, FLEX_BITS_PER_PHASE, pb);
                    int v = count_valid(d, pb);
                    /* Las palabras de relleno son válidas con casi cualquier umbral: solo se
                     * abandona el teórico (2/3) si otro gana con claridad. */
                    if (q == 0 ? 1 : v >= bv + 4) {
                        bv = v;
                        bt = cand[q];
                    }
                    if (q == 0 && v >= FLEX_WORDS_PER_PHASE - 2)
                        break;
                }
                phase_bits(d->buf, m, d->inv, bt, L, bests, FLEX_BITS_PER_PHASE, pb);
            } else {
                phase_bits(d->buf, m, d->inv, 0.0f, L, bests, FLEX_BITS_PER_PHASE, pb);
            }
            decode_phase(d, L, pb);
        }
    }
    free(pb);
}

/* ------------------------------------------------------------------------- */
/*  Máquina de estados                                                       */
/* ------------------------------------------------------------------------- */
static void start_data(flex_t *d)
{
    const flex_mode_t *m = &FLEX_MODES[d->mode];
    d->need = flex_sync2_syms(m) + FLEX_SYNC2_SLACK + flex_data_syms(m);
    d->bufn = 0;
    d->st = ST_DATA;
}

static void data_sym(flex_t *d, float y)
{
    d->buf[d->bufn++] = y;
    /* En 4 niveles los seguidores de pico del receptor (caída lenta hacia el centro)
     * encogen la amplitud estimada cuando hay cientos de símbolos de nivel interno
     * seguidos (palabras de relleno) y entonces el nivel interno se normaliza a "externo".
     * Los picos medidos al final de Sync 2 (puntos de nivel externo) pasan a ser un suelo: la
     * amplitud puede crecer pero no encoger durante los datos. */
    const flex_mode_t *m = &FLEX_MODES[d->mode];
    if (m->levels == 4) {
        fskch_t *c = m->baud == 1600 ? &d->c16 : &d->c32;
        int cap = flex_sync2_syms(m) - 4;
        if (d->bufn == cap) {
            d->hp_hold = c->hp;
            d->ln_hold = c->ln;
        } else if (d->bufn > cap) {
            if (c->hp < d->hp_hold)
                c->hp = d->hp_hold;
            if (c->ln > d->ln_hold)
                c->ln = d->ln_hold;
            c->ctr = 0.5f * (c->hp + c->ln);
            c->amp = fmaxf(0.5f * (c->hp - c->ln), 0.02f);
        }
    }
    if (d->bufn >= d->need) {
        decode_frame(d);
        d->c16.kp = KP_ACQ;
        d->st = ST_SEARCH;
        d->nsr = 0;
        d->sr = 0;
    }
}

/* ¿Casa el registro de 64 bits con [A][0xA6C6][0xAAAA][~A]? Devuelve la palabra A
 * (16 bits) o −1. Tolerancia: < 4 errores en el marcador de 32 bits y en la
 * relación A/~A, como [M]. */
static int sync_match(uint64_t r)
{
    uint32_t marker = (uint32_t)((r >> 16) & 0xFFFFFFFFu);
    uint16_t hi = (uint16_t)(r >> 48), lo = (uint16_t)~(r & 0xFFFF);
    if (bits_popcount32(marker ^ (((uint32_t)FLEX_SYNC_B << 16) | FLEX_SYNC_C)) < 4 && bits_popcount32((uint32_t)(hi ^ lo)) < 4)
        return hi;
    return -1;
}

static int mode_of(int code)
{
    int best = -1, bd = 4;
    for (int m = 0; m < FLEX_NMODES; m++) {
        int dd = bits_popcount32((uint32_t)(code ^ FLEX_MODES[m].code));
        if (dd < bd) {
            bd = dd;
            best = m;
        }
    }
    return best;
}

static void sym16(flex_t *d, float y)
{
    int bit = y > 0;
    switch (d->st) {
    case ST_SEARCH:
        d->sr = (d->sr << 1) | (uint64_t)bit;
        if (d->nsr < 64)
            d->nsr++;
        if (d->nsr >= 64) {
            int code = sync_match(d->sr), pol = 0;
            if (code < 0) {
                code = sync_match(~d->sr);
                pol = 1;
            }
            int m = code >= 0 ? mode_of(code) : -1;
            if (m >= 0) {
                d->mode = m;
                d->inv = pol;
                d->c16.kp = KP_TRACK;
                d->st = ST_FIW;
                d->nfiw = -16; /* 16 bits de puntos antes del FIW */
                /* BS1 (32) + A/A6C6/AAAA/~A (64) = 96 símbolos de 1600 Bd */
                d->f_smp0 = d->cur - (int64_t)(96.0 * d->rate / 1600.0);
                if (d->f_smp0 < 0)
                    d->f_smp0 = 0;
            }
        }
        break;
    case ST_FIW:
        if (d->nfiw >= 0)
            d->fiw[d->nfiw] = (uint8_t)(bit ^ d->inv ^ 1); /* dato = complemento del bit de sincronía */
        if (++d->nfiw == 32) {
            uint32_t w;
            int e = flex_word_fix(&d->bch, d->fiw, &w);
            if (e >= 0 && flex_nib_sum(w) == 0xF && ((w >> 4) & 0xF) <= 14) {
                d->cycle = (int)((w >> 4) & 0xF);
                d->frame = (int)((w >> 8) & 0x7F);
                start_data(d);
            } else {
                d->n_bad_frames++;
                d->st = ST_SEARCH;
                d->nsr = 0;
                d->sr = 0;
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
    d->c16.kp = KP_ACQ;
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
        d->cur = d->samples + k;
        float y;
        if (fskch_push(&d->c16, d->fm[k], &y))
            sym16(d, y);
        if (fskch_push(&d->c32, d->fm[k], &y))
            sym32(d, y);
    }
    d->samples += n;
}

static void flex_state(void *st, sb_t *sb, double now)
{
    flex_t *d = st;
    sb_printf(sb, "{\"frames\":%lu,\"bad_frames\":%lu,\"msgs\":%lu,\"long_addr\":%lu,\"mode\":", d->n_frames,
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
        sb_printf(sb, "{\"capcode\":%lld,\"phase\":\"%c\",\"kind\":\"%s\",\"mode\":\"%s\",\"count\":%d,\"seen\":%.1f,\"msg\":",
                  (long long)r->cap, r->phase, r->kind, r->mode, r->count, fmax(0.0, now - r->t));
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
