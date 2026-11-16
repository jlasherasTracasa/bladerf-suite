/*
 * spec_model.c — Modelo de referencia exacto al bit del motor de espectro.
 *
 * Aritmética (idéntica al HDL):
 *  1. Ventana. x = I o Q con signo de 12 bits (±2048 = fondo de escala Q11).
 *     wq = round(32768 * w[i]) con w[i] = a0 - a1 cos(2πi/(N-1)) + a2 cos(4πi/(N-1))
 *     - a3 cos(6πi/(N-1)) (la misma ventana que src/dsp/fft.c).
 *     xw = (x * wq + 1024) >> 11   (desplazamiento aritmético): xw = x*w*16,
 *     es decir, 4 bits fraccionarios, 16 bits con signo.
 *  2. FFT DIF en flujo (SDF), radix-2^2, etapas con retardo D = 2^e, e = n-1..0
 *     (n = log2 N). La primera etapa es e0 = n-1. Cada etapa recibe tramas de
 *     2D elementos y emite [sumas(D), diferencias(D)] (orden de salida del SDF):
 *        s[i] = a[i] + b[i],  d[i] = a[i] - b[i],  a = primera mitad, b = segunda.
 *     Etapa "II" (e par y e != e0): en los bloques impares (la segunda mitad de
 *     la trama de 4D) b se multiplica por -j antes de sumar/restar:
 *        -j*(br + j*bi) = (bi, -br).
 *     Ancho de los datos que entran en la etapa e: WD(e) = min(25, 29-e) bits.
 *     Si WD(e)+1 > 25 (e <= 4) la salida se desplaza un bit: (v + 1) >> 1.
 *  3. Tras las etapas e par con e >= 2 se multiplica por la rotación:
 *        (a + jb) * (wr + j wi), wr = round(65536 cos θ), wi = round(-65536 sin θ)
 *        re' = (a*wr - b*wi + 32768) >> 16 ; im' = (a*wi + b*wr + 32768) >> 16
 *     con posición p dentro del flujo de salida de la etapa:
 *        etapa II (par, e != e0): trama de 4D; q = bitrev2((p >> e) & 3),
 *          n = p & (D-1), θ = 2π n q / (4D)
 *        etapa sola (e = e0 par):   trama de 2D; si (p >> e) & 1: θ = 2π n / (2D)
 *          con n = p & (D-1); si no, rotación unidad.
 *  4. Salida: X_out = 0.5 * FFT_{N}(x*w) en unidades de cuenta (porque x*w*16 y
 *     5 desplazamientos de 1 bit = 2^-5), sin factor N, con 25 bits.
 *  5. P = re^2 + im^2 (exacto, 64 bits), acumulada K veces (exacta).
 *  6. Palabra: msb = posición del bit más alto de acc (acc=0 -> E=0),
 *     E = max(0, msb - 25), M = (acc >> E) & (2^26 - 1), palabra = E<<26 | M.
 */
#include "spec_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wd_bits(int e) { int w = 29 - e; return w > SPEC_DATA_BITS ? SPEC_DATA_BITS : w; }
static int shift_needed(int e) { return wd_bits(e) + 1 > SPEC_DATA_BITS; }

void spec_window_q15(int log2n, uint16_t *w)
{
    const int n = 1 << log2n;
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    for (int i = 0; i < n / 2; i++) {
        double x = 2.0 * M_PI * i / (n - 1);
        double v = a0 - a1 * cos(x) + a2 * cos(2 * x) - a3 * cos(3 * x);
        long q = lround(v * 32768.0);
        if (q < 0) q = 0;
        if (q > 32768) q = 32768;
        w[i] = (uint16_t)q;
        w[n - 1 - i] = (uint16_t)q; /* ventana simétrica */
    }
}

unsigned spec_bitrev(int log2n, unsigned j)
{
    unsigned r = 0;
    for (int b = 0; b < log2n; b++)
        if (j & (1u << b)) r |= 1u << (log2n - 1 - b);
    return r;
}

static int g_range_err;
static int32_t chk(int64_t v, int bits)
{
    const int64_t lim = (int64_t)1 << (bits - 1);
    if (v < -lim || v >= lim) g_range_err = 1;
    return (int32_t)v;
}

static unsigned bitrev2(unsigned q) { return ((q & 1) << 1) | ((q >> 1) & 1); }

/* wr, wi de e^{-j 2π m / L} en Q16 */
static int64_t gcd64(int64_t a, int64_t b) { while (b) { int64_t t = a % b; a = b; b = t; } return a; }

/* Se reduce la fracción m/l para que W_{4D}^{2n} y W_{2D}^{n} den exactamente
 * los mismos valores (la ROM del HDL se genera con esta misma función). */
void spec_twiddle(int64_t m, int64_t l, int32_t *wr, int32_t *wi)
{
    if (m) { int64_t g = gcd64(m, l); m /= g; l /= g; }
    double th = 2.0 * M_PI * (double)m / (double)l;
    *wr = (int32_t)lround(65536.0 * cos(th));
    *wi = (int32_t)lround(-65536.0 * sin(th));
}

int spec_fft_frame(int log2n, const int16_t *iq, int32_t *re, int32_t *im)
{
    const int n = 1 << log2n;
    uint16_t *w = malloc(sizeof(uint16_t) * (size_t)n);
    spec_window_q15(log2n, w);
    g_range_err = 0;
    for (int i = 0; i < n; i++) {
        re[i] = (int32_t)(((int64_t)iq[2 * i] * w[i] + 1024) >> 11);
        im[i] = (int32_t)(((int64_t)iq[2 * i + 1] * w[i] + 1024) >> 11);
    }
    free(w);
    const int e0 = log2n - 1;
    int32_t *tr = malloc(sizeof(int32_t) * (size_t)n), *ti = malloc(sizeof(int32_t) * (size_t)n);
    for (int e = e0; e >= 0; e--) {
        const int d = 1 << e;
        const int is_ii = (e % 2 == 0) && (e != e0);
        const int shift = shift_needed(e);
        const int ow = shift ? SPEC_DATA_BITS : wd_bits(e) + 1;
        for (int b = 0; b < n / (2 * d); b++) {
            for (int i = 0; i < d; i++) {
                const int ia = b * 2 * d + i, ib = ia + d;
                int64_t ar = re[ia], ai = im[ia], br = re[ib], bi = im[ib];
                if (is_ii && (b & 1)) {
                    int64_t t = br;
                    br = bi;
                    bi = -t;
                }
                int64_t sr = ar + br, si = ai + bi, dr = ar - br, di = ai - bi;
                if (shift) {
                    sr = (sr + 1) >> 1; si = (si + 1) >> 1;
                    dr = (dr + 1) >> 1; di = (di + 1) >> 1;
                }
                tr[ia] = chk(sr, ow); ti[ia] = chk(si, ow);
                tr[ib] = chk(dr, ow); ti[ib] = chk(di, ow);
            }
        }
        memcpy(re, tr, sizeof(int32_t) * (size_t)n);
        memcpy(im, ti, sizeof(int32_t) * (size_t)n);
        if (e % 2 == 0 && e >= 2) {
            const int lone = (e == e0);
            for (int p = 0; p < n; p++) {
                int64_t m, l;
                const int nn = p & (d - 1);
                if (lone) {
                    if (!((p >> e) & 1)) continue;
                    m = nn; l = 2 * d;
                } else {
                    m = (int64_t)nn * bitrev2((unsigned)((p >> e) & 3)); l = 4 * d;
                }
                if (m == 0) continue; /* rotación unidad: (a*65536+32768)>>16 = a */
                int32_t wr, wi;
                spec_twiddle(m, l, &wr, &wi);
                int64_t a = re[p], bq = im[p];
                re[p] = chk((a * wr - bq * wi + 32768) >> 16, ow);
                im[p] = chk((a * wi + bq * wr + 32768) >> 16, ow);
            }
        }
    }
    free(tr); free(ti);
    return g_range_err ? -1 : 0;
}

uint64_t spec_power(int32_t re, int32_t im)
{
    return (uint64_t)((int64_t)re * re) + (uint64_t)((int64_t)im * im);
}

uint32_t spec_pack_power(uint64_t acc)
{
    int msb = -1;
    for (int b = 63; b >= 0; b--)
        if (acc >> b & 1) { msb = b; break; }
    int e = msb > 25 ? msb - 25 : 0;
    uint32_t m = (uint32_t)((acc >> e) & 0x3FFFFFFu);
    return ((uint32_t)e << 26) | m;
}

double spec_unpack_power(uint32_t w)
{
    return ldexp((double)(w & 0x3FFFFFFu), (int)(w >> 26));
}

int spec_group(int log2n, int k, const int16_t *iq, uint32_t group_counter,
               uint64_t ts, uint32_t flags, int pad, uint32_t *out)
{
    const int n = 1 << log2n;
    uint64_t *acc = calloc((size_t)n, sizeof(uint64_t));
    int32_t *re = malloc(sizeof(int32_t) * (size_t)n), *im = malloc(sizeof(int32_t) * (size_t)n);
    int bad = 0;
    for (int f = 0; f < k; f++) {
        if (spec_fft_frame(log2n, iq + 2 * (size_t)f * n, re, im)) bad = 1;
        for (int j = 0; j < n; j++)
            acc[spec_bitrev(log2n, (unsigned)j)] += spec_power(re[j], im[j]);
    }
    int total = SPEC_HDR_WORDS + n;
    if (pad) total = (total + SPEC_PAD_WORDS - 1) / SPEC_PAD_WORDS * SPEC_PAD_WORDS;
    out[0] = SPEC_MAGIC0;
    out[1] = SPEC_MAGIC1;
    out[2] = ((uint32_t)k << 16) | (uint32_t)n;
    out[3] = group_counter;
    out[4] = (uint32_t)ts;
    out[5] = (uint32_t)(ts >> 32);
    out[6] = (SPEC_HDR_WORDS) /* [7:0]=8, [15:8]=1 versión, [16]=perdida */ | (1u << 8) | (flags & 0x10000u);
    out[7] = (uint32_t)total;
    for (int i = 0; i < n; i++)
        out[SPEC_HDR_WORDS + i] = spec_pack_power(acc[(i + n / 2) & (n - 1)]);
    for (int i = SPEC_HDR_WORDS + n; i < total; i++) out[i] = 0;
    free(acc); free(re); free(im);
    return bad ? -total : total;
}
