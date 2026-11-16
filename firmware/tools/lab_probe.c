/*
 * lab_probe.c — Herramienta de prueba en hardware de la imagen de FPGA "lab".
 * NO la ejecutan los agentes: la usa el orquestador, con permiso del usuario, tras
 * cargar la imagen EN RAM con `bladeRF-cli -l firmware/out/hosted-xA9-lab-0.16.101.rbf`
 * (nunca -L). No transmite, no escribe la flash, no carga nada en la FPGA.
 *
 *   lab_probe                       versión de FPGA y detección segura de la imagen lab;
 *                                   si es lab: prueba del registro SCRATCH y contadores.
 *                                   Con la imagen de serie NO toca el bus Wishbone.
 *   lab_probe --iq [freq_hz]        (cualquier imagen) 1 s de RX normal (rx_mux = 0):
 *                                   nivel RMS y saturación; sirve para comparar lab y serie.
 *   lab_probe --spectrum log2n k tramas [freq_hz] [tasa_sps]
 *                                   (solo lab) activa rx_mux = 3, lee 'tramas' tramas de espectro
 *                                   y muestra el pico y el suelo en dBFS; al terminar vuelve a rx_mux = 0.
 *
 * Compilación (desde la raíz del proyecto):
 *   gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -DFPGA_LAB_WITH_LIBBLADERF -Isrc \
 *       firmware/tools/lab_probe.c src/sdr/fpga_lab.c -lbladeRF -lm -o build/f1/lab_probe
 */
#include "sdr/fpga_lab.h"

#include <libbladeRF.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH BLADERF_CHANNEL_RX(0)

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

typedef struct {
    int frames, want;
    float *tmp;
} cap_t;

static void on_frame(void *ctx, const fpga_lab_frame_t *f)
{
    cap_t *c = ctx;
    float *db = malloc(sizeof(float) * (size_t)f->n);
    fpga_lab_frame_to_dbfs(f, db);
    int pk = 0;
    for (int i = 0; i < f->n; i++) if (db[i] > db[pk]) pk = i;
    memcpy(c->tmp, db, sizeof(float) * (size_t)f->n);
    qsort(c->tmp, (size_t)f->n, sizeof(float), cmp_float);
    printf("trama %u  N=%d K=%d  ts=%llu%s  pico %.1f dBFS en el bin %d  mediana %.1f dBFS\n",
           f->group, f->n, f->k, (unsigned long long)f->ts, f->dropped ? " (perdida previa)" : "",
           db[pk], pk - f->n / 2, c->tmp[f->n / 2]);
    free(db);
    c->frames++;
}

static int setup_rx(struct bladerf *dev, uint64_t freq, uint32_t rate)
{
    uint32_t actual = 0;
    int st = bladerf_set_sample_rate(dev, CH, rate, &actual);
    if (st < 0) return st;
    st = bladerf_set_frequency(dev, CH, freq);
    if (st < 0) return st;
    bladerf_set_gain_mode(dev, CH, BLADERF_GAIN_MGC);
    bladerf_set_gain(dev, CH, 20);
    /* Un canal, SC16_Q11 sin metadatos (el motor de espectro no usa metadatos) */
    st = bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 16, 16384, 8, 5000);
    if (st < 0) return st;
    return bladerf_enable_module(dev, CH, true);
}

int main(int argc, char **argv)
{
    struct bladerf *dev = NULL;
    int st = bladerf_open(&dev, NULL);
    if (st < 0) { fprintf(stderr, "no se pudo abrir el bladeRF: %s\n", bladerf_strerror(st)); return 1; }

    struct bladerf_version v;
    bladerf_fpga_version(dev, &v);
    printf("FPGA %u.%u.%u\n", v.major, v.minor, v.patch);

    fpga_lab_io_t io;
    fpga_lab_io_bladerf(&io, dev);
    fpga_lab_t lab;
    char err[200] = "";
    int pr = fpga_lab_probe(&io, &lab, err, sizeof err);
    printf("detección: %s%s%s\n", pr == 1 ? "IMAGEN LAB" : pr == 0 ? "no es la imagen lab" : "ERROR", err[0] ? " — " : "", err);

    int rc = 0;
    if (argc >= 2 && !strcmp(argv[1], "--iq")) {
        uint64_t f = argc > 2 ? (uint64_t)atoll(argv[2]) : 100000000ull;
        if (setup_rx(dev, f, 2000000) < 0) { fprintf(stderr, "error configurando RX\n"); rc = 1; goto out; }
        int16_t *buf = malloc(sizeof(int16_t) * 2 * 16384);
        double acc = 0; long n = 0; int sat = 0;
        for (int i = 0; i < 120; i++) {
            if (bladerf_sync_rx(dev, buf, 16384, NULL, 5000) < 0) { rc = 1; break; }
            for (int j = 0; j < 2 * 16384; j++) { acc += (double)buf[j] * buf[j]; if (buf[j] >= 2047 || buf[j] <= -2048) sat++; }
            n += 2 * 16384;
        }
        printf("RX normal (rx_mux = 0): RMS = %.1f cuentas (%.1f dBFS por componente), saturaciones = %d\n",
               sqrt(acc / (double)n), 20 * log10(sqrt(acc / (double)n) / 2048.0), sat);
        free(buf);
        bladerf_enable_module(dev, CH, false);
    } else if (pr == 1) {
        if (fpga_lab_selftest(&lab, err, sizeof err) < 0) { printf("selftest SCRATCH: FALLO (%s)\n", err); rc = 1; goto out; }
        printf("selftest SCRATCH: OK; VERSION lab = 0x%08x, FEATURES = 0x%08x\n", lab.version, lab.features);
        if (argc >= 5 && !strcmp(argv[1], "--spectrum")) {
            int log2n = atoi(argv[2]), k = atoi(argv[3]), want = atoi(argv[4]);
            uint64_t f = argc > 5 ? (uint64_t)atoll(argv[5]) : 100000000ull;
            uint32_t rate = argc > 6 ? (uint32_t)atol(argv[6]) : 30720000u;
            if (fpga_lab_config_spectrum(&lab, log2n, k, true) < 0) { fprintf(stderr, "configuración no válida\n"); rc = 1; goto out; }
            fpga_lab_clear_counters(&lab);
            if (setup_rx(dev, f, rate) < 0) { fprintf(stderr, "error configurando RX\n"); rc = 1; goto out; }
            if (fpga_lab_set_spectrum_mode(&lab, true) < 0) { fprintf(stderr, "no se pudo seleccionar rx_mux = 3\n"); rc = 1; goto out; }
            cap_t cap = {.frames = 0, .want = want, .tmp = malloc(sizeof(float) * FPGA_SPEC_NMAX)};
            fpga_lab_parser_t *p = fpga_lab_parser_create(on_frame, &cap);
            int16_t *buf = malloc(sizeof(int16_t) * 2 * 16384);
            for (int guard = 0; cap.frames < want && guard < 20000; guard++) {
                if (bladerf_sync_rx(dev, buf, 16384, NULL, 5000) < 0) { fprintf(stderr, "timeout/error de RX\n"); rc = 1; break; }
                fpga_lab_parser_feed_iq(p, buf, 16384);
            }
            uint64_t fr, sk, gp;
            fpga_lab_parser_stats(p, &fr, &sk, &gp);
            printf("tramas %llu, palabras descartadas %llu, huecos de contador %llu\n", (unsigned long long)fr, (unsigned long long)sk, (unsigned long long)gp);
            bladerf_enable_module(dev, CH, false);
            fpga_lab_set_spectrum_mode(&lab, false); /* vuelve a rx_mux = 0 */
            fpga_lab_parser_destroy(p);
            free(buf);
            free(cap.tmp);
        }
        fpga_lab_counters_t c;
        if (fpga_lab_read_counters(&lab, &c) == 0)
            printf("contadores: muestras=%u fifo_ovf=%u fft=%u espectro=%u tarde=%u palabras=%u activo=%d\n",
                   c.samples, c.fifo_ovf, c.fft_frames, c.spec_frames, c.late, c.words, c.active);
    }
out:
    bladerf_close(dev);
    return rc;
}
