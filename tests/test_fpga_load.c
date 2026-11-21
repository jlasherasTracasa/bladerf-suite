/*
 * test_fpga_load.c — Carga de la imagen de FPGA en RAM (F19), sin placa.
 *
 * Se prueba la lógica de decisión de fpga_lab_ensure() con una placa simulada (fpga_loader_t):
 *   - SHA-256 propio frente a vectores de FIPS 180-4 (abc, vacío, 56 bytes, un millón de 'a' no: basta
 *     un fichero de >64 KiB comparado con la suma conocida de una repetición de bloques).
 *   - Ruta inexistente → error y NO se llama a load.
 *   - Suma errónea o .sha256 ausente → error y NO se carga.
 *   - Modelo incorrecto: placa xA4 con imagen xA9 (y lab en xA4), libbladeRF antigua → error sin cargar.
 *   - Imagen ya lab → omitido (no recarga); pedir stock con la de serie → omitido.
 *   - Camino feliz: se carga en RAM una sola vez y se elige la versión más alta de la carpeta.
 *   - Fallo de la carga → error con el motivo y sin marcar ok.
 * La política de no escribir nunca la flash se comprueba de forma estática en tests/ci (grep) y aquí
 * porque el cargador simulado solo expone «load» (RAM).
 *
 * Compilación aislada:
 *   gcc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Isrc -Itests tests/test_fpga_load.c src/sdr/fpga_lab.c \
 *       -lm -o build/f19/test_fpga_load && build/f19/test_fpga_load
 */
#include "test_util.h"

#include "sdr/fpga_lab.h"

#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int kle;
    int lib_maj, lib_min;
    bool lab;
    int loads, reopens;
    bool need_reopen;   /* true tras load hasta reopen: cualquier otro uso de la placa es un fallo */
    int reopen_rc;
    int load_rc;
    char last[512];
} mock_t;

static int m_kle(void *c, int *k) { *k = ((mock_t *)c)->kle; return 0; }
static int m_lib(void *c, int *a, int *b, int *p) { mock_t *m = c; *a = m->lib_maj; *b = m->lib_min; *p = 0; return 0; }
static int m_lab(void *c, bool *l) { *l = ((mock_t *)c)->lab; return 0; }
static int m_load(void *c, const char *path, char *err, size_t el)
{
    mock_t *m = c;
    m->loads++;
    m->need_reopen = true;
    snprintf(m->last, sizeof m->last, "%s", path);
    if (m->load_rc < 0) {
        snprintf(err, el, "USB desconectado");
        return -1;
    }
    m->lab = strstr(path, "-lab-") != NULL;
    return 0;
}

static int m_reopen(void *c, char *err, size_t el)
{
    mock_t *m = c;
    m->reopens++;
    m->need_reopen = false;
    if (m->reopen_rc < 0) {
        snprintf(err, el, "no aparece en el USB");
        return -1;
    }
    return 0;
}

static fpga_loader_t loader(mock_t *m)
{
    fpga_loader_t l = {.ctx = m, .fpga_kle = m_kle, .lib_version = m_lib, .is_lab = m_lab, .load = m_load, .reopen = m_reopen};
    return l;
}

static void put(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(d, 1, n, f);
    fclose(f);
}

static void put_sum(const char *rbf)
{
    char hex[65], p[600];
    CHECK(fpga_sha256_file(rbf, hex) == 0);
    snprintf(p, sizeof p, "%s.sha256", rbf);
    FILE *f = fopen(p, "w");
    fprintf(f, "%s  %s\n", hex, strrchr(rbf, '/') + 1);
    fclose(f);
}

int main(void)
{
    char dir[] = "/tmp/bladerf_f19_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char p[1024], hex[65];

    /* --- SHA-256: vectores de FIPS 180-4 --- */
    snprintf(p, sizeof p, "%s/v.bin", dir);
    put(p, "abc", 3);
    CHECK(fpga_sha256_file(p, hex) == 0);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    put(p, "", 0);
    CHECK(fpga_sha256_file(p, hex) == 0);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    put(p, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    CHECK(fpga_sha256_file(p, hex) == 0);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    {   /* un millón de 'a' (FIPS 180-4, ejemplo largo): cruza muchos bloques y el búfer interno */
        char *big = malloc(1000000);
        memset(big, 'a', 1000000);
        put(p, big, 1000000);
        free(big);
        CHECK(fpga_sha256_file(p, hex) == 0);
        CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    }
    CHECK(fpga_sha256_file("/no/existe", hex) < 0);

    /* --- carpeta de imágenes simulada --- */
    char d1[512];
    snprintf(d1, sizeof d1, "%s/out", dir);
    mkdir(d1, 0755);
    const char *names[] = {"hosted-xA9-lab-0.16.101.rbf", "hosted-xA9-lab-0.16.103.rbf", "hosted-xA9-lab-0.16.102.rbf",
                           "hosted-xA9-stock-0.16.0.rbf"};
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", d1, names[i]);
        char payload[64];
        snprintf(payload, sizeof payload, "imagen simulada %d", i);
        put(p, payload, strlen(payload));
        put_sum(p);
    }
    const char *dirs[] = {"/no/existe/firmware", d1, NULL};
    char out[512], ver[32];
    CHECK(fpga_image_find(d1, "xA9", "lab", out, sizeof out, ver, sizeof ver) == 0);
    CHECK(strcmp(ver, "0.16.103") == 0);
    CHECK(fpga_image_find(d1, "xA4", "lab", out, sizeof out, ver, sizeof ver) < 0);

    fpga_load_result_t r;
    mock_t m;

    /* 1) Camino feliz: serie → lab, versión más alta, una sola carga */
    m = (mock_t){.kle = 301, .lib_maj = 2, .lib_min = 6};
    fpga_loader_t ld = loader(&m);
    fpga_lab_ensure(&ld, "lab", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_OK);
    CHECK_EQ(m.loads, 1);
    CHECK_EQ(m.reopens, 1);          /* tras cargar hay que cerrar y reabrir */
    CHECK(!m.need_reopen);
    CHECK(strstr(m.last, "0.16.103") != NULL);
    CHECK(strcmp(r.version, "0.16.103") == 0);
    CHECK(strlen(r.sha256) == 64);
    CHECK(strstr(r.msg, "flash no se ha tocado") != NULL);

    /* 2) Imagen ya lab: no recarga */
    fpga_lab_ensure(&ld, "lab", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_SKIPPED);
    CHECK_EQ(m.loads, 1);

    /* 3) Pedir la de serie con la de serie cargada: omitido; con la lab cargada: carga stock */
    m.lab = false;
    fpga_lab_ensure(&ld, "stock", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_SKIPPED);
    CHECK_EQ(m.loads, 1);
    m.lab = true;
    fpga_lab_ensure(&ld, "stock", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_OK);
    CHECK_EQ(m.loads, 2);
    CHECK(strstr(m.last, "stock-0.16.0") != NULL);

    /* 4) Ruta inexistente (fichero y carpeta): error, sin cargar */
    m = (mock_t){.kle = 301, .lib_maj = 2, .lib_min = 6};
    fpga_lab_ensure(&ld, "lab", "/no/existe/lab.rbf", dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 0);
    CHECK(strstr(r.msg, "no se encuentra") != NULL);
    char empty[512];
    snprintf(empty, sizeof empty, "%s/vacia", dir);
    mkdir(empty, 0755);
    fpga_lab_ensure(&ld, "lab", empty, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 0);
    /* sin ruta y sin imágenes en las carpetas candidatas: omitido (no es un fallo) */
    const char *nodirs[] = {empty, NULL};
    fpga_lab_ensure(&ld, "lab", NULL, nodirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_SKIPPED);
    CHECK_EQ(m.loads, 0);

    /* 5) Suma errónea y .sha256 ausente */
    snprintf(p, sizeof p, "%s/hosted-xA9-lab-0.16.103.rbf", d1);
    char sp[1100];
    snprintf(sp, sizeof sp, "%s.sha256", p);
    FILE *f = fopen(sp, "w");
    fprintf(f, "%064d  x\n", 0);
    fclose(f);
    fpga_lab_ensure(&ld, "lab", p, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 0);
    CHECK(strstr(r.msg, "no coincide") != NULL);
    unlink(sp);
    fpga_lab_ensure(&ld, "lab", p, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 0);
    CHECK(strstr(r.msg, "sha256") != NULL);
    put_sum(p);

    /* 6) Modelo incorrecto: placa xA4 (49 kLE) con la lab xA9; placa de modelo desconocido; imagen xA4 en xA9 */
    m.kle = 49;
    fpga_lab_ensure(&ld, "lab", p, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 0);
    CHECK(strstr(r.msg, "xA9") != NULL);
    m.kle = 115;
    fpga_lab_ensure(&ld, "lab", p, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 0);
    m.kle = 301;
    char wrong[1024];
    snprintf(wrong, sizeof wrong, "%s/hosted-xA4-lab-0.16.103.rbf", d1);
    put(wrong, "x", 1);
    put_sum(wrong);
    fpga_lab_ensure(&ld, "lab", wrong, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 0);
    CHECK(strstr(r.msg, "xA4") != NULL);

    /* 7) libbladeRF antigua */
    m.lib_maj = 2;
    m.lib_min = 4;
    fpga_lab_ensure(&ld, "lab", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 0);
    CHECK(strstr(r.msg, "libbladeRF") != NULL);
    m.lib_min = 6;

    /* 8) Fallo de la carga: error con el motivo, sin marcar ok */
    m.load_rc = -1;
    fpga_lab_ensure(&ld, "lab", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK_EQ(m.loads, 1);
    CHECK(strstr(r.msg, "USB desconectado") != NULL);
    CHECK_EQ(m.reopens, 1);          /* también tras un fallo de carga */
    /* carga bien pero la placa no reaparece: error */
    m.load_rc = 0;
    m.lab = false;
    m.reopen_rc = -1;
    fpga_lab_ensure(&ld, "lab", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    CHECK(strstr(r.msg, "reabrir") != NULL);
    /* omitidos y errores previos a la carga no tocan la placa */
    m.reopen_rc = 0;
    m.lab = true;
    int ro = m.reopens;
    fpga_lab_ensure(&ld, "lab", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_SKIPPED);
    CHECK_EQ(m.reopens, ro);
    CHECK(strcmp(fpga_load_state_name(r.state), "omitido") == 0);
    CHECK(strcmp(fpga_load_state_name(FPGA_LOAD_SKIPPED), "omitido") == 0);

    /* 9) Imagen desconocida y cargador ausente */
    fpga_lab_ensure(&ld, "otra", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_ERROR);
    fpga_lab_ensure(NULL, "lab", NULL, dirs, &r);
    CHECK_EQ(r.state, FPGA_LOAD_SKIPPED);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) {}
    return TEST_SUMMARY();
}
