/*
 * test_fpga_lab.c — Pruebas de src/sdr/fpga_lab.c (cliente de la imagen de FPGA "lab").
 *
 *  1. Detección segura con una E/S simulada: la imagen de serie (versión 0.16.0, sin
 *     marca en config_gpio) NO debe provocar ningún acceso Wishbone (en la imagen real
 *     colgaría el Nios); la imagen lab sí se detecta y se prueba el registro SCRATCH.
 *  2. Intérprete de tramas con una VOLCADA DE LA SIMULACIÓN del HDL (Questa,
 *     firmware/sim/run_lab_spec.sh, N = 1024, K = 3, dos tramas de salida con relleno a
 *     2048 palabras) y las palabras esperadas del modelo en C exacto al bit
 *     (firmware/model/spec_model.c): los 1024 bins de cada trama deben coincidir, y
 *     las marcas de tiempo avanzar K*N.
 *  3. Escala a dBFS: la entrada de la simulación son dos tonos complejos (1000 y 40 de
 *     amplitud en cuentas Q11) más ruido gaussiano de σ = 12 por componente; los
 *     niveles medidos deben coincidir con los teóricos y el suelo de ruido con la
 *     fórmula Σw² (mismo criterio que src/dsp/spectrum.c: tono de fondo de escala = 0 dBFS).
 *  4. Resincronización tras pérdida de palabras.
 *
 * Compilación aislada (desde la raíz del proyecto):
 *   gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Isrc -Itests tests/test_fpga_lab.c \
 *       src/sdr/fpga_lab.c -lm -o build/f1/test_fpga_lab && build/f1/test_fpga_lab
 * (ejecutar desde la raíz del proyecto: lee los .hex de tests/data)
 */
#include "test_util.h"

#include "sdr/fpga_lab.h"

/* ---- E/S simulada -------------------------------------------------------- */
typedef struct {
    uint32_t patch;      /* parche de la versión de FPGA */
    uint32_t gpio;       /* lectura de config_gpio */
    bool lab_bus;        /* el bus Wishbone responde como la lab */
    int wb_accesses;     /* accesos Wishbone realizados */
    uint32_t scratch;
    uint32_t cfg[16];
} mock_t;

static int m_ver(void *c, uint32_t *ma, uint32_t *mi, uint32_t *pa)
{
    mock_t *m = c;
    *ma = 0; *mi = 16; *pa = m->patch;
    return 0;
}
static int m_gr(void *c, uint32_t *v) { *v = ((mock_t *)c)->gpio; return 0; }
static int m_gw(void *c, uint32_t v) { ((mock_t *)c)->gpio = (v & 0x0FFFFFFFu) | (((mock_t *)c)->gpio & 0xF0000000u); return 0; }
static int m_wr(void *c, uint32_t a, uint32_t *v)
{
    mock_t *m = c;
    m->wb_accesses++;
    switch (a / 4) {
    case 0: *v = 0x4C414231u; break;
    case 1: *v = 0x00010001u; break;
    case 2: *v = 0x00000DA1u; break;
    case 3: *v = m->scratch; break;
    case 4: *v = ~m->scratch; break;
    default: *v = m->cfg[(a / 4) & 15]; break;
    }
    return 0;
}
static int m_ww(void *c, uint32_t a, uint32_t v)
{
    mock_t *m = c;
    m->wb_accesses++;
    if (a / 4 == 3) m->scratch = v; else m->cfg[(a / 4) & 15] = v;
    return 0;
}

static fpga_lab_io_t mock_io(mock_t *m)
{
    fpga_lab_io_t io = {m, m_ver, m_gr, m_gw, m_wr, m_ww};
    return io;
}

static void test_probe(void)
{
    char err[160];
    fpga_lab_t lab;
    mock_t stock = {.patch = 0, .gpio = 0x00000000};
    fpga_lab_io_t io = mock_io(&stock);
    CHECK_EQ(fpga_lab_probe(&io, &lab, err, sizeof err), 0);
    CHECK_EQ(stock.wb_accesses, 0); /* imagen de serie: el bus Wishbone no se toca */
    CHECK(!lab.present);

    /* versión engañosa pero sin marca de config_gpio: tampoco se toca el bus */
    mock_t fake = {.patch = 101, .gpio = 0x00000000};
    io = mock_io(&fake);
    CHECK_EQ(fpga_lab_probe(&io, &lab, err, sizeof err), 0);
    CHECK_EQ(fake.wb_accesses, 0);

    /* marca presente pero versión de serie */
    mock_t fake2 = {.patch = 0, .gpio = 0x0A000000};
    io = mock_io(&fake2);
    CHECK_EQ(fpga_lab_probe(&io, &lab, err, sizeof err), 0);
    CHECK_EQ(fake2.wb_accesses, 0);

    mock_t labm = {.patch = 101, .gpio = 0x0A000100 | (4u << 8)};
    io = mock_io(&labm);
    CHECK_EQ(fpga_lab_probe(&io, &lab, err, sizeof err), 1);
    CHECK(lab.present);
    CHECK_EQ(lab.version, 0x00010001u);
    CHECK_EQ(fpga_lab_selftest(&lab, err, sizeof err), 0);
    CHECK_EQ(fpga_lab_config_spectrum(&lab, 12, 32, true), 0);
    CHECK_EQ(labm.cfg[5], 12u);
    CHECK_EQ(labm.cfg[6], 32u);
    CHECK_EQ(labm.cfg[7], 1u);
    CHECK(fpga_lab_config_spectrum(&lab, 9, 1, true) < 0);  /* N fuera de rango */
    CHECK(fpga_lab_config_spectrum(&lab, 12, 0, true) < 0); /* K = 0 */
    CHECK_EQ(fpga_lab_set_spectrum_mode(&lab, true), 0);
    CHECK_EQ((labm.gpio >> 8) & 7u, 3u);
    CHECK_EQ(labm.gpio & 0x0F000000u, 0x0A000000u & 0x0F000000u);
    CHECK_EQ(fpga_lab_set_spectrum_mode(&lab, false), 0);
    CHECK_EQ((labm.gpio >> 8) & 7u, 0u);
}

/* ---- Intérprete con la volcada del simulador ----------------------------- */
#define MAXW 20000
static uint32_t dump_w[MAXW], exp_w[MAXW];

static int load_hex(const char *path, uint32_t *w)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned v;
    int n = 0;
    while (n < MAXW && fscanf(f, "%x", &v) == 1) w[n++] = v;
    fclose(f);
    return n;
}

typedef struct {
    int count;
    uint64_t ts[4];
    float db[4][1024];
    uint32_t power[4][1024];
    int k, n;
    uint32_t group[4];
} cap_t;

static void on_frame(void *ctx, const fpga_lab_frame_t *f)
{
    cap_t *c = ctx;
    if (c->count < 4) {
        c->ts[c->count] = f->ts;
        c->n = f->n;
        c->k = f->k;
        c->group[c->count] = f->group;
        memcpy(c->power[c->count], f->power, sizeof(uint32_t) * (size_t)f->n);
        fpga_lab_frame_to_dbfs(f, c->db[c->count]);
    }
    c->count++;
}

static void test_dump(void)
{
    int nd = load_hex("tests/data/lab_spec_n1024_k3_sim_dump.hex", dump_w);
    int ne = load_hex("tests/data/lab_spec_n1024_k3_model.hex", exp_w);
    CHECK(nd == 4096 && ne == 4096);
    if (nd != 4096 || ne != 4096) return;

    cap_t cap;
    memset(&cap, 0, sizeof cap);
    fpga_lab_parser_t *p = fpga_lab_parser_create(on_frame, &cap);
    /* se alimenta en trozos irregulares para probar el estado entre llamadas */
    for (int i = 0; i < nd;) {
        int m = 1 + (i * 37) % 1500;
        if (i + m > nd) m = nd - i;
        fpga_lab_parser_feed_words(p, dump_w + i, (size_t)m);
        i += m;
    }
    uint64_t frames, skipped, gaps;
    fpga_lab_parser_stats(p, &frames, &skipped, &gaps);
    CHECK_EQ(cap.count, 2);
    CHECK_EQ(frames, 2);
    CHECK_EQ(skipped, 0);
    CHECK_EQ(gaps, 0);
    CHECK_EQ(cap.n, 1024);
    CHECK_EQ(cap.k, 3);
    CHECK_EQ(cap.group[0], 0);
    CHECK_EQ(cap.group[1], 1);
    CHECK_EQ(cap.ts[1] - cap.ts[0], 3 * 1024);

    /* bins exactos al modelo (las palabras 8.. de cada trama de 2048) */
    int bad = 0;
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < 1024; i++)
            if (cap.power[f][i] != exp_w[f * 2048 + 8 + i]) bad++;
    CHECK_EQ(bad, 0);

    /* Escala: tono 1 de amplitud 1000 a 0,1013 ciclos/muestra; tono 2 de amplitud 40 a -0,2371. */
    const float *db = cap.db[1];
    int pk = 0;
    for (int i = 0; i < 1024; i++) if (db[i] > db[pk]) pk = i;
    const int expect_pk = 512 + (int)lround(0.1013 * 1024);
    CHECK(abs(pk - expect_pk) <= 1);
    const double lvl1 = 20 * log10(1000.0 / 2048.0);
    /* con el tono entre dos bins, la pérdida por escalón (BH) es < 1,1 dB; se suman los 3 bins centrales */
    double e1 = 0;
    for (int i = pk - 3; i <= pk + 3; i++) e1 += pow(10.0, db[i] / 10.0);
    printf("  tono 1: pico %.2f dBFS (bin %d), potencia 7 bins %.2f dBFS, teórico %.2f dBFS\n", db[pk], pk - 512, 10 * log10(e1), lvl1);
    CHECK(db[pk] < lvl1 + 0.2 && db[pk] > lvl1 - 1.3);
    /* La potencia total de la ventana sobre los bins del tono = A^2 * Σw^2/(Σw)^2 relativo; se comprueba con ENBW */
    double sw = fpga_lab_window_sum(1024), sw2 = 0;
    {
        for (int i = 0; i < 1024; i++) {
            double x = 2.0 * M_PI * i / 1023.0;
            double w = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
            sw2 += w * w;
        }
    }
    /* potencia total del tono = N·A²·Σw² / (2048·Σw)² (teorema de Parseval con la ventana) */
    const double tot1 = 10 * log10(1024.0 * 1000.0 * 1000.0 * sw2 / (2048.0 * sw * 2048.0 * sw));
    CHECK_NEAR(10 * log10(e1), tot1, 0.3);

    int pk2 = 512 + (int)lround(-0.2371 * 1024);
    float best2 = -200;
    for (int i = pk2 - 3; i <= pk2 + 3; i++) if (db[i] > best2) best2 = db[i];
    const double lvl2 = 20 * log10(40.0 / 2048.0);
    printf("  tono 2: pico %.2f dBFS, teórico %.2f dBFS\n", best2, lvl2);
    CHECK(best2 < lvl2 + 0.2 && best2 > lvl2 - 1.3);

    /* Suelo de ruido: ruido complejo de potencia 2σ² (+1/12 de cuantización por componente);
     * E|FFT|² = 2σ²·Σw²  => nivel en dBFS = 10·log10(2σ²·Σw²) − 20·log10(2048·Σw) */
    const double sig2 = 2.0 * (144.0 + 1.0 / 12.0);
    const double floor_db = 10 * log10(sig2 * sw2) - 20 * log10(2048.0 * sw);
    double acc = 0;
    int cnt = 0;
    for (int i = 0; i < 1024; i++) {
        if (abs(i - (512 + (int)lround(0.1013 * 1024))) < 30 || abs(i - pk2) < 30) continue;
        acc += pow(10.0, db[i] / 10.0);
        cnt++;
    }
    const double meas = 10 * log10(acc / cnt);
    printf("  suelo de ruido: medido %.2f dBFS/bin, teórico %.2f dBFS/bin (%d bins)\n", meas, floor_db, cnt);
    CHECK_NEAR(meas, floor_db, 0.5);
    /* conversión directa de una potencia: acc = |X_hw|^2*K con un tono de fondo de escala */
    {
        const double wsum = fpga_lab_window_sum(1024);
        const double xhw = 0.5 * 2048.0 * wsum; /* |X_hw| de un tono de amplitud 2048 */
        CHECK_NEAR(fpga_lab_power_to_dbfs(xhw * xhw * 5, 1024, 5), 0.0, 1e-9);
    }

    /* Resincronización: se pierden 700 palabras en medio de la primera trama */
    cap.count = 0;
    fpga_lab_parser_t *q = fpga_lab_parser_create(on_frame, &cap);
    fpga_lab_parser_feed_words(q, dump_w, 500);
    fpga_lab_parser_feed_words(q, dump_w + 1200, (size_t)(nd - 1200));
    fpga_lab_parser_stats(q, &frames, &skipped, &gaps);
    CHECK_EQ(cap.count, 1); /* solo la segunda trama se recupera */
    CHECK(skipped > 0);
    CHECK_EQ(cap.group[0], 1);
    fpga_lab_parser_destroy(q);
    fpga_lab_parser_destroy(p);

    /* Conversión de palabras {E,M} */
    CHECK_NEAR(fpga_lab_power_value((3u << 26) | 5u), 40.0, 0);
    CHECK_NEAR(fpga_lab_power_value(0), 0.0, 0);
}

int main(void)
{
    test_probe();
    test_dump();
    return TEST_SUMMARY();
}
