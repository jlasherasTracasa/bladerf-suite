/*
 * test_adsb.c — Decodificador ADS-B con los vectores publicados en
 *   Junzi Sun, "The 1090 Megahertz Riddle", 2ª ed., TU Delft OPEN, 2021
 *   (cap. 3: identificación, cap. 5: posición CPR, cap. 6: velocidad).
 */
#include "test_util.h"

#include "decoders/adsb.h"
#include "tx/mod.h"

/* ---- Mejora cuantificada: tráfico sintético con ruido y solapamientos ------ */
#define NAC 24  /* aeronaves */
#define NCALL 3 /* ráfagas de 4 tramas por aeronave (todas las tramas son distintas) */
#define NFR (NAC * NCALL * 4)
#define FLEN 240 /* muestras de una trama (preámbulo + 112 bits) */

static char truth[NFR][29];
static int ntruth;
static int hits, falses;
static unsigned char seen_t[NFR];

/* Verdad de terreno: hex de cada trama decodificada en limpio. */
static void on_truth(void *ctx, const char *json, size_t len)
{
    (void)ctx;
    (void)len;
    const char *h = strstr(json, "\"hex\":\"");
    if (h && ntruth < NFR) {
        memcpy(truth[ntruth], h + 7, 28);
        truth[ntruth][28] = 0;
        ntruth++;
    }
}

/* Cada transmisión cuenta una vez; un hex que no está en la verdad es una trama falsa. */
static void on_ev(void *ctx, const char *json, size_t len)
{
    (void)ctx;
    (void)len;
    const char *h = strstr(json, "\"hex\":\"");
    if (!h)
        return;
    h += 7;
    for (int i = 0; i < ntruth; i++)
        if (strncmp(h, truth[i], 28) == 0) {
            if (!seen_t[i]) {
                seen_t[i] = 1;
                hits++;
            }
            return;
        }
    falses++;
}

static uint32_t g_seed = 777;
static double urand(void) { return (tu_rand(&g_seed) >> 8) / 16777216.0; }

static cf32 *frames_pool; /* NFR × FLEN muestras */

/* Una llamada al modulador por aeronave y ráfaga; las 4 tramas se recortan de la
 * salida (400 muestras de silencio y luego 240 + 20000 de hueco por trama). */
static void build_pool(void)
{
    const modulator_desc_t *m = modulator_find("adsb");
    frames_pool = malloc(sizeof(cf32) * (size_t)NFR * FLEN);
    int k = 0;
    for (int ac = 0; ac < NAC; ac++)
        for (int c = 0; c < NCALL; c++) {
            char par[220], err[100];
            snprintf(par, sizeof par,
                     "{\"icao\":\"%06X\",\"callsign\":\"TST%02d%d\",\"alt\":%d,\"lat\":%.4f,\"lon\":%.4f,"
                     "\"gs\":%d,\"trk\":%d,\"vr\":%d,\"repeat\":1}",
                     0x400000 + ac * 7919, ac, c, 20000 + 900 * ac + 200 * c, 51.0 + 0.1 * ac + 0.01 * c,
                     2.0 + 0.05 * ac, 200 + ac + c, (ac * 13 + c * 40) % 360, -500 + 20 * ac);
            iqbuf_t b = {0};
            CHECK(m->modulate(par, &b, err, sizeof err) == 0);
            for (int f = 0; f < 4; f++, k++)
                memcpy(frames_pool + (size_t)k * FLEN, b.s + 400 + f * (FLEN + 20000), sizeof(cf32) * FLEN);
            iqbuf_free(&b);
        }
    adsb_t *a = adsb_create();
    adsb_opts_t o = {.fix_max = 0, .raw = true};
    adsb_set_options(a, &o);
    adsb_set_emit(a, on_truth, NULL);
    cf32 buf[1000];
    int16_t q[2000];
    ntruth = 0;
    for (int i = 0; i < NFR; i++) {
        memset(buf, 0, sizeof buf);
        memcpy(buf + 100, frames_pool + (size_t)i * FLEN, sizeof(cf32) * FLEN);
        cf32_to_q11(buf, 1000, 1.0, q);
        adsb_process(a, q, 1000, 0.0);
    }
    adsb_destroy(a);
}

/* Mezcla las tramas en una ventana de w muestras con desfases aleatorios (los
 * solapamientos salen solos), amplitud g ∈ [gmin,1] y fase aleatoria, y añade
 * ruido blanco; snr_db es la relación del pulso de amplitud 0,8 (g = 1). */
static void synth(cf32 *x, int w, double gmin, double snr_db)
{
    memset(x, 0, sizeof(cf32) * (size_t)w);
    for (int i = 0; i < NFR; i++) {
        int off = (int)(urand() * (w - FLEN - 10)) + 5;
        double g = gmin + (1 - gmin) * urand(), ph = 6.2831853 * urand();
        float c = (float)(g * cos(ph)), s = (float)(g * sin(ph));
        const cf32 *f = frames_pool + (size_t)i * FLEN;
        for (int k = 0; k < FLEN; k++) {
            x[off + k].re += f[k].re * c - f[k].im * s;
            x[off + k].im += f[k].re * s + f[k].im * c;
        }
    }
    double sig = 0.8 / sqrt(2.0 * pow(10.0, snr_db / 10.0));
    for (int i = 0; i < w; i++) {
        x[i].re += (float)(sig * tu_gauss(&g_seed));
        x[i].im += (float)(sig * tu_gauss(&g_seed));
    }
}

/* Decodifica con una configuración; devuelve las tramas correctas recuperadas. */
static int run_cfg(const int16_t *q, int w, int fix, bool crcframe, adsb_stats_t *st)
{
    adsb_t *a = adsb_create();
    adsb_opts_t o = {.fix_max = fix, .crcframe = crcframe, .raw = true};
    adsb_set_options(a, &o);
    adsb_set_emit(a, on_ev, NULL);
    hits = falses = 0;
    memset(seen_t, 0, sizeof seen_t);
    for (int off = 0; off < w; off += 65536)
        adsb_process(a, q + 2 * (size_t)off, MIN(65536, w - off), off / 2e6);
    adsb_get_stats(a, st);
    adsb_destroy(a);
    return hits;
}

static void test_improvement(void)
{
    printf("Mejora cuantificada (%d tramas únicas de %d aeronaves)\n", NFR, NAC);
    build_pool();
    CHECK_EQ(ntruth, NFR);
    printf("  escenario           SNR dB  antes  +fix1-2  +crcframe  ambos | corr1 corr2 crc | falsas\n");
    static const struct {
        const char *name;
        int w;
        double gmin;
    } sc[] = {{"esparso (7% duty)", 1000000, 1.0}, {"denso (28% duty)", 250000, 1.0}, {"denso, cerca-lejos", 250000, 0.3}};
    static const double snrs[] = {8, 10, 12, 15, 20};
    long tb = 0, tf = 0, tc = 0, ta = 0;
    int tfalse = 0;
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < 5; k++) {
            int w = sc[s].w;
            cf32 *x = malloc(sizeof(cf32) * (size_t)w);
            int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)w);
            synth(x, w, sc[s].gmin, snrs[k]);
            cf32_to_q11(x, w, 1.0, q);
            adsb_stats_t st0, st1, st2, st3;
            int before = run_cfg(q, w, 0, false, &st0), f0 = falses;
            int fixed = run_cfg(q, w, 2, false, &st1), f1 = falses;
            int crc = run_cfg(q, w, 0, true, &st2), f2 = falses;
            int both = run_cfg(q, w, 2, true, &st3), f3 = falses;
            printf("  %-19s %5.0f  %5d  %7d  %9d  %5d | %5lu %5lu %3lu | %d\n", sc[s].name, snrs[k], before, fixed, crc,
                   both, st3.fix1, st3.fix2, st3.crc_rec, f0 + f1 + f2 + f3);
            tb += before;
            tf += fixed;
            tc += crc;
            ta += both;
            tfalse += f0 + f1 + f2 + f3;
            CHECK(fixed >= before && both >= before);
            free(x);
            free(q);
        }
    printf("  TOTAL (de %d): antes %ld, +fix %ld, +crcframe %ld, ambos %ld (%+.1f %%); falsas: %d\n", NFR * 15, tb, tf, tc,
           ta, 100.0 * (double)(ta - tb) / (double)tb, tfalse);
    CHECK(ta > tb);
    CHECK_EQ(tfalse, 0);
    free(frames_pool);
}

/* Un minuto de solo ruido a 2 Msps: ni una trama aceptada con todo activado. */
static void test_noise_only(void)
{
    printf("Un minuto de solo ruido (fix 2 + crcframe, 120 Mmuestras)\n");
    adsb_t *a = adsb_create();
    adsb_opts_t o = {.fix_max = 2, .crcframe = true, .raw = true};
    adsb_set_options(a, &o);
    hits = falses = 0;
    ntruth = 0;
    adsb_set_emit(a, on_ev, NULL);
    const int B = 2000000;
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)B);
    uint32_t seed = 4242;
    for (int sec = 0; sec < 60; sec++) {
        for (int i = 0; i < B; i++) {
            double u1 = (tu_rand(&seed) + 1.0) / 4294967297.0, u2 = (tu_rand(&seed) + 1.0) / 4294967297.0;
            double r = 250.0 * sqrt(-2.0 * log(u1)), ph = 2.0 * M_PI * u2;
            q[2 * i] = (int16_t)lrint(r * cos(ph));
            q[2 * i + 1] = (int16_t)lrint(r * sin(ph));
        }
        adsb_process(a, q, B, 1000.0 + sec);
    }
    adsb_stats_t st;
    adsb_get_stats(a, &st);
    printf("  tramas aceptadas %lu, eventos %d, candidatos DF17/18 rechazados %lu\n", st.frames, hits + falses, st.rejected);
    CHECK_EQ(st.frames, 0);
    CHECK_EQ(hits + falses, 0);
    free(q);
    adsb_destroy(a);
}

int main(void)
{
    test_improvement();
    test_noise_only();
    adsb_t *a = adsb_create();
    double now = 1000.0;

    /* Identificación: 8D4840D6202CC371C32CE0576098 → KLM1023 (ICAO 4840D6) */
    CHECK_EQ(adsb_decode_hex(a, "8D4840D6202CC371C32CE0576098", now), 17);
    const aircraft_t *p = adsb_find(a, 0x4840D6);
    CHECK(p != NULL);
    if (p) {
        CHECK(strcmp(p->callsign, "KLM1023") == 0);
        CHECK(strcmp(p->category, "A0") == 0);
    }

    /* Par CPR par/impar → 52,2572° N, 3,9194° E (libro, cap. 5) */
    const aircraft_t *q = NULL;
    /* El libro toma como referencia la trama PAR (la más reciente del par). Si la
     * impar fuera la más reciente, la misma pareja daría 52,2658° / 3,9389°: las
     * tramas se capturaron en instantes distintos y el avión se movió ~1 km. */
    CHECK_EQ(adsb_decode_hex(a, "8D40621D58C386435CC412692AD6", now + 1), 17); /* trama impar */
    CHECK_EQ(adsb_decode_hex(a, "8D40621D58C382D690C8AC2863A7", now + 2), 17); /* trama par (reciente) */
    q = adsb_find(a, 0x40621D);
    CHECK(q != NULL);
    if (q) {
        CHECK(q->pos_valid);
        CHECK_NEAR(q->lat, 52.2572, 0.001);
        CHECK_NEAR(q->lon, 3.9194, 0.001);
        CHECK(q->alt_valid);
        CHECK_EQ(q->altitude_ft, 38000);
    }

    /* Velocidad: 8D485020994409940838175B284F → 159 kt, 182,9°, −832 ft/min (cap. 6) */
    CHECK_EQ(adsb_decode_hex(a, "8D485020994409940838175B284F", now + 3), 17);
    const aircraft_t *v = adsb_find(a, 0x485020);
    CHECK(v != NULL);
    if (v) {
        CHECK(v->vel_valid);
        CHECK_NEAR(v->gs_kt, 159.0, 0.5);
        CHECK_NEAR(v->track_deg, 182.9, 0.2);
        CHECK(v->vr_valid);
        CHECK_EQ(v->vrate_fpm, -832);
    }

    /* Integridad: un bit erróneo se corrige; dos no */
    adsb_t *b = adsb_create();
    CHECK_EQ(adsb_decode_hex(b, "8D4840D6202CC371C32CE0576099", now), 17); /* último bit del CRC cambiado */
    CHECK(adsb_find(b, 0x4840D6) != NULL);
    CHECK_EQ(adsb_decode_hex(b, "8D4840D6202CC371C32CE0576F99", now), -1);
    CHECK_EQ(adsb_decode_hex(b, "8D4840D6202CC371C32CE057609", now), -1); /* longitud errónea */
    adsb_destroy(b);

    /* CRC-24 y NL */
    uint8_t m[14] = {0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98};
    CHECK_EQ(modes_crc24(m, 88), 0x576098);
    CHECK_EQ(cpr_nl(0.0), 59);
    CHECK_EQ(cpr_nl(52.25), 36);
    CHECK_EQ(cpr_nl(87.0), 2);
    CHECK_EQ(cpr_nl(88.0), 1);

    /* Estado JSON: mismos campos que el mensaje antiguo, sin "type" */
    sb_t sb = {0};
    adsb_state_json(a, &sb, now + 4);
    CHECK(strstr(sb.data, "\"msgs\":") && strstr(sb.data, "\"ac\":[") && strstr(sb.data, "\"icao\":\"4840D6\""));
    CHECK(strstr(sb.data, "\"type\"") == NULL);
    sb_free(&sb);
    adsb_json(a, &sb, now + 4);
    CHECK(strncmp(sb.data, "{\"type\":\"adsb\",\"msgs\":", 21) == 0);
    sb_free(&sb);
    adsb_destroy(a);
    return TEST_SUMMARY();
}
