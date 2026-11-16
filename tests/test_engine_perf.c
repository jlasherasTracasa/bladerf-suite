/*
 * test_engine_perf.c — Rendimiento del motor (docs/RENDIMIENTO.md).
 *
 *  A) S-meter por espectro frente al método por muestras. Un cliente que no
 *     escucha ya no mezcla ni diezma el flujo de banda ancha: su nivel sale de la
 *     última trama de espectro (channel_feed_spectrum). Aquí se compara con el
 *     nivel que mide channel_process() sobre las mismas muestras. Resultado
 *     esperado y demostrado: con el canal ≥ ~8 bins de ancho la diferencia es
 *     < 1 dB; con canales más estrechos que ~ENBW bins (a 61,44 Msps y FFT 4096
 *     un bin son 15 kHz y un canal NFM de 12,5 kHz cabe en uno) el espectro NO
 *     resuelve el canal: el ruido se mide de más (≈ 10·log10(ENBW·bin/bw)) y se
 *     comprueba con cifras que la diferencia pasa de 1 dB.
 *  B) El audio sigue funcionando: un canal que arranca sin audio y se activa
 *     después (cadena diferida) demodula un tono NFM con su frecuencia y nivel.
 *  C) Motor con fuente de fichero a ritmo real (hilo lector + anillo): la tasa
 *     medida es la pedida, 0 overruns y 0 drops, y sigue así tras retunear.
 */
#include "test_util.h"

#include "dsp/channel.h"
#include "dsp/spectrum.h"
#include "engine.h"
#include "tx/mod.h"
#include "tx/tx.h"

#include <unistd.h>

#define ENBW_BH 2.0044 /* Blackman-Harris 4 términos */

/* ---- Utilidades -------------------------------------------------------- */
static void gen_signal(cf32 *x, long n, double fs, double noise_db, double tone_off_hz, double tone_db, uint32_t *seed)
{
    const double sg = sqrt(pow(10.0, noise_db / 10.0) / 2.0); /* por componente */
    const double a = pow(10.0, tone_db / 20.0);
    const double w = 2.0 * M_PI * tone_off_hz / fs;
    for (long i = 0; i < n; i++) {
        float re = noise_db > -200 ? (float)(sg * tu_gauss(seed)) : 0.f;
        float im = noise_db > -200 ? (float)(sg * tu_gauss(seed)) : 0.f;
        if (tone_db > -200) {
            double ph = fmod(w * (double)i, 2.0 * M_PI);
            re += (float)(a * cos(ph));
            im += (float)(a * sin(ph));
        }
        x[i] = (cf32){re, im};
    }
}

typedef struct {
    float *db;
    int n;
    bool have;
} frame_t;

static void on_frame(void *ctx, const float *db, int n, int navg)
{
    (void)navg;
    frame_t *f = ctx;
    if (!f->db)
        f->db = malloc(sizeof(float) * (size_t)n);
    memcpy(f->db, db, sizeof(float) * (size_t)n);
    f->n = n;
    f->have = true;
}

static void sink_level(void *ctx, const int16_t *pcm, int nframes, float level, unsigned flags)
{
    (void)pcm;
    (void)nframes;
    (void)flags;
    *(float *)ctx = level;
}

/* Última trama de espectro (CPU) de x a la tasa fs. */
static void spectrum_of(const cf32 *x, long n, double fs, int fft, frame_t *fr)
{
    spectrum_t *sp = spectrum_create(fft, fs, 20, false);
    int16_t *q = malloc(sizeof(int16_t) * 2 * 65536);
    for (long i0 = 0; i0 < n; i0 += 65536) {
        int m = (int)MIN(65536L, n - i0);
        for (int i = 0; i < m; i++) {
            q[2 * i] = (int16_t)lrintf(x[i0 + i].re * 2048.f);
            q[2 * i + 1] = (int16_t)lrintf(x[i0 + i].im * 2048.f);
        }
        spectrum_feed(sp, q, m, on_frame, fr);
    }
    free(q);
    spectrum_destroy(sp);
}

/* Nivel (dBFS) por muestras: channel_process() durante todo x. */
static float level_by_samples(const cf32 *x, long n, double fs, int64_t center, const chan_params_t *p)
{
    channel_t *c = channel_create();
    float lvl = -150.f;
    channel_configure(c, fs, center, p);
    /* El nivel arranca en −150 dBFS y converge con τ = 50 ms: se recorre x en bucle
     * hasta 0,4 s (8τ) de señal (el ruido y los tonos son estacionarios). */
    const long total = (long)(fs * 0.4);
    for (long done = 0, i0 = 0; done < total;) {
        int m = (int)MIN(MIN(65536L, n - i0), total - done);
        channel_process(c, x + i0, m, sink_level, &lvl);
        done += m;
        i0 = (i0 + m) % n;
    }
    lvl = channel_level(c);
    channel_destroy(c);
    return lvl;
}

static float level_by_spectrum(const frame_t *fr, double fs, int64_t center, const chan_params_t *p, double enbw)
{
    channel_t *c = channel_create();
    float lvl = -150.f;
    channel_configure(c, fs, center, p);
    for (int k = 0; k < 100; k++) /* 5 s a 20 fps: el suavizado de 50 ms converge */
        channel_feed_spectrum(c, fr->db, fr->n, enbw, 0.05, sink_level, &lvl);
    lvl = channel_level(c);
    channel_destroy(c);
    return lvl;
}

typedef struct {
    const char *name;
    double fs;
    int fft;
    demod_mode_t mode;
    int bw;
    double ch_off;      /* canal a ch_off del centro */
    double noise_db;    /* potencia total del ruido de banda ancha (dBFS) */
    double tone_off;    /* tono respecto al centro */
    double tone_db;
    double tol;         /* diferencia máxima esperada (dB) */
    bool expect_gt1;    /* demostrar que la diferencia pasa de 1 dB */
} meter_case_t;

static void test_meter(void)
{
    const meter_case_t cases[] = {
        {"NFM 12,5k  ruido    2,4 Msps FFT4096 ", 2.4e6, 4096, MODE_NFM, 12500, 200e3, -40, 0, -300, 1.0, false},
        {"NFM 12,5k  tono     2,4 Msps FFT4096 ", 2.4e6, 4096, MODE_NFM, 12500, 200e3, -70, 202e3, -25, 1.0, false},
        {"WFM 180k   ruido    2,4 Msps FFT4096 ", 2.4e6, 4096, MODE_WFM, 180000, -300e3, -40, 0, -300, 1.0, false},
        {"AM 10k     ruido    2,4 Msps FFT4096 ", 2.4e6, 4096, MODE_AM, 10000, 100e3, -40, 0, -300, 1.0, false},
        {"USB 2,7k   ruido    2,4 Msps FFT4096 ", 2.4e6, 4096, MODE_USB, 2700, 100e3, -40, 0, -300, 1.0, false},
        {"NFM 12,5k  ruido   61,44 Msps FFT65536", 61.44e6, 65536, MODE_NFM, 12500, 5e6, -30, 0, -300, 1.0, false},
        /* tono fuera de centro de bin (festoneo de la ventana, hasta ~1 dB) con canal < 1 bin */
        {"NFM 12,5k  tono    61,44 Msps FFT4096 ", 61.44e6, 4096, MODE_NFM, 12500, 5e6, -80, 5.001e6, -30, 1.5, false},
        /* Canales más estrechos que ~2 bins: el espectro no los resuelve (ruido de más) */
        {"NFM 12,5k  ruido   61,44 Msps FFT4096 ", 61.44e6, 4096, MODE_NFM, 12500, 5e6, -30, 0, -300, 5.0, true},
        {"CW 500     ruido    2,4 Msps FFT4096 ", 2.4e6, 4096, MODE_CW, 500, 100e3, -40, 0, -300, 5.0, true},
    };
    printf("A) S-meter: por espectro frente a por muestras (dBFS)\n");
    printf("   %-40s  muestras  espectro  dif.   bins del canal\n", "caso");
    uint32_t seed = 12345;
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const meter_case_t *m = &cases[k];
        long n = (long)(m->fs * 0.12);                  /* 120 ms: ≥ 2 constantes de tiempo */
        n = MAX(n, (long)m->fft * 64L);
        n = MIN(n, 8L << 20);
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        gen_signal(x, n, m->fs, m->noise_db, m->tone_off, m->tone_db, &seed);
        frame_t fr = {0};
        spectrum_of(x, n, m->fs, m->fft, &fr);
        CHECK(fr.have);
        const int64_t center = 100000000;
        chan_params_t p = {.mode = m->mode, .freq_hz = center + (int64_t)m->ch_off, .bw_hz = m->bw,
                           .squelch_db = NAN, .deemph_us = 50, .stereo = false, .audio_on = false};
        if (m->tone_db > -200)
            p.freq_hz = center + (int64_t)m->tone_off;
        float ls = level_by_samples(x, n, m->fs, center, &p);
        float le = level_by_spectrum(&fr, m->fs, center, &p, ENBW_BH);
        double bins = m->bw / (m->fs / m->fft);
        printf("   %-40s  %7.2f  %8.2f  %+5.2f  %6.1f\n", m->name, ls, le, le - ls, bins);
        if (m->expect_gt1) {
            CHECK(fabs(le - ls) > 1.0); /* demostrado: aquí el espectro NO equivale */
            CHECK(fabs(le - ls) < m->tol);
        } else {
            CHECK(fabs(le - ls) <= m->tol);
        }
        free(fr.db);
        free(x);
    }
}

/* ---- B) audio con cadena diferida --------------------------------------- */
typedef struct {
    long frames;
    int16_t *pcm;
    long cap;
    float level;
    unsigned flags;
} pcm_sink_t;

static void sink_pcm(void *ctx, const int16_t *pcm, int nframes, float level, unsigned flags)
{
    pcm_sink_t *s = ctx;
    s->level = level;
    s->flags = flags;
    if (!pcm)
        return;
    int ch = (flags & CHAN_FLAG_STEREO) ? 2 : 1;
    for (int i = 0; i < nframes && s->frames < s->cap; i++)
        s->pcm[s->frames++] = pcm[i * ch];
}

static void test_audio(void)
{
    printf("B) Audio NFM con cadena diferida\n");
    const double fs = 2.4e6, f_off = 150e3, f_mod = 1000.0, dev = 3000.0;
    const long n = (long)(fs * 1.0);
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    double ph = 0;
    for (long i = 0; i < n; i++) {
        double t = (double)i / fs;
        double f = f_off + dev * sin(2.0 * M_PI * f_mod * t);
        ph += 2.0 * M_PI * f / fs;
        ph = fmod(ph, 2.0 * M_PI);
        x[i] = (cf32){(float)(0.1 * cos(ph)), (float)(0.1 * sin(ph))}; /* −20 dBFS */
    }
    const int64_t center = 100000000;
    channel_t *c = channel_create();
    chan_params_t p = {.mode = MODE_NFM, .freq_hz = center + (int64_t)f_off, .bw_hz = 12500, .squelch_db = NAN,
                       .deemph_us = 50, .stereo = false, .audio_on = false};
    channel_configure(c, fs, center, &p);
    /* Sin audio, channel_feed_spectrum no necesita la cadena: se activa el audio después */
    p.audio_on = true;
    channel_configure(c, fs, center, &p);
    CHECK(channel_wants_samples(c));
    pcm_sink_t sk = {.cap = 60000};
    sk.pcm = malloc(sizeof(int16_t) * (size_t)sk.cap);
    for (long i0 = 0; i0 < n; i0 += 24000)
        channel_process(c, x + i0, (int)MIN(24000L, n - i0), sink_pcm, &sk);
    printf("   tramas PCM: %ld (esperadas ≈ %d), nivel %.1f dBFS\n", sk.frames, AUDIO_RATE, sk.level);
    CHECK(sk.frames > AUDIO_RATE * 0.9 && sk.frames < AUDIO_RATE * 1.05);
    CHECK_NEAR(sk.level, -20.0, 1.0);
    /* Tono de 1 kHz: pasos por cero en la segunda mitad (primeras muestras = transitorio) */
    long zc = 0, i0 = sk.frames / 2;
    double e = 0;
    for (long i = i0 + 1; i < sk.frames; i++) {
        if ((sk.pcm[i - 1] < 0) != (sk.pcm[i] < 0))
            zc++;
        e += (double)sk.pcm[i] * sk.pcm[i];
    }
    double secs = (double)(sk.frames - i0) / AUDIO_RATE, f_est = zc / 2.0 / secs;
    double rms = sqrt(e / (double)(sk.frames - i0)) / 32767.0;
    printf("   tono recuperado: %.1f Hz, RMS %.3f\n", f_est, rms);
    CHECK_NEAR(f_est, f_mod, 30.0);
    CHECK(rms > 0.05);

    /* Apagar el audio y volver a encenderlo reinicia la cadena sin fallar */
    p.audio_on = false;
    channel_configure(c, fs, center, &p);
    CHECK(!channel_wants_samples(c));
    p.audio_on = true;
    channel_configure(c, fs, center, &p);
    sk.frames = 0;
    channel_process(c, x, 240000, sink_pcm, &sk);
    CHECK(sk.frames > 0);
    /* Fuera de banda: sin audio ni cadena, solo trama de nivel */
    p.freq_hz = center + 5000000;
    channel_configure(c, fs, center, &p);
    CHECK(!channel_wants_samples(c));
    channel_destroy(c);
    free(sk.pcm);
    free(x);
}

/* ---- B2) continuidad entre bloques y ruta Q11 -------------------------- */
static void test_blocks(void)
{
    printf("B2) Mismo audio con bloques de tamaños irregulares y con la ruta Q11\n");
    const double fs = 6.0e6, f_off = 700e3, f_mod = 800.0, dev = 3000.0;
    const long n = (long)(fs * 0.5);
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
    double ph = 0;
    for (long i = 0; i < n; i++) {
        ph += 2.0 * M_PI * (f_off + dev * sin(2.0 * M_PI * f_mod * (double)i / fs)) / fs;
        ph = fmod(ph, 2.0 * M_PI);
        /* Q11 con continua (+20, −10 cuentas) para probar su resta */
        q[2 * i] = (int16_t)lrint(400.0 * cos(ph) + 20.0);
        q[2 * i + 1] = (int16_t)lrint(400.0 * sin(ph) - 10.0);
        x[i] = (cf32){(float)(q[2 * i] - 20) / 2048.f, (float)(q[2 * i + 1] + 10) / 2048.f};
    }
    const int64_t center = 100000000;
    chan_params_t p = {.mode = MODE_NFM, .freq_hz = center + (int64_t)f_off, .bw_hz = 12500, .squelch_db = NAN,
                       .deemph_us = 50, .stereo = false, .audio_on = true};
    pcm_sink_t ref = {.cap = 40000}, irr = {.cap = 40000}, q11 = {.cap = 40000};
    ref.pcm = malloc(sizeof(int16_t) * (size_t)ref.cap);
    irr.pcm = malloc(sizeof(int16_t) * (size_t)irr.cap);
    q11.pcm = malloc(sizeof(int16_t) * (size_t)q11.cap);
    channel_t *a = channel_create(), *b = channel_create(), *c = channel_create();
    channel_configure(a, fs, center, &p);
    channel_configure(b, fs, center, &p);
    channel_configure(c, fs, center, &p);
    channel_process(a, x, (int)n, sink_pcm, &ref);
    static const int sizes[] = {1000, 777, 8191, 8192, 3, 100003, 65536, 12345};
    long pos = 0;
    for (int k = 0; pos < n; k++) {
        int m = (int)MIN((long)sizes[k % 8], n - pos);
        channel_process(b, x + pos, m, sink_pcm, &irr);
        channel_process_q11(c, q + 2 * pos, m, 20.f, -10.f, sink_pcm, &q11);
        pos += m;
    }
    long nn = MIN(ref.frames, MIN(irr.frames, q11.frames));
    double e_ref = 0, e_irr = 0, e_q = 0;
    for (long i = 2000; i < nn; i++) { /* tras el transitorio inicial */
        double r = ref.pcm[i], d1 = irr.pcm[i] - r, d2 = q11.pcm[i] - r;
        e_ref += r * r;
        e_irr += d1 * d1;
        e_q += d2 * d2;
    }
    printf("   tramas ref %ld, irregular %ld, q11 %ld; error relativo irregular %.1e, q11 %.1e\n", ref.frames,
           irr.frames, q11.frames, sqrt(e_irr / e_ref), sqrt(e_q / e_ref));
    CHECK(ref.frames > 20000 && ref.frames == irr.frames && ref.frames == q11.frames);
    CHECK(sqrt(e_irr / e_ref) < 2e-3); /* el fasor float del mezclador deriva ~1e-4 rad por trozo */
    CHECK(sqrt(e_q / e_ref) < 2e-3);   /* Q11 redondeado a enteros: ruido de cuantización */
    channel_destroy(a);
    channel_destroy(b);
    channel_destroy(c);
    free(ref.pcm);
    free(irr.pcm);
    free(q11.pcm);
    free(x);
    free(q);
}

/* ---- C) motor completo con fuente de fichero ---------------------------- */
static double json_num(const char *js, const char *key)
{
    const char *p = strstr(js, key);
    return p ? atof(p + strlen(key)) : -1;
}

static void test_engine(void)
{
    printf("C) Motor con hilo lector (fichero a ritmo real)\n");
    char prefix[64], meta[80];
    snprintf(prefix, sizeof prefix, "/tmp/test_engine_perf_src_%d", (int)getpid()); /* único: pruebas en paralelo */
    snprintf(meta, sizeof meta, "%s.sigmf-meta", prefix);
    iqbuf_t iq = {0};
    iq.rate = 4e6;
    iq.n = 800000;
    iq.s = calloc((size_t)iq.n, sizeof(cf32));
    for (int i = 0; i < iq.n; i++)
        iq.s[i] = (cf32){0.3f * cosf(2.f * (float)M_PI * 0.05f * (float)i), 0.3f * sinf(2.f * (float)M_PI * 0.05f * (float)i)};
    char err[300] = "";
    CHECK(tx_write_sigmf(prefix, &iq, 1, 433920000, "tono de prueba", err, sizeof err) == 0);
    source_t *src = source_open_file(meta, 0, 0, err, sizeof err);
    CHECK(src != NULL);
    if (!src)
        return;
    server_callbacks_t cb = {0};
    server_t *srv = server_create("127.0.0.1", 0, "web", "/tmp", &cb, NULL, err, sizeof err);
    CHECK(srv != NULL);
    engine_cfg_t cfg = {.center_hz = 433920000, .rate = 4000000, .fft_size = 1024, .fps = 20, .det_snr = 10};
    engine_t *e = engine_create(src, srv, &cfg, false, "/tmp");
    CHECK(e != NULL);
    /* Dos clientes: uno escucha NFM (200 kHz sobre el tono), otro solo mide */
    engine_add_client(e, 1);
    engine_add_client(e, 2);
    chan_params_t p;
    CHECK(engine_get_channel(e, 1, &p));
    p.freq_hz = 433920000 + 200000;
    p.audio_on = true;
    engine_set_channel(e, 1, &p);
    CHECK(engine_start(e) == 0);
    sleep(3);
    /* Un retune pausa y reanuda el lector (el fichero ignora la frecuencia, pero el
     * camino de pausa, vaciado del anillo y reanudación se recorre igual) */
    engine_cfg_t c2;
    engine_get_config(e, &c2);
    c2.center_hz += 1000;
    engine_request(e, &c2, CFG_CENTER);
    sleep(3);
    sb_t sb = {0};
    engine_status_json(e, &sb);
    double rate = json_num(sb.data, "\"stats\":{\"rate\":");
    double ovr = json_num(sb.data, "\"overruns\":");
    double drops = json_num(sb.data, "\"drops\":");
    double load = json_num(sb.data, "\"load\":");
    printf("   tasa medida %.2f Msps, overruns %.0f, drops %.0f, carga DSP %.3f\n", rate / 1e6, ovr, drops, load);
    CHECK(rate > 3.6e6 && rate < 4.4e6);
    CHECK(ovr == 0);
    CHECK(drops == 0);
    CHECK(strstr(sb.data, "\"ring_ms\":") != NULL);
    sb_free(&sb);

    /* DSP atrasado: 1,5 s sin consumir con un anillo de 0,5 s (4 Msps → 0,5 s) → el
     * lector descarta ≈ 1 s de bloques enteros (≈ 50 de 20 ms), sin overruns de
     * hardware, y el flujo se recupera */
    engine_debug_stall(e, 1.5);
    sleep(5);
    sb_t sb2 = {0};
    engine_status_json(e, &sb2);
    double ovr2 = json_num(sb2.data, "\"overruns\":"), drops2 = json_num(sb2.data, "\"drops\":");
    printf("   tras parar el DSP 1,5 s: drops %.0f (≈ 50 esperados), overruns %.0f\n", drops2, ovr2);
    CHECK(drops2 >= 25 && drops2 <= 80);
    CHECK(ovr2 == 0);
    sb_free(&sb2);
    engine_destroy(e);
    server_destroy(srv);
    source_close(src);
    iqbuf_free(&iq);
    unlink(meta);
    snprintf(meta, sizeof meta, "%s.sigmf-data", prefix);
    unlink(meta);
}

int main(void)
{
    test_meter();
    test_audio();
    test_blocks();
    test_engine();
    return TEST_SUMMARY();
}
