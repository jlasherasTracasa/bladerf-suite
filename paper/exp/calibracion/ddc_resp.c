/*
 * ddc_resp.c — Respuesta en potencia |H(f)|² del canal (DDC real de BladeRF Lab) de cada decodificador.
 *
 * Para la segunda definición de SNR de la calibración (P3d): la potencia de ruido "tras el filtro de
 * canal" se calcula en numpy ponderando la PSD local del fondo con |H(f − f0)|², la misma forma que ve
 * el decodificador. Aquí se mide esa forma con el DDC real (no con un diseño aparte): se inyecta un tono
 * complejo a cada frecuencia de una rejilla, se pasa por ddc_process() a la tasa de la serie y se mide la
 * potencia de salida. Se normaliza a 1 en f = 0. Un tono de amplitud 1 da potencia de salida |H|².
 *
 * Uso:  ddc_resp <proto> <fs_in> <paso_hz>   → CSV "f_hz,h2" por la salida estándar
 * Compilar (ver medir.sh): enlaza con los objetos de build/<dir> salvo main.o.
 */
#include "common.h"
#include "decoders/decoder.h"
#include "dsp/ddc.h"

#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    const decoder_desc_t *d = decoder_find(argv[1]);
    const double fs = atof(argv[2]), step = atof(argv[3]);
    if (!d || d->kind != DEC_NARROW)
        return 2;
    const int64_t c = 100000000;
    const int n = (int)(fs * 0.25); /* 250 ms por tono */
    cf32 *in = malloc(sizeof(cf32) * (size_t)n);
    size_t cap = ddc_max_out(ddc_create(fs, c, c, d->rate, d->bw_hz), n) + 64;
    cf32 *out = malloc(sizeof(cf32) * cap);
    double h0 = 0;
    const int np = (int)(fs / step);
    for (int k = -np / 2; k <= np / 2; k++) {
        const double f = k * step;
        ddc_t *dd = ddc_create(fs, c, c, d->rate, d->bw_hz);
        for (int i = 0; i < n; i++) {
            const double ph = 2.0 * M_PI * f * (double)i / fs;
            in[i].re = (float)cos(ph);
            in[i].im = (float)sin(ph);
        }
        int m = ddc_process(dd, in, n, out, (int)cap);
        double p = 0;
        int a = m / 2;
        for (int i = a; i < m; i++)
            p += (double)out[i].re * out[i].re + (double)out[i].im * out[i].im;
        p /= (double)MAX(m - a, 1);
        if (k == 0)
            h0 = p;
        printf("%.1f,%.6g\n", f, p);
        ddc_destroy(dd);
    }
    fprintf(stderr, "h0=%g\n", h0);
    return 0;
}
