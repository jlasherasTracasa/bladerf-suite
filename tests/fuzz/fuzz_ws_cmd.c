/*
 * fuzz_ws_cmd.c — Fuzzing del despacho de órdenes JSON del WebSocket.
 *
 * El despacho (on_text y cmd_* en src/main.c) es static y está ligado al motor, al
 * host de decodificadores y al hilo de TX, así que no se puede llamar aislado sin
 * refactorizar main.c (prohibido en esta tarea). En su lugar el arnés lanza el
 * binario REAL (compilado con ASan+UBSan; ruta en $BIN) con --replay sobre un
 * fichero AIS generado con --gen, en un puerto 8972, con grabaciones y
 * configuración en /tmp/q3, y le habla por WebSocket como lo haría el navegador.
 *
 * Generación: para cada orden del protocolo (AGENTS.md §4.6) y cada uno de sus
 * campos se elige entre: ausente, valor correcto, valor fuera de rango (0, −1,
 * 2^31, 2^32, 1e18, 1e300, 1e999→inf, NaN-like), tipo equivocado (cadena por número,
 * null, array, objeto, booleano) y cadena larga o con escapes inválidos. También se
 * mutan órdenes serializadas a nivel de bytes y se envían fragmentadas.
 *
 * Qué se vigila: que el proceso hijo siga vivo (ASan/UBSan abortan con
 * halt_on_error=1), que el servidor siga respondiendo y que al cerrar con SIGINT
 * salga con código 0. La salida del hijo va a /tmp/q3/ws_cmd.log. Como la entrada
 * vive en otro proceso, la última tanda de mensajes enviados se guarda en
 * /tmp/fuzz_ws_cmd_crash.bin (una orden por línea) para convertirla en caso de
 * regresión; los casos de tests/fuzz/casos/ws_cmd/ se envían igual al arrancar.
 *
 * Limitación: las ramas que exigen hardware o --enable-tx (loopback/air, tx_cal)
 * solo se prueban hasta el rechazo.
 */
#include "fuzz_net.h"

#include <math.h>

#include <signal.h>
#include <sys/wait.h>

#define PORT 8972
#define DIR "/tmp/q3"

static pid_t g_child;
static char g_hist[1 << 16];
static size_t g_hist_len;

static void hist_add(const char *m, size_t l)
{
    if (g_hist_len + l + 2 > sizeof g_hist) {
        g_hist_len = 0; /* solo interesa la cola reciente */
    }
    memcpy(g_hist + g_hist_len, m, l);
    g_hist_len += l;
    g_hist[g_hist_len++] = '\n';
    fz_current(g_hist, g_hist_len);
}

static int child_alive(void)
{
    int st;
    pid_t r = waitpid(g_child, &st, WNOHANG);
    if (r == 0)
        return 1;
    fprintf(stderr, "[fuzz:ws_cmd] el servidor terminó inesperadamente (estado 0x%x); ver %s/ws_cmd.log\n", st, DIR);
    return 0;
}

static int spawn(const char *bin)
{
    mkdir(DIR, 0755);
    mkdir(DIR "/rec_ws", 0755);
    char cmd[512];
    if (access(DIR "/x.sigmf-meta", R_OK) != 0) {
        snprintf(cmd, sizeof cmd,
                 "'%s' --gen ais --gen-freq 161975000 --gen-center 162000000 --gen-rate 1000000 --out " DIR "/x >/dev/null 2>&1",
                 bin);
        if (system(cmd) != 0)
            return -1;
    }
    g_child = fork();
    if (g_child == 0) {
        if (!freopen(DIR "/ws_cmd.log", "w", stdout))
            _exit(126);
        dup2(fileno(stdout), 2);
        setenv("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0:allocator_may_return_null=1", 1);
        setenv("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1", 1);
        execl(bin, bin, "--replay", DIR "/x.sigmf-meta", "--port", "8972", "--recdir", DIR "/rec_ws", "--config",
              DIR "/ws_cfg.json", "--web", "web", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) { /* espera a que escuche */
        usleep(100000);
        if (!child_alive())
            return -1;
        int fd = net_connect(PORT);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
    }
    return -1;
}

/* ---- generación de valores JSON ------------------------------------------ */
typedef enum { K_NUM, K_STR, K_BOOL, K_OBJ, K_ID, K_FREQ, K_MODE, K_PROTO } kind_t;

static const char *PROTOS[] = {"adsb", "rds", "dab", "ais", "dsc", "aprs", "pocsag", "flex", "dmr", "acars", "vdl2",
                               "ism", "rs41", "lora", "meshtastic", "lte", "nr", "gsm", "wifi", "dvbt", "tdt", "nope"};
static const char *PROFILES[] = {"fm", "airband", "adsb", "maritimo", "acars", "vdl2", "dab", "busca", "aprs", "ham2m",
                                 "ham70", "dmr", "ism433", "ism868", "lora", "meshtastic", "sonda", "lte", "nr", "gsm",
                                 "wifi", "dvbt"};
static const char *MODES[] = {"wfm", "nfm", "am", "usb", "lsb", "cw", "file", "loopback", "air", "auto", "manual",
                              "fast", "slow", "hybrid", "xyz", ""};

static void put_num(fz_rng_t *r, char *o, size_t n, kind_t k)
{
    static const char *X[] = {"0",           "-1",          "1",           "2",          "100",
                              "1000",        "48000",       "1e6",         "2.4e6",      "6e9",
                              "7e9",         "1e12",        "1e18",        "1e300",      "-1e300",
                              "1e999",       "-1e999",      "2147483647",  "2147483648", "-2147483649",
                              "4294967295",  "4294967296",  "9223372036854775807", "-9223372036854775808",
                              "0.5",         "-0.0",        "1e-300",      "61440000",   "122880000",
                              "162000000",   "100300000",   "1090000000",  "99999999999999999999"};
    if (fz_below(r, 5) == 0) {
        snprintf(o, n, "%.17g", (fz_unit(r) - 0.5) * (k == K_FREQ ? 4e9 : 1e5));
        return;
    }
    /* Por omisión se descartan los valores cuyo paso a entero es indefinido en C
     * ((int64_t)1e300…): main.c los convierte sin acotar (hallazgo H1 del informe de Q3,
     * fuera de las ficheros que Q3 puede tocar). FUZZ_WS_FULL=1 los incluye. */
    static int full = -1;
    if (full < 0)
        full = getenv("FUZZ_WS_FULL") != NULL;
    double lim = k == K_FREQ ? 1e13 : 2147483647.0;
    for (int tries = 0; tries < 20; tries++) {
        const char *c = X[fz_below(r, sizeof X / sizeof *X)];
        double v = strtod(c, NULL);
        if (full || fabs(v) <= lim) {
            snprintf(o, n, "%s", c);
            return;
        }
    }
    snprintf(o, n, "1");
}

static void put_str(fz_rng_t *r, char *o, size_t n, kind_t k)
{
    const char *s = "";
    switch (k) {
    case K_PROTO: s = PROTOS[fz_below(r, sizeof PROTOS / sizeof *PROTOS)]; break;
    case K_ID: s = PROFILES[fz_below(r, sizeof PROFILES / sizeof *PROFILES)]; break;
    case K_MODE: s = MODES[fz_below(r, sizeof MODES / sizeof *MODES)]; break;
    default: {
        static const char *J[] = {"x.sigmf-meta", "../../etc/passwd", "a/b", "", ".", "..", "tx_ais_2026.sigmf-meta",
                                  "\\u00e9\\n\\\"", "\\ud800", "\\u", "%00", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
        s = J[fz_below(r, sizeof J / sizeof *J)];
    }
    }
    snprintf(o, n, "\"%s\"", s);
}

static void put_opts(fz_rng_t *r, char *o, size_t n);

/* tipo equivocado o extremo para cualquier campo */
static void put_any(fz_rng_t *r, char *o, size_t n)
{
    switch (fz_below(r, 8)) {
    case 0: snprintf(o, n, "null"); break;
    case 1: snprintf(o, n, "true"); break;
    case 2: snprintf(o, n, "false"); break;
    case 3: snprintf(o, n, "[]"); break;
    case 4: snprintf(o, n, "[1,[2,[3]]]"); break;
    case 5: snprintf(o, n, "{}"); break;
    case 6: snprintf(o, n, "{\"lat\":\"a\",\"lon\":[1]}"); break;
    default: put_str(r, o, n, K_STR);
    }
}

static void put_opts(fz_rng_t *r, char *o, size_t n)
{
    static const char *OPTS[] = {
        "{}", "{\"text\":\"HOLA\",\"addr\":1234}", "{\"baud\":9999999999}", "{\"sf\":99,\"bw\":-5}",
        "{\"sf\":7,\"bw\":125000,\"cr\":5,\"payload\":\"00112233\"}", "{\"mmsi\":-1,\"lat\":1e999}",
        "{\"key\":\"zz\",\"text\":\"\"}", "{\"a\":{\"b\":{\"c\":[]}}}", "{\"msg\":\"\\u\"}",
        "{\"text\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
        "{\"proto\":\"x\",\"rate\":0}", "{\"callsign\":\"N0CALL\",\"lat\":91,\"lon\":181}",
        "{\"ssid\":\"\",\"channel\":0}", "{\"sid\":65536,\"pid\":-1}"};
    if (fz_below(r, 3) == 0)
        put_any(r, o, n);
    else
        snprintf(o, n, "%s", OPTS[fz_below(r, sizeof OPTS / sizeof *OPTS)]);
}

typedef struct {
    const char *name;
    kind_t kind;
} field_t;

typedef struct {
    const char *cmd;
    field_t f[10];
} cmdspec_t;

static const cmdspec_t SPECS[] = {
    {"config", {{"center", K_FREQ}, {"rate", K_NUM}, {"rf_bw", K_NUM}, {"gain_mode", K_MODE}, {"gain", K_NUM},
                {"biastee", K_BOOL}, {"fft", K_NUM}, {"fps", K_NUM}, {"det_snr", K_NUM}, {"loc", K_OBJ}}},
    {"channel", {{"freq", K_FREQ}, {"mode", K_MODE}, {"bw", K_NUM}, {"squelch", K_NUM}, {"deemph", K_NUM},
                 {"stereo", K_BOOL}, {"audio", K_BOOL}}},
    {"profile", {{"id", K_ID}}},
    {"dec_add", {{"proto", K_PROTO}, {"freq", K_FREQ}, {"opts", K_OBJ}}},
    {"dec_remove", {{"inst", K_NUM}}},
    {"dec_tune", {{"inst", K_NUM}, {"freq", K_FREQ}}},
    {"record", {{"on", K_BOOL}}},
    {"rec_delete", {{"name", K_STR}}},
    {"tx", {{"proto", K_PROTO}, {"mode", K_MODE}, {"freq", K_FREQ}, {"gain", K_NUM}, {"center", K_FREQ},
            {"rate", K_NUM}, {"repeat", K_NUM}, {"params", K_OBJ}}},
    {"tx_play", {{"name", K_STR}, {"mode", K_MODE}, {"repeat", K_NUM}}},
    {"tx_cal", {{"freq", K_FREQ}, {"rate", K_NUM}}},
    {"tx_stop", {{0}}},
    {"sweep", {{"start", K_FREQ}, {"stop", K_FREQ}, {"fft", K_NUM}}},
    {"sweep_stop", {{0}}},
    {"tv", {{"freq", K_FREQ}, {"sid", K_NUM}}},
    {"tv_service", {{"sid", K_NUM}}},
    {"tv_stop", {{0}}},
    {"tv_scan", {{"start", K_FREQ}, {"stop", K_FREQ}}},
};
#define NSPEC ((int)(sizeof SPECS / sizeof *SPECS))

static size_t gen_cmd(fz_rng_t *r, char *out, size_t cap)
{
    const cmdspec_t *c = &SPECS[fz_below(r, NSPEC)];
    size_t o = (size_t)snprintf(out, cap, "{\"cmd\":\"%s\"", c->cmd);
    for (int i = 0; i < 10 && c->f[i].name; i++) {
        uint32_t p = fz_below(r, 10);
        if (p < 2)
            continue; /* ausente */
        char v[600];
        if (p < 4) {
            put_any(r, v, sizeof v); /* tipo equivocado */
        } else {
            switch (c->f[i].kind) {
            case K_NUM:
            case K_FREQ: put_num(r, v, sizeof v, c->f[i].kind); break;
            case K_BOOL: snprintf(v, sizeof v, "%s", fz_below(r, 2) ? "true" : "false"); break;
            case K_OBJ:
                if (!strcmp(c->f[i].name, "loc"))
                    snprintf(v, sizeof v, "{\"lat\":%.6f,\"lon\":%.6f}", (fz_unit(r) - 0.5) * 400, (fz_unit(r) - 0.5) * 800);
                else
                    put_opts(r, v, sizeof v);
                break;
            default: put_str(r, v, sizeof v, c->f[i].kind);
            }
        }
        o += (size_t)snprintf(out + o, cap - o, ",\"%s\":%s", c->f[i].name, v);
    }
    if (fz_below(r, 12) == 0) /* campo desconocido o clave repetida */
        o += (size_t)snprintf(out + o, cap - o, ",\"cmd\":%s", fz_below(r, 2) ? "1" : "\"x\"");
    o += (size_t)snprintf(out + o, cap - o, "}");
    return o;
}

/* true si el mensaje lleva un número cuyo paso a entero en main.c es indefinido
 * (≥ 16 dígitos seguidos o exponente de 2+ dígitos). Ver hallazgo H1. */
static int has_huge_number(const char *m, size_t l)
{
    for (size_t i = 0; i < l; i++) {
        if (m[i] < '0' || m[i] > '9' || (i && m[i - 1] >= '0' && m[i - 1] <= '9'))
            continue;
        size_t j = i;
        while (j < l && m[j] >= '0' && m[j] <= '9')
            j++;
        if (j - i >= 16)
            return 1;
        size_t k = i;
        if (k && (m[k - 1] == '+' || m[k - 1] == '-'))
            k--;
        if (k && (m[k - 1] == 'e' || m[k - 1] == 'E') && j - i >= 2)
            return 1;
    }
    return 0;
}

static void clean_rec(void)
{
    if (system("rm -f " DIR "/rec_ws/* 2>/dev/null") != 0) {}
}

static void drain(int fd)
{
    char rb[65536];
    while (net_recv(fd, rb, sizeof rb, 0) > 0) {}
}

/* Envía un mensaje de texto, a veces fragmentado. */
static int send_msg(int fd, fz_rng_t *r, const char *m, size_t l)
{
    static uint8_t out[1 << 17];
    size_t n;
    if (l > 4 && fz_below(r, 10) == 0) { /* en dos fragmentos */
        size_t h = 1 + fz_below(r, (uint32_t)l - 1);
        n = ws_frame_build(out, 1, 0, m, h, r);
        if (!net_send(fd, out, n))
            return 0;
        n = ws_frame_build(out, 0, 1, m + h, l - h, r);
    } else {
        n = ws_frame_build(out, fz_below(r, 40) == 0 ? 2 : 1, 1, m, l, r);
    }
    return net_send(fd, out, n);
}

static int g_fd = -1;
static fz_rng_t *g_r;

static void regression_case(const uint8_t *b, size_t l, void *ctx)
{
    (void)ctx;
    /* un caso = órdenes separadas por salto de línea */
    for (size_t o = 0; o < l;) {
        size_t e = o;
        while (e < l && b[e] != '\n')
            e++;
        if (g_fd < 0)
            g_fd = ws_connect(PORT);
        if (g_fd >= 0 && !send_msg(g_fd, g_r, (const char *)b + o, e - o)) {
            close(g_fd);
            g_fd = -1;
        }
        o = e + 1;
        usleep(2000);
    }
    drain(g_fd);
}

int main(int argc, char **argv)
{
    fz_t z;
    fz_init(&z, "ws_cmd", argc, argv, 60);
    g_r = &z.rng;
    const char *bin = getenv("BIN");
    if (!bin) {
        fprintf(stderr, "falta la variable BIN con la ruta de bladerf-lab (compilado con SAN=1)\n");
        return 2;
    }
    if (spawn(bin) < 0) {
        fprintf(stderr, "no se pudo lanzar el servidor (ver %s/ws_cmd.log)\n", DIR);
        return 2;
    }
    int rc = 0;
    fz_run_cases("ws_cmd", regression_case, NULL);
    unsigned long it = 0;
    static char msg[1 << 16];
    while (fz_alive(&z)) {
        if (g_fd < 0 && (g_fd = ws_connect(PORT)) < 0) {
            if (!child_alive()) { rc = 1; break; }
            usleep(50000);
            continue;
        }
        size_t l;
        uint32_t k = fz_below(&z.rng, 10);
        if (k < 6) {
            l = gen_cmd(&z.rng, msg, sizeof msg);
        } else if (k < 9) { /* orden válida mutada a nivel de bytes */
            l = gen_cmd(&z.rng, msg, 2000);
            l = fz_mutate(&z.rng, (uint8_t *)msg, l, sizeof msg);
        } else { /* basura o JSON no objeto */
            static const char *J[] = {"[]", "null", "42", "\"cmd\"", "{", "{\"cmd\":", "{\"cmd\":\"\"}", "{\"cmd\":[]}",
                                      "{\"cmd\":\"configconfigconfigconfigconfig\"}", ""};
            const char *s = J[fz_below(&z.rng, sizeof J / sizeof *J)];
            l = strlen(s);
            memcpy(msg, s, l);
        }
        if (!getenv("FUZZ_WS_FULL") && has_huge_number(msg, l))
            continue;
        hist_add(msg, l);
        if (!send_msg(g_fd, &z.rng, msg, l)) {
            close(g_fd);
            g_fd = -1;
            if (!child_alive()) { rc = 1; break; }
            continue;
        }
        it++;
        if (it % 20 == 0) {
            drain(g_fd);
            usleep(1000);
        }
        if (it % 100 == 0) {
            clean_rec();
            if (!child_alive()) { rc = 1; break; }
        }
        if (it % 5000 == 0) { /* reconectar: prueba on_close / on_open con estado sucio */
            close(g_fd);
            g_fd = -1;
        }
    }
    if (g_fd >= 0)
        close(g_fd);
    if (rc == 0 && !child_alive())
        rc = 1;
    if (rc == 0) {
        /* cierre ordenado: SIGINT y comprobar el código de salida */
        kill(g_child, SIGINT);
        int st = 0;
        for (int i = 0; i < 200; i++) {
            if (waitpid(g_child, &st, WNOHANG) == g_child)
                break;
            usleep(50000);
            if (i == 199) {
                kill(g_child, SIGKILL);
                waitpid(g_child, &st, 0);
                fprintf(stderr, "[fuzz:ws_cmd] el servidor no termina tras SIGINT (bloqueo?)\n");
                rc = 1;
            }
        }
        if (rc == 0 && (!WIFEXITED(st) || WEXITSTATUS(st) != 0)) {
            fprintf(stderr, "[fuzz:ws_cmd] salida anómala al cerrar (estado 0x%x)\n", st);
            rc = 1;
        }
    }
    clean_rec();
    if (rc)
        fprintf(stderr, "[fuzz:ws_cmd] FALLO tras %lu órdenes; últimas órdenes en /tmp/fuzz_ws_cmd_crash.bin\n", it);
    else
        printf("[fuzz:ws_cmd] %lu órdenes enviadas, sin fallos\n", it);
    if (rc)
        fz_death();
    return rc;
}
