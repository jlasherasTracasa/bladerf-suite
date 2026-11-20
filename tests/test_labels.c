/*
 * test_labels.c — Índice de muestra por evento, contrapresión (--replay-fast) y etiquetas en tres
 * niveles (--labels). Ver decoders/decoder.h (dec_emit_span), decoders/host.h y util/labels.h.
 *
 * Parte 1 (host, sin servidor): cada modulador propio genera una ráfaga que se coloca en un índice
 *   CONOCIDO de una captura con ruido, desplazada de frecuencia, a una tasa de fuente mayor que la
 *   del canal (así pasa por el DDC real) y se alimenta con dec_host_feed_at() en bloques que no están
 *   alineados con la ráfaga. El evento debe traer "sample" a ±1 símbolo de la posición verdadera
 *   (inicio de la bandera de apertura en AIS/APRS, inicio del preámbulo en ADS-B y LoRa), con
 *   "sample_src":"mark", "snr_db" cerca de la SNR de canal inyectada y "rssi_dbfs". POCSAG (que aún no
 *   marca posición) debe dar el valor por omisión: "sample_src":"block".
 * Parte 2 (binario, solo si existe ../bladerf-lab): genera un fichero con una ráfaga AIS y una portadora
 *   continua sin decodificador, y lanza `bladerf-lab --replay f --labels l.sigmf-meta`. Comprueba que
 *   sale con código 0 sin pérdidas (ningún "gap"), que el fichero de etiquetas tiene la clase
 *   "verified" (el mensaje AIS) y la clase "unknown" (la portadora, que ningún decodificador explica) y
 *   que el resto es "empty" implícito (declarado en el meta).
 */
#include "test_util.h"

#include "decoders/host.h"
#include "tx/mod.h"
#include "util/json.h"

#include <libgen.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static evs_t g_ev;

static void on_event(void *ctx, const char *line)
{
    (void)ctx;
    pthread_mutex_lock(&lock);
    tu_collect(&g_ev, line, strlen(line));
    pthread_mutex_unlock(&lock);
}

/* Valor numérico de "key": en la primera línea de g_ev que contenga 'needle'. false si no hay. */
static bool ev_num(const char *needle, const char *key, double *out)
{
    pthread_mutex_lock(&lock);
    bool ok = false;
    const char *p = g_ev.data ? strstr(g_ev.data, needle) : NULL;
    if (p) {
        const char *eol = strchr(p, '\n');
        char k[40];
        snprintf(k, sizeof k, "\"%s\":", key);
        const char *q = strstr(p, k);
        if (q && (!eol || q < eol)) {
            *out = strtod(q + strlen(k), NULL);
            ok = true;
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

typedef struct {
    const char *proto;     /* decodificador y modulador */
    const char *params;    /* parámetros del modulador (NULL = ejemplo) */
    double rate;           /* tasa de la captura */
    int64_t center, freq;  /* centro de la captura y frecuencia del canal */
    double snr_db;         /* SNR de canal (en el ancho del DDC) o en toda la banda si es WIDE */
    double lead_s;         /* posición de la ráfaga en la captura */
    double truth_off;      /* inicio verdadero: segundos tras el principio de la salida del modulador */
} case_t;

/* Genera el caso y lo pasa por el host. truth = índice de muestra verdadero del inicio. */
static bool run_case(const case_t *c, int64_t *truth, int *nsamp)
{
    const modulator_desc_t *m = modulator_find(c->proto);
    if (!m)
        return false;
    iqbuf_t b = {0};
    char err[200] = "";
    if (m->modulate(c->params, &b, err, sizeof err) < 0) {
        printf("  (modulador %s: %s)\n", c->proto, err);
        iqbuf_free(&b);
        return false;
    }
    const decoder_desc_t *d = decoder_find(c->proto);
    const bool wide = d->kind == DEC_WIDE;
    /* a la tasa de la captura */
    int n;
    cf32 *x;
    if (fabs(b.rate - c->rate) < 1) {
        n = b.n;
        x = malloc(sizeof(cf32) * (size_t)n);
        memcpy(x, b.s, sizeof(cf32) * (size_t)n);
    } else {
        double ratio = c->rate / b.rate;
        x = malloc(sizeof(cf32) * ((size_t)((double)b.n * ratio) + 64));
        n = chan_resample(b.s, b.n, ratio, x);
    }
    /* potencia de las muestras "encendidas" y desvío al canal */
    double pmax = 0;
    for (int i = 0; i < n; i++)
        pmax = fmax(pmax, (double)x[i].re * x[i].re + (double)x[i].im * x[i].im);
    double ps = 0;
    int non = 0;
    for (int i = 0; i < n; i++) {
        double p = (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
        if (p >= 0.25 * pmax) {
            ps += p;
            non++;
        }
    }
    ps /= non;
    /* escala: amplitud de pico 0,3 (deja sitio al ruido sin recortar en Q11) */
    float g = (float)(0.3 / sqrt(pmax));
    for (int i = 0; i < n; i++) {
        x[i].re *= g;
        x[i].im *= g;
    }
    ps *= (double)g * g;
    chan_cfo(x, n, c->rate, wide ? 0.0 : (double)(c->freq - c->center));

    const int64_t k0 = (int64_t)(c->lead_s * c->rate);
    const int total = (int)(k0 + n + (int)(0.3 * c->rate));
    /* ruido: SNR en el ancho del canal (bw_hz del DDC); WIDE: en toda la banda */
    double bw = wide ? c->rate : (double)d->bw_hz;
    double pn_total = ps / pow(10.0, c->snr_db / 10.0) * (c->rate / bw);
    double sg = sqrt(pn_total / 2.0);
    cf32 *y = calloc((size_t)total, sizeof(cf32));
    uint32_t seed = 4242;
    for (int i = 0; i < total; i++) {
        y[i].re = (float)(sg * tu_gauss(&seed));
        y[i].im = (float)(sg * tu_gauss(&seed));
    }
    for (int i = 0; i < n; i++) {
        y[k0 + i].re += x[i].re;
        y[k0 + i].im += x[i].im;
    }
    *truth = k0 + (int64_t)lrint(c->truth_off * c->rate);
    *nsamp = total;

    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)total);
    cf32_to_q11(y, total, 1.0, q);

    char perr[200] = "";
    dec_host_t *h = dec_host_create(NULL);
    dec_host_on_event(h, on_event, NULL);
    pthread_mutex_lock(&lock);
    evs_free(&g_ev);
    pthread_mutex_unlock(&lock);
    bool ok = dec_host_add(h, c->proto, c->freq, NULL, 0, perr, sizeof perr) > 0;
    if (ok) {
        const int B = 20480; /* como el motor a ~1 Msps; la ráfaga no cae en una frontera de bloque */
        uint64_t s0 = 0;
        for (int off = 0; off < total; off += B) {
            int cnt = MIN(B, total - off);
            dec_host_feed_at(h, q + 2 * (size_t)off, y + off, (unsigned)cnt, c->rate, c->center, 0.0, s0);
            dec_host_flush(h, 10.0);
            s0 += (uint64_t)cnt;
        }
    }
    dec_host_flush(h, 10.0);
    dec_host_destroy(h);
    free(q);
    free(y);
    free(x);
    iqbuf_free(&b);
    return ok;
}

/* Comprueba un caso: el primer evento trae sample a ±tol_syms símbolos de la verdad. */
static void check_case(const char *title, const case_t *c, double sym_samples, const char *src)
{
    printf("%s\n", title);
    int64_t truth = 0;
    int total = 0;
    CHECK(run_case(c, &truth, &total));
    double smp = 0, snr = 0, end = 0;
    CHECK(ev_num("\"type\":\"dec\"", "sample", &smp));
    printf("  sample = %.0f, verdad = %lld, error = %+.1f muestras (%.2f símbolos)\n", smp, (long long)truth,
           smp - (double)truth, (smp - (double)truth) / sym_samples);
    CHECK(fabs(smp - (double)truth) <= sym_samples);
    pthread_mutex_lock(&lock);
    CHECK(ev_has(&g_ev, src));
    pthread_mutex_unlock(&lock);
    if (strstr(src, "mark")) {
        CHECK(ev_num("\"type\":\"dec\"", "sample_end", &end));
        CHECK(end > smp);
        bool hs = ev_num("\"type\":\"dec\"", "snr_db", &snr);
        CHECK(hs);
        if (hs) {
            printf("  snr_db = %.1f (inyectada %.1f en el canal)\n", snr, c->snr_db);
            CHECK_NEAR(snr, c->snr_db, 3.5);
        }
        CHECK(ev_num("\"type\":\"dec\"", "rssi_dbfs", &snr));
    }
}

static void test_positions(void)
{
    /* AIS: 48 kHz → captura de 1 Msps con el canal 25 kHz por debajo del centro */
    case_t ais = {.proto = "ais", .params = "{\"types\":[18]}", .rate = 1e6, .center = 162000000LL,
                  .freq = 161975000LL, .snr_db = 15, .lead_s = 0.2345,
                  .truth_off = 0.1 + (24 * 5 + 5) / 48000.0};
    check_case("AIS: sample del evento (inicio de la bandera de apertura)", &ais, 1e6 / 9600.0, "\"sample_src\":\"mark\"");

    /* APRS: 2400 muestras de silencio + 15 banderas de relleno, la 16.ª abre la trama (40 muestras/bit) */
    case_t aprs = {.proto = "aprs", .params = "{\"type\":\"status\",\"callsign\":\"EA2XYZ\",\"text\":\"prueba\"}",
                   .rate = 1e6, .center = 144800000LL + 100000LL, .freq = 144800000LL, .snr_db = 22, .lead_s = 0.31,
                   .truth_off = (2400 + 15 * 8 * 40 + 20) / 48000.0};
    check_case("APRS: sample del evento (inicio de la bandera de apertura)", &aprs, 1e6 / 1200.0, "\"sample_src\":\"mark\"");

    /* ADS-B: WIDE a 2 Msps; verdad = primera muestra de la ráfaga (hueco inicial de 400 muestras) */
    case_t adsb = {.proto = "adsb", .params = NULL, .rate = 2e6, .center = 1090000000LL, .freq = 1090000000LL,
                   .snr_db = 25, .lead_s = 0.1234, .truth_off = 400 / 2e6};
    check_case("ADS-B: sample del evento (inicio del preámbulo)", &adsb, 2.0, "\"sample_src\":\"mark\"");

    /* LoRa: SF7 BW125 a 500 kHz; 0,02 s de silencio y empieza el preámbulo (símbolo = 1,024 ms) */
    case_t lora = {.proto = "lora", .params = NULL, .rate = 2e6, .center = 868000000LL, .freq = 868100000LL,
                   .snr_db = 12, .lead_s = 0.4, .truth_off = 0.02};
    check_case("LoRa: sample del evento (inicio del preámbulo)", &lora, 2e6 * 1.024e-3, "\"sample_src\":\"mark\"");

    /* POCSAG: por omisión (inicio del bloque en que se emitió): "sample_src":"block" */
    printf("POCSAG: valor por omisión (inicio de bloque)\n");
    case_t poc = {.proto = "pocsag", .params = NULL, .rate = 1e6, .center = 466000000LL + 200000LL,
                  .freq = 466000000LL, .snr_db = 20, .lead_s = 0.2, .truth_off = 0};
    int64_t tr;
    int nt;
    CHECK(run_case(&poc, &tr, &nt));
    double smp = 0;
    CHECK(ev_num("\"type\":\"dec\"", "sample", &smp));
    pthread_mutex_lock(&lock);
    CHECK(ev_has(&g_ev, "\"sample_src\":\"block\""));
    pthread_mutex_unlock(&lock);
    CHECK(smp >= 0 && smp <= (double)nt);
}

/* ---- Parte 2: el binario con --replay-fast y --labels ---------------------------------------- */
static bool write_sigmf(const char *base, const cf32 *x, int n, double rate, int64_t center)
{
    char path[760];
    snprintf(path, sizeof path, "%s.sigmf-data", base);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    int16_t *buf = malloc(sizeof(int16_t) * 2 * (size_t)n);
    for (int i = 0; i < n; i++) {
        buf[2 * i] = (int16_t)lrint(fmax(-32767, fmin(32767, x[i].re * 32767.0)));
        buf[2 * i + 1] = (int16_t)lrint(fmax(-32767, fmin(32767, x[i].im * 32767.0)));
    }
    fwrite(buf, sizeof(int16_t) * 2, (size_t)n, f);
    fclose(f);
    free(buf);
    snprintf(path, sizeof path, "%s.sigmf-meta", base);
    f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f,
            "{\"global\":{\"core:datatype\":\"ci16_le\",\"core:sample_rate\":%.0f,\"core:version\":\"1.0.0\"},"
            "\"captures\":[{\"core:sample_start\":0,\"core:frequency\":%lld}],\"annotations\":[]}\n",
            rate, (long long)center);
    fclose(f);
    return true;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = calloc((size_t)n + 1, 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    fclose(f);
    return s;
}

static int count_str(const char *s, const char *needle)
{
    int n = 0;
    for (const char *p = s; p && (p = strstr(p, needle)); p += strlen(needle))
        n++;
    return n;
}

static void test_binary(const char *argv0)
{
    char exe[600];
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", argv0);
    snprintf(exe, sizeof exe, "%s/../bladerf-lab", dirname(tmp));
    if (access(exe, X_OK) != 0) {
        printf("(sin binario %s: se omite la parte del ejecutable)\n", exe);
        return;
    }
    printf("--replay-fast + --labels con el ejecutable\n");
    char dir[600], tmp2[600];
    snprintf(tmp2, sizeof tmp2, "%s", argv0);
    snprintf(dir, sizeof dir, "%s/labels_tmp", dirname(tmp2));
    mkdir(dir, 0755);
    char base[700], cmd[4096];
    snprintf(base, sizeof base, "%s/inj", dir);

    /* Captura: 4 s a 1 Msps, centro 162 MHz. Ruido + 1 ráfaga AIS en 161,975 MHz + portadora continua
     * en 162,1 MHz de 1,0 s a 3,2 s (sin decodificador: debe quedar "unknown"). */
    const double R = 1e6;
    const int N = (int)(4.0 * R);
    const modulator_desc_t *m = modulator_find("ais");
    iqbuf_t b = {0};
    char err[200] = "";
    CHECK(m && m->modulate("{\"types\":[1]}", &b, err, sizeof err) == 0);
    cf32 *x = calloc((size_t)N, sizeof(cf32));
    cf32 *r = malloc(sizeof(cf32) * ((size_t)(b.n * (R / b.rate)) + 64));
    int nr = chan_resample(b.s, b.n, R / b.rate, r);
    chan_cfo(r, nr, R, -25e3);
    const int k_ais = (int)(0.5 * R);
    uint32_t seed = 99;
    for (int i = 0; i < N; i++) {
        x[i].re = (float)(0.02 * tu_gauss(&seed));
        x[i].im = (float)(0.02 * tu_gauss(&seed));
    }
    for (int i = 0; i < nr; i++) {
        x[k_ais + i].re += 0.3f * r[i].re;
        x[k_ais + i].im += 0.3f * r[i].im;
    }
    for (int i = (int)(1.0 * R); i < (int)(3.2 * R); i++) {
        double ph = 2 * M_PI * 100e3 * i / R;
        x[i].re += (float)(0.2 * cos(ph));
        x[i].im += (float)(0.2 * sin(ph));
    }
    CHECK(write_sigmf(base, x, N, R, 162000000LL));
    free(x);
    free(r);
    iqbuf_free(&b);

    char out[700], lab[700];
    snprintf(out, sizeof out, "%s/ev.jsonl", dir);
    snprintf(lab, sizeof lab, "%s/lab.sigmf-meta", dir);
    snprintf(cmd, sizeof cmd,
             "%s --replay %s.sigmf-meta --decode ais@161975000 --decode ais@162025000 --print --labels %s "
             "--config none --port 8993 > %s 2>%s/err.txt",
             exe, base, lab, out, dir);
    int rc = system(cmd);
    CHECK_EQ(rc, 0); /* termina solo y con código 0 */
    char *ev = slurp(out), *lj = slurp(lab);
    CHECK(ev && lj);
    if (ev && lj) {
        CHECK_EQ(count_str(ev, "\"type\":\"gap\""), 0); /* contrapresión: ni un bloque perdido */
        CHECK(count_str(ev, "\"type\":\"dec\"") >= 1);
        CHECK(count_str(lj, "\"lab:level\": \"verified\"") >= 1);
        int nu = count_str(lj, "\"lab:level\": \"unknown\"");
        printf("  verified=%d unknown=%d\n", count_str(lj, "\"lab:level\": \"verified\""), nu);
        CHECK(nu >= 1);
        CHECK(strstr(lj, "lab:empty_convention") != NULL);
        /* hay una región unknown sobre la portadora (162,1 MHz) y ninguna sobre el AIS (161,975 MHz): el
         * detector ve la ráfaga con su media de ~1 s, pero un evento verificado cubre ese tramo */
        int on_cw = 0, ghost = 0;
        for (const char *u = lj; (u = strstr(u, "\"core:label\": \"unknown\"")); u += 10) {
            const char *line = u;
            while (line > lj && *(line - 1) != '\n')
                line--;
            double flo = 0, fhi = 0, s0 = 0, cnt = 0;
            const char *p;
            if ((p = strstr(line, "\"core:freq_lower_edge\": ")))
                flo = strtod(p + strlen("\"core:freq_lower_edge\": "), NULL);
            if ((p = strstr(line, "\"core:freq_upper_edge\": ")))
                fhi = strtod(p + strlen("\"core:freq_upper_edge\": "), NULL);
            if ((p = strstr(line, "\"core:sample_start\": ")))
                s0 = strtod(p + strlen("\"core:sample_start\": "), NULL);
            if ((p = strstr(line, "\"core:sample_count\": ")))
                cnt = strtod(p + strlen("\"core:sample_count\": "), NULL);
            printf("  unknown: %.0f–%.0f Hz, muestras %.0f..%.0f\n", flo, fhi, s0, s0 + cnt);
            if (flo <= 162100000.0 && fhi >= 162100000.0 && s0 > 0.4e6 && s0 < 1.8e6 && s0 + cnt > 2.4e6 &&
                s0 + cnt < 3.8e6)
                on_cw++;
            else if (fhi < 162000000.0)
                ghost++;
        }
        CHECK(on_cw >= 1);
        CHECK_EQ(ghost, 0);
    }
    free(ev);
    free(lj);
    /* limpieza */
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        printf("  (no se pudo borrar %s)\n", dir);
}

int main(int argc, char **argv)
{
    (void)argc;
    test_positions();
    test_binary(argv[0]);
    return TEST_SUMMARY();
}
