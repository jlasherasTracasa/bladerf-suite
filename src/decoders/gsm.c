/*
 * gsm.c — Sincronización GSM (FCCH, SCH → BSIC y número de trama) y System Information 3/4
 * del BCCH (MCC, MNC, LAC, Cell ID). Ver docs/protocolos/gsm.md.
 *
 * SOLO información de difusión: FCCH, SCH y las 4 ráfagas del BCCH (tramas 2–5 de la
 * multitrama de 51 en la ranura 0). No se decodifica ningún canal de aviso/acceso (PCH,
 * AGCH), ni tráfico, ni se interpreta ninguna identidad de abonado.
 *
 * Teoría:
 *  - GMSK (BT 0,3, h = ½). Con la codificación diferencial de GSM, el desarrollo de Laurent
 *    a un solo término  x(t) ≈ Σ_k a_k·c0(t − kT),  a_k = j^k·b_k,  b_k = 1 − 2d_k,  convierte
 *    la señal en BPSK de los bits d_k girado j^k por símbolo: al multiplicar la muestra
 *    del símbolo k por (−j)^k queda  z_k = Σ_m h_m·b_{k−m} + ruido, donde h reúne el pulso
 *    c0 (≈ 5 símbolos) y el canal. Eso es lo que resuelve el ecualizador (≈ 99 % de la
 *    energía de GMSK está en el primer término de Laurent).
 *  - FCCH: 148 bits a cero = fase que sube π/2 por símbolo = tono puro a +fb/4 =
 *    +67 708 Hz. Detector: z[n] = x[n]·conj(x[n−4]) (un símbolo) y media en 96 símbolos;
 *    M = |Σz|/Σ|z| ≈ 1 en un tono, ≈ 0,1 en GMSK con datos (los saltos ±π/2 se cancelan).
 *    La frecuencia (CFO = f_tono − 67 708 Hz) se mide con el desfase a lag 4 (grueso) y
 *    a lag 64 (fino, ±8 kHz de rango); error de unas decenas de Hz.
 *  - SCH exactamente 1 trama (5000 muestras) después del FCCH. Se busca la posición
 *    exacta (±60 muestras = ±15 símbolos) con el mínimo error de estimación de canal por
 *    mínimos cuadrados (5 coeficientes) sobre la secuencia de entrenamiento de 64 bits,
 *    se ecualiza con MLSE de 16 estados en versión max-log-MAP (salida blanda), y se
 *    decodifica 78 bits con Viterbi K=5 r=½ (conv_gsm_k5) + paridad de 10 bits → BSIC (NCC,
 *    BCC) y T1/T2/T3' → FN (45.002 §4.3.3).
 *  - BCCH: si FN mod 51 = 1 los 4 bloques siguientes (tramas 2–5 = SCH + 5000·k) forman un
 *    mensaje: TSC = BCC; se demodula cada ráfaga igual, se desentrelazan 456 bits
 *    (45.003 §4.1.4), Viterbi a 224 bits y el código Fire de 40 bits valida. SI3 (tipo
 *    0x1B) lleva Cell Identity y LAI (MCC/MNC/LAC); SI4 (0x1C) solo el LAI.
 *  - Un filtro paso bajo de 33 coeficientes (±170 kHz) reduce el ruido de banda ancha.
 *
 *  - Secuencia de entrenamiento del BCCH: la norma (45.002 §5.2.3) exige TSC = BCC, pero la
 *    grabación real de referencia (BSIC 23 → BCC 7) lleva la TSC 6 (residuo de ajuste 100 veces
 *    menor que con la 7). Como no se puede asegurar que toda red cumpla la regla, la primera vez
 *    se prueban las 8 TSC (la BCC primero) sobre la 1.ª ráfaga y se elige la de menor residuo;
 *    la elegida se memoriza cuando el código Fire valida el bloque.
 *  - Los octetos del nivel 3 se transmiten con el LSB primero (45.003 §4.1.1): ver gsm_phy.h.
 *  - System Information (44.018 §9.1.31–9.1.36): SI1 (Cell Channel Description), SI2/2bis/2ter
 *    (lista BA de portadoras vecinas, NCC permitidos), SI3 (Cell Identity, LAI, CCCH, opciones,
 *    selección de celda, RACH) y SI4 (LAI, selección, RACH). Se emite un evento por cada SI
 *    nuevo o distinto del último de su tipo en esa celda (kind "si1"…"si4").
 *
 * Posición (dec_emit_span): sample = 1.er símbolo de la ráfaga (tail bits) del primer bloque de
 * la transmisión (la ráfaga del SCH para "sch"; la B0 del bloque BCCH para los SI); sample_end =
 * último símbolo de la última ráfaga (B3). Los desplazamientos son negativos (el mensaje acaba
 * en el bloque actual pero empezó 4–5 tramas antes) y se corrigen con el retardo del FIR (16).
 *
 * Limitaciones: un solo término de Laurent; canal de ≤ 5 símbolos (sin multitrayecto
 * largo); sin saltos de frecuencia; cada tipo de SI aparece una vez cada 2–8 multitramas
 * (0,5–1,9 s); sin combinación de ráfagas de varias multitramas; las listas de ARFCN solo se
 * decodifican en formato «bit map 0» (124 portadoras GSM900; los rangos 1024/512/256/128 y el
 * mapa variable de DCS 1800 no); los rest octets (GPRS, SI13) no se interpretan; no hay
 * cifrado A5 (el BCCH no se cifra).
 */
#include "decoder.h"

#include "gsm_phy.h"
#include "../dsp/filter.h"
#include "../dsp/fec.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RB 262144           /* anillo de muestras filtradas (242 ms) */
#define RBMASK (RB - 1)
#define LFIR 33
#define LWIN 384            /* 96 símbolos */
#define FCCH_HZ (GSM_FB / 4.0)
#define MAXCELLS 8
#define MAXPEND 8
#define FIR_DELAY ((LFIR - 1) / 2)
#define NSI 6

typedef struct {
    int bsic;
    uint32_t fn;
    int tsc, bad;           /* TSC de la portadora (−1 = por descubrir) y fallos de Fire seguidos */
    int arfcn;              /* −1 si la frecuencia no cae en una banda GSM conocida */
    gsm_si_t si[NSI];       /* último SI de cada tipo (ver si_slot) */
    uint8_t raw[NSI][23];
    bool have[NSI];
    double cfo_hz, level_dbfs, first_seen, last_seen;
    int hits;
} gsm_cell_t;

typedef struct {
    int type;               /* 0 = intento de SCH tras un FCCH, 1 = intento de BCCH */
    int64_t t_ready;
    int64_t start;          /* FCCH: inicio estimado; BCCH: inicio de la ráfaga del SCH */
    double cfo;
    int bsic;
    uint32_t fn;
} pend_t;

typedef struct {
    float llr[152];         /* positivo = b = +1 (d = 0) */
    double sigma2, power;
    int64_t start;          /* posición del símbolo 0 */
    bool ok;
} dburst_t;

typedef struct gsm {
    double fs;
    float taps[LFIR];
    cf32 hist[LFIR - 1];
    cf32 *tmp;
    int tmp_cap;
    cf32 *rb;
    int64_t n;              /* nº de muestras escritas en el anillo */
    /* detector de FCCH */
    cf32 zr[LWIN];
    float zabs[LWIN];
    double sre, sim, sabs;
    int zi;
    bool in_pl;
    int64_t n0, holdoff;
    double now0;            /* hora del bloque actual */
    int64_t blk0;           /* índice absoluto de su primera muestra */
    pend_t pend[MAXPEND];
    int npend;
    gsm_cell_t cells[MAXCELLS];
    int ncells;
    int64_t freq_hz;
    unsigned long n_fcch, n_sch, n_bcch, n_events;
    sb_t evsb;
} gsm_t;

static inline cf32 rbat(const gsm_t *st, int64_t i) { return st->rb[i & RBMASK]; }
static inline float cabs2(cf32 a) { return a.re * a.re + a.im * a.im; }

/* ---------------------------------------------------------------------- */
/*  Demodulación de una ráfaga: temporización, canal LS, MLSE max-log-MAP   */
/* ---------------------------------------------------------------------- */
/* Resuelve A·x = B (n×n real, 2 RHS) por Gauss con pivote parcial. */
static bool solve5(double A[5][5], double inv[5][5])
{
    double M[5][10];
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 10; j++)
            M[i][j] = j < 5 ? A[i][j] + (i == j ? 1e-6 * A[0][0] : 0) : (j - 5 == i ? 1.0 : 0.0);
    for (int c = 0; c < 5; c++) {
        int p = c;
        for (int r = c + 1; r < 5; r++)
            if (fabs(M[r][c]) > fabs(M[p][c]))
                p = r;
        if (fabs(M[p][c]) < 1e-12)
            return false;
        if (p != c)
            for (int j = 0; j < 10; j++) {
                double t = M[c][j];
                M[c][j] = M[p][j];
                M[p][j] = t;
            }
        double d = M[c][c];
        for (int j = 0; j < 10; j++)
            M[c][j] /= d;
        for (int r = 0; r < 5; r++)
            if (r != c) {
                double f = M[r][c];
                for (int j = 0; j < 10; j++)
                    M[r][j] -= f * M[c][j];
            }
    }
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            inv[i][j] = M[i][j + 5];
    return true;
}

static const cf32 ROT[4] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}};   /* (−j)^k */

/* Demodula una ráfaga de 148 símbolos cuyo símbolo 0 está nominalmente en 'nominal'.
 * train: bits de entrenamiento (tlen) en los símbolos tpos..tpos+tlen−1. */
static bool demod_burst(gsm_t *st, int64_t nominal, int search, const uint8_t *train, int tpos, int tlen, double cfo,
                        dburst_t *o)
{
    const int T = 151;
    int64_t base = nominal - search - 16;
    int len = 4 * 152 + 2 * search + 48;
    if (base < st->n - RB + 8 || base + len > st->n)
        return false;
    cf32 *seg = malloc(sizeof(cf32) * (size_t)len);
    if (!seg)
        return false;
    double w = -2.0 * M_PI * cfo / st->fs, cr = 1, ci = 0, dr = cos(w), di = sin(w);
    for (int i = 0; i < len; i++) {
        cf32 v = rbat(st, base + i);
        seg[i] = (cf32){(float)(v.re * cr - v.im * ci), (float)(v.re * ci + v.im * cr)};
        double nr = cr * dr - ci * di, ni = cr * di + ci * dr;
        cr = nr;
        ci = ni;
    }
    /* símbolos conocidos */
    int8_t known[152];
    memset(known, 0, sizeof known);       /* 0 = desconocido, ±1 = valor de b */
    for (int t = 0; t < 3; t++)
        known[t] = 1;
    for (int t = 145; t < 148; t++)
        known[t] = 1;
    for (int i = 0; i < tlen; i++)
        known[tpos + i] = (int8_t)(1 - 2 * train[i]);
    /* matriz normal del entrenamiento */
    double A[5][5], Ai[5][5];
    memset(A, 0, sizeof A);
    for (int t = tpos + 4; t < tpos + tlen; t++)
        for (int j = 0; j < 5; j++)
            for (int l = 0; l < 5; l++)
                A[j][l] += (double)known[t - j] * known[t - l];
    if (!solve5(A, Ai)) {
        free(seg);
        return false;
    }
    double best = -1;
    int bd = 0;
    for (int d = -search; d <= search; d++) {
        double rr[5] = {0}, ri[5] = {0};
        for (int t = tpos + 4; t < tpos + tlen; t++) {
            cf32 z = seg[nominal + d + 4 * (t - 2) - base];
            cf32 q = ROT[(t - 2) & 3];
            double zr_ = z.re * q.re - z.im * q.im, zi_ = z.re * q.im + z.im * q.re;
            for (int j = 0; j < 5; j++) {
                rr[j] += zr_ * known[t - j];
                ri[j] += zi_ * known[t - j];
            }
        }
        double ex = 0;
        for (int j = 0; j < 5; j++)
            for (int l = 0; l < 5; l++)
                ex += Ai[j][l] * (rr[j] * rr[l] + ri[j] * ri[l]);
        if (ex > best) {
            best = ex;
            bd = d;
        }
    }
    /* canal en el mejor retardo */
    cf32 z[152];
    for (int t = 0; t < T; t++) {
        cf32 s = seg[nominal + bd + 4 * (t - 2) - base], q = ROT[(t - 2) & 3];
        z[t] = (cf32){s.re * q.re - s.im * q.im, s.re * q.im + s.im * q.re};
    }
    double rr[5] = {0}, ri[5] = {0}, zz = 0;
    for (int t = tpos + 4; t < tpos + tlen; t++) {
        for (int j = 0; j < 5; j++) {
            rr[j] += z[t].re * known[t - j];
            ri[j] += z[t].im * known[t - j];
        }
        zz += cabs2(z[t]);
    }
    double hr[5], hi[5];
    for (int j = 0; j < 5; j++) {
        hr[j] = hi[j] = 0;
        for (int l = 0; l < 5; l++) {
            hr[j] += Ai[j][l] * rr[l];
            hi[j] += Ai[j][l] * ri[l];
        }
    }
    double expl = 0;
    for (int j = 0; j < 5; j++)
        expl += hr[j] * rr[j] + hi[j] * ri[j];
    int neq = tlen - 4;
    double sigma2 = fmax((zz - expl) / (double)(neq > 5 ? neq - 5 : 1), 1e-6 * zz / neq + 1e-12);
    /* ---- MLSE max-log-MAP de 16 estados ---- */
    float expr[16][2], expi[16][2];
    for (int s = 0; s < 16; s++)
        for (int nb = 0; nb < 2; nb++) {
            double a = 0, b = 0;
            for (int j = 0; j < 5; j++) {
                int bit = j == 0 ? nb : ((s >> (j - 1)) & 1);
                double sg = bit ? 1.0 : -1.0;
                a += hr[j] * sg;
                b += hi[j] * sg;
            }
            expr[s][nb] = (float)a;
            expi[s][nb] = (float)b;
        }
    free(seg);
    {
        float *al = malloc(sizeof(float) * 16 * 153), *be = malloc(sizeof(float) * 16 * 153);
        float *gam = malloc(sizeof(float) * 32 * 152);
        if (!al || !be || !gam) {
            free(al);
            free(be);
            free(gam);
            return false;
        }
        const float NEG = -1e30f;
        for (int t = 0; t < T; t++)
            for (int s = 0; s < 16; s++)
                for (int nb = 0; nb < 2; nb++) {
                    float g = 0;
                    if (t >= 4) {
                        float er = z[t].re - expr[s][nb], ei = z[t].im - expi[s][nb];
                        g = -(er * er + ei * ei) / (float)sigma2;
                    }
                    bool ok = known[t] == 0 || (known[t] > 0) == (nb == 1);
                    gam[(t * 16 + s) * 2 + nb] = ok ? g : NEG;
                }
        for (int s = 0; s < 16; s++)
            al[s] = 0;
        for (int t = 0; t < T; t++) {
            float mx = NEG;
            for (int s2 = 0; s2 < 16; s2++)
                al[(t + 1) * 16 + s2] = NEG;
            for (int s = 0; s < 16; s++)
                for (int nb = 0; nb < 2; nb++) {
                    int s2 = ((s << 1) | nb) & 15;
                    float v = al[t * 16 + s] + gam[(t * 16 + s) * 2 + nb];
                    if (v > al[(t + 1) * 16 + s2])
                        al[(t + 1) * 16 + s2] = v;
                }
            for (int s2 = 0; s2 < 16; s2++)
                mx = fmaxf(mx, al[(t + 1) * 16 + s2]);
            for (int s2 = 0; s2 < 16; s2++)
                al[(t + 1) * 16 + s2] -= mx;
        }
        for (int s = 0; s < 16; s++)
            be[T * 16 + s] = 0;
        for (int t = T - 1; t >= 0; t--) {
            float mx = NEG;
            for (int s = 0; s < 16; s++) {
                float b = NEG;
                for (int nb = 0; nb < 2; nb++) {
                    int s2 = ((s << 1) | nb) & 15;
                    float v = gam[(t * 16 + s) * 2 + nb] + be[(t + 1) * 16 + s2];
                    if (v > b)
                        b = v;
                }
                be[t * 16 + s] = b;
                mx = fmaxf(mx, b);
            }
            for (int s = 0; s < 16; s++)
                be[t * 16 + s] -= mx;
        }
        for (int t = 0; t < T; t++) {
            float m1 = NEG, m0 = NEG;
            for (int s = 0; s < 16; s++)
                for (int nb = 0; nb < 2; nb++) {
                    int s2 = ((s << 1) | nb) & 15;
                    float v = al[t * 16 + s] + gam[(t * 16 + s) * 2 + nb] + be[(t + 1) * 16 + s2];
                    if (nb) {
                        if (v > m1)
                            m1 = v;
                    } else if (v > m0) {
                        m0 = v;
                    }
                }
            float l = m1 - m0;
            o->llr[t] = CLAMP(l, -40.0f, 40.0f);
        }
        free(al);
        free(be);
        free(gam);
    }
    double pw = 0;
    for (int i = 0; i < 592; i++)
        pw += cabs2(rbat(st, nominal + bd + i - 8));
    o->power = pw / 592.0;
    o->sigma2 = sigma2;
    o->start = nominal + bd;
    o->ok = true;
    return true;
}

/* ---------------------------------------------------------------------- */
/*  Tabla de celdas y eventos                                              */
/* ---------------------------------------------------------------------- */
static gsm_cell_t *cell_get(gsm_t *st, int bsic, bool *created, double now)
{
    for (int i = 0; i < st->ncells; i++)
        if (st->cells[i].bsic == bsic) {
            *created = false;
            return &st->cells[i];
        }
    gsm_cell_t *c;
    if (st->ncells == MAXCELLS) {
        int o = 0;
        for (int i = 1; i < st->ncells; i++)
            if (st->cells[i].last_seen < st->cells[o].last_seen)
                o = i;
        c = &st->cells[o];
    } else {
        c = &st->cells[st->ncells++];
    }
    memset(c, 0, sizeof *c);
    c->bsic = bsic;
    c->tsc = -1;
    c->arfcn = -1;
    c->first_seen = now;
    *created = true;
    return c;
}

static int si_slot(int type)
{
    switch (type) {
    case GSM_SI1: return 0;
    case GSM_SI2: return 1;
    case GSM_SI2BIS: return 2;
    case GSM_SI2TER: return 3;
    case GSM_SI3: return 4;
    case GSM_SI4: return 5;
    }
    return -1;
}

static const char *const SI_NAME[NSI] = {"si1", "si2", "si2bis", "si2ter", "si3", "si4"};

static void put_arr(sb_t *sb, const char *key, const uint16_t *a, int n)
{
    sb_printf(sb, ",\"%s\":[", key);
    for (int i = 0; i < n; i++)
        sb_printf(sb, "%s%d", i ? "," : "", a[i]);
    sb_puts(sb, "]");
}

/* Unión de las listas de vecinas (SI2, 2bis, 2ter) sin repetidos, ordenada. */
static int neighbours(const gsm_cell_t *c, uint16_t *out)
{
    bool seen[125] = {false};
    int n = 0;
    for (int k = 1; k <= 3; k++)
        if (c->have[k] && c->si[k].arfcn_ok)
            for (int i = 0; i < c->si[k].n_arfcn; i++)
                seen[c->si[k].arfcn[i]] = true;
    for (int a = 1; a <= 124; a++)
        if (seen[a])
            out[n++] = (uint16_t)a;
    return n;
}

static void cell_text(const gsm_cell_t *c, const char *kind, char *t, size_t tl)
{
    int n = snprintf(t, tl, "GSM BSIC %d-%d", c->bsic >> 3, c->bsic & 7);
    if (c->arfcn >= 0)
        n += snprintf(t + n, tl - (size_t)n, " ARFCN %d", c->arfcn);
    const gsm_si_t *l = c->have[4] ? &c->si[4] : c->have[5] ? &c->si[5] : NULL;
    if (l) {
        n += snprintf(t + n, tl - (size_t)n, ": MCC %03d MNC %0*d LAC %d", l->mcc, l->mnc_digits, l->mnc, l->lac);
        if (c->have[4])
            n += snprintf(t + n, tl - (size_t)n, " CI %d", c->si[4].ci);
    }
    n += snprintf(t + n, tl - (size_t)n, " [%s]", kind);
    if (c->have[1] || c->have[2] || c->have[3]) {
        uint16_t nb[124];
        n += snprintf(t + n, tl - (size_t)n, ", %d vecinas", neighbours(c, nb));
    }
    snprintf(t + n, tl - (size_t)n, ", nivel %.1f dBFS", c->level_dbfs);
}

/* Cuerpo común de eventos y de la tabla de estado. 'ev' = tipo de SI que dispara el evento (−1 = estado). */
static void cell_json(const gsm_cell_t *c, sb_t *sb, const char *kind, int ev, double now)
{
    bool state = !kind;
    sb_puts(sb, "{");
    if (kind) {
        char t[260];
        cell_text(c, kind, t, sizeof t);
        sb_puts(sb, "\"text\":");
        sb_json_str(sb, t);
        sb_printf(sb, ",\"kind\":\"%s\",", kind);
    }
    sb_printf(sb, "\"bsic\":%d,\"ncc\":%d,\"bcc\":%d,\"fn\":%u,", c->bsic, c->bsic >> 3, c->bsic & 7, c->fn);
    sb_puts(sb, "\"cfo_hz\":");
    sb_json_num(sb, c->cfo_hz, 0);
    sb_puts(sb, ",\"level_dbfs\":");
    sb_json_num(sb, c->level_dbfs, 1);
    if (c->arfcn >= 0)
        sb_printf(sb, ",\"arfcn\":%d", c->arfcn);
    if (c->tsc >= 0)
        sb_printf(sb, ",\"tsc\":%d", c->tsc);
    const gsm_si_t *l = c->have[4] ? &c->si[4] : c->have[5] ? &c->si[5] : NULL;
    if (l) {
        sb_printf(sb, ",\"mcc\":%d,\"mnc\":%d,\"mnc_digits\":%d,\"lac\":%d", l->mcc, l->mnc, l->mnc_digits, l->lac);
        sb_printf(sb, ",\"plmn\":\"%03d-%0*d\"", l->mcc, l->mnc_digits, l->mnc);
        if (c->have[4])
            sb_printf(sb, ",\"ci\":%d", c->si[4].ci);
        if (l->sel_ok)
            sb_printf(sb, ",\"hyst_db\":%d,\"rxlev_min_dbm\":%d,\"txpwr_max_cch\":%d,\"neci\":%d", l->hyst_db,
                      l->rxlev_min_dbm, l->txpwr_max_cch, l->neci);
        if (l->rach_ok)
            sb_printf(sb, ",\"cell_bar\":%d,\"max_retrans\":%d,\"reest\":%d,\"ac_barred\":%u", l->cell_bar,
                      l->max_retrans, l->reest_allowed, l->ac_barred);
        if (l->cc_ok)
            sb_printf(sb, ",\"ccch_conf\":%d,\"bs_ag_blks_res\":%d,\"bs_pa_mfrms\":%d,\"t3212_decih\":%d,\"att\":%d,"
                          "\"dtx\":%d,\"rlt\":%d,\"pwrc\":%d",
                      l->ccch_conf, l->bs_ag_blks_res, l->bs_pa_mfrms, l->t3212_decih, l->att, l->dtx,
                      l->radio_link_timeout, l->pwrc);
    }
    if (c->have[1] && c->si[1].ncc_permitted >= 0)
        sb_printf(sb, ",\"ncc_permitted\":%d", c->si[1].ncc_permitted);
    uint16_t nb[124];
    int nn = neighbours(c, nb);
    if (nn || state)
        put_arr(sb, "neighbours", nb, nn);
    if (c->have[0] && c->si[0].arfcn_ok)
        put_arr(sb, "cell_alloc", c->si[0].arfcn, c->si[0].n_arfcn);
    if (ev >= 0) {
        const gsm_si_t *s = &c->si[ev];
        sb_printf(sb, ",\"si_type\":%d", s->type);
        if (s->n_arfcn || (s->arfcn_fmt && !s->arfcn_ok))
            sb_printf(sb, ",\"arfcn_list_ok\":%s", s->arfcn_ok ? "true" : "false");
        if (s->n_arfcn)
            put_arr(sb, "arfcns", s->arfcn, s->n_arfcn);
        if (s->ba_ind >= 0)
            sb_printf(sb, ",\"ba_ind\":%d", s->ba_ind);
    }
    sb_printf(sb, ",\"hits\":%d", c->hits);
    if (state) {
        sb_puts(sb, ",\"si_seen\":[");
        bool f = true;
        for (int i = 0; i < NSI; i++)
            if (c->have[i]) {
                sb_printf(sb, "%s\"%s\"", f ? "" : ",", SI_NAME[i]);
                f = false;
            }
        sb_puts(sb, "],\"age\":");
        sb_json_num(sb, now - c->last_seen, 1);
    }
    sb_puts(sb, "}");
}

/* off0/off1: desplazamientos de entrada respecto al inicio del bloque (dec_emit_span). */
static void emit_cell(gsm_t *st, const gsm_cell_t *c, const char *kind, int ev, double now, int64_t ring0,
                      int64_t ring1, dec_emit_fn emit, void *ctx)
{
    st->evsb.len = 0;
    cell_json(c, &st->evsb, kind, ev, now);
    st->n_events++;
    dec_emit_span(emit, ctx, ring0 - FIR_DELAY - st->blk0, ring1 - FIR_DELAY - st->blk0, st->evsb.data, st->evsb.len);
}

/* ---------------------------------------------------------------------- */
/*  Tareas pendientes: SCH y BCCH                                          */
/* ---------------------------------------------------------------------- */
static void push_pend(gsm_t *st, const pend_t *p)
{
    if (st->npend < MAXPEND)
        st->pend[st->npend++] = *p;
}

/* Instantes de muestra de los símbolos 0 y 147 de una ráfaga cuyo 'start' devuelve demod_burst:
 * el símbolo t se muestrea en start + 4(t−2); el símbolo empieza ≈ 2 muestras antes de su pico. */
#define BURST_FIRST(s) ((s) - 8 - 2)
#define BURST_LAST(s) ((s) + 4 * 145 + 2)

static void do_sch(gsm_t *st, const pend_t *p, double now, dec_emit_fn emit, void *ctx)
{
    dburst_t b;
    if (!demod_burst(st, p->start + GSM_FRAME, 100, GSM_SCH_TRAIN, 42, 64, p->cfo, &b))
        return;
    float soft[78];
    for (int i = 0; i < 78; i++)
        soft[i] = -b.llr[i < 39 ? 3 + i : 106 + (i - 39)];
    int bsic;
    uint32_t fn;
    if (gsm_sch_decode(soft, &bsic, &fn) < 0)
        return;
    st->n_sch++;
    bool created;
    gsm_cell_t *c = cell_get(st, bsic, &created, now);
    c->fn = fn;
    c->cfo_hz = p->cfo;
    c->level_dbfs = 10.0 * log10(b.power + 1e-20);
    c->last_seen = now;
    c->arfcn = st->freq_hz ? gsm_arfcn_from_hz((double)st->freq_hz + p->cfo) : -1;
    c->hits++;
    if (created)
        emit_cell(st, c, "sch", -1, now, BURST_FIRST(b.start), BURST_LAST(b.start), emit, ctx);
    if (fn % 51 == 1) {      /* las 4 tramas siguientes son el BCCH (45.002 §7, tabla 3) */
        pend_t q = {.type = 1, .start = b.start, .cfo = p->cfo, .bsic = bsic, .fn = fn};
        q.t_ready = b.start + 4 * GSM_FRAME + 1300;
        push_pend(st, &q);
    }
}

/* Demodula las 4 ráfagas del bloque con la TSC 'tsc' y rellena soft[456]. 'burst0' (opcional)
 * recibe la 1.ª ráfaga ya demodulada. */
static bool bcch_soft(gsm_t *st, const pend_t *p, int tsc, int search, float soft[456], int64_t *first, int64_t *last)
{
    for (int B = 0; B < 4; B++) {
        dburst_t b;
        if (!demod_burst(st, p->start + (int64_t)GSM_FRAME * (B + 1), search, GSM_TSC[tsc], 61, 26, p->cfo, &b))
            return false;
        if (B == 0)
            *first = BURST_FIRST(b.start);
        if (B == 3)
            *last = BURST_LAST(b.start);
        for (int k = B; k < 456; k += 4) {
            int bb, j;
            gsm_interleave_pos(k, &bb, &j);
            soft[k] = -b.llr[j < 57 ? 3 + j : 88 + (j - 57)];
        }
    }
    return true;
}

/* TSC más probable: la de menor residuo del ajuste de canal sobre la 1.ª ráfaga. */
static int guess_tsc(gsm_t *st, const pend_t *p)
{
    int best = -1;
    double bs = 1e30;
    for (int i = 0; i < 8; i++) {
        int t = (int)((p->bsic + i) & 7);   /* la BCC primero */
        dburst_t b;
        if (!demod_burst(st, p->start + GSM_FRAME, 20, GSM_TSC[t], 61, 26, p->cfo, &b))
            return -1;
        double r = b.sigma2 / (b.power + 1e-20);
        if (r < bs) {
            bs = r;
            best = t;
        }
    }
    return bs < 0.25 ? best : -1;           /* con ruido puro todas valen lo mismo */
}

static void do_bcch(gsm_t *st, const pend_t *p, double now, dec_emit_fn emit, void *ctx)
{
    bool created;
    gsm_cell_t *c = cell_get(st, p->bsic, &created, now);
    int tsc = c->tsc >= 0 ? c->tsc : guess_tsc(st, p);
    if (tsc < 0)
        return;
    float soft[456];
    int64_t first = 0, last = 0;
    if (!bcch_soft(st, p, tsc, 20, soft, &first, &last))
        return;
    uint8_t l3[23];
    if (gsm_bcch_decode(soft, l3) < 0) {
        if (++c->bad >= 4)
            c->tsc = -1;                    /* volver a buscar la TSC */
        return;
    }
    c->bad = 0;
    c->tsc = tsc;
    st->n_bcch++;
    gsm_si_t si;
    if (gsm_si_parse(l3, &si) < 0)
        return;
    int k = si_slot(si.type);
    if (k < 0)
        return;                             /* otro mensaje RR (rellenos, SI13…): se ignora */
    c->last_seen = now;
    if (c->have[k] && memcmp(c->raw[k], l3, 23) == 0)
        return;
    c->si[k] = si;
    c->have[k] = true;
    memcpy(c->raw[k], l3, 23);
    emit_cell(st, c, SI_NAME[k], k, now, first, last, emit, ctx);
}

static void run_pending(gsm_t *st, double now, dec_emit_fn emit, void *ctx)
{
    for (int i = 0; i < st->npend;) {
        if (st->n < st->pend[i].t_ready) {
            i++;
            continue;
        }
        pend_t p = st->pend[i];
        st->pend[i] = st->pend[--st->npend];
        if (p.type == 0)
            do_sch(st, &p, now, emit, ctx);
        else
            do_bcch(st, &p, now, emit, ctx);
    }
}

/* ---------------------------------------------------------------------- */
/*  Detector de FCCH                                                        */
/* ---------------------------------------------------------------------- */
/* Frecuencia del tono en [a, b): lag 4 (grueso) y lag 64 (fino). Devuelve false si
 * no es un tono coherente. */
static bool tone_freq(const gsm_t *st, int64_t a, int64_t b, double *f_out)
{
    double sr = 0, si = 0;
    for (int64_t n = a; n < b; n++) {
        cf32 x = rbat(st, n + 4), y = rbat(st, n);
        sr += x.re * y.re + x.im * y.im;
        si += x.im * y.re - x.re * y.im;
    }
    double f0 = atan2(si, sr) * st->fs / (2.0 * M_PI * 4.0);
    if (fabs(f0 - FCCH_HZ) > 62000.0)
        return false;
    double w = -2.0 * M_PI * f0 * 64.0 / st->fs, c = cos(w), s = sin(w);
    sr = si = 0;
    double ab = 0;
    for (int64_t n = a; n < b - 64; n++) {
        cf32 x = rbat(st, n + 64), y = rbat(st, n);
        double pr = x.re * y.re + x.im * y.im, pi_ = x.im * y.re - x.re * y.im;
        sr += pr * c - pi_ * s;
        si += pr * s + pi_ * c;
        ab += sqrt(pr * pr + pi_ * pi_);
    }
    if (hypot(sr, si) < 0.5 * ab)
        return false;
    *f_out = f0 + atan2(si, sr) * st->fs / (2.0 * M_PI * 64.0);
    return true;
}

static void fcch_candidate(gsm_t *st, int64_t n0, int64_t n1)
{
    int64_t len = n1 - n0;
    if (len < 150 || len > 900 || n1 < st->holdoff)
        return;
    int64_t center = (n0 + n1) / 2, cb = center - LWIN / 2;
    double f;
    if (!tone_freq(st, cb - 200, cb + 200, &f))
        return;
    st->n_fcch++;
    st->holdoff = n1 + 3000;
    /* el centro de la meseta va ≈ 50 muestras retrasado (retardo del paso bajo, del pulso GMSK
     * y del umbral): se corrige y la búsqueda del SCH cubre ±100 muestras */
    pend_t p = {.type = 0, .start = cb - 296 - 50, .cfo = f - FCCH_HZ};
    p.t_ready = p.start + GSM_FRAME + 1300;
    push_pend(st, &p);
}

static inline void push_sample(gsm_t *st, cf32 y)
{
    int64_t idx = st->n;
    cf32 prev = idx >= 4 ? rbat(st, idx - 4) : (cf32){0, 0};
    st->rb[idx & RBMASK] = y;
    cf32 z = {y.re * prev.re + y.im * prev.im, y.im * prev.re - y.re * prev.im};
    float za = sqrtf(cabs2(y) * cabs2(prev));
    st->sre += (double)z.re - st->zr[st->zi].re;
    st->sim += (double)z.im - st->zr[st->zi].im;
    st->sabs += (double)za - st->zabs[st->zi];
    st->zr[st->zi] = z;
    st->zabs[st->zi] = za;
    st->zi = (st->zi + 1) % LWIN;
    st->n = idx + 1;
    if (idx < LWIN + 8)
        return;
    double M = hypot(st->sre, st->sim) / (st->sabs + 1e-12);
    if (!st->in_pl && M >= 0.75) {
        st->in_pl = true;
        st->n0 = idx;
    } else if (st->in_pl && M < 0.6) {
        st->in_pl = false;
        fcch_candidate(st, st->n0, idx - 1);
    }
}

/* ---------------------------------------------------------------------- */
/*  Interfaz                                                               */
/* ---------------------------------------------------------------------- */
static void gsm_destroy(void *p)
{
    gsm_t *st = p;
    if (!st)
        return;
    free(st->rb);
    free(st->tmp);
    sb_free(&st->evsb);
    free(st);
}

static void *gsm_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    if (fabs(cfg->rate - GSM_RATE) > 300.0) {
        snprintf(err, errlen, "GSM requiere 1 083 333 Hz (4 muestras/símbolo)");
        return NULL;
    }
    gsm_t *st = calloc(1, sizeof *st);
    if (!st)
        return NULL;
    st->fs = cfg->rate;
    st->freq_hz = cfg->freq_hz;
    st->rb = calloc(RB, sizeof(cf32));
    if (!st->rb) {
        gsm_destroy(st);
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    /* paso bajo de ±170 kHz (transición hasta ≈ 290 kHz) con ventana de Kaiser */
    fir_design_lowpass(st->taps, LFIR, 230000.0 / cfg->rate, kaiser_beta(60.0));
    return st;
}

static void gsm_process(void *p, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    gsm_t *st = p;
    if (st->tmp_cap < n + LFIR - 1) {
        free(st->tmp);
        st->tmp_cap = n + LFIR - 1;
        st->tmp = malloc(sizeof(cf32) * (size_t)st->tmp_cap);
        if (!st->tmp) {
            st->tmp_cap = 0;
            return;
        }
    }
    st->blk0 = st->n;
    memcpy(st->tmp, st->hist, sizeof(cf32) * (LFIR - 1));
    memcpy(st->tmp + LFIR - 1, iq, sizeof(cf32) * (size_t)n);
    for (int i = 0; i < n; i++) {
        float re = 0, im = 0;
        const cf32 *x = st->tmp + i + LFIR - 1;
        for (int k = 0; k < LFIR; k++) {
            re += st->taps[k] * x[-k].re;
            im += st->taps[k] * x[-k].im;
        }
        push_sample(st, (cf32){re, im});
    }
    memcpy(st->hist, st->tmp + n, sizeof(cf32) * (LFIR - 1));
    st->now0 = now;
    run_pending(st, now + (double)n / st->fs, emit, ctx);
}

static void gsm_state(void *p, sb_t *sb, double now)
{
    gsm_t *st = p;
    sb_printf(sb, "{\"fcch\":%lu,\"sch_ok\":%lu,\"bcch_ok\":%lu,\"cells\":[", st->n_fcch, st->n_sch, st->n_bcch);
    bool first = true;
    for (int i = 0; i < st->ncells; i++) {
        if (now - st->cells[i].last_seen > 120.0)
            continue;
        if (!first)
            sb_puts(sb, ",");
        first = false;
        cell_json(&st->cells[i], sb, NULL, -1, now);
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_GSM = {
    .id = "gsm",
    .name = "GSM",
    .group = "Celular",
    .desc = "GSM: FCCH, SCH (BSIC, FN) y System Information 1-4 del BCCH (MCC/MNC/LAC/CI, ARFCN vecinos).",
    .kind = DEC_NARROW,
    .rate = 1083333,
    .bw_hz = 200000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {0LL, 0LL, 0LL, 0LL},
    .create = gsm_create,
    .destroy = gsm_destroy,
    .process = gsm_process,
    .state_json = gsm_state,
};
