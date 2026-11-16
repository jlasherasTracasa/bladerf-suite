/*
 * test_dab.c — Decodificador DAB (src/decoders/dab.c) y su modulador.
 *
 *  1. Tablas: desentrelazador de frecuencia (permutación de las 1536 portadoras),
 *     símbolo de referencia de fase (módulo 1, autocorrelación impulsiva), CRC-16
 *     del FIB, ida y vuelta del FIC (con errores de canal), tabla UEP.
 *  2. Modulador → canal (CFO fraccional y entero, ±100 ppm, AWGN 10 dB) →
 *     decodificador: conjunto, servicios, subcanales, tasas y protecciones.
 *  3. Solo ruido → 0 eventos.
 *  4. Coste de CPU por segundo de señal.
 *
 * VECTOR REAL (sección 1b): tests/data/dab_real_fic_soft.bin = 3 tramas × 4 bloques FIC
 * × 2304 bits blandos (int8) que el decodificador obtuvo, tras OFDM/DQPSK/desentrelazado
 * de frecuencia y tiempo, de la grabación pública DAB.zip de sigidwiki.com (IQ 8 bit
 * a 2 MS/s remuestreado a 2,048 MS/s; conjunto 5001 "Towercom", Eslovaquia). Prueban
 * punzonado, desentrelazado, dispersión y CRC del FIC con tráfico real, y que la cadena
 * OFDM completa lo sincroniza (docs/decoders/dab.md). Un fragmento IQ de una trama
 * ocupa ≥ 390 kB, por encima del límite de 200 kB, y recortado no sincroniza.
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/dab.h"
#include "dsp/crc.h"
#include "dsp/fec.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_DAB;
extern const modulator_desc_t MOD_DAB;

/* AWGN con SNR respecto a la potencia media de las muestras no nulas de OFDM. */
static void awgn_mean(cf32 *x, int n, double snr_db, uint32_t *seed)
{
    double sum = 0;
    long cnt = 0;
    for (int i = 0; i < n; i++) {
        double p = (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
        if (p > 0) {
            sum += p;
            cnt++;
        }
    }
    double sigma = sqrt(sum / (double)cnt / pow(10.0, snr_db / 10.0) / 2.0);
    for (int i = 0; i < n; i++) {
        x[i].re += (float)(sigma * tu_gauss(seed));
        x[i].im += (float)(sigma * tu_gauss(seed));
    }
}

typedef struct {
    evs_t evs;
    char state[4096];
    double cpu;
} res_t;

static void run_dab(const cf32 *iq, int n, res_t *r)
{
    memset(r, 0, sizeof *r);
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
    cf32_to_q11(iq, n, 1.0, q);
    char err[200] = "";
    dec_cfg_t cfg = {.rate = DAB_RATE, .freq_hz = 227360000, .center_hz = 227360000};
    void *st = DEC_DAB.create(&cfg, err, sizeof err);
    if (!st) {
        printf("  create falló: %s\n", err);
        free(q);
        return;
    }
    clock_t c0 = clock();
    for (int off = 0; off < n; off += 65536)
        DEC_DAB.process_raw(st, q + 2 * (size_t)off, n - off < 65536 ? n - off : 65536, 1000.0 + off / (double)DAB_RATE,
                            tu_collect, &r->evs);
    r->cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
    sb_t sb = {0};
    DEC_DAB.state_json(st, &sb, 1000.0 + n / (double)DAB_RATE);
    snprintf(r->state, sizeof r->state, "%s", sb.data);
    sb_free(&sb);
    DEC_DAB.destroy(st);
    free(q);
}

static void check_services(const char *state)
{
    CHECK(strstr(state, "\"ensemble\":{\"id\":\"C1D2\",\"label\":\"BladeRF Lab DAB\"}"));
    CHECK(strstr(state, "{\"sid\":\"C201\",\"label\":\"Radio Uno\",\"subch\":1,\"kbps\":128,\"prot\":\"EEP 3-A\",\"type\":\"DAB+\",\"pty\":10}"));
    CHECK(strstr(state, "{\"sid\":\"C202\",\"label\":\"Radio Dos\",\"subch\":2,\"kbps\":96,\"prot\":\"UEP 5\",\"type\":\"DAB\",\"pty\":11}"));
    CHECK(strstr(state, "{\"sid\":\"C203\",\"label\":\"Radio Tres\",\"subch\":3,\"kbps\":64,\"prot\":\"EEP 4-B\",\"type\":\"DAB+\",\"pty\":12}"));
}

static void case_dab(const char *name, double cfo_hz, double ppm, double snr_db, int frames)
{
    char params[64];
    snprintf(params, sizeof params, "{\"frames\":%d}", frames);
    iqbuf_t b = {0};
    char err[200];
    if (MOD_DAB.modulate(params, &b, err, sizeof err) < 0) {
        printf("  modulate falló: %s\n", err);
        CHECK(0);
        return;
    }
    CHECK_EQ(b.n, frames * DAB_FRAME);
    cf32 *x = malloc(sizeof(cf32) * (size_t)(b.n + b.n / 5000 + 64));
    int n = b.n;
    if (ppm != 0) {
        int m = chan_resample(b.s, b.n, 1.0 + ppm * 1e-6, x);
        n = MIN(m, b.n);
    } else {
        memcpy(x, b.s, sizeof(cf32) * (size_t)b.n);
    }
    if (cfo_hz != 0)
        chan_cfo(x, n, DAB_RATE, cfo_hz);
    uint32_t seed = 777;
    if (snr_db > 0)
        awgn_mean(x, n, snr_db, &seed);
    res_t r;
    run_dab(x, n, &r);
    printf("  %-40s eventos=%d cpu=%.3f s/s\n    %s\n", name, r.evs.count, r.cpu / ((double)n / DAB_RATE), r.state);
    check_services(r.state);
    CHECK(ev_has(&r.evs, "\"kind\":\"ensemble\",\"eid\":\"C1D2\""));
    CHECK(ev_has(&r.evs, "\"kind\":\"service\",\"sid\":\"C201\""));
    evs_free(&r.evs);
    free(x);
    free(b.s);
}

int main(void)
{
    /* ---- 1. Tablas ---- */
    const int16_t *map = dab_carrier_map();
    CHECK(map != NULL);
    static int seen[2048];
    int ok = 1;
    for (int n = 0; n < DAB_K; n++) {
        int k = map[n];
        ok &= (k >= -768 && k <= 768 && k != 0);
        seen[k + 1024]++;
    }
    CHECK(ok);
    int dup = 0;
    for (int k = -768; k <= 768; k++)
        if (k != 0 && seen[k + 1024] != 1)
            dup++;
    CHECK_EQ(dup, 0); /* es una permutación de las 1536 portadoras */
    /* Los primeros términos de Π: 511 → k = −513; (13·511+511) mod 2048 = 1010 → −14 */
    CHECK_EQ(map[0], 511 - 1024);
    CHECK_EQ(map[1], 1010 - 1024);

    cf32 prs[DAB_TU];
    dab_prs_spectrum(prs);
    int cnt = 0;
    for (int b = 0; b < DAB_TU; b++) {
        double m = hypot(prs[b].re, prs[b].im);
        if (m > 0.5) {
            cnt++;
            CHECK_NEAR(m, 1.0, 1e-5);
        }
    }
    CHECK_EQ(cnt, DAB_K);
    CHECK(hypot(prs[0].re, prs[0].im) < 1e-6); /* sin portadora central */
    /* CRC-16 de FIB: "123456789" con el preset CRC-16/GENIBUS da 0xD64E */
    crc_t c;
    crc_init(&c, crc_preset("CRC-16/GENIBUS"));
    CHECK_EQ(crc_compute(&c, (const uint8_t *)"123456789", 9), 0xD64E);
    uint8_t fib[32] = {0x00, 0x05, 0x00, 0xC1, 0xD2, 0x00, 0x00, 0xFF};
    dab_fib_set_crc(fib);
    CHECK(dab_fib_crc_ok(fib));
    fib[3] ^= 1;
    CHECK(!dab_fib_crc_ok(fib));

    /* UEP */
    int sz, lv, kb;
    CHECK(dab_uep_info(0, &sz, &lv, &kb) && sz == 16 && lv == 5 && kb == 32);
    CHECK(dab_uep_info(63, &sz, &lv, &kb) && sz == 416 && lv == 1 && kb == 384);
    CHECK(!dab_uep_info(64, &sz, &lv, &kb));

    /* (1b) FIC con bits blandos de una grabación real */
    {
        FILE *f = fopen("tests/data/dab_real_fic_soft.bin", "rb");
        if (!f)
            printf("  (sin tests/data/dab_real_fic_soft.bin: se omite)\n");
        else {
            static int8_t soft[3 * 4 * 2304];
            size_t nr = fread(soft, 1, sizeof soft, f);
            fclose(f);
            CHECK_EQ((int)nr, (int)sizeof soft);
            conv_code_t code = conv_dab_k7();
            viterbi_t *v = viterbi_create(&code, 768);
            int good = 0, total = 0, label = 0;
            for (int blk = 0; blk < 12; blk++) {
                uint8_t fibs[3][32];
                if (dab_fic_decode(v, soft + blk * 2304, fibs) < 0)
                    continue;
                for (int k = 0; k < 3; k++) {
                    total++;
                    if (dab_fib_crc_ok(fibs[k]))
                        good++;
                    for (int i = 0; i + 8 <= 30; i++)
                        if (!memcmp(fibs[k] + i, "Towercom", 8) || !memcmp(fibs[k] + i, "Radio Sl", 8))
                            label++;
                }
            }
            printf("  FIC real: %d/%d FIB con CRC correcto, %d etiquetas\n", good, total, label);
            CHECK(good >= 30);
            CHECK(label >= 1);
            viterbi_destroy(v);
        }
    }

    /* FIC: ida y vuelta, con y sin errores de canal (el código corrige ráfagas de ruido blando) */
    {
        conv_code_t code = conv_dab_k7();
        viterbi_t *v = viterbi_create(&code, 768);
        uint8_t fibs[3][32];
        uint32_t rs = 99;
        for (int f = 0; f < 3; f++) {
            for (int i = 0; i < 30; i++)
                fibs[f][i] = (uint8_t)tu_rand(&rs);
            dab_fib_set_crc(fibs[f]);
        }
        uint8_t coded[2304];
        dab_fic_encode((const uint8_t(*)[32])fibs, coded);
        int8_t soft[2304];
        for (int i = 0; i < 2304; i++)
            soft[i] = coded[i] ? 50 : -50;
        uint8_t out[3][32];
        CHECK_EQ(dab_fic_decode(v, soft, out), 0);
        CHECK(memcmp(out, fibs, sizeof out) == 0);
        /* ruido blando: SNR por bit ≈ 6 dB */
        for (int i = 0; i < 2304; i++)
            soft[i] = (int8_t)CLAMP(lrint((coded[i] ? 50 : -50) + 25.0 * tu_gauss(&rs)), -127, 127);
        int e = dab_fic_decode(v, soft, out);
        CHECK(e >= 0);
        CHECK(memcmp(out, fibs, sizeof out) == 0);
        viterbi_destroy(v);
    }

    /* ---- 2. Modulador → canal → decodificador ---- */
    printf("Síntesis (SNR respecto a la potencia OFDM en 2,048 MHz):\n");
    case_dab("limpio", 0, 0, -1, 8);
    case_dab("10 dB", 0, 0, 10, 10);
    case_dab("CFO +37,4 kHz, +100 ppm, 10 dB", 37400, 100, 10, 12);
    case_dab("CFO -100,3 kHz, -100 ppm, 10 dB", -100300, -100, 10, 12);
    case_dab("CFO +200,6 kHz (entero 200), 10 dB", 200600, 0, 10, 12);

    /* ---- 3. Solo ruido ---- */
    {
        int n = DAB_RATE * 3;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        uint32_t seed = 4242;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        res_t r;
        run_dab(x, n, &r);
        printf("Solo ruido (3 s): eventos=%d, estado=%s\n", r.evs.count, r.state);
        CHECK_EQ(r.evs.count, 0);
        CHECK(strstr(r.state, "\"services\":[]"));
        evs_free(&r.evs);
        free(x);
    }

    /* ---- 4. Coste ---- */
    {
        iqbuf_t b = {0};
        char err[100];
        MOD_DAB.modulate("{\"frames\":30}", &b, err, sizeof err);
        uint32_t seed = 5;
        awgn_mean(b.s, b.n, 15, &seed);
        res_t r;
        run_dab(b.s, b.n, &r);
        double cost = r.cpu / ((double)b.n / DAB_RATE);
        printf("Coste: %.4f s de CPU por s de señal\n", cost);
        CHECK(cost < VR_CPU(0.3));
        evs_free(&r.evs);
        free(b.s);
    }
    return TEST_SUMMARY();
}
