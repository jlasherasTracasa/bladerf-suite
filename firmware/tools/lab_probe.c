/*
 * lab_probe.c — Herramienta de prueba en hardware de la imagen de FPGA "lab".
 * NO la ejecutan los agentes: la usa el orquestador, con permiso del usuario, tras
 * cargar la imagen EN RAM con `bladeRF-cli -l firmware/out/hosted-xA9-lab-0.16.101.rbf`
 * (nunca -L). No transmite, no escribe la flash, no carga nada en la FPGA.
 *
 *   lab_probe                       versión de FPGA y detección segura de la imagen lab;
 *                                   si es lab: prueba del registro SCRATCH y contadores.
 *                                   Con la imagen de serie NO toca el bus Wishbone.
 *   lab_probe --regs                (solo lab) vuelca los 16 registros (configuración, contadores, estado)
 *                                   y config_gpio; sirve para diagnosticar la configuración.
 *   lab_probe --iq [freq_hz] [tasa_sps] [n_búferes]  (cualquier imagen) RX normal (rx_mux = 0) con caudal medido:
 *                                   nivel RMS y saturación; sirve para comparar lab y serie.
 *   lab_probe --spectrum log2n k tramas [freq_hz] [tasa_sps] [--quiet] [--every M]
 *                                   (solo lab) activa rx_mux = 3, lee 'tramas' tramas de espectro
 *                                   y muestra el pico y el suelo en dBFS; al terminar vuelve a rx_mux = 0.
 *                                   --quiet: sin E/S por trama (solo el resumen con el caudal medido).
 *                                   --late-mux: habilita RX con rx_mux = 0, lee unos buffers y entonces pasa a rx_mux = 3
 *                                   (el flujo USB ya está en marcha cuando arranca el motor; descarta palabras normales).
 *                                   --every M: analiza/imprime 1 de cada M tramas (todas se cuentan). Por defecto
 *                                   M = 1; con --quiet, M = 1000 (el análisis completo de cada trama, 1024 log10 y un
 *                                   qsort, no sostiene más de ~6-10 Mpalabras/s en un hilo: medido con bench_parser).
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
#include <time.h>

#define CH BLADERF_CHANNEL_RX(0)

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

typedef struct {
    int frames, want;
    int every, quiet;
    unsigned long long words;
    float *tmp, *db;
    double last_peak_db, last_med_db;
    int last_peak_bin, have_last;
} cap_t;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Todas las tramas se cuentan; solo 1 de cada 'every' se convierte a dBFS (log10 por bin + mediana). */
static void on_frame(void *ctx, const fpga_lab_frame_t *f)
{
    cap_t *c = ctx;
    c->frames++;
    c->words += f->total_words;
    if (c->every > 1 && (c->frames % c->every) != 1) return;
    fpga_lab_frame_to_dbfs(f, c->db);
    int pk = 0;
    for (int i = 0; i < f->n; i++) if (c->db[i] > c->db[pk]) pk = i;
    memcpy(c->tmp, c->db, sizeof(float) * (size_t)f->n);
    qsort(c->tmp, (size_t)f->n, sizeof(float), cmp_float);
    c->last_peak_db = c->db[pk];
    c->last_peak_bin = pk - f->n / 2;
    c->last_med_db = c->tmp[f->n / 2];
    c->have_last = 1;
    if (!c->quiet)
        printf("trama %u  N=%d K=%d  ts=%llu%s  pico %.1f dBFS en el bin %d  mediana %.1f dBFS\n",
               f->group, f->n, f->k, (unsigned long long)f->ts, f->dropped ? " (perdida previa)" : "",
               c->db[pk], pk - f->n / 2, c->tmp[f->n / 2]);
}

static const char *const REGN[16] = {"ID", "VERSION", "FEATURES", "SCRATCH", "SCRATCH_N", "SPEC_LOG2N", "SPEC_K", "SPEC_CTRL",
                                     "CNT_SAMPLES", "CNT_FIFO_OVF", "CNT_FFT", "CNT_SPEC", "CNT_LATE", "CNT_WORDS", "STATUS", "CLEAR"};

static void dump_regs(const fpga_lab_t *lab, const char *when)
{
    printf("-- registros lab (%s)\n", when);
    for (int i = 0; i < 16; i++) {
        uint32_t v = 0;
        if (fpga_lab_reg_read(lab, i, &v) < 0) { printf("  [%2d] %-13s ERROR de lectura\n", i, REGN[i]); continue; }
        printf("  [%2d] %-13s 0x%08x (%u)\n", i, REGN[i], v, v);
    }
    uint32_t g = 0;
    if (lab->io.gpio_read(lab->io.ctx, &g) == 0)
        printf("  config_gpio = 0x%08x (rx_mux = %u, marca lab = 0x%x)\n", g, (g >> 8) & 7u, (g >> 24) & 0xFu);
}

/* Comprueba que SPEC_LOG2N/K/CTRL contienen lo escrito; devuelve 0 si coinciden. */
static int check_cfg(const fpga_lab_t *lab, int log2n, int k, const char *when)
{
    uint32_t a = 0, b = 0, c = 0;
    fpga_lab_reg_read(lab, FPGA_LAB_REG_LOG2N, &a);
    fpga_lab_reg_read(lab, FPGA_LAB_REG_K, &b);
    fpga_lab_reg_read(lab, FPGA_LAB_REG_CTRL, &c);
    int ok = (a == (uint32_t)log2n && b == (uint32_t)k);
    printf("configuración leída (%s): log2n=%u k=%u ctrl=%u -> %s\n", when, a, b, c, ok ? "coincide" : "NO COINCIDE con lo escrito");
    return ok ? 0 : 1;
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
    st = bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 64, 32768, 16, 5000);
    if (st < 0) return st;
    return 0;
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
    if (argc >= 2 && !strcmp(argv[1], "--regs")) {
        if (pr == 1) dump_regs(&lab, "ahora");
        else printf("no es la imagen lab: no se leen registros\n");
    } else if (argc >= 2 && !strcmp(argv[1], "--iq")) {
        /* RX normal (rx_mux = 0) con los mismos búferes que --spectrum: mide el caudal USB de referencia. */
        uint64_t f = argc > 2 ? (uint64_t)atoll(argv[2]) : 100000000ull;
        uint32_t rate = argc > 3 ? (uint32_t)atol(argv[3]) : 2000000u;
        int nch = argc > 4 ? atoi(argv[4]) : 120;
        const size_t chunk = 32768;
        if (setup_rx(dev, f, rate) < 0 || bladerf_enable_module(dev, CH, true) < 0) { fprintf(stderr, "error configurando RX\n"); rc = 1; goto out; }
        int16_t *buf = malloc(sizeof(int16_t) * 2 * chunk);
        double acc = 0; long n = 0; int sat = 0;
        const double t0 = now_s();
        for (int i = 0; i < nch; i++) {
            if (bladerf_sync_rx(dev, buf, (unsigned)chunk, NULL, 5000) < 0) { fprintf(stderr, "timeout/error de RX\n"); rc = 1; break; }
            for (size_t j = 0; j < 2 * chunk; j++) { acc += (double)buf[j] * buf[j]; if (buf[j] >= 2047 || buf[j] <= -2048) sat++; }
            n += (long)(2 * chunk);
        }
        const double dt = now_s() - t0;
        printf("caudal RX normal (rx_mux = 0, tasa %u sps): %.3f Mmuestras/s (%.1f MB/s) en %.2f s\n", rate, (double)n / 2 / dt / 1e6, (double)n * 2 / dt / 1e6, dt);
        printf("RX normal (rx_mux = 0): RMS = %.1f cuentas (%.1f dBFS por componente), saturaciones = %d\n",
               sqrt(acc / (double)n), 20 * log10(sqrt(acc / (double)n) / 2048.0), sat);
        free(buf);
        bladerf_enable_module(dev, CH, false);
    } else if (pr == 1) {
        if (fpga_lab_selftest(&lab, err, sizeof err) < 0) { printf("selftest SCRATCH: FALLO (%s)\n", err); rc = 1; goto out; }
        printf("selftest SCRATCH: OK; VERSION lab = 0x%08x, FEATURES = 0x%08x\n", lab.version, lab.features);
        if (argc >= 5 && !strcmp(argv[1], "--spectrum")) {
            int log2n = atoi(argv[2]), k = atoi(argv[3]), want = atoi(argv[4]);
            uint64_t f = 100000000ull;
            uint32_t rate = 30720000u;
            int quiet = 0, every = 0, pos = 0, late_mux = 1;
            for (int a = 5; a < argc; a++) {
                if (!strcmp(argv[a], "--quiet")) quiet = 1;
                else if (!strcmp(argv[a], "--early-mux")) late_mux = 0;
                else if (!strcmp(argv[a], "--late-mux")) late_mux = 1;
                else if (!strcmp(argv[a], "--every") && a + 1 < argc) every = atoi(argv[++a]);
                else if (pos == 0) { f = (uint64_t)atoll(argv[a]); pos++; }
                else if (pos == 1) { rate = (uint32_t)atol(argv[a]); pos++; }
            }
            if (every < 1) every = quiet ? 1000 : 1;
            if (fpga_lab_config_spectrum(&lab, log2n, k, true) < 0) { fprintf(stderr, "configuración no válida\n"); rc = 1; goto out; }
            fpga_lab_clear_counters(&lab);
            check_cfg(&lab, log2n, k, "tras escribir");
            if (setup_rx(dev, f, rate) < 0) { fprintf(stderr, "error configurando RX\n"); rc = 1; goto out; }
            check_cfg(&lab, log2n, k, "tras configurar el flujo"); /* el FX3 pulsa sys_reset al preparar el flujo */
            /* Todo lo que cuesta tiempo se hace ANTES de habilitar RX: el motor empieza a emitir en cuanto
             * se habilita el módulo y la FIFO de la FPGA (16384 palabras) se llena en unos ms; cualquier
             * lectura Wishbone (ms) entre enable_module y el primer sync_rx pierde tramas. */
            cap_t cap = {.frames = 0, .want = want, .every = every, .quiet = quiet,
                         .tmp = malloc(sizeof(float) * FPGA_SPEC_NMAX), .db = malloc(sizeof(float) * FPGA_SPEC_NMAX)};
            fpga_lab_parser_t *p = fpga_lab_parser_create(on_frame, &cap);
            const size_t chunk = 32768;
            int16_t *buf = malloc(sizeof(int16_t) * 2 * chunk);
            /* rx_mux = 3 ANTES de habilitar el módulo: no llegan muestras normales y no hay que descartar palabras */
            if (!late_mux && fpga_lab_set_spectrum_mode(&lab, true) < 0) { fprintf(stderr, "no se pudo seleccionar rx_mux = 3\n"); rc = 1; goto out; }
            if (bladerf_enable_module(dev, CH, true) < 0) { fprintf(stderr, "no se pudo habilitar RX\n"); rc = 1; goto out; }
            if (late_mux) {
                for (int i = 0; i < 8; i++) bladerf_sync_rx(dev, buf, (unsigned)chunk, NULL, 5000);
                if (fpga_lab_set_spectrum_mode(&lab, true) < 0) { fprintf(stderr, "no se pudo seleccionar rx_mux = 3\n"); rc = 1; goto out; }
            }
            const double t0 = now_s();
            for (int guard = 0; cap.frames < want && guard < 2000000; guard++) {
                if (bladerf_sync_rx(dev, buf, (unsigned)chunk, NULL, 5000) < 0) { fprintf(stderr, "timeout/error de RX\n"); rc = 1; break; }
                fpga_lab_parser_feed_iq(p, buf, chunk);
            }
            const double dt = now_s() - t0;
            bladerf_enable_module(dev, CH, false);
            fpga_lab_set_spectrum_mode(&lab, false); /* vuelve a rx_mux = 0 */
            uint64_t fr, sk, gp;
            fpga_lab_parser_stats(p, &fr, &sk, &gp);
            if (cap.have_last)
                printf("última trama analizada: pico %.1f dBFS en el bin %d, mediana %.1f dBFS\n", cap.last_peak_db, cap.last_peak_bin, cap.last_med_db);
            printf("tramas %llu, palabras descartadas %llu, huecos de contador %llu\n", (unsigned long long)fr, (unsigned long long)sk, (unsigned long long)gp);
            printf("caudal medido: %.3f Mpalabras/s (%.1f MB/s) en %.2f s; %.1f tramas/s\n", (double)cap.words / dt / 1e6, (double)cap.words * 4 / dt / 1e6, dt,
                   (double)cap.frames / dt);
            check_cfg(&lab, log2n, k, "al terminar");
            if (!quiet) dump_regs(&lab, "al terminar");
            fpga_lab_parser_destroy(p);
            free(buf);
            free(cap.tmp);
            free(cap.db);
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
