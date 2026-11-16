/*
 * test_mp2.c — Decodificador MPEG-1/2 Layer II (src/tv/mp2.c).
 *
 * Pruebas sin dependencias (tramas construidas bit a bit, ISO 11172-3 §2.4.1):
 *   - silencio: asignación 0 en todas las subbandas → PCM nulo, tasa y canales de la cabecera;
 *   - una subbanda con código constante (16 bits, 65535 niveles) produce un tono en la
 *     frecuencia central de la subbanda, (k + ½)·fs/64, y nada más (Goertzel);
 *   - estéreo: canales independientes; joint stereo: bound compartido;
 *   - alimentación byte a byte, con basura delante y trama de 44,1 kHz con relleno (417/418 B);
 *   - rechazo de cabeceras no válidas (Layer III, tasa libre).
 * Referencia numérica: si hay `ffmpeg`, se codifican señales con su codificador MP2 (y
 * twolame para joint stereo) y se compara la salida con la de su decodificador en coma
 * flotante convertida a int16: error rms < 0,2 LSB en 48/44,1/32 kHz, mono/estéreo/joint
 * y MPEG-2 (24 kHz). ffmpeg solo se usa como generador y referencia, nunca se enlaza.
 */
#include "test_util.h"
#include "tv/mp2.h"

#include <unistd.h>

typedef struct { uint8_t b[4096]; int n; } bw_t;
static void put(bw_t *w, uint32_t v, int bits)
{
    for (int i = bits - 1; i >= 0; i--) {
        if (w->n % 8 == 0)
            w->b[w->n / 8] = 0;
        w->b[w->n / 8] |= (uint8_t)(((v >> i) & 1) << (7 - w->n % 8));
        w->n++;
    }
}

static const int KBPS[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};

/* Trama MPEG-1 Layer II. mode: 0 estéreo, 1 joint, 3 mono. Tablas 'a' (≥56 kbit/s por canal). */
typedef struct { int sb0_alloc[2]; int code[2]; int rate_idx, br_idx, pad, mode, mode_ext, alt; } fspec_t;

static size_t make_frame(uint8_t *out, const fspec_t *f)
{
    static const int RATE[] = {44100, 48000, 32000};
    bw_t w = {{0}, 0};
    put(&w, 0xFFF, 12);
    put(&w, 1, 1);
    put(&w, 2, 2);
    put(&w, 1, 1);
    put(&w, (uint32_t)f->br_idx, 4);
    put(&w, (uint32_t)f->rate_idx, 2);
    put(&w, (uint32_t)f->pad, 1);
    put(&w, 0, 1);
    put(&w, (uint32_t)f->mode, 2);
    put(&w, (uint32_t)f->mode_ext, 2);
    put(&w, 0, 4);
    int nch = f->mode == 3 ? 1 : 2;
    int bound = f->mode == 1 ? 4 + 4 * f->mode_ext : 27;
    /* asignación: sb0 (4 bits) con el índice pedido; el resto 0. Tabla a: sb 0–10 4 bits,
     * 11–22 3 bits, 23–26 2 bits. */
    for (int sb = 0; sb < 27; sb++) {
        int nb = sb < 11 ? 4 : sb < 23 ? 3 : 2;
        if (sb < bound)
            for (int c = 0; c < nch; c++)
                put(&w, sb == 0 ? (uint32_t)f->sb0_alloc[c] : 0, nb);
        else
            put(&w, 0, nb);
    }
    for (int c = 0; c < nch; c++)
        if (f->sb0_alloc[c])
            put(&w, 2, 2); /* scfsi 2: un solo factor para las tres partes (todos los scfsi, luego los factores) */
    for (int c = 0; c < nch; c++)
        if (f->sb0_alloc[c])
            put(&w, 0, 6); /* índice 0 → factor 2,0 */
    for (int gr = 0; gr < 12; gr++)
        for (int c = 0; c < nch; c++) /* por subbanda y canal van las 3 muestras seguidas */
            for (int k = 0; k < 3; k++)
                if (f->sb0_alloc[c]) {
                    /* índice 15 de sb0 = 65535 niveles; con alt, el signo alterna en cada instante
                     * (código espejo 65534 − c): la muestra de subbanda oscila a fs/64 */
                    int t = gr * 3 + k;
                    int code = (f->alt && (t & 1)) ? 65534 - f->code[c] : f->code[c];
                    put(&w, (uint32_t)code, 16);
                }
    int bytes = 144000 * KBPS[f->br_idx] / RATE[f->rate_idx] + f->pad;
    if (w.n > bytes * 8)
        return 0;
    memset(w.b + (w.n + 7) / 8, 0, (size_t)bytes - (size_t)(w.n + 7) / 8);
    memcpy(out, w.b, (size_t)bytes);
    return (size_t)bytes;
}

typedef struct {
    int16_t pcm[1152 * 2 * 64];
    int n, ch, rate, frames;
} sink_t;
static void on_frame(void *ctx, const int16_t *pcm, int n, int ch, int rate)
{
    sink_t *s = ctx;
    if (s->frames < 64)
        memcpy(s->pcm + s->frames * 1152 * ch, pcm, (size_t)n * (size_t)ch * 2);
    s->frames++;
    s->n += n;
    s->ch = ch;
    s->rate = rate;
}

static double goertzel(const int16_t *x, int n, int stride, double f, double fs)
{
    double w = 2 * M_PI * f / fs, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double s = x[i * stride] + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n * 2;
}

static void test_synthetic(void)
{
    uint8_t fr[1500];
    fspec_t f = {{0, 0}, {0, 0}, 1, 10, 0, 3, 0, 0}; /* 48 kHz, 192 kbit/s, mono */
    size_t len = make_frame(fr, &f);
    CHECK_EQ(len, 576);
    mp2_t *d = mp2_create();
    sink_t *s = calloc(1, sizeof *s);

    /* silencio: 4 tramas, la última queda a la espera de la sincronía siguiente */
    uint8_t stream[8000];
    size_t n = 0;
    for (int i = 0; i < 4; i++) {
        memcpy(stream + n, fr, len);
        n += len;
    }
    mp2_decode(d, stream, n, on_frame, s);
    CHECK_EQ(s->frames, 3);
    CHECK_EQ(s->rate, 48000);
    CHECK_EQ(s->ch, 1);
    int nz = 0;
    for (int i = 0; i < 1152 * 3; i++)
        nz += s->pcm[i] != 0;
    CHECK_EQ(nz, 0);

    /* subbanda 0 con muestras que alternan de signo: oscilan a fs/64 = 750 Hz (borde superior de la
     * subbanda); en la salida solo hay energía en 750 Hz (y su imagen), no en 375 ni en 3 kHz. */
    f.sb0_alloc[0] = 15;
    f.code[0] = 36043; /* (2c − 65534)/65535 ≈ 0,1 */
    f.alt = 1;
    len = make_frame(fr, &f);
    CHECK_EQ(len, 576);
    mp2_reset(d);
    memset(s, 0, sizeof *s);
    n = 0;
    for (int i = 0; i < 6; i++) {
        memcpy(stream + n, fr, len);
        n += len;
    }
    /* byte a byte y con basura delante: misma salida */
    uint8_t junk[40];
    memset(junk, 0x5A, sizeof junk);
    mp2_decode(d, junk, sizeof junk, on_frame, s);
    for (size_t i = 0; i < n; i++)
        mp2_decode(d, stream + i, 1, on_frame, s);
    CHECK_EQ(s->frames, 5);
    const int16_t *tail = s->pcm + 1152 * 3;
    double a750 = goertzel(tail, 1152, 1, 750.0, 48000.0);
    double a375 = goertzel(tail, 1152, 1, 375.0, 48000.0);
    double a3000 = goertzel(tail, 1152, 1, 3000.0, 48000.0);
    printf("  sb0 alternante: %.0f (750 Hz), %.1f (375 Hz), %.1f (3 kHz)\n", a750, a375, a3000);
    CHECK(a750 > 1000);
    CHECK(a750 > 50 * a375);
    CHECK(a750 > 50 * a3000);

    /* linealidad: el código espejo invierte la señal y la mitad de amplitud da la mitad */
    int16_t first[1152], half[1152];
    memcpy(first, tail, sizeof first);
    f.code[0] = 34405; /* valor ≈ 0,05 */
    len = make_frame(fr, &f);
    mp2_reset(d);
    memset(s, 0, sizeof *s);
    n = 0;
    for (int i = 0; i < 5; i++) {
        memcpy(stream + n, fr, len);
        n += len;
    }
    mp2_decode(d, stream, n, on_frame, s);
    memcpy(half, s->pcm + 1152 * 3, sizeof half);
    double e = 0;
    for (int i = 0; i < 1152; i++)
        e = fmax(e, fabs(2.0 * half[i] - first[i]));
    CHECK(e <= 2.5);

    /* código central (valor 0) → silencio exacto aunque haya asignación */
    f.code[0] = 32767;
    f.alt = 0;
    len = make_frame(fr, &f);
    mp2_reset(d);
    memset(s, 0, sizeof *s);
    n = 0;
    for (int i = 0; i < 4; i++) {
        memcpy(stream + n, fr, len);
        n += len;
    }
    mp2_decode(d, stream, n, on_frame, s);
    nz = 0;
    for (int i = 0; i < 1152 * 3; i++)
        nz += s->pcm[i] != 0;
    CHECK_EQ(nz, 0);

    /* estéreo: L con tono, R en silencio */
    fspec_t g = {{15, 0}, {36043, 0}, 1, 10, 0, 0, 0, 1};
    len = make_frame(fr, &g);
    CHECK(len > 0);
    mp2_reset(d);
    memset(s, 0, sizeof *s);
    n = 0;
    for (int i = 0; i < 5; i++) {
        memcpy(stream + n, fr, len);
        n += len;
    }
    mp2_decode(d, stream, n, on_frame, s);
    CHECK_EQ(s->ch, 2);
    tail = s->pcm + 1152 * 2 * 2;
    double l = goertzel(tail, 1152, 2, 750.0, 48000.0), r = goertzel(tail + 1, 1152, 2, 750.0, 48000.0);
    CHECK(l > 1000);
    CHECK(r < 1.0);

    /* joint stereo con bound = 4: sb0 < bound, canales separados (R con la mitad de amplitud) */
    fspec_t j = {{15, 15}, {36043, 34405}, 1, 10, 0, 1, 0, 1};
    len = make_frame(fr, &j);
    CHECK(len > 0);
    mp2_reset(d);
    memset(s, 0, sizeof *s);
    n = 0;
    for (int i = 0; i < 5; i++) {
        memcpy(stream + n, fr, len);
        n += len;
    }
    mp2_decode(d, stream, n, on_frame, s);
    tail = s->pcm + 1152 * 2 * 2;
    l = goertzel(tail, 1152, 2, 750.0, 48000.0);
    r = goertzel(tail + 1, 1152, 2, 750.0, 48000.0);
    CHECK(l > 1000 && r > 1000);
    CHECK_NEAR(l / r, 2.0, 0.05);

    /* 44,1 kHz: 417 o 418 bytes según el relleno; mp2_decode_one devuelve el tamaño */
    fspec_t p = {{0, 0}, {0, 0}, 0, 8, 0, 3, 0, 0}; /* 128 kbit/s a 44,1 kHz: 417,96 → 417 o 418 */
    CHECK_EQ(make_frame(fr, &p), 417);
    p.pad = 1;
    CHECK_EQ(make_frame(fr, &p), 418);
    int16_t pcm[2304];
    int ch, rate;
    CHECK_EQ(mp2_decode_one(d, fr, 418, pcm, &ch, &rate), 418);
    CHECK_EQ(rate, 44100);

    /* cabeceras no válidas: Layer III (capa 01) y tasa libre */
    uint8_t bad[8] = {0xFF, 0xFB, 0x90, 0x00};
    CHECK(mp2_decode_one(d, bad, sizeof bad, pcm, &ch, &rate) < 0);
    bad[1] = 0xFD;
    bad[2] = 0x00; /* bitrate_index 0 = libre */
    CHECK(mp2_decode_one(d, bad, sizeof bad, pcm, &ch, &rate) < 0);

    mp2_destroy(d);
    free(s);
}

/* ---- comparación con ffmpeg ---- */
static bool have_ffmpeg(void) { return system("ffmpeg -version >/dev/null 2>&1") == 0; }

typedef struct { int16_t *pcm; size_t n; } buf_t;
static void on_frame_all(void *ctx, const int16_t *pcm, int n, int ch, int rate)
{
    (void)rate;
    buf_t *b = ctx;
    b->pcm = realloc(b->pcm, (b->n + (size_t)n * (size_t)ch) * 2);
    memcpy(b->pcm + b->n, pcm, (size_t)n * (size_t)ch * 2);
    b->n += (size_t)n * (size_t)ch;
}

static void ffmpeg_case(const char *name, const char *src_filter, int ch, const char *enc_args)
{
    char path[200], cmd[1200];
    snprintf(path, sizeof path, "/tmp/test_mp2_%d_%s.mp2", (int)getpid(), name);
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -y -f lavfi -i \"%s\" -ac %d %s %s", src_filter, ch, enc_args, path);
    if (system(cmd) != 0) {
        printf("  (%s: ffmpeg no pudo codificar; se omite)\n", name);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)sz);
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    mp2_t *d = mp2_create();
    buf_t mine = {0};
    mp2_decode(d, data, rd, on_frame_all, &mine);
    mp2_destroy(d);
    free(data);
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -c:a mp2float -i %s -f f32le - 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    size_t cap = 1 << 20, n = 0;
    float *ref = malloc(cap * sizeof(float));
    for (;;) {
        if (n == cap) {
            cap *= 2;
            ref = realloc(ref, cap * sizeof(float));
        }
        size_t k = fread(ref + n, sizeof(float), cap - n, p);
        if (k == 0)
            break;
        n += k;
    }
    pclose(p);
    unlink(path);
    size_t m = n < mine.n ? n : mine.n;
    double se = 0, ss = 0;
    for (size_t i = 0; i < m; i++) {
        double r = fmin(fmax(lrintf(ref[i] * 32768.0f), -32768), 32767);
        double e = r - mine.pcm[i];
        se += e * e;
        ss += r * r;
    }
    double rms = sqrt(se / (double)m), sig = sqrt(ss / (double)m);
    printf("  ffmpeg %-8s: %zu muestras, señal %.0f rms, error %.3f LSB rms\n", name, m, sig, rms);
    CHECK(m > 40000);
    CHECK(sig > 500);
    CHECK(rms < 0.2);
    free(ref);
    free(mine.pcm);
}

/* Frecuencia de salida: un tono de 440 Hz codificado a 48 kHz debe salir a 440 Hz (error < 0,05 %):
 * cruces por cero ascendentes en 2 s, con interpolación lineal entre muestras. Descarta errores de
 * tasa, muestras perdidas o duplicadas al trocear (se alimenta en trozos de 777 bytes). */
static void test_freq(void)
{
    if (!have_ffmpeg())
        return;
    char path[200], cmd[600];
    snprintf(path, sizeof path, "/tmp/test_mp2_f_%d.mp2", (int)getpid());
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -y -f lavfi -i sine=frequency=440:sample_rate=48000:duration=3 -ac 2 -c:a mp2 -b:a 192k %s", path);
    if (system(cmd) != 0)
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    static uint8_t data[400000];
    size_t n = fread(data, 1, sizeof data, f);
    fclose(f);
    unlink(path);
    mp2_t *d = mp2_create();
    buf_t b = {0};
    for (size_t o = 0; o < n; o += 777)
        mp2_decode(d, data + o, (n - o) < 777 ? n - o : 777, on_frame_all, &b);
    mp2_destroy(d);
    size_t ns = b.n / 2, start = 4800; /* sin el arranque del filtro */
    double first = -1, last = -1;
    int cross = 0;
    for (size_t i = start + 1; i < ns; i++) {
        double a = b.pcm[2 * (i - 1)], c = b.pcm[2 * i];
        if (a < 0 && c >= 0) {
            double t = (double)(i - 1) + (-a) / (c - a);
            if (first < 0)
                first = t;
            last = t;
            cross++;
        }
    }
    double hz = (cross - 1) * 48000.0 / (last - first);
    printf("  frecuencia medida: %.3f Hz (%zu muestras)\n", hz, ns);
    CHECK_NEAR(hz, 440.0, 440.0 * 0.0005);
    /* número de muestras = tramas · 1152 (nada perdido ni duplicado) */
    CHECK_EQ(b.n % (1152 * 2), 0);
    free(b.pcm);
}

static void test_ffmpeg(void)
{
    if (!have_ffmpeg()) {
        printf("  (ffmpeg no está instalado: se omite la comparación numérica)\n");
        return;
    }
    const char *src = "aevalsrc=0.3*sin(2*PI*440*t)+0.2*sin(2*PI*1234*t)+0.1*random(0)|0.3*sin(2*PI*660*t)"
                      "+0.15*sin(2*PI*3000*t):d=2:s=%d";
    char s[400];
    snprintf(s, sizeof s, src, 48000);
    ffmpeg_case("e48_192", s, 2, "-c:a mp2 -b:a 192k");
    snprintf(s, sizeof s, src, 44100);
    ffmpeg_case("e44_128", s, 2, "-c:a mp2 -b:a 128k");
    ffmpeg_case("m44_64", s, 1, "-c:a mp2 -b:a 64k");
    snprintf(s, sizeof s, src, 32000);
    ffmpeg_case("e32_96", s, 2, "-c:a mp2 -b:a 96k");
    snprintf(s, sizeof s, src, 48000);
    ffmpeg_case("e48_384", s, 2, "-c:a mp2 -b:a 384k");
    ffmpeg_case("m48_32", s, 1, "-c:a mp2 -b:a 32k");
    snprintf(s, sizeof s, src, 24000);
    ffmpeg_case("lsf24", s, 2, "-c:a mp2 -b:a 64k");
    snprintf(s, sizeof s, src, 44100);
    ffmpeg_case("joint44", s, 2, "-c:a libtwolame -b:a 128k -mode joint_stereo");
}

int main(void)
{
    test_synthetic();
    test_ffmpeg();
    test_freq();
    return TEST_SUMMARY();
}
