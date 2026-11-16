/*
 * fec.h — Corrección de errores (FEC): convolucional + Viterbi, Reed-Solomon,
 * BCH binario, Golay, Hamming y códigos de bloque lineales pequeños.
 *
 * Convenciones comunes:
 *   - Los bits se pasan como "un bit por byte" (0/1) salvo donde se indique.
 *   - Decisión BLANDA (soft): int8 en [-127,127] o float. Signo POSITIVO = el bit
 *     recibido es probablemente 1, NEGATIVO = 0, 0 = borrado (sin información).
 *     Con BPSK 0→−1, 1→+1: soft = ruido+símbolo escalado. La magnitud es la
 *     confianza; el escalado absoluto no importa (la métrica es una correlación).
 *   - Todas las funciones de decodificación devuelven < 0 si fallan y, si no,
 *     el nº de errores corregidos (o de discrepancias en el caso de Viterbi).
 *   - Sin malloc en las rutas de proceso; la reserva se hace en *_create.
 *
 * ---------------------------------------------------------------------------
 * 1. Código convolucional y Viterbi
 * ---------------------------------------------------------------------------
 * Un código (n, 1, K) calcula n bits de salida por cada bit de entrada con un
 * registro de K bits (el actual y los K-1 anteriores):  y_j = ⊕ (g_j · registro).
 * Los polinomios se escriben en la notación OCTAL habitual de los estándares:
 * el bit MÁS significativo (bit K-1) multiplica a la entrada ACTUAL y el menos
 * significativo al bit más antiguo. Ej.: NASA/DVB/802.11 K=7: (0171, 0133);
 * GSM K=5: (023, 033). 'invert' invierte las salidas indicadas (CCSDS invierte
 * la segunda: invert = 2).
 *
 * El decodificador de Viterbi busca la secuencia de estados de máxima
 * verosimilitud: en cada paso, para cada uno de los 2^(K-1) estados, compara
 * los dos caminos entrantes (ACS: suma-compara-selecciona) acumulando la
 * correlación entre los símbolos recibidos y los esperados. La métrica de
 * rama de un patrón p de n bits es  Σ_j (p_j ? +s_j : −s_j). Guarda 1 bit de
 * decisión por estado y paso y reconstruye el camino hacia atrás. Es un
 * decodificador por BLOQUES (max_info_bits al crear). Terminación:
 *   CONV_TERM_NONE   el estado final es libre (se elige el mejor camino).
 *   CONV_TERM_ZERO   se añadieron K-1 ceros: el camino acaba en el estado 0.
 *   CONV_TAILBITING  estado inicial = estado final (LTE, DAB-FIC no, 802.16):
 *                    se decodifica la trama con prefijo/sufijo circular de
 *                    6K pasos y se conserva la parte central (casi óptimo).
 * Punzonado: patrón periódico 'keep' (period × n); 1 = el bit se transmite.
 * El decodificador reinserta borrados (0) en los huecos.
 *
 * ---------------------------------------------------------------------------
 * 2. Reed-Solomon sobre GF(2^8)  (estilo de Phil Karn, libfec)
 * ---------------------------------------------------------------------------
 * Código [255, 255−nroots], t = nroots/2 símbolos corregibles; el generador es
 *     g(x) = Π_{i=0}^{nroots-1} (x − α^{(fcr+i)·prim})
 * con α raíz del polinomio primitivo 'gfpoly' (p. ej. 0x11D). Soporta bloques
 * ACORTADOS (nblock < 255: los símbolos que faltan son ceros implícitos al
 * principio) y BORRADOS: se corrigen e errores y s borrados con 2e+s ≤ nroots.
 * Un bloque = datos (ndata) seguidos de nroots símbolos de paridad.
 * Presets (fuentes en rs_init_*): CCSDS, RS41, VDL2, DVB (204,188), DAB (120,110).
 *
 * ---------------------------------------------------------------------------
 * 3. BCH binario (n = 2^m − 1, corrige t) — POCSAG/FLEX BCH(31,21), t=2
 * ---------------------------------------------------------------------------
 * g(x) = mcm de los polinomios mínimos de α, α², …, α^{2t}. Decodificación:
 * síndromes S_j = r(α^j) → Berlekamp-Massey (polinomio localizador) → Chien
 * (raíces). Admite acortado (nbits < n). Los bits van MSB-first: el primer bit
 * de la palabra es el coeficiente de mayor grado.
 *
 * ---------------------------------------------------------------------------
 * 4. Golay (23,12,7) y (24,12,8), Hamming, y bloque lineal genérico
 * ---------------------------------------------------------------------------
 * Golay(23,12) es un código PERFECTO: toda palabra está a ≤ 3 bits de una
 * palabra de código, así que un patrón de 4 errores se "corrige" mal. El
 * (24,12) añade paridad global y detecta 4 errores. Disposición de palabra:
 * bits 22..11 datos, 10..0 paridad (g = x^11+x^10+x^6+x^5+x^4+x^2+1 = 0xC75);
 * en el (24,12) el bit 23 es la paridad par global.
 * Hamming: palabra = [datos d0..d(k-1)] [paridades p0..p(r-1)] [paridad global
 * opcional]. d0 es el bit MÁS significativo. Con columnas propias se puede
 * reproducir cualquier variante de un estándar (p. ej. ETSI TS 102 361-1 B.3).
 * linblock: código lineal cualquiera (k ≤ 12) dado por su matriz generadora;
 * decodifica por búsqueda exhaustiva de la palabra más cercana (QR(16,7,6)).
 */
#ifndef BLADERF_LAB_FEC_H
#define BLADERF_LAB_FEC_H

#include "../common.h"

/* ======================== Convolucional + Viterbi ========================= */
typedef enum { CONV_TERM_NONE = 0, CONV_TERM_ZERO = 1, CONV_TAILBITING = 2 } conv_term_t;

typedef struct {
    int k;              /* longitud de restricción K (2..9) */
    int n;              /* salidas por bit de entrada (2..4) */
    uint32_t poly[4];   /* polinomios (bit K-1 = entrada actual) */
    uint32_t invert;    /* bit j = 1 invierte la salida j */
} conv_code_t;

typedef struct {
    int period;                 /* ≤ 64 */
    int n;                      /* = conv_code_t.n */
    const uint8_t *keep;        /* keep[t*n + j] = 1 si se transmite la salida j del paso t */
} conv_punct_t;

/* Presets */
conv_code_t conv_nasa_k7(void);     /* (0171,0133): DVB-T/S, 802.11, Iridium...; sin inversión */
conv_code_t conv_ccsds_k7(void);    /* igual con la 2ª salida invertida */
conv_code_t conv_gsm_k5(void);      /* (023,033) GSM TCH/FS, SACCH */
conv_code_t conv_dab_k7(void);      /* tasa 1/4 (0133,0171,0145,0133) DAB */
conv_code_t conv_lte_k7_r3(void);   /* tasa 1/3 (0133,0171,0165) LTE, tail-biting */
conv_code_t conv_k9_r2(void);       /* (0561,0753) CDMA/UMTS */
conv_code_t conv_k9_r3(void);       /* (0557,0663,0711) UMTS */
/* Punzonados DVB para el K=7 NASA: "2/3","3/4","5/6","7/8" (NULL si no existe). */
const conv_punct_t *conv_punct_dvb(const char *rate);

/* Codifica n bits de 'in'. Devuelve el nº de bits escritos en 'out' (cap.
 * máxima (n+K-1)·code->n). TAILBITING exige n ≥ K-1. */
int conv_encode(const conv_code_t *code, const uint8_t *in, int n, uint8_t *out,
                conv_term_t term, const conv_punct_t *punct);

typedef struct viterbi viterbi_t;
/* max_info_bits = mayor nº de bits de datos por bloque. NULL si falla. */
viterbi_t *viterbi_create(const conv_code_t *code, int max_info_bits);
void viterbi_destroy(viterbi_t *v);
/* Decodifica n_info bits de datos (más la cola si TERM_ZERO) a partir de nsoft
 * símbolos blandos (ya punzonados si punct != NULL). Escribe n_info bits en
 * out_bits. Devuelve el nº de símbolos del canal que contradicen la secuencia
 * decodificada (≈ errores de canal), o −1 si los argumentos no son válidos. */
int viterbi_decode_i8(viterbi_t *v, const int8_t *soft, int nsoft, int n_info, conv_term_t term,
                      const conv_punct_t *punct, uint8_t *out_bits);
int viterbi_decode_f(viterbi_t *v, const float *soft, int nsoft, int n_info, conv_term_t term,
                     const conv_punct_t *punct, uint8_t *out_bits);

/* ============================ Reed-Solomon ================================ */
typedef struct {
    int nroots, fcr, prim, gfpoly;
    uint8_t exp[512];       /* α^i, duplicada para evitar el módulo */
    uint8_t log[256];       /* log[0] sin uso */
    uint8_t gen[256];       /* g(x) sin el término líder: gen[i] = coef. de x^(nroots-1-i) */
} rs_t;

/* Devuelve 0, o −1 si gfpoly no es primitivo / prim no es coprimo con 255 /
 * nroots fuera de 1..128. */
int rs_init(rs_t *rs, int gfpoly, int fcr, int prim, int nroots);
/* Calcula 'nroots' bytes de paridad de data[0..ndata-1] (ndata ≤ 255−nroots). */
void rs_encode(const rs_t *rs, const uint8_t *data, int ndata, uint8_t *parity);
/* Corrige in situ block[0..nblock-1] (datos+paridad, nblock ≤ 255).
 * erasures: índices (0..nblock-1) de símbolos que se sabe dudosos (o NULL).
 * Devuelve el nº de posiciones corregidas o −1 si no es corregible. */
int rs_decode(const rs_t *rs, uint8_t *block, int nblock, const int *erasures, int n_era);
/* Presets: devuelven rs_init(...). */
int rs_init_ccsds(rs_t *rs);   /* RS(255,223): 0x187, fcr 112, prim 11, 32 raíces (base convencional, no dual) */
int rs_init_rs41(rs_t *rs);    /* Vaisala RS41: RS(255,231) t=12, 0x11D, fcr 0, prim 1, 24 raíces */
int rs_init_vdl2(rs_t *rs);    /* VDL modo 2: RS(255,249) acortado, 0x187, fcr 120, prim 1, 6 raíces */
int rs_init_dvb(rs_t *rs);     /* DVB RS(204,188) t=8: 0x11D, fcr 0, prim 1, 16 raíces */
int rs_init_dab(rs_t *rs);     /* DAB superframe RS(120,110) t=5: 0x11D, fcr 0, prim 1, 10 raíces */

/* ============================== BCH binario =============================== */
typedef struct {
    int m, n, k, t, np;     /* GF(2^m), n = 2^m−1, k, t, np = n−k */
    int16_t exp[2048];
    int16_t log[1024];
    uint8_t gen[336];       /* coeficientes binarios de g(x), gen[i] = coef. de x^i */
} bch_t;

/* m ≤ 10, prim_poly = polinomio primitivo de GF(2^m) (con el término x^m).
 * Devuelve 0 o −1. */
int bch_init(bch_t *b, int m, int prim_poly, int t);
/* BCH(31,21) t=2 de POCSAG y FLEX: m=5, x^5+x^2+1, g = 0x769. */
int bch_init_pocsag(bch_t *b);
/* Codificación sistemática: nbits_data ≤ k; escribe np bits de paridad. */
void bch_encode(const bch_t *b, const uint8_t *data, int nbits_data, uint8_t *parity);
/* Corrige in situ nbits bits (datos+paridad, nbits ≤ n). Devuelve nº de bits
 * corregidos (0..t) o −1 si no es corregible. */
int bch_decode(const bch_t *b, uint8_t *bits, int nbits);
/* Variantes con la palabra en un entero (MSB = primer bit, nbits ≤ 32). */
uint32_t bch_encode_u32(const bch_t *b, uint32_t data, int nbits_data);   /* devuelve la palabra completa */
int bch_decode_u32(const bch_t *b, uint32_t *word, int nbits);

/* =============================== Golay ==================================== */
typedef struct { uint32_t err[2048]; } golay_t;   /* patrón de error por síndrome */
void golay_init(golay_t *g);
uint32_t golay23_encode(uint32_t data12);                    /* palabra de 23 bits */
uint32_t golay24_encode(uint32_t data12);                    /* palabra de 24 bits */
int golay23_decode(const golay_t *g, uint32_t *cw);          /* 0..3 corregidos */
int golay24_decode(const golay_t *g, uint32_t *cw);          /* 0..3 corregidos o −1 (≥4 errores) */

/* =============================== Hamming ================================== */
typedef struct {
    int k, r, ext, n;
    uint16_t col[32];        /* columna de H de cada bit de dato */
    int16_t pos_of[256];     /* síndrome -> índice de bit (desde el MSB) o −1 */
} hamming_t;

/* Columnas automáticas (los valores no potencia de 2: 3,5,6,7,9...). */
int hamming_init(hamming_t *h, int k, int r, int ext);
/* Columnas propias (cols[i] ≠ 0, distintas, no potencias de 2 salvo que se desee). */
int hamming_init_cols(hamming_t *h, int k, int r, int ext, const uint16_t *cols);
int hamming_init_7_4(hamming_t *h);
int hamming_init_13_9(hamming_t *h);
int hamming_init_15_11(hamming_t *h);
int hamming_init_16_11(hamming_t *h);     /* (15,11) + paridad global: SEC-DED */
uint32_t hamming_encode(const hamming_t *h, uint32_t data);
/* Corrige 1 error; con ext detecta 2 (−1). Devuelve 0/1 o −1. */
int hamming_decode(const hamming_t *h, uint32_t *cw);
static inline uint32_t hamming_data(const hamming_t *h, uint32_t cw) { return cw >> (h->r + h->ext); }

/* ========================= Bloque lineal genérico ========================= */
typedef struct {
    int n, k, dmin, t;
    uint32_t cw[4096];       /* palabra de código por cada valor de los k bits de datos */
} linblock_t;

/* rows[i] = palabra de código (n ≤ 32 bits) de los datos con SOLO el bit (k−1−i)
 * a 1. Devuelve 0, o −1 si k > 12 o n > 32. Calcula dmin y t = (dmin−1)/2. */
int linblock_init(linblock_t *lb, int n, int k, const uint32_t *rows);
uint32_t linblock_encode(const linblock_t *lb, uint32_t data);
/* Decodificación a distancia acotada t: devuelve el nº de bits corregidos, o −1
 * si hay más de t errores (o empate). Escribe la palabra corregida y los datos. */
int linblock_decode(const linblock_t *lb, uint32_t *cw, uint32_t *data);
/* QR(16,7,6) (código cuadrático residual (17,9) extendido y acortado, el tipo
 * que usa DMR en el campo EMB). NOTA: equivalente en distancia, pero la matriz
 * exacta de ETSI TS 102 361-1 anexo B puede diferir: pásala con linblock_init. */
int linblock_init_qr16_7(linblock_t *lb);

#endif
