/*
 * test_source.c — Pruebas de src/sdr/source.c (fuente de fichero, modos de
 * ganancia, conversión SC8→Q11) y de src/sdr/sweep.c (barrido con un radio
 * simulado). No necesita hardware.
 *
 * El radio simulado de sweep imita lo que importa del bladeRF 2.0:
 *   - reloj de muestras (timestamp) que avanza con la lectura, con un adelanto
 *     fijo del FPGA sobre el lector ("lag", el colchón USB);
 *   - retunes programados que se ejecutan en su marca de tiempo (si la marca ya
 *     pasó, se ejecutan "ya" y se cuentan como tardíos), con cola limitada;
 *   - unos cuantos "milisegundos" de basura tras cada cambio de LO;
 *   - tonos en frecuencias absolutas: el contenido de cada muestra depende del
 *     LO vigente en ese instante.
 */
#include "sdr/source.h"
#include "sdr/sweep.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/*  Ficheros temporales                                                   */
/* ---------------------------------------------------------------------- */
static char g_dir[64];

static void write_file(const char *name, const void *data, size_t n)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) {
        printf("  no se puede crear %s\n", p);
        tu_fails++;
        return;
    }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void path_of(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", g_dir, name); }

static source_t *open_file(const char *name, uint32_t rate, int64_t center)
{
    char p[160], err[200] = "";
    path_of(p, sizeof p, name);
    source_t *s = source_open_file(p, rate, center, err, sizeof err);
    if (!s)
        printf("  open(%s): %s\n", name, err);
    return s;
}

/* ---------------------------------------------------------------------- */
/*  Fuente de fichero                                                     */
/* ---------------------------------------------------------------------- */
static void test_gain_modes(void)
{
    printf("modos de ganancia\n");
    const char *names[] = {"auto", "manual", "fast", "slow", "hybrid"};
    for (int i = 0; i < 5; i++) {
        gain_mode_t m = GAIN_AUTO;
        CHECK(gain_mode_parse(names[i], &m));
        CHECK_EQ(m, i);
        CHECK(strcmp(gain_mode_name(m), names[i]) == 0);
    }
    gain_mode_t m = GAIN_AUTO;
    CHECK(gain_mode_parse("MGC", &m) && m == GAIN_MANUAL);
    CHECK(gain_mode_parse("agc_fast", &m) && m == GAIN_FAST);
    CHECK(!gain_mode_parse("turbo", &m));
}

static void test_formats(void)
{
    printf("formatos de fichero\n");
    const int16_t cs16[8] = {100, -100, 2047, -2048, 0, 1, -1, 1234};
    write_file("a.cs16", cs16, sizeof cs16);
    const int8_t cs8[8] = {1, -1, 127, -128, 0, 5, -5, 64};
    write_file("b.cs8", cs8, sizeof cs8);
    const uint8_t cu8[8] = {128, 129, 255, 0, 127, 200, 50, 128};
    write_file("c.cu8", cu8, sizeof cu8);

    int16_t o[8];
    source_t *s = open_file("a.cs16", 48000, 100000000);
    CHECK(s != NULL);
    if (s) {
        const source_info_t *in = source_info(s);
        CHECK(strcmp(in->kind, "file") == 0);
        CHECK(!in->can_tune);
        CHECK_EQ(source_rate(s), 48000);
        CHECK_EQ(source_frequency(s), 100000000);
        CHECK(strcmp(source_format(s), "cs16") == 0);
        CHECK(!source_oversample(s));
        double off;
        CHECK(source_dbm_offset(s, &off) < 0); /* sin hardware no hay calibración */
        CHECK(source_start(s) == 0);
        CHECK(source_read(s, o, 4, NULL) == 0);
        CHECK(memcmp(o, cs16, sizeof o) == 0); /* cs16 crudo = Q11 tal cual */
        source_close(s);
    }
    s = open_file("b.cs8", 48000, 0);
    if (s) {
        CHECK(strcmp(source_format(s), "cs8") == 0);
        source_start(s);
        CHECK(source_read(s, o, 4, NULL) == 0);
        for (int i = 0; i < 8; i++)
            CHECK_EQ(o[i], cs8[i] * 16); /* int8 → Q11: ×16 */
        source_close(s);
    }
    s = open_file("c.cu8", 48000, 0);
    if (s) {
        CHECK(strcmp(source_format(s), "cu8") == 0);
        source_start(s);
        CHECK(source_read(s, o, 4, NULL) == 0);
        for (int i = 0; i < 8; i++)
            CHECK_EQ(o[i], ((int)cu8[i] - 128) * 16); /* cu8 con sesgo 128 */
        source_close(s);
    }
    /* Sin tasa ni metadatos: error claro */
    char err[200] = "", p[160];
    path_of(p, sizeof p, "a.cs16");
    CHECK(source_open_file(p, 0, 0, err, sizeof err) == NULL);
    CHECK(strstr(err, "tasa") != NULL);
    CHECK(source_open_file("/no/existe.cs16", 1000, 0, err, sizeof err) == NULL);
}

static void test_sigmf(void)
{
    printf("SigMF\n");
    /* ci16_le con la escala propia de la app (full_scale 2048): se entrega tal cual */
    const int16_t d16[8] = {10, -10, 2000, -2000, 3, 4, 5, 6};
    write_file("q.sigmf-data", d16, sizeof d16);
    const char *meta_q =
        "{\"global\":{\"core:datatype\":\"ci16_le\",\"core:sample_rate\":250000,\"bladerf_lab:full_scale\":2048},"
        "\"captures\":[{\"core:sample_start\":0,\"core:frequency\":433920000}],\"annotations\":[]}";
    write_file("q.sigmf-meta", meta_q, strlen(meta_q));
    /* ci16_le de otra herramienta (escala de 16 bit): se divide entre 16 */
    write_file("f.sigmf-data", d16, sizeof d16);
    const char *meta_f =
        "{\"global\":{\"core:datatype\":\"ci16_le\",\"core:sample_rate\":96000},"
        "\"captures\":[{\"core:sample_start\":0,\"core:frequency\":868000000}]}";
    write_file("f.sigmf-meta", meta_f, strlen(meta_f));
    /* ci8 */
    const int8_t d8[8] = {1, 2, 3, 4, -1, -2, -3, -128};
    write_file("g.sigmf-data", d8, sizeof d8);
    const char *meta_8 =
        "{\"global\":{\"core:datatype\":\"ci8\",\"core:sample_rate\":10000},"
        "\"captures\":[{\"core:frequency\":1090000000}]}";
    write_file("g.sigmf-meta", meta_8, strlen(meta_8));
    /* tipo no soportado */
    const char *meta_x = "{\"global\":{\"core:datatype\":\"cf32_le\",\"core:sample_rate\":1000},\"captures\":[{}]}";
    write_file("x.sigmf-meta", meta_x, strlen(meta_x));
    write_file("x.sigmf-data", d16, sizeof d16);

    int16_t o[8];
    source_t *s = open_file("q.sigmf-meta", 1, 0); /* tasa y centro salen de los metadatos */
    CHECK(s != NULL);
    if (s) {
        CHECK_EQ(source_rate(s), 250000);
        CHECK_EQ(source_frequency(s), 433920000);
        source_start(s);
        CHECK(source_read(s, o, 4, NULL) == 0);
        CHECK(memcmp(o, d16, sizeof o) == 0);
        source_close(s);
    }
    s = open_file("f.sigmf-data", 1, 0); /* también se acepta apuntar al .sigmf-data */
    if (s) {
        CHECK_EQ(source_rate(s), 96000);
        CHECK_EQ(source_frequency(s), 868000000);
        source_start(s);
        CHECK(source_read(s, o, 4, NULL) == 0);
        for (int i = 0; i < 8; i++)
            CHECK_EQ(o[i], d16[i] / 16);
        source_close(s);
    }
    s = open_file("g.sigmf-meta", 1, 0);
    if (s) {
        CHECK_EQ(source_frequency(s), 1090000000);
        source_start(s);
        CHECK(source_read(s, o, 4, NULL) == 0);
        for (int i = 0; i < 8; i++)
            CHECK_EQ(o[i], d8[i] * 16);
        source_close(s);
    }
    char err[200] = "", p[160];
    path_of(p, sizeof p, "x.sigmf-meta");
    CHECK(source_open_file(p, 0, 0, err, sizeof err) == NULL);
    CHECK(strstr(err, "cf32_le") != NULL);
}

static void test_sc8(void)
{
    printf("conversión SC8 → Q11\n");
    int8_t in[512];
    int16_t out[512];
    for (int i = 0; i < 512; i++)
        in[i] = (int8_t)(i - 256 + (i > 383 ? 0 : 0)); /* recorre −128..127 dos veces por truncado */
    for (int i = 0; i < 256; i++)
        in[i] = (int8_t)(i - 128);
    for (int i = 256; i < 512; i++)
        in[i] = (int8_t)(127 - (i - 256));
    source_sc8_to_q11(in, out, 512);
    for (int i = 0; i < 512; i++)
        CHECK_EQ(out[i], in[i] * 16);
    CHECK_EQ(out[0], -2048); /* −128 → −2048: el fondo de escala negativo se conserva */
    CHECK_EQ(out[255], 2032); /* 127 → 2032 (el positivo pierde 1 LSB de Q7) */
    /* Un tono de amplitud ±1,0 en Q7 conserva su nivel en dBFS (±1 LSB de Q7 = 0,07 dB) */
    double pin = 0, pout = 0;
    for (int i = 0; i < 512; i++) {
        pin += (double)in[i] * in[i] / (128.0 * 128.0);
        pout += (double)out[i] * out[i] / (2048.0 * 2048.0);
    }
    CHECK_NEAR(pin, pout, 1e-9);
}

/* ---------------------------------------------------------------------- */
/*  Formatos de RX por el USB (F11c)                                      */
/* ---------------------------------------------------------------------- */
/* Modelo independiente de la FPGA (rx_lab.vhd, gain8_comb + pack_eight_bit_mode): x12 de 12 bit → ×2^s,
 * saturado a 12 bit con signo → 8 bit altos (desplazamiento aritmético, es decir, truncado). */
static int8_t fpga_sc8(int x12, unsigned s, int *clipped)
{
    int g = x12 * (1 << s);
    *clipped = g > 2047 || g < -2048;
    g = g > 2047 ? 2047 : g < -2048 ? -2048 : g;
    return (int8_t)(g >> 4);
}

static void test_rxfmt_names_and_choice(void)
{
    printf("formatos RX: nombres y elección automática\n");
    const char *n[] = {"auto", "sc16", "packed12", "sc8g"};
    for (int i = 0; i < 4; i++) {
        rxfmt_t f = RXFMT_AUTO;
        CHECK(rxfmt_parse(n[i], &f));
        CHECK_EQ(f, i);
        CHECK(strcmp(rxfmt_name(f), n[i]) == 0);
    }
    rxfmt_t f;
    CHECK(rxfmt_parse("PACKED12", &f) && f == RXFMT_PACKED12);
    CHECK(!rxfmt_parse("sc8", &f) && !rxfmt_parse(NULL, &f));

    /* USB 2 (45 MB/s): SC16 hasta 11,25 Msps, packed12 hasta 15, sc8g después */
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 2000000, 2, true, true), RXFMT_SC16);
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 11250000, 2, true, true), RXFMT_SC16);
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 11250001, 2, true, true), RXFMT_PACKED12);
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 15000000, 2, true, true), RXFMT_PACKED12);
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 15360000, 2, true, true), RXFMT_SC8G);
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 30720000, 2, true, true), RXFMT_SC8G);
    /* Sin imagen lab: lo más estrecho disponible es packed12; sin packed12, SC16 */
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 20000000, 2, true, false), RXFMT_PACKED12);
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 20000000, 2, false, false), RXFMT_SC16);
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 20000000, 2, false, true), RXFMT_SC8G);
    /* USB 3 (o desconocido): SC16 en toda la gama nativa */
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 61440000, 3, true, true), RXFMT_SC16);
    CHECK_EQ(source_choose_format(RXFMT_AUTO, 61440000, 0, true, true), RXFMT_SC16);
    /* Explícito: se respeta si está disponible; si no, el más próximo */
    CHECK_EQ(source_choose_format(RXFMT_SC16, 30000000, 2, true, true), RXFMT_SC16);
    CHECK_EQ(source_choose_format(RXFMT_PACKED12, 1000000, 3, true, true), RXFMT_PACKED12);
    CHECK_EQ(source_choose_format(RXFMT_SC8G, 1000000, 3, true, true), RXFMT_SC8G);
    CHECK_EQ(source_choose_format(RXFMT_SC8G, 1000000, 3, true, false), RXFMT_PACKED12);
    CHECK_EQ(source_choose_format(RXFMT_PACKED12, 1000000, 3, false, false), RXFMT_SC16);
}

static void test_sc8g_conversion(void)
{
    printf("conversión SC8 con GAIN8 → Q11 (bit a bit contra el modelo)\n");
    int8_t in[256];
    int16_t out[256];
    for (int i = 0; i < 256; i++)
        in[i] = (int8_t)(i - 128);
    for (unsigned sh = 0; sh <= 3; sh++) {
        int pk = source_sc8g_to_q11(in, out, 256, sh);
        CHECK_EQ(pk, 128);
        int bad = 0;
        for (int i = 0; i < 256; i++) {
            /* modelo: v = ((b·16 + 8) / 2^s) redondeado al más próximo, con división en coma flotante */
            double v = floor(((double)in[i] * 16 + 8) / (double)(1u << sh) + 0.5);
            if (out[i] != (int16_t)v)
                bad++;
        }
        CHECK_EQ(bad, 0);
    }
    /* Sin ganancia: ±½ LSB de Q7 respecto a la conversión de serie (×16, sin sesgo) */
    source_sc8g_to_q11(in, out, 256, 0);
    for (int i = 0; i < 256; i++)
        CHECK_EQ(out[i], in[i] * 16 + 8);
    /* Ida y vuelta con el modelo de la FPGA: entrada de 12 bit sin recorte → error ≤ 8/2^s (+1 por redondeo) */
    uint32_t r = 12345;
    for (unsigned sh = 0; sh <= 3; sh++) {
        long clip = 0, over = 0, n = 20000;
        for (long k = 0; k < n; k++) {
            r = r * 1664525u + 1013904223u;
            int x = (int)((r >> 8) % 4096u) - 2048;
            int c;
            int8_t b = fpga_sc8(x, sh, &c);
            int16_t y;
            source_sc8g_to_q11(&b, &y, 1, sh);
            if (c) {
                clip++;
                continue;
            }
            if (abs(y - x) > (8 >> sh) + 1)
                over++;
        }
        CHECK_EQ(over, 0);
        CHECK(sh == 0 ? clip == 0 : clip > 0); /* el recorte solo existe con ganancia */
    }
    /* El pico se mide sobre el valor ya amplificado */
    int8_t one[4] = {3, -7, 100, -128};
    int16_t o4[4];
    CHECK_EQ(source_sc8g_to_q11(one, o4, 4, 1), 128);
    one[3] = 5;
    CHECK_EQ(source_sc8g_to_q11(one, o4, 4, 1), 100);
}

static void test_packed12(void)
{
    printf("conversión SC16_Q11_PACKED → Q11 (bit a bit)\n");
    enum { NS = 1024 };
    static int16_t q[2 * NS], got[2 * NS];
    static uint8_t pk[3 * NS];
    uint32_t r = 777;
    for (int i = 0; i < 2 * NS; i++) {
        r = r * 1664525u + 1013904223u;
        q[i] = (int16_t)((int)((r >> 8) % 4096u) - 2048);
    }
    q[0] = -2048;
    q[1] = 2047;
    /* Empaquetador de referencia: flujo de bits LSB primero, componentes I0 Q0 I1 Q1… de 12 bit */
    memset(pk, 0, sizeof pk);
    for (int c = 0; c < 2 * NS; c++)
        for (int b = 0; b < 12; b++)
            if ((q[c] >> b) & 1)
                pk[(c * 12 + b) / 8] |= (uint8_t)(1u << ((c * 12 + b) % 8));
    source_packed12_to_q11(pk, got, NS);
    CHECK(memcmp(q, got, sizeof q) == 0);
    /* La misma aritmética con enteros de 16 bit que usa libbladeRF en sync_rx (sync.c) */
    static int16_t lib[2 * NS];
    for (size_t zz = 0, jj = 0; zz < 2 * NS; zz += 4, jj += 3) {
        const uint16_t *m = (const uint16_t *)(const void *)pk;
        lib[zz + 0] = (int16_t)((int16_t)((m[jj + 0] & 0x0FFF) << 4) >> 4);
        lib[zz + 1] = (int16_t)(((int16_t)((m[jj + 1] & 0x00FF) << 8) >> 4) | ((m[jj + 0] & 0xF000) >> 12));
        lib[zz + 2] = (int16_t)(((int16_t)((m[jj + 2] & 0x000F) << 12) >> 4) | ((m[jj + 1] & 0xFF00) >> 8));
        lib[zz + 3] = (int16_t)((int16_t)(m[jj + 2] & 0xFFF0) >> 4);
    }
    CHECK(memcmp(lib, got, sizeof got) == 0);
}

static void test_sc8g_auto_shift(void)
{
    printf("ajuste automático de GAIN8\n");
    CHECK_EQ(sc8g_auto_shift(0, 0.0, 10), 1);    /* señal débil: sube */
    CHECK_EQ(sc8g_auto_shift(1, 0.0, 40), 2);
    CHECK_EQ(sc8g_auto_shift(1, 0.0, 60), 1);    /* doblar pasaría de 112: se queda */
    CHECK_EQ(sc8g_auto_shift(3, 0.0, 5), 3);     /* tope */
    CHECK_EQ(sc8g_auto_shift(2, 0.5, 127), 1);   /* recorte: baja */
    CHECK_EQ(sc8g_auto_shift(0, 5.0, 127), 0);   /* ya sin ganancia */
    CHECK_EQ(sc8g_auto_shift(2, 0.005, 120), 2); /* recorte despreciable: se mantiene */
    CHECK_EQ(sc8g_auto_shift(0, 0.0, 0), 0);     /* sin señal: no se toca */
}

/* Banco de registros simulado para el cliente GAIN8 de fpga_lab (sin placa) */
static uint32_t g_regs[32];
static int mock_rd(void *c, uint32_t a, uint32_t *v) { (void)c; *v = g_regs[a / 4]; return 0; }
static int mock_wr(void *c, uint32_t a, uint32_t v) { (void)c; g_regs[a / 4] = v; return 0; }

static void test_gain8_registers(void)
{
    printf("registros GAIN8 y CNT_SAT8 (E/S simulada)\n");
    fpga_lab_t lab;
    memset(&lab, 0, sizeof lab);
    lab.io.wb_read = mock_rd;
    lab.io.wb_write = mock_wr;
    lab.present = true;
    lab.version = FPGA_LAB_VERSION_1_2;
    lab.v12 = true;
    lab.caps = 0xF;
    CHECK(!fpga_lab_has_gain8(&lab)); /* 1.2: no existe */
    CHECK(fpga_lab_gain8_set(&lab, 1) < 0);
    lab.version = FPGA_LAB_VERSION_1_3;
    CHECK(!fpga_lab_has_gain8(&lab)); /* versión 1.3 pero sin CAPS bit 8 */
    lab.caps |= FPGA_LAB_CAP_GAIN8;
    CHECK(fpga_lab_has_gain8(&lab));
    CHECK(fpga_lab_gain8_set(&lab, 4) < 0);
    CHECK(fpga_lab_gain8_set(&lab, 2) == 0);
    CHECK_EQ(g_regs[23], 2);
    unsigned sh = 0;
    CHECK(fpga_lab_gain8_get(&lab, &sh) == 0 && sh == 2);
    g_regs[24] = 4242;
    uint32_t c = 0;
    CHECK(fpga_lab_read_sat8(&lab, &c) == 0 && c == 4242);
}

static void test_format_file(void)
{
    printf("formato de RX con fichero: no aplica\n");
    int16_t iq[2 * 4096];
    for (int i = 0; i < 2 * 4096; i++)
        iq[i] = (int16_t)(i % 1000);
    write_file("fmt.cs16", iq, sizeof iq);
    source_t *s = open_file("fmt.cs16", 48000, 100000000);
    if (!s) {
        tu_fails++;
        return;
    }
    CHECK(source_set_format(s, RXFMT_PACKED12) < 0);
    CHECK(strstr(source_error(s), "no aplica") != NULL);
    CHECK(source_set_format(s, RXFMT_SC8G) < 0);
    CHECK_EQ(source_format_effective(s), RXFMT_SC16);
    CHECK(strcmp(source_format(s), "cs16") == 0);
    CHECK(!source_info(s)->fmt_packed12 && !source_info(s)->fmt_sc8g);
    CHECK_EQ(source_info(s)->usb_speed, 0);
    CHECK(source_has_timestamps(s));
    CHECK(source_require_timestamps(s, true) == 0);
    CHECK(source_sc8g_shift(s) == 0);
    double pct;
    int sh;
    CHECK(!source_sc8g_poll(s, &pct, &sh));
    /* y la lectura sigue entregando las muestras del fichero sin alterar */
    int16_t got[2 * 64];
    unsigned ovr = 0;
    source_start(s);
    CHECK(source_read(s, got, 64, &ovr) == 0);
    CHECK(memcmp(got, iq, sizeof got) == 0);
    source_close(s);
}

static void test_pacing(void)
{
    printf("ritmo real y bucle\n");
    /* 1000 muestras cs16 = 4000 bytes que se repiten en bucle */
    int16_t d[2000];
    for (int i = 0; i < 2000; i++)
        d[i] = (int16_t)i;
    write_file("loop.cs16", d, sizeof d);
    source_t *s = open_file("loop.cs16", 100000, 0); /* 100 ksps */
    CHECK(s != NULL);
    if (!s)
        return;
    int16_t *o = malloc(sizeof(int16_t) * 2 * 5000);
    source_start(s);
    double t0 = now_monotonic();
    /* 10 bloques de 2500 muestras = 25 000 muestras = 0,25 s a 100 ksps */
    for (int b = 0; b < 10; b++) {
        CHECK(source_read(s, o, 2500, NULL) == 0);
        if (b == 0) {
            /* 2500 muestras sobre un fichero de 1000: da la vuelta (2500 mod 1000 = 500) */
            CHECK_EQ(o[2 * 999], 1998);
            CHECK_EQ(o[2 * 1000], 0);
            CHECK_EQ(o[2 * 2499], 2 * 499);
        }
    }
    double dt = now_monotonic() - t0;
    printf("  25000 muestras a 100 ksps: %.3f s (esperado 0,250)\n", dt);
    CHECK(dt > 0.20 && dt < 0.40);
    uint64_t ts = 0;
    CHECK(source_read_ts(s, o, 10, &ts, NULL) == 0);
    CHECK_EQ(ts, 25000);
    free(o);
    source_close(s);
}

/* ---------------------------------------------------------------------- */
/*  Radio simulado para el barrido                                        */
/* ---------------------------------------------------------------------- */
typedef struct {
    double fs;
    uint64_t pos, lag;
    struct { uint64_t ts; double lo; } ev[4096];
    int nev;
    struct { uint64_t ts; int id; } q[64];
    int nq, qmax;
    double qt_freq[256];
    int nqt, qt_budget;
    double tone[6];
    int ntone;
    double amp;
    uint64_t garbage;
    int late, started, reads, inject_overrun_at;
    uint32_t rng;
} fake_t;

static void ev_add(fake_t *f, uint64_t ts, double lo)
{
    int i = f->nev++;
    while (i > 0 && f->ev[i - 1].ts > ts) {
        f->ev[i] = f->ev[i - 1];
        i--;
    }
    f->ev[i].ts = ts;
    f->ev[i].lo = lo;
}

static int f_configure(void *c, uint32_t rate)
{
    fake_t *f = c;
    f->fs = rate;
    return 0;
}
static int f_start(void *c)
{
    fake_t *f = c;
    f->started = 1;
    f->pos = 0;
    f->nev = 0;
    f->nq = 0;
    ev_add(f, 0, 50e6);
    return 0;
}
static void f_stop(void *c) { ((fake_t *)c)->started = 0; }
static int f_set_freq(void *c, int64_t hz)
{
    fake_t *f = c;
    ev_add(f, f->pos + f->lag, (double)hz);
    return 0;
}
static int f_qt_prepare(void *c, int64_t hz)
{
    fake_t *f = c;
    for (int i = 0; i < f->nqt; i++)
        if (f->qt_freq[i] == (double)hz)
            return i;
    f->qt_freq[f->nqt] = (double)hz;
    return f->nqt++;
}
static int f_qt_free(void *c)
{
    fake_t *f = c;
    return f->qt_budget - f->nqt;
}
static void q_drain(fake_t *f)
{
    int k = 0;
    for (int i = 0; i < f->nq; i++)
        if (f->q[i].ts > f->pos + f->lag)
            f->q[k++] = f->q[i];
    f->nq = k;
}
static int f_retune_at(void *c, uint64_t ts, int id)
{
    fake_t *f = c;
    q_drain(f);
    if (f->nq >= f->qmax)
        return SWEEP_QUEUE_FULL;
    uint64_t now = f->pos + f->lag;
    if (ts <= now) { /* en el pasado: el FPGA lo ejecuta ya */
        f->late++;
        ts = now;
    }
    ev_add(f, ts, f->qt_freq[id]);
    f->q[f->nq].ts = ts;
    f->q[f->nq++].id = id;
    return 0;
}
static int f_retune_cancel(void *c)
{
    fake_t *f = c;
    /* borra los eventos futuros */
    int k = 0;
    for (int i = 0; i < f->nev; i++)
        if (f->ev[i].ts <= f->pos + f->lag)
            f->ev[k++] = f->ev[i];
    f->nev = k;
    f->nq = 0;
    return 0;
}
static int f_timestamp(void *c, uint64_t *ts)
{
    fake_t *f = c;
    *ts = f->pos + f->lag;
    return 0;
}
static int f_read(void *c, int16_t *iq, unsigned n, uint64_t *ts, unsigned *ovr)
{
    fake_t *f = c;
    *ts = f->pos;
    for (unsigned k = 0; k < n; k++) {
        uint64_t i = f->pos + k;
        int e = 0;
        while (e + 1 < f->nev && f->ev[e + 1].ts <= i)
            e++;
        double lo = f->ev[e].lo;
        bool bad = e > 0 && i < f->ev[e].ts + f->garbage; /* el PLL aún no se ha asentado */
        double re = 0, im = 0;
        if (bad) {
            re = 800.0 * ((tu_rand(&f->rng) & 1023) / 512.0 - 1.0);
            im = 800.0 * ((tu_rand(&f->rng) & 1023) / 512.0 - 1.0);
        } else {
            for (int t = 0; t < f->ntone; t++) {
                if (fabs(f->tone[t] - lo) > 0.45 * f->fs)
                    continue; /* fuera de la banda: el filtro analógico lo elimina (sin alias) */
                double ph = 2 * M_PI * fmod((f->tone[t] - lo) / f->fs * (double)(i % 100000000ull), 1.0);
                re += f->amp * cos(ph);
                im += f->amp * sin(ph);
            }
            re += 3.0 * ((tu_rand(&f->rng) & 1023) / 512.0 - 1.0);
            im += 3.0 * ((tu_rand(&f->rng) & 1023) / 512.0 - 1.0);
        }
        re += 40; /* fuga de continua del receptor */
        iq[2 * k] = (int16_t)lrint(re);
        iq[2 * k + 1] = (int16_t)lrint(im);
    }
    f->pos += n;
    if (ovr && ++f->reads == f->inject_overrun_at)
        *ovr = 1;
    return 0;
}
static const char *f_error(void *c)
{
    (void)c;
    return "error simulado";
}

static sweep_io_t fake_io(fake_t *f)
{
    sweep_io_t io = {f, f_configure, f_start, f_stop, f_set_freq, f_qt_prepare, f_qt_free, f_retune_at,
                     f_retune_cancel, f_timestamp, f_read, f_error};
    return io;
}

/* Barre hasta obtener una trama completa; devuelve true y deja una copia en *db. */
static bool run_until_complete(sweep_t *w, float **db, int *n, double *bin_hz, int64_t *start, int max_steps)
{
    sweep_frame_t fr;
    char err[200];
    for (int i = 0; i < max_steps; i++) {
        int r = sweep_step(w, &fr, err, sizeof err);
        if (r < 0) {
            printf("  sweep_step: %s\n", err);
            return false;
        }
        if (r > 0 && fr.complete) {
            *n = fr.n;
            *bin_hz = fr.bin_hz;
            *start = fr.start_hz;
            *db = malloc(sizeof(float) * (size_t)fr.n);
            memcpy(*db, fr.db, sizeof(float) * (size_t)fr.n);
            return true;
        }
    }
    return false;
}

/* Comprueba que cada tono aparece en su bin con ≈ −6,02 dBFS y que el resto está limpio. */
static void check_tones(const float *db, int n, int64_t start, double bin_hz, const double *tones, int nt, double amp)
{
    const double expect = 20 * log10(amp / 2048.0);
    for (int t = 0; t < nt; t++) {
        int b = (int)lround((tones[t] - (double)start) / bin_hz);
        int pk = b;
        for (int k = b - 2; k <= b + 2; k++)
            if (k >= 0 && k < n && db[k] > db[pk])
                pk = k;
        printf("  tono %.3f MHz: bin %d pico en %d = %.2f dBFS (esperado %.2f)\n", tones[t] / 1e6, b, pk, db[pk],
               expect);
        CHECK_EQ(pk, b);
        CHECK_NEAR(db[pk], expect, 0.7);
    }
    int dirty = 0;
    float worst = -200;
    for (int i = 0; i < n; i++) {
        bool near = false;
        for (int t = 0; t < nt; t++)
            if (abs(i - (int)lround((tones[t] - (double)start) / bin_hz)) <= 8)
                near = true;
        if (!near && db[i] > -50) {
            dirty++;
            if (db[i] > worst)
                worst = db[i];
        }
    }
    printf("  bins espurios > −50 dBFS: %d (peor %.1f)\n", dirty, worst);
    CHECK_EQ(dirty, 0);
}

static void test_sweep_quick(void)
{
    printf("barrido: quick-tune (20 Msps, FFT 1024)\n");
    static fake_t f;
    memset(&f, 0, sizeof f);
    f.lag = 3000;
    f.qmax = 8;
    f.qt_budget = 256;
    f.garbage = 1500;
    f.amp = 1024; /* tono complejo de ½ fondo de escala: −6,02 dBFS */
    f.rng = 12345;
    sweep_cfg_t c = {.start_hz = 100000000, .stop_hz = 200000000, .fmin_hz = 47000000, .fmax_hz = 6000000000LL,
                     .rate = 20000000, .fft = 1024, .allow_quick = true};
    const double binw = 20e6 / 1024, K = 716;
    /* tonos: uno normal, dos a ambos lados de la costura entre los saltos 2 y 3, y otro al final */
    const double seam = 100e6 + 3 * K * binw;
    f.tone[0] = 100e6 + 100 * binw;
    f.tone[1] = seam - 5 * binw;
    f.tone[2] = seam + 5 * binw;
    f.tone[3] = 100e6 + 5000 * binw;
    f.ntone = 4;
    char err[200] = "";
    sweep_io_t io = fake_io(&f);
    sweep_t *w = sweep_create(&c, &io, err, sizeof err);
    CHECK(w != NULL);
    if (!w) {
        printf("  %s\n", err);
        return;
    }
    sweep_info_t inf;
    sweep_get_info(w, &inf);
    printf("  %d saltos, %d bins, %s, cobertura %.3f–%.3f MHz\n", inf.hops, inf.bins, inf.quick ? "quick" : "lento",
           inf.start_hz / 1e6, inf.stop_hz / 1e6);
    CHECK(inf.quick);
    CHECK_EQ(inf.hops, (int)ceil(100e6 / (K * binw)));
    CHECK_EQ(inf.bins, (int)ceil(100e6 / binw)); /* la trama se recorta a [inicio, fin] */
    CHECK_EQ(f.nqt, inf.hops);
    float *db = NULL;
    int n = 0;
    double bin_hz = 0;
    int64_t start = 0;
    bool ok = run_until_complete(w, &db, &n, &bin_hz, &start, 200);
    CHECK(ok);
    if (ok) {
        CHECK_NEAR(bin_hz, binw, 1e-6);
        CHECK_EQ(start, 100000000);
        check_tones(db, n, start, bin_hz, f.tone, f.ntone, f.amp);
        /* La continua (+40 LSB) no debe verse: la media se resta antes de la FFT */
        int hop_center_bin = 358; /* centro del salto 0 = bin K/2 */
        CHECK(db[hop_center_bin] < -60);
        /* Mensaje tipo 4 */
        sweep_frame_t fr = {db, n, start, bin_hz, true, 100};
        uint8_t *m = malloc(sweep_msg_size(n));
        CHECK_EQ(sweep_pack(&fr, m), sweep_msg_size(n));
        uint32_t nn;
        double ctr, span;
        float dbmin, dbstep;
        memcpy(&nn, m + 4, 4);
        memcpy(&ctr, m + 8, 8);
        memcpy(&span, m + 16, 8);
        memcpy(&dbmin, m + 24, 4);
        memcpy(&dbstep, m + 28, 4);
        CHECK_EQ(m[0], 4);
        CHECK_EQ(m[2] & 1, 1);
        CHECK_EQ(m[3], 100);
        CHECK_EQ(nn, n);
        CHECK_NEAR(ctr - span / 2, 100e6, 1e-3);
        CHECK_NEAR(ctr + span / 2, inf.stop_hz, 1.0);
        CHECK_NEAR(dbmin + m[32 + 50] * dbstep, db[50], 0.3);
        free(m);
        free(db);
    }
    sweep_get_info(w, &inf);
    printf("  retunes tardíos: %d, re-sincronizaciones: %llu\n", f.late, (unsigned long long)inf.restarts);
    CHECK_EQ(f.late, 0);
    CHECK_EQ(inf.restarts, 0);
    sweep_destroy(w);
    CHECK_EQ(f.started, 0);
}

static void test_sweep_resync(void)
{
    printf("barrido: recuperación tras overrun\n");
    static fake_t f;
    memset(&f, 0, sizeof f);
    f.lag = 3000;
    f.qmax = 8;
    f.qt_budget = 256;
    f.garbage = 1500;
    f.amp = 1024;
    f.rng = 777;
    f.inject_overrun_at = 29;
    sweep_cfg_t c = {.start_hz = 400000000, .stop_hz = 450000000, .fmin_hz = 47000000, .fmax_hz = 6000000000LL,
                     .rate = 20000000, .fft = 1024, .allow_quick = true};
    f.tone[0] = 400e6 + 800 * (20e6 / 1024);
    f.ntone = 1;
    char err[200];
    sweep_io_t io = fake_io(&f);
    sweep_t *w = sweep_create(&c, &io, err, sizeof err);
    CHECK(w != NULL);
    if (!w)
        return;
    float *db = NULL;
    int n = 0;
    double bin_hz = 0;
    int64_t start = 0;
    CHECK(run_until_complete(w, &db, &n, &bin_hz, &start, 300));
    sweep_info_t inf;
    sweep_get_info(w, &inf);
    printf("  re-sincronizaciones: %llu\n", (unsigned long long)inf.restarts);
    CHECK(inf.restarts >= 1);
    /* tras re-sincronizar, una pasada entera posterior sale limpia */
    free(db);
    db = NULL;
    CHECK(run_until_complete(w, &db, &n, &bin_hz, &start, 300));
    if (db) {
        check_tones(db, n, start, bin_hz, f.tone, 1, f.amp);
        free(db);
    }
    sweep_destroy(w);
}

static void test_sweep_slow_and_budget(void)
{
    printf("barrido: elección de tasa y modo lento\n");
    static fake_t f;
    char err[200];
    sweep_cfg_t c = {.start_hz = 100000000, .stop_hz = 400000000, .fmin_hz = 47000000, .fmax_hz = 6000000000LL,
                     .fft = 1024, .allow_quick = true};
    /* Con pocos perfiles libres se sube la tasa hasta que quepan los saltos */
    memset(&f, 0, sizeof f);
    f.qmax = 8;
    f.qt_budget = 20; /* 18 utilizables */
    f.lag = 3000;
    f.rng = 1;
    sweep_io_t io = fake_io(&f);
    sweep_t *w = sweep_create(&c, &io, err, sizeof err);
    CHECK(w != NULL);
    sweep_info_t inf;
    if (w) {
        sweep_get_info(w, &inf);
        printf("  presupuesto 18 perfiles: %.2f Msps, %d saltos, %s\n", inf.rate / 1e6, inf.hops,
               inf.quick ? "quick" : "lento");
        CHECK(inf.quick);
        CHECK(inf.hops <= 18);
        CHECK_EQ(inf.rate, 30720000);
        sweep_destroy(w);
    }
    /* Sin perfiles: modo lento a la tasa máxima, y el resultado sigue siendo correcto */
    memset(&f, 0, sizeof f);
    f.qmax = 8;
    f.qt_budget = 3;
    f.lag = 30000; /* colchón USB grande: el modo lento debe descartarlo */
    f.garbage = 1500;
    f.amp = 1024;
    f.rng = 2;
    f.ntone = 2;
    const double binw = 61.44e6 / 1024;
    f.tone[0] = 100e6 + 3000 * binw;
    f.tone[1] = 100e6 + 4000 * binw;
    io = fake_io(&f);
    w = sweep_create(&c, &io, err, sizeof err);
    CHECK(w != NULL);
    if (w) {
        sweep_get_info(w, &inf);
        printf("  sin perfiles: %.2f Msps, %d saltos, %s\n", inf.rate / 1e6, inf.hops, inf.quick ? "quick" : "lento");
        CHECK(!inf.quick);
        CHECK_EQ(inf.rate, 61440000);
        float *db = NULL;
        int n = 0;
        double bin_hz = 0;
        int64_t start = 0;
        CHECK(run_until_complete(w, &db, &n, &bin_hz, &start, 100));
        if (db) {
            check_tones(db, n, start, bin_hz, f.tone, 2, f.amp);
            free(db);
        }
        sweep_destroy(w);
    }
    /* Parámetros no válidos */
    sweep_cfg_t bad = c;
    bad.fft = 1000;
    memset(&f, 0, sizeof f);
    io = fake_io(&f);
    CHECK(sweep_create(&bad, &io, err, sizeof err) == NULL);
    bad = c;
    bad.start_hz = 7000000000LL;
    bad.stop_hz = 8000000000LL;
    CHECK(sweep_create(&bad, &io, err, sizeof err) == NULL);
    printf("  error esperado: %s\n", err);
}

static void test_sweep_top_of_range(void)
{
    printf("barrido: último salto en el límite superior\n");
    static fake_t f;
    memset(&f, 0, sizeof f);
    f.qmax = 8;
    f.qt_budget = 256;
    f.lag = 3000;
    f.garbage = 1500;
    f.amp = 1024;
    f.rng = 5;
    const double binw = 20e6 / 1024;
    f.tone[0] = 5.9e9 + 4608 * binw; /* ≈ 5,99 GHz */
    f.tone[1] = 5.9e9 + 5100 * binw; /* ≈ 5,9995 GHz: en el último salto, desplazado bajo fmax */
    f.ntone = 2;
    sweep_cfg_t c = {.start_hz = 5900000000LL, .stop_hz = 6000000000LL, .fmin_hz = 47000000,
                     .fmax_hz = 6000000000LL, .rate = 20000000, .fft = 1024, .allow_quick = true};
    char err[200];
    sweep_io_t io = fake_io(&f);
    sweep_t *w = sweep_create(&c, &io, err, sizeof err);
    CHECK(w != NULL);
    if (!w)
        return;
    sweep_info_t inf;
    sweep_get_info(w, &inf);
    printf("  cobertura %.3f–%.3f MHz (%d bins)\n", inf.start_hz / 1e6, inf.stop_hz / 1e6, inf.bins);
    /* ningún salto se sintoniza por encima de fmax */
    for (int i = 0; i < f.nqt; i++)
        CHECK(f.qt_freq[i] <= 6000000000.0);
    CHECK(inf.stop_hz <= 6000000000LL + (int64_t)binw);
    CHECK(inf.stop_hz > 5999000000LL); /* sigue cubriendo todo el rango */
    float *db = NULL;
    int n = 0;
    double bin_hz = 0;
    int64_t start = 0;
    CHECK(run_until_complete(w, &db, &n, &bin_hz, &start, 200));
    if (db) {
        check_tones(db, n, start, bin_hz, f.tone, 2, f.amp);
        free(db);
    }
    sweep_destroy(w);
}

int main(void)
{
    snprintf(g_dir, sizeof g_dir, "/tmp/test_source_XXXXXX");
    if (!mkdtemp(g_dir)) {
        printf("no se pudo crear el directorio temporal\n");
        return 1;
    }
    test_gain_modes();
    test_formats();
    test_sigmf();
    test_sc8();
    test_rxfmt_names_and_choice();
    test_sc8g_conversion();
    test_packed12();
    test_sc8g_auto_shift();
    test_gain8_registers();
    test_format_file();
    test_pacing();
    test_sweep_quick();
    test_sweep_resync();
    test_sweep_slow_and_budget();
    test_sweep_top_of_range();

    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd) != 0)
        printf("(no se pudo borrar %s)\n", g_dir);
    return TEST_SUMMARY();
}
