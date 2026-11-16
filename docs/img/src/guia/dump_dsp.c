/* dump_dsp.c (DOC2b) — vuelca coeficientes REALES de src/dsp/filter.c y fft.c
 * para dibujar su respuesta en frecuencia. Uso: dump_dsp <directorio_salida>
 * Cada fichero: cabecera "# clave=valor ..." y luego un valor por línea
 * (re o "re im" para coeficientes complejos). */
#include <stdio.h>
#include <stdlib.h>
#include "dsp/filter.h"
#include "dsp/fft.h"

static FILE *op(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(1); }
    return f;
}

static void dump_firdec(const char *dir, const char *name, int decim, double fs_in, double pass)
{
    firdec_t f;
    if (firdec_init(&f, decim, fs_in, pass, 1023) < 0) { fprintf(stderr, "firdec %s\n", name); exit(1); }
    FILE *o = op(dir, name);
    fprintf(o, "# fs=%.1f decim=%d pass=%.1f ntaps=%d kind=real\n", fs_in, decim, pass, f.ntaps);
    for (int i = 0; i < f.ntaps; i++) fprintf(o, "%.9g\n", f.taps[i]);
    fclose(o);
    firdec_free(&f);
}

static void dump_cfir(const char *dir, const char *name, double fs, double lo, double hi, double trans)
{
    cfir_t f;
    if (cfir_init_bandpass(&f, fs, lo, hi, trans, 1023) < 0) { fprintf(stderr, "cfir %s\n", name); exit(1); }
    FILE *o = op(dir, name);
    fprintf(o, "# fs=%.1f lo=%.1f hi=%.1f trans=%.1f ntaps=%d kind=complex\n", fs, lo, hi, trans, f.ntaps);
    for (int i = 0; i < f.ntaps; i++) fprintf(o, "%.9g %.9g\n", f.taps[i].re, f.taps[i].im);
    fclose(o);
    cfir_free(&f);
}

static void dump_resamp(const char *dir, const char *name, double fin, double fout, double fc, double trans)
{
    resamp_t r;
    if (resamp_init(&r, fin, fout, fc, trans) < 0) { fprintf(stderr, "resamp %s\n", name); exit(1); }
    FILE *o = op(dir, name);
    fprintf(o, "# fs_in=%.1f fs_out=%.1f cutoff=%.1f trans=%.1f L=%d P=%d kind=bank\n", fin, fout, fc, trans, r.L, r.P);
    /* prototipo a P·fs_in: instante t = (L/2-1-j) + p/P, creciente con j↓ y p↑ */
    for (int j = r.L - 1; j >= 0; j--)
        for (int p = 0; p < r.P; p++) fprintf(o, "%.9g\n", r.bank[(size_t)p * r.L + j]);
    fclose(o);
    resamp_free(&r);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "uso: dump_dsp dir\n"); return 1; }
    const char *d = argv[1];
    /* Diezmador ÷8 de un DDC con canal de 200 kHz (ddc.c: pass = bw/2 + (r-bw)/4) */
    dump_firdec(d, "firdec_a.csv", 8, 2.4e6, 100e3 + (300e3 - 200e3) / 4);
    dump_firdec(d, "firdec_b.csv", 8, 61.44e6, 100e3 + (7.68e6 - 200e3) / 4);
    /* Filtros de canal (channel.c) a fch = 48 kHz */
    dump_cfir(d, "cfir_usb.csv", 48000, 200.0, 2700.0, 250.0);
    dump_cfir(d, "cfir_lsb.csv", 48000, -2700.0, -200.0, 250.0);
    dump_cfir(d, "cfir_am.csv", 48000, -5000.0, 5000.0, 1200.0);
    dump_cfir(d, "cfir_nfm.csv", 48000, -6250.0, 6250.0, 800.0);
    dump_resamp(d, "resamp_wfm.csv", 240000, 48000, 15000, 3500);
    dump_resamp(d, "resamp_nfm.csv", 52083.333, 48000, 3000, 600);
    float w[1024];
    window_blackman_harris(w, 1024);
    FILE *o = op(d, "window_bh.csv");
    fprintf(o, "# n=1024\n");
    for (int i = 0; i < 1024; i++) fprintf(o, "%.9g\n", w[i]);
    fclose(o);
    return 0;
}
