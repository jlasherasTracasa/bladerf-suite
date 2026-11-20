/*
 * fpga_lab.h — Cliente de la imagen de FPGA "lab" (hosted + banco de registros
 * Wishbone + motor de espectro). Ver docs/hardware/FIRMWARE.md.
 *
 * Qué ofrece (todo con libbladeRF 2.6.1 sin parchear):
 *   1. Detección SEGURA de la imagen lab. La imagen de serie tiene el bus
 *      Wishbone sin conectar y un acceso Wishbone la dejaría colgada hasta
 *      reiniciar el Nios, así que antes de tocar el bus se comprueban dos cosas
 *      que son seguras en cualquier imagen:
 *        a) la versión de FPGA (paquete NIOS VERSION): la lab es 0.16.101 y la
 *           de serie 0.16.0;
 *        b) los bits 27:24 de la lectura de config_gpio valen 0xA (solo la lab).
 *      Solo si ambas coinciden se lee el registro ID (0x4C414231) por Wishbone.
 *   2. Configuración del motor: N (2^10..2^13), K tramas promediadas, relleno,
 *      borrado de contadores, y selección de rx_mux = 3 (vía config_gpio).
 *   3. Intérprete del flujo de muestras SC16 que sale del motor: sincroniza por
 *      el número mágico, valida la cabecera y entrega tramas con las potencias
 *      acumuladas, y las convierte a dBFS con el mismo criterio que
 *      src/dsp/spectrum.c (un tono complejo de amplitud de fondo de escala = 0 dBFS).
 *
 * El acceso al hardware pasa por fpga_lab_io_t: así se puede probar sin placa
 * (tests/test_fpga_lab.c) y la integración con libbladeRF es un adaptador de
 * cuatro funciones (fpga_lab_io_bladerf, compilado solo si se define
 * FPGA_LAB_WITH_LIBBLADERF para no obligar a enlazar libbladeRF a las pruebas).
 *
 * Escala de la trama (derivación en docs/hardware/FIRMWARE.md §escala):
 *   X_hw = 0.5 · FFT_N(x · w)   con x en cuentas Q11 (±2048) y w la ventana
 *   Blackman-Harris simétrica de N puntos (cuantizada a Q15 en la FPGA)
 *   acc  = Σ_{K tramas} |X_hw|²   (entero exacto; la palabra lleva M·2^E)
 *   dBFS = 10·log10( 4·acc / K ) − 20·log10( 2048 · Σw )
 */
#ifndef BLADERF_LAB_FPGA_LAB_H
#define BLADERF_LAB_FPGA_LAB_H

#include "../common.h"

#define FPGA_LAB_ID_VALUE 0x4C414231u
#define FPGA_LAB_VERSION_PATCH_MIN 100 /* 0.16.101 = lab 1.1; la de serie es 0.16.0 */
#define FPGA_LAB_GPIO_MARK_MASK (0xFu << 24)
#define FPGA_LAB_GPIO_MARK_VALUE (0xAu << 24)
#define FPGA_LAB_GPIO_RXMUX_MASK (0x7u << 8)
#define FPGA_LAB_RXMUX_SPECTRUM 3u

/* Registros: dirección de byte para bladerf_wishbone_master_read/write = 4 * índice */
enum {
    FPGA_LAB_REG_ID = 0, FPGA_LAB_REG_VERSION = 1, FPGA_LAB_REG_FEATURES = 2,
    FPGA_LAB_REG_SCRATCH = 3, FPGA_LAB_REG_SCRATCH_N = 4,
    FPGA_LAB_REG_LOG2N = 5, FPGA_LAB_REG_K = 6, FPGA_LAB_REG_CTRL = 7,
    FPGA_LAB_REG_CNT_SAMPLES = 8, FPGA_LAB_REG_CNT_FIFO_OVF = 9, FPGA_LAB_REG_CNT_FFT = 10,
    FPGA_LAB_REG_CNT_SPEC = 11, FPGA_LAB_REG_CNT_LATE = 12, FPGA_LAB_REG_CNT_WORDS = 13,
    FPGA_LAB_REG_STATUS = 14, FPGA_LAB_REG_CLEAR = 15
};

#define FPGA_SPEC_MAGIC0 0x4C415031u
#define FPGA_SPEC_MAGIC1 0xB3BEAFCEu
#define FPGA_SPEC_HDR_WORDS 8
#define FPGA_SPEC_NMAX 8192

/* Acceso al dispositivo. Todas devuelven 0 si van bien, < 0 si no. */
typedef struct {
    void *ctx;
    int (*fpga_version)(void *ctx, uint32_t *major, uint32_t *minor, uint32_t *patch);
    int (*gpio_read)(void *ctx, uint32_t *val);
    int (*gpio_write)(void *ctx, uint32_t val);
    int (*wb_read)(void *ctx, uint32_t byte_addr, uint32_t *val);
    int (*wb_write)(void *ctx, uint32_t byte_addr, uint32_t val);
} fpga_lab_io_t;

typedef struct {
    fpga_lab_io_t io;
    bool present;          /* imagen lab detectada y verificada */
    uint32_t version;      /* registro VERSION */
    uint32_t features;     /* registro FEATURES */
} fpga_lab_t;

typedef struct {
    uint32_t samples, fifo_ovf, fft_frames, spec_frames, late, words;
    bool active;
} fpga_lab_counters_t;

/* Detección segura. Devuelve 1 si es la imagen lab (y rellena lab), 0 si no lo es
 * (imagen de serie u otra: NO se ha tocado el bus Wishbone) y < 0 si hay un error
 * de comunicación o la imagen anuncia ser lab pero el registro ID no coincide. */
int fpga_lab_probe(const fpga_lab_io_t *io, fpga_lab_t *lab, char *err, size_t errlen);

/* Escritura y lectura de un registro de la lab (solo si lab->present). */
int fpga_lab_reg_read(const fpga_lab_t *lab, int index, uint32_t *val);
int fpga_lab_reg_write(const fpga_lab_t *lab, int index, uint32_t val);

/* Comprobación de extremo a extremo del bus: escribe patrones en SCRATCH y los lee. */
int fpga_lab_selftest(const fpga_lab_t *lab, char *err, size_t errlen);

/* Configura el motor (log2n 10..13, k 1..65535, pad = completar a múltiplos de 2048 palabras). */
int fpga_lab_config_spectrum(const fpga_lab_t *lab, int log2n, int k, bool pad);

/* Activa (true) o desactiva (false) el modo espectro: rx_mux = 3 o 0 en config_gpio
 * (lectura-modificación-escritura, no toca el resto de bits). Al activarlo también borra los contadores.
 *
 * SECUENCIA RECOMENDADA DE ARRANQUE (medida en hardware, ver docs/hardware/FIRMWARE.md §16): configurar N/K,
 * habilitar RX CON rx_mux = 0, leer unos búferes (el flujo USB ya está en marcha) y SOLO ENTONCES llamar
 * a esta función con true; no hacer accesos de control entre `enable_module` y las primeras lecturas.
 * Si se activa el modo antes de habilitar RX, la FIFO de la FPGA se llena durante el arranque del flujo
 * USB y se pierde una trama (y el parser descarta palabras hasta resincronizar). */
int fpga_lab_set_spectrum_mode(const fpga_lab_t *lab, bool on);

int fpga_lab_read_counters(const fpga_lab_t *lab, fpga_lab_counters_t *c);
int fpga_lab_clear_counters(const fpga_lab_t *lab);

/* ---- Intérprete de tramas ------------------------------------------------ */

typedef struct {
    int n;               /* puntos de la FFT */
    int k;               /* tramas promediadas */
    uint32_t group;      /* contador de trama de la FPGA */
    uint64_t ts;         /* índice de la primera muestra del grupo desde el arranque del motor (avanza K*N por trama); no es el timestamp del AD9361 */
    bool dropped;        /* la FPGA perdió una trama antes de esta */
    uint32_t total_words;/* palabras de la trama en el flujo, con relleno */
    uint32_t power[FPGA_SPEC_NMAX]; /* palabras {E[5:0], M[25:0]}, orden fftshift (bin -N/2 ... N/2-1) */
} fpga_lab_frame_t;

typedef void (*fpga_lab_frame_fn)(void *ctx, const fpga_lab_frame_t *f);

typedef struct fpga_lab_parser fpga_lab_parser_t;
fpga_lab_parser_t *fpga_lab_parser_create(fpga_lab_frame_fn cb, void *ctx);
void fpga_lab_parser_destroy(fpga_lab_parser_t *p);

/* Entrega pares I,Q (int16, formato SC16_Q11 tal como llega de sync_rx). Una
 * palabra de 32 bits = (uint16)I | (uint16)Q << 16. */
void fpga_lab_parser_feed_iq(fpga_lab_parser_t *p, const int16_t *iq, size_t nsamples);
void fpga_lab_parser_feed_words(fpga_lab_parser_t *p, const uint32_t *w, size_t n);

/* Estadísticas: tramas entregadas, palabras desechadas buscando sincronía, huecos de contador. */
void fpga_lab_parser_stats(const fpga_lab_parser_t *p, uint64_t *frames, uint64_t *skipped_words, uint64_t *gaps);

/* Palabra {E,M} -> potencia acumulada (double, exacta hasta 26 bits de mantisa). */
double fpga_lab_power_value(uint32_t word);

/* Suma de la ventana Blackman-Harris Q15 de N puntos (la de la FPGA) en unidades de 1.0 = máximo. */
double fpga_lab_window_sum(int n);

/* Convierte una trama a dBFS (n valores, bin -N/2 ... N/2-1, igual que spectrum.c tras el fftshift). */
void fpga_lab_frame_to_dbfs(const fpga_lab_frame_t *f, float *db);

/* dBFS de una potencia acumulada concreta, para un N y un K dados. */
double fpga_lab_power_to_dbfs(double acc, int n, int k);

/* ---- Integración con el motor (fuente de espectro alternativa) ------------ */

/* Origen del espectro, byte [1] del mensaje binario tipo 1 (antes solo 0/1 = CPU/GPU). */
enum { SPEC_SRC_CPU = 0, SPEC_SRC_GPU = 1, SPEC_SRC_FPGA = 2 };

/* Elige N (2^log2n, 1024..8192) y K para una cascada de 'fps' imágenes/s a la tasa 'rate':
 * K = rate/(fps·N) (un grupo de K tramas de FFT por imagen), con el mínimo que mantiene el
 * flujo USB por debajo de FPGA_LAB_MAX_WORDS_PER_S (docs/hardware/FIRMWARE.md §16.4). 'fft' es
 * el tamaño pedido a la interfaz: se acota a 1024..8192. Devuelve las palabras por trama con
 * relleno (múltiplo de 2048) o < 0 si los parámetros no son válidos. */
#define FPGA_LAB_MAX_WORDS_PER_S 3000000.0
int fpga_lab_choose_params(int fft, double rate, int fps, int *log2n, int *k);

/* Empaqueta el mensaje binario tipo 1 (espectro): cabecera de 32 bytes + n bytes,
 * v = (dB − dbmin)/dbstep saturado a 0..255. Es el mismo formato para las tres fuentes. Devuelve el
 * tamaño total (32 + n). out debe tener 32 + n bytes. */
size_t spectrum_msg_pack(uint8_t *out, const float *db, int n, int src, double center, double rate, float dbmin,
                         float dbstep);

/* Adaptador a libbladeRF (solo con -DFPGA_LAB_WITH_LIBBLADERF). dev es un struct bladerf *. */
#ifdef FPGA_LAB_WITH_LIBBLADERF
void fpga_lab_io_bladerf(fpga_lab_io_t *io, void *bladerf_dev);
#endif

#endif
