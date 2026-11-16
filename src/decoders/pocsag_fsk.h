/*
 * pocsag_fsk.h — Receptor FSK común de POCSAG, FLEX y DMR (solo cabecera).
 *
 * Los tres protocolos llegan por un canal de FM estrecho (12,5 kHz) y son FSK
 * de 2 o 4 niveles. La cadena de recepción es la misma:
 *
 *   IQ ──► FIR complejo paso bajo ──► discriminador FM ──► filtro adaptado
 *      ──► recuperación de reloj (Gardner) ──► símbolos normalizados
 *
 *  1. Filtro de canal (fskfe_t). El DDC del host ya filtra a bw_hz, pero el
 *     propio decodificador se protege del ruido de banda ancha (los tests le
 *     meten ruido a toda la tasa): un FIR de ventana Hamming de 41 coeficientes
 *     con corte configurable. Sin él, a SNR de 10 dB el discriminador produciría
 *     "clics" (saltos de 2π) que rompen los bits.
 *
 *  2. Discriminador: Δφ[n] = arg(y[n]·conj(y[n−1])), escalado a ±1 para ±dev_hz.
 *     Un desvío de portadora (CFO) solo suma una constante: se elimina después
 *     con el seguimiento de picos.
 *
 *  3. Filtro adaptado (fskch_t). Para NRZ rectangular (POCSAG, FLEX) es la
 *     media sobre un símbolo (boxcar); para 4FSK conformada con coseno alzado
 *     raíz (DMR, TS 102 361-1 §10.2.2.2) es el propio RRC. Se puede pasar
 *     cualquier juego de coeficientes.
 *
 *  4. Reloj de símbolo con detector de error de Gardner,
 *         e = y_medio · (y[k] − y[k−1]),
 *     válido para cualquier nº de niveles y sin decisiones. Un oscilador de fase
 *     t ∈ [0,1) avanza 1/sps por muestra; se muestrea en t = 1 (símbolo) y en
 *     t = ½ (punto medio), con interpolación lineal (sps ≥ 8 en todos los
 *     usos, el error es despreciable). Si el punto de muestreo va tarde
 *     (e > 0) se adelanta la fase: t += kp·e. El error de reloj de ±100 ppm es
 *     un 0,01 % de símbolo por símbolo; con kp ≈ 0,03 queda absorbido.
 *
 *  5. Normalización. Dos seguidores de pico (máximo y mínimo del símbolo, con
 *     ataque rápido y caída lenta hacia el centro) dan el centro (c = media de
 *     ambos: elimina el CFO) y la amplitud (a = semidiferencia). La salida es
 *     (y − c)/a: ±1 en los niveles externos. Para 4 niveles los internos
 *     quedan en ±1/3 y los umbrales en 0 y ±2/3.
 */
#ifndef BLADERF_LAB_POCSAG_FSK_H
#define BLADERF_LAB_POCSAG_FSK_H

#include "../common.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/*  Filtro de canal + discriminador                                          */
/* ------------------------------------------------------------------------- */
#define FSK_FE_TAPS 41

typedef struct {
    float h[FSK_FE_TAPS];
    cf32 dl[2 * FSK_FE_TAPS]; /* línea de retardo con doble escritura */
    int pos;
    cf32 prev;
    float gain;               /* fs/(2π·dev) */
} fskfe_t;

static inline void fskfe_init(fskfe_t *f, double fs, double cutoff_hz, double dev_hz)
{
    memset(f, 0, sizeof *f);
    double fc = cutoff_hz / fs, sum = 0;
    int m = FSK_FE_TAPS / 2;
    for (int i = 0; i < FSK_FE_TAPS; i++) {
        double x = i - m;
        double s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
        double w = 0.54 - 0.46 * cos(2 * M_PI * i / (FSK_FE_TAPS - 1));
        f->h[i] = (float)(s * w);
        sum += s * w;
    }
    for (int i = 0; i < FSK_FE_TAPS; i++)
        f->h[i] = (float)(f->h[i] / sum); /* ganancia 1 en continua */
    f->gain = (float)(fs / (2 * M_PI * dev_hz));
}

static inline void fskfe_process(fskfe_t *f, const cf32 *in, int n, float *out)
{
    for (int k = 0; k < n; k++) {
        f->pos = f->pos ? f->pos - 1 : FSK_FE_TAPS - 1;
        f->dl[f->pos] = in[k];
        f->dl[f->pos + FSK_FE_TAPS] = in[k];
        const cf32 *d = &f->dl[f->pos];
        float re = 0, im = 0;
        for (int i = 0; i < FSK_FE_TAPS; i++) {
            re += f->h[i] * d[i].re;
            im += f->h[i] * d[i].im;
        }
        /* y·conj(prev) */
        float pr = re * f->prev.re + im * f->prev.im;
        float pi = im * f->prev.re - re * f->prev.im;
        out[k] = atan2f(pi, pr) * f->gain;
        f->prev.re = re;
        f->prev.im = im;
    }
}

/* ------------------------------------------------------------------------- */
/*  Filtro adaptado + reloj + normalización                                  */
/* ------------------------------------------------------------------------- */
typedef struct {
    double sps;
    int nt;
    float *taps;     /* nt coeficientes, Σ|h| arbitrario (se normaliza fuera) */
    float *dl;       /* línea de retardo 2·nt */
    int pos;
    double t;        /* fase del reloj en símbolos, [0,1) */
    float f_prev;    /* salida anterior del filtro */
    float y_prev;    /* símbolo anterior (sin normalizar) */
    float y_mid;
    bool have_mid;
    float hp, ln;    /* seguidores de pico */
    double kp;
    float ctr, amp;  /* último centro y amplitud (para el llamador) */
} fskch_t;

/* taps == NULL → media sobre un símbolo (boxcar de round(sps) muestras). */
static inline int fskch_init(fskch_t *c, double sps, const float *taps, int nt, double kp)
{
    memset(c, 0, sizeof *c);
    c->sps = sps;
    if (!taps) {
        nt = (int)lround(sps);
        c->taps = malloc(sizeof(float) * (size_t)nt);
        if (c->taps)
            for (int i = 0; i < nt; i++)
                c->taps[i] = 1.0f / (float)nt;
    } else {
        c->taps = malloc(sizeof(float) * (size_t)nt);
        if (c->taps)
            memcpy(c->taps, taps, sizeof(float) * (size_t)nt);
    }
    c->nt = nt;
    c->dl = calloc((size_t)(2 * nt), sizeof(float));
    if (!c->taps || !c->dl) {
        free(c->taps);
        free(c->dl);
        return -1;
    }
    c->kp = kp;
    c->hp = 0.1f;
    c->ln = -0.1f;
    c->ctr = 0;
    c->amp = 0.1f;
    return 0;
}

static inline void fskch_free(fskch_t *c)
{
    free(c->taps);
    free(c->dl);
    c->taps = NULL;
    c->dl = NULL;
}

/* Cuerpo del filtro adaptado: devuelve la salida para la muestra x. */
static inline float fskch_filter(fskch_t *c, float x)
{
    c->pos = c->pos ? c->pos - 1 : c->nt - 1;
    c->dl[c->pos] = x;
    c->dl[c->pos + c->nt] = x;
    const float *d = &c->dl[c->pos];
    float acc = 0;
    for (int i = 0; i < c->nt; i++)
        acc += c->taps[i] * d[i];
    return acc;
}

/* Introduce una muestra del discriminador. Devuelve 1 y escribe el símbolo
 * normalizado en *y cuando toca muestrear. */
static inline int fskch_push(fskch_t *c, float x, float *y)
{
    float f = fskch_filter(c, x);
    double dt = 1.0 / c->sps, t0 = c->t, t1 = t0 + dt;
    int got = 0;
    if (t0 < 0.5 && t1 >= 0.5) {
        double fr = (0.5 - t0) / dt;
        c->y_mid = c->f_prev + (float)fr * (f - c->f_prev);
        c->have_mid = true;
    }
    if (t1 >= 1.0) {
        double fr = (1.0 - t0) / dt;
        float yk = c->f_prev + (float)fr * (f - c->f_prev);
        /* seguidores de pico */
        if (yk > c->hp)
            c->hp += 0.3f * (yk - c->hp);
        else
            c->hp += 0.004f * (c->ctr - c->hp);
        if (yk < c->ln)
            c->ln += 0.3f * (yk - c->ln);
        else
            c->ln += 0.004f * (c->ctr - c->ln);
        c->ctr = 0.5f * (c->hp + c->ln);
        c->amp = fmaxf(0.5f * (c->hp - c->ln), 0.02f);
        /* Gardner con amplitud normalizada */
        if (c->have_mid) {
            float e = ((c->y_mid - c->ctr) / c->amp) * ((yk - c->y_prev) / c->amp);
            if (e > 2.0f)
                e = 2.0f;
            if (e < -2.0f)
                e = -2.0f;
            t1 += c->kp * e;
        }
        c->y_prev = yk;
        c->have_mid = false;
        t1 -= 1.0;
        if (t1 < 0)
            t1 = 0;
        *y = (yk - c->ctr) / c->amp;
        got = 1;
    }
    c->t = t1;
    c->f_prev = f;
    return got;
}

#endif
