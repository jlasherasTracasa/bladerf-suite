/*
 * spec_tool.c — Herramienta del modelo del motor de espectro.
 *
 *   spec_tool genrom <fichero.vhd>     genera lab_rom_p.vhd (ventana y twiddles)
 *   spec_tool selftest                 rangos y exactitud contra una DFT en doble
 *   spec_tool vec <log2n> <k> <grupos> <escenario> <semilla> <pad> <in.txt> <exp.txt>
 *        escenario 0: tonos + ruido; 1: estrés (±fondo de escala aleatorio);
 *        2: tono de fondo de escala en bin entero
 *   spec_tool check <log2n> <k> <pad> <exp.txt> <volcado.txt>
 *        compara el volcado del simulador con el modelo (palabras hex, una por línea,
 *        ignorando las marcas de tiempo y comprobando su incremento = K*N)
 */
#include "spec_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 11);
}
static double gauss(void)
{
    double u = (rnd() + 1.0) / 4294967297.0, v = (rnd() + 1.0) / 4294967297.0;
    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

static int genrom(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f, "-- Generado por firmware/model/spec_tool genrom. NO EDITAR A MANO.\n"
               "-- Ventana Blackman-Harris Q15 (mitades) y twiddles Q16 del motor de espectro.\n"
               "library ieee;\n    use ieee.std_logic_1164.all;\n    use ieee.numeric_std.all;\n\n"
               "package lab_rom_p is\n"
               "    type win_rom_t is array (natural range <>) of std_logic_vector(15 downto 0);\n"
               "    type tw_rom_t  is array (natural range <>) of std_logic_vector(35 downto 0);\n\n");
    /* Ventana: concatenación de mitades de N = 1024, 2048, 4096, 8192 */
    int total = 512 + 1024 + 2048 + 4096;
    fprintf(f, "    -- Desplazamiento de la mitad de cada N: 1024->0, 2048->512, 4096->1536, 8192->3584\n");
    fprintf(f, "    constant WIN_ROM : win_rom_t(0 to %d) := (\n", total - 1);
    int cnt = 0;
    for (int l = 10; l <= 13; l++) {
        uint16_t *w = malloc(sizeof(uint16_t) << l);
        spec_window_q15(l, w);
        for (int i = 0; i < (1 << l) / 2; i++) {
            cnt++;
            fprintf(f, "        x\"%04X\"%s\n", w[i], cnt == total ? "" : ",");
        }
        free(w);
    }
    fprintf(f, "    );\n\n");
    for (int e = 2; e <= 12; e += 2) {
        int size = (e == 12) ? 4096 : 3 * (1 << e);
        int psize = (e == 12) ? 4096 : 4 * (1 << e); /* potencia de 2: direccionamiento directo, sin módulo */
        fprintf(f, "    -- e=%d: %s\n", e, e == 12 ? "W_8192^n (solo etapa sola)" : "W_(2^(e+2))^m, m<3*2^e");
        fprintf(f, "    constant TW_ROM_%d : tw_rom_t(0 to %d) := (\n", e, psize - 1);
        for (int m = 0; m < psize; m++) {
            int32_t wr = 0, wi = 0;
            if (m >= size) { /* relleno */ }
            else if (e == 12) spec_twiddle(m, 8192, &wr, &wi);
            else spec_twiddle(m, (int64_t)1 << (e + 2), &wr, &wi);
            fprintf(f, "        x\"%09llX\"%s\n",
                    (((unsigned long long)((unsigned)wr & 0x3FFFF)) << 18) | ((unsigned)wi & 0x3FFFF),
                    m == psize - 1 ? "" : ",");
        }
        fprintf(f, "    );\n\n");
    }
    fprintf(f, "end package;\n");
    fclose(f);
    return 0;
}

static int selftest(void)
{
    int fails = 0;
    for (int l = SPEC_NMIN_LOG2; l <= SPEC_NMAX_LOG2; l++) {
        int n = 1 << l;
        int16_t *iq = malloc(sizeof(int16_t) * 2 * (size_t)n);
        int32_t *re = malloc(4 * (size_t)n), *im = malloc(4 * (size_t)n);
        /* 1) estrés: valores extremos aleatorios, sin desbordes */
        for (int trial = 0; trial < 6; trial++) {
            for (int i = 0; i < n; i++) {
                int sel = trial % 3;
                int a = sel == 0 ? (rnd() & 1 ? 2047 : -2048) : sel == 1 ? (int)(rnd() & 4095) - 2048 : 2047;
                int b = sel == 0 ? (rnd() & 1 ? 2047 : -2048) : sel == 1 ? (int)(rnd() & 4095) - 2048 : -2048;
                if (sel == 2 && trial == 5) { a = (i & 1) ? 2047 : -2048; b = a; }
                iq[2 * i] = (int16_t)a; iq[2 * i + 1] = (int16_t)b;
            }
            if (spec_fft_frame(l, iq, re, im)) { printf("N=%d trial %d: DESBORDE\n", n, trial); fails++; }
        }
        /* 2) exactitud: tono + ruido frente a una DFT en doble */
        uint16_t *w = malloc(sizeof(uint16_t) * (size_t)n);
        spec_window_q15(l, w);
        double fk = 37.3 * n / 1024.0;
        for (int i = 0; i < n; i++) {
            double ph = 2 * M_PI * fk * i / n;
            iq[2 * i] = (int16_t)lround(1500 * cos(ph) + 3 * gauss());
            iq[2 * i + 1] = (int16_t)lround(1500 * sin(ph) + 3 * gauss());
        }
        spec_fft_frame(l, iq, re, im);
        double maxerr = 0, maxref = 0;
        for (int j = 0; j < n; j += 1) {
            unsigned k = spec_bitrev(l, (unsigned)j);
            if (k % 7 != 0 && k != (unsigned)lround(fk)) continue;
            double sr = 0, si = 0;
            for (int i = 0; i < n; i++) {
                double wv = w[i] / 32768.0, th = -2 * M_PI * (double)k * i / n;
                sr += wv * (iq[2 * i] * cos(th) - iq[2 * i + 1] * sin(th));
                si += wv * (iq[2 * i] * sin(th) + iq[2 * i + 1] * cos(th));
            }
            sr *= 0.5; si *= 0.5; /* X_out = 0.5 * FFT */
            double er = hypot(sr - re[j], si - im[j]);
            if (er > maxerr) maxerr = er;
            if (hypot(sr, si) > maxref) maxref = hypot(sr, si);
        }
        printf("N=%5d: estrés sin desbordes; error máx. frente a DFT doble = %.2f (pico %.0f; %.1f dB)\n",
               n, maxerr, maxref, 20 * log10(maxerr / maxref));
        if (maxerr / maxref > 1e-3) fails++;
        free(iq); free(re); free(im); free(w);
    }
    printf(fails ? "SELFTEST: FALLO\n" : "SELFTEST: OK\n");
    return fails != 0;
}

static int clip12(double v)
{
    long q = lround(v);
    return q > 2047 ? 2047 : q < -2048 ? -2048 : (int)q;
}

static int make_vec(int l, int k, int groups, int scen, uint32_t seed, int pad, const char *fin, const char *fexp)
{
    rng_s ^= (uint64_t)seed * 0x2545F4914F6CDD1Dull;
    int n = 1 << l;
    size_t ns = (size_t)groups * k * n;
    int16_t *iq = malloc(sizeof(int16_t) * 2 * ns);
    for (size_t i = 0; i < ns; i++) {
        double t = (double)(i % n);
        double a, b;
        if (scen == 0) {
            double p1 = 2 * M_PI * 0.1013 * (double)i, p2 = 2 * M_PI * (-0.2371) * (double)i;
            a = 1000 * cos(p1) + 40 * cos(p2) + 12 * gauss();
            b = 1000 * sin(p1) + 40 * sin(p2) + 12 * gauss();
        } else if (scen == 1) {
            a = (rnd() & 1) ? 2047 : -2048; b = (rnd() & 1) ? 2047 : -2048;
        } else {
            double ph = 2 * M_PI * (double)(n / 8) * t / n; /* tono de fondo de escala en bin entero */
            a = 2047 * cos(ph); b = 2047 * sin(ph);
        }
        iq[2 * i] = (int16_t)clip12(a); iq[2 * i + 1] = (int16_t)clip12(b);
    }
    FILE *fi = fopen(fin, "w"), *fe = fopen(fexp, "w");
    if (!fi || !fe) return 1;
    for (size_t i = 0; i < ns; i++) fprintf(fi, "%d %d\n", iq[2 * i], iq[2 * i + 1]);
    uint32_t *out = malloc(4 * (size_t)(SPEC_HDR_WORDS + n + 4096));
    for (int g = 0; g < groups; g++) {
        int tot = spec_group(l, k, iq + 2 * (size_t)g * k * n, (uint32_t)g, 0, 0, pad, out);
        if (tot < 0) { fprintf(stderr, "desborde en el modelo\n"); return 2; }
        for (int i = 0; i < tot; i++) fprintf(fe, "%08X\n", out[i]);
    }
    fclose(fi); fclose(fe);
    return 0;
}

static int check(int l, int k, int pad, const char *fexp, const char *fdump)
{
    int n = 1 << l, tot = SPEC_HDR_WORDS + n;
    if (pad) tot = (tot + SPEC_PAD_WORDS - 1) / SPEC_PAD_WORDS * SPEC_PAD_WORDS;
    FILE *fe = fopen(fexp, "r"), *fd = fopen(fdump, "r");
    if (!fe || !fd) { printf("no se pueden abrir los ficheros\n"); return 1; }
    unsigned ew, dw;
    long idx = 0, bad = 0, groups = 0;
    uint64_t prev_ts = 0;
    int ts_ok = 1;
    while (fscanf(fe, "%x", &ew) == 1) {
        if (fscanf(fd, "%x", &dw) != 1) { printf("volcado más corto que el esperado (palabra %ld)\n", idx); return 1; }
        long pos = idx % tot;
        if (pos == 4 || pos == 5) { /* marca de tiempo: no comparar, solo recoger */
            static uint32_t lo;
            if (pos == 4) lo = dw;
            else {
                uint64_t ts = ((uint64_t)dw << 32) | lo;
                if (groups > 0 && ts - prev_ts != (uint64_t)k * n) { ts_ok = 0; printf("ts: delta %llu != %d\n", (unsigned long long)(ts - prev_ts), k * n); }
                prev_ts = ts;
            }
        } else if (ew != dw) {
            if (bad < 8) printf("diferencia en palabra %ld (trama %ld, pos %ld): modelo %08X, HDL %08X\n", idx, idx / tot, pos, ew, dw);
            bad++;
        }
        idx++;
        if (idx % tot == 0) groups++;
    }
    printf("%ld palabras (%ld tramas): %ld diferencias; marcas de tiempo %s\n", idx, groups, bad, ts_ok ? "OK" : "MAL");
    return bad || !ts_ok;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "genrom")) return genrom(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return selftest();
    if (argc >= 10 && !strcmp(argv[1], "vec"))
        return make_vec(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), (uint32_t)atoi(argv[6]), atoi(argv[7]), argv[8], argv[9]);
    if (argc >= 7 && !strcmp(argv[1], "check")) return check(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), argv[5], argv[6]);
    fprintf(stderr, "uso: ver cabecera\n");
    return 2;
}
