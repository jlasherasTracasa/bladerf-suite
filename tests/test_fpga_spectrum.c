/*
 * test_fpga_spectrum.c — Espectro de la FPGA como fuente alternativa del motor (sin placa).
 *
 *  1. fpga_lab_choose_params: N y K para una cascada de 'fps' imágenes/s y el límite de caudal.
 *  2. Tramas sintéticas con el formato del HDL (firmware/hdl, docs/hardware/FIRMWARE.md §10:
 *     cabecera de 8 palabras {magic0, magic1, N|K<<16, grupo, ts_lo, ts_hi, 8|versión<<8, total},
 *     N palabras {E[5:0], M[25:0]} en orden fftshift y relleno hasta múltiplo de 2048) →
 *     intérprete → dBFS → mensaje binario tipo 1 (byte [1] = 2): cabecera y bytes cuantizados.
 *     Un tono de amplitud 1000 (Q11) en el bin +100 debe leerse a su nivel teórico.
 *  3. Motor con fuente de fichero: pedir "fpga" da error claro; "cpu"/"gpu" valen y el motor
 *     sigue funcionando; "status" dice que no hay lab.
 */
#include "test_util.h"

#include "engine.h"
#include "sdr/fpga_lab.h"
#include "tx/mod.h"
#include "tx/tx.h"

#include <unistd.h>

#define N 1024
#define K 4
#define TOTAL 2048

/* Palabra de potencia {E,M} para un valor acc (exacto hasta 26 bits de mantisa). */
static uint32_t make_power(uint64_t acc)
{
    unsigned e = 0;
    while ((acc >> e) > 0x3FFFFFFu) e++;
    return (uint32_t)((acc >> e) & 0x3FFFFFFu) | (e << 26);
}

/* Trama sintética: ruido de potencia 'floor' y un tono de potencia 'tone' en el bin idx (0..N-1 = -N/2..N/2-1). */
static int build_frame(uint32_t *w, uint32_t group, int tone_idx, uint64_t floor_p, uint64_t tone_p, bool dropped)
{
    w[0] = FPGA_SPEC_MAGIC0;
    w[1] = FPGA_SPEC_MAGIC1;
    w[2] = (uint32_t)N | ((uint32_t)K << 16);
    w[3] = group;
    uint64_t ts = (uint64_t)group * K * N;
    w[4] = (uint32_t)ts;
    w[5] = (uint32_t)(ts >> 32);
    w[6] = 8u | (1u << 8) | (dropped ? 1u << 16 : 0);
    w[7] = TOTAL;
    for (int i = 0; i < N; i++)
        w[8 + i] = make_power(i == tone_idx ? tone_p : floor_p + (uint64_t)(i % 7));
    for (int i = 8 + N; i < TOTAL; i++)
        w[i] = 0xDEADBEEFu; /* relleno */
    return TOTAL;
}

typedef struct {
    int count;
    float db[N];
    uint8_t msg[32 + N];
    size_t msglen;
    bool dropped;
    int k;
} cap_t;

static void on_frame(void *ctx, const fpga_lab_frame_t *f)
{
    cap_t *c = ctx;
    c->count++;
    c->dropped = f->dropped;
    c->k = f->k;
    fpga_lab_frame_to_dbfs(f, c->db);
    c->msglen = spectrum_msg_pack(c->msg, c->db, f->n, SPEC_SRC_FPGA, 100e6, 30.72e6, -145.f, 0.6f);
}


/* ---- Placa simulada (registros lab 1.1 / 1.2) ------------------------------------------------ */
typedef struct {
    uint32_t reg[32];
    uint32_t version;      /* 0x00010001 o 0x00010002 */
    uint32_t gpio;
    int bad_reads;         /* lecturas Wishbone de registros >= 16 en una 1.1 (devuelven 0xBAD0BAD0) */
    int wr_k, wr_other, gpio_writes;
    uint64_t time;         /* contador de 64 bit; sube en cada lectura */
    uint32_t hi_latched;
    int order_ok;          /* TIME_HI leído solo tras TIME_LO */
    bool lo_read;
} mock_t;

static int m_ver(void *c, uint32_t *ma, uint32_t *mi, uint32_t *pa) { (void)c; *ma = 0; *mi = 16; *pa = 101; return 0; }
static int m_gr(void *c, uint32_t *v) { *v = ((mock_t *)c)->gpio | (0xAu << 24); return 0; }
static int m_gw(void *c, uint32_t v) { ((mock_t *)c)->gpio = v; ((mock_t *)c)->gpio_writes++; return 0; }
static int m_wr(void *c, uint32_t a, uint32_t *v)
{
    mock_t *m = c;
    const unsigned i = a / 4;
    if (i >= 16 && m->version < FPGA_LAB_VERSION_1_2) { m->bad_reads++; *v = 0xBAD0BAD0u; return 0; }
    if (i == FPGA_LAB_REG_TIME_LO) {
        m->time += 0x7FFFFFFFull; /* el contador avanza entre lecturas: cruza el acarreo de 32 bit */
        m->hi_latched = (uint32_t)(m->time >> 32);
        m->lo_read = true;
        *v = (uint32_t)m->time;
        return 0;
    }
    if (i == FPGA_LAB_REG_TIME_HI) {
        if (!m->lo_read) m->order_ok = 0;
        *v = m->hi_latched;
        return 0;
    }
    *v = m->reg[i];
    return 0;
}
static int m_ww(void *c, uint32_t a, uint32_t v)
{
    mock_t *m = c;
    if (a / 4 == FPGA_LAB_REG_K) m->wr_k++; else m->wr_other++;
    m->reg[a / 4] = v;
    return 0;
}

static void mock_init(mock_t *m, uint32_t version, fpga_lab_io_t *io)
{
    memset(m, 0, sizeof *m);
    m->version = version;
    m->order_ok = 1;
    m->reg[FPGA_LAB_REG_ID] = FPGA_LAB_ID_VALUE;
    m->reg[FPGA_LAB_REG_VERSION] = version;
    m->reg[FPGA_LAB_REG_FEATURES] = 0x0DA1;
    m->reg[FPGA_LAB_REG_CAPS] = 0x0F;
    m->reg[FPGA_LAB_REG_SYNTH] = 0x100A0D0Cu;
    m->reg[FPGA_LAB_REG_CNT_ADC_SAT] = 1234;
    m->reg[FPGA_LAB_REG_SAT_WIN] = 6554; /* ~10 % de 65536 */
    m->reg[FPGA_LAB_REG_CNT_SPEC_DROP] = 3;
    m->time = 0xFFFFFFFF00ull;
    io->ctx = m;
    io->fpga_version = m_ver;
    io->gpio_read = m_gr;
    io->gpio_write = m_gw;
    io->wb_read = m_wr;
    io->wb_write = m_ww;
}

static void test_lab12_regs(void)
{
    mock_t m;
    fpga_lab_io_t io;
    fpga_lab_t lab;
    char err[200];
    /* 1.2: CAPS/SYNTH tras el probe, telemetría, marca de tiempo, K en caliente */
    mock_init(&m, FPGA_LAB_VERSION_1_2, &io);
    CHECK_EQ(fpga_lab_probe(&io, &lab, err, sizeof err), 1);
    CHECK(lab.v12);
    CHECK_EQ(lab.caps, 0x0F);
    CHECK((lab.caps & FPGA_LAB_CAP_ADC_SAT) && (lab.caps & FPGA_LAB_CAP_TIMESTAMP));
    CHECK_EQ(lab.synth, 0x100A0D0Cu);
    fpga_lab_telem_t t;
    CHECK_EQ(fpga_lab_read_telemetry(&lab, &t), 0);
    CHECK_EQ(t.adc_sat, 1234);
    CHECK_EQ(t.spec_drop, 3);
    CHECK_NEAR(t.sat_pct, 10.0, 0.01);
    uint64_t ts = 0;
    CHECK_EQ(fpga_lab_read_time(&lab, &ts), 0);
    CHECK(m.order_ok); /* TIME_LO antes que TIME_HI */
    CHECK_EQ(ts, m.time); /* instantánea coherente a través del acarreo de 32 bit */
    const int gw = m.gpio_writes;
    CHECK_EQ(fpga_lab_set_k_hot(&lab, 40), 0);
    CHECK_EQ(m.wr_k, 1);
    CHECK_EQ(m.wr_other, 0);          /* ni N ni CTRL ni CLEAR */
    CHECK_EQ(m.gpio_writes, gw);      /* rx_mux intacto */
    CHECK_EQ(m.reg[FPGA_LAB_REG_K], 40);
    CHECK(fpga_lab_set_k_hot(&lab, 0) < 0);

    /* 1.1: no se leen los registros 16..22 (darían 0xBAD0BAD0) y no hay K en caliente */
    mock_init(&m, FPGA_LAB_VERSION_1_1, &io);
    CHECK_EQ(fpga_lab_probe(&io, &lab, err, sizeof err), 1);
    CHECK(!lab.v12);
    CHECK_EQ(lab.caps, 0);
    CHECK(fpga_lab_read_telemetry(&lab, &t) < 0);
    CHECK(fpga_lab_read_time(&lab, &ts) < 0);
    CHECK(fpga_lab_set_k_hot(&lab, 40) < 0);
    CHECK_EQ(m.bad_reads, 0);
    CHECK_EQ(m.wr_k, 0);
}

/* K cambiado en caliente: la trama que se estaba leyendo sale truncada (la FPGA reinicia el motor y
 * la siguiente empieza con el K nuevo). El intérprete debe descartarla, contar UN hueco y entregar
 * las siguientes con el K nuevo en la cabecera. */
static void test_hot_k_resync(void)
{
    cap_t cap;
    memset(&cap, 0, sizeof cap);
    fpga_lab_parser_t *p = fpga_lab_parser_create(on_frame, &cap);
    static uint32_t st[6 * TOTAL];
    int n = 0;
    n += build_frame(st + n, 0, 100, 1000, 100000, false);
    n += build_frame(st + n, 1, 100, 1000, 100000, false);
    /* trama 2 truncada a mitad de los bins (cambio de K) ... */
    uint32_t tmp[TOTAL];
    build_frame(tmp, 2, 100, 1000, 100000, false);
    memcpy(st + n, tmp, 300 * 4);
    n += 300;
    /* ... y la siguiente ya con K = 8 (cabecera palabra 2) */
    build_frame(tmp, 3, 100, 1000, 100000, false);
    tmp[2] = (uint32_t)N | (8u << 16);
    memcpy(st + n, tmp, TOTAL * 4);
    n += TOTAL;
    n += build_frame(st + n, 4, 100, 1000, 100000, false);
    st[n - TOTAL + 2] = (uint32_t)N | (8u << 16);
    fpga_lab_parser_feed_words(p, st, (size_t)n);
    uint64_t frames, skipped, gaps;
    fpga_lab_parser_stats(p, &frames, &skipped, &gaps);
    CHECK_EQ(frames, 4); /* 0, 1, 3 y 4: la 2 se descarta */
    CHECK_EQ(cap.count, 4);
    CHECK_EQ(cap.k, 8); /* la última trama ya lleva el K nuevo */
    CHECK(gaps >= 1 && gaps <= 2); /* el hueco de la trama cortada (+ el salto de grupo 1→3) */
    CHECK(skipped >= 300);
    fpga_lab_parser_destroy(p);
}

static void test_params(void)
{
    int l, k;
    /* 30,72 Msps, FFT 4096, 20 imágenes/s: K ≈ 375, 6144 palabras/trama */
    CHECK_EQ(fpga_lab_choose_params(4096, 30.72e6, 20, &l, &k), 6144);
    CHECK_EQ(l, 12);
    CHECK_EQ(k, 375);
    /* FFT pedida fuera de rango: se acota a 1024..8192 */
    CHECK_EQ(fpga_lab_choose_params(256, 2e6, 30, &l, &k), 2048);
    CHECK_EQ(l, 10);
    CHECK_EQ(fpga_lab_choose_params(65536, 2e6, 30, &l, &k), 10240);
    CHECK_EQ(l, 13);
    /* Caudal acotado: nunca más de 3 Mpalabras/s */
    for (int fps = 1; fps <= 60; fps += 7)
        for (int fft = 1024; fft <= 8192; fft *= 2) {
            int tot = fpga_lab_choose_params(fft, 61.44e6, fps, &l, &k);
            CHECK(tot > 0 && k >= 1);
            double wps = 61.44e6 * tot / ((double)(1 << l) * k);
            CHECK(wps <= FPGA_LAB_MAX_WORDS_PER_S * 1.001);
        }
    CHECK(fpga_lab_choose_params(4096, 0, 20, &l, &k) < 0);
    CHECK(fpga_lab_choose_params(4096, 1e6, 0, &l, &k) < 0);
}

static void test_frames(void)
{
    cap_t cap;
    memset(&cap, 0, sizeof cap);
    fpga_lab_parser_t *p = fpga_lab_parser_create(on_frame, &cap);
    static uint32_t stream[4 * TOTAL];
    const int tone_idx = N / 2 + 100;
    /* acc = K·|X|²/4 con |X|² = (A·Σw·2048…)²: se parte de la potencia teórica en dBFS */
    const double wsum = fpga_lab_window_sum(N);
    const double tone_dbfs = 20 * log10(1000.0 / 2048.0);
    const double acc_tone = pow(10.0, tone_dbfs / 10.0) * pow(2048.0 * wsum, 2) * K / 4.0;
    const double floor_dbfs = -80.0;
    const double acc_floor = pow(10.0, floor_dbfs / 10.0) * pow(2048.0 * wsum, 2) * K / 4.0;
    int n = 0;
    n += build_frame(stream + n, 0, tone_idx, (uint64_t)acc_floor, (uint64_t)acc_tone, false);
    /* basura entre tramas (palabras perdidas) y una trama con grupo saltado */
    stream[n++] = 0x12345678u;
    stream[n++] = 0x9ABCDEF0u;
    n += build_frame(stream + n, 2, tone_idx, (uint64_t)acc_floor, (uint64_t)acc_tone, true);
    /* en trozos irregulares */
    for (int i = 0; i < n;) {
        int m = 1 + (i * 131) % 1000;
        if (i + m > n) m = n - i;
        fpga_lab_parser_feed_words(p, stream + i, (size_t)m);
        i += m;
    }
    uint64_t frames, skipped, gaps;
    fpga_lab_parser_stats(p, &frames, &skipped, &gaps);
    CHECK_EQ(cap.count, 2);
    CHECK_EQ(frames, 2);
    CHECK_EQ(gaps, 1);
    CHECK(cap.dropped);
    CHECK(skipped >= 2);

    /* Mensaje tipo 1 de la última trama */
    const uint8_t *m = cap.msg;
    CHECK_EQ(cap.msglen, 32 + N);
    CHECK_EQ(m[0], 1);
    CHECK_EQ(m[1], SPEC_SRC_FPGA);
    uint32_t nn;
    double ctr, rate;
    float dbmin, dbstep;
    memcpy(&nn, m + 4, 4);
    memcpy(&ctr, m + 8, 8);
    memcpy(&rate, m + 16, 8);
    memcpy(&dbmin, m + 24, 4);
    memcpy(&dbstep, m + 28, 4);
    CHECK_EQ(nn, N);
    CHECK_NEAR(ctr, 100e6, 1);
    CHECK_NEAR(rate, 30.72e6, 1);
    CHECK_NEAR(dbmin, -145.0, 1e-6);
    CHECK_NEAR(dbstep, 0.6, 1e-6);
    /* byte → dBFS = dbmin + v·dbstep, con 0,6 dB de resolución */
    CHECK_NEAR(dbmin + m[32 + tone_idx] * dbstep, tone_dbfs, 0.7);
    CHECK_NEAR(dbmin + m[32 + 10] * dbstep, floor_dbfs, 0.7);
    CHECK_NEAR(cap.db[tone_idx], tone_dbfs, 0.05);
    CHECK_NEAR(cap.db[10], floor_dbfs, 0.05);
    printf("  tono: %.2f dBFS (teórico %.2f), suelo %.2f dBFS (teórico %.1f)\n", cap.db[tone_idx], tone_dbfs,
           cap.db[10], floor_dbfs);
    /* saturación: fuera de rango se acota a 0 / 255 */
    float x[2] = {-300.f, 50.f};
    uint8_t o[34];
    spectrum_msg_pack(o, x, 2, SPEC_SRC_CPU, 1.0, 2.0, -145.f, 0.6f);
    CHECK_EQ(o[1], 0);
    CHECK_EQ(o[32], 0);
    CHECK_EQ(o[33], 255);
    fpga_lab_parser_destroy(p);
}

static void test_engine_file(void)
{
    const char *prefix = "/tmp/test_fpga_spec_src";
    iqbuf_t iq = {0};
    iq.rate = 1e6;
    iq.n = 200000;
    iq.s = calloc((size_t)iq.n, sizeof(cf32));
    for (int i = 0; i < iq.n; i++)
        iq.s[i] = (cf32){0.5f * cosf(2.f * (float)M_PI * 0.1f * (float)i), 0.5f * sinf(2.f * (float)M_PI * 0.1f * (float)i)};
    char err[300] = "";
    CHECK(tx_write_sigmf(prefix, &iq, 1, 433920000, "tono de prueba", err, sizeof err) == 0);
    source_t *src = source_open_file("/tmp/test_fpga_spec_src.sigmf-meta", 0, 0, err, sizeof err);
    CHECK(src != NULL);
    if (!src) return;
    server_callbacks_t cb = {0};
    server_t *srv = server_create("127.0.0.1", 0, "web", "/tmp", &cb, NULL, err, sizeof err);
    CHECK(srv != NULL);
    engine_cfg_t cfg = {.center_hz = 433920000, .rate = 1000000, .fft_size = 1024, .fps = 20, .det_snr = 10};
    engine_t *e = engine_create(src, srv, &cfg, false, "/tmp");
    CHECK(e != NULL);
    CHECK(engine_start(e) == 0);

    CHECK(engine_set_spectrum_src(e, "fpga", err, sizeof err) < 0);
    CHECK(strstr(err, "bladeRF") != NULL && strstr(err, "fichero") != NULL);
    CHECK(engine_set_spectrum_src(e, "nada", err, sizeof err) < 0);
    CHECK(engine_set_spectrum_src(e, "cpu", err, sizeof err) == 0);
    CHECK(engine_set_spectrum_src(e, "gpu", err, sizeof err) == 0);
    /* el motor sigue vivo: sintoniza y reporta que la lab no está */
    CHECK(engine_wait_tuned(e, 433920000, 1000000, 3.0));
    usleep(200000);
    sb_t sb = {0};
    engine_status_json(e, &sb);
    CHECK(strstr(sb.data, "\"spectrum_src\":\"cpu\"") != NULL);
    CHECK(strstr(sb.data, "\"fpga_lab\":{\"present\":false,\"active\":false}") != NULL);
    sb_free(&sb);

    engine_destroy(e);
    server_destroy(srv);
    source_close(src);
    iqbuf_free(&iq);
}

/* La conversión rápida a dB (polinomio + caché de Σw) debe coincidir con la exacta (log10 en doble). */
static void test_dbfs_fast(void)
{
    static fpga_lab_frame_t f;
    f.n = 4096;
    f.k = 375;
    uint32_t x = 12345;
    for (int i = 0; i < f.n; i++) {
        x = x * 1664525u + 1013904223u;
        f.power[i] = (i % 97 == 0) ? 0u : ((x >> 6) & 0x3FFFFFFu) | (((x >> 3) % 40u) << 26);
    }
    static float db[4096];
    fpga_lab_frame_to_dbfs(&f, db);
    double maxerr = 0;
    for (int i = 0; i < f.n; i++) {
        if ((f.power[i] & 0x3FFFFFFu) == 0) continue;
        double ex = fpga_lab_power_to_dbfs(fpga_lab_power_value(f.power[i]) , f.n, f.k);
        double e = fabs(ex - db[i]);
        if (e > maxerr) maxerr = e;
    }
    CHECK(maxerr < 0.01);
}

int main(void)
{
    test_dbfs_fast();
    test_params();
    test_lab12_regs();
    test_hot_k_resync();
    test_frames();
    test_engine_file();
    return TEST_SUMMARY();
}
