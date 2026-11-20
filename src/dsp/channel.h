/*
 * channel.h — Receptor de un canal: de las muestras IQ de banda ancha al audio.
 *
 * Cada cliente conectado tiene su propio canal, así varias personas pueden
 * escuchar frecuencias distintas dentro de la misma banda capturada.
 *
 * Cadena de procesado (ver docs/arquitectura/DSP.md para las fórmulas y el porqué):
 *
 *   IQ @ fs ──► NCO (desplaza la emisora a 0 Hz)
 *          ──► diezmado en etapas (÷8, ÷8, ÷6…) hasta la tasa de canal fch
 *          ──► filtro de canal (anchura elegida por el usuario)
 *          ──► medida de nivel (S-meter) y squelch
 *          ──► demodulador (WFM / NFM / AM / USB / LSB / CW)
 *          ──► remuestreo a 48 kHz + filtro de audio
 *          ──► PCM 16 bits mono o estéreo
 *
 * Tasas de canal mínimas por modo (fch) y anchura máxima de filtro:
 *     WFM  ≥ 250 kHz   (hasta 220 kHz de ancho; estéreo y RDS caben)
 *     NFM  ≥  48 kHz   (hasta 40 kHz)
 *     AM   ≥  24 kHz   (hasta 20 kHz)
 *     USB/LSB/CW ≥ 12 kHz
 */
#ifndef BLADERF_LAB_CHANNEL_H
#define BLADERF_LAB_CHANNEL_H

#include "../common.h"
#include "filter.h"

typedef enum { MODE_WFM = 0, MODE_NFM, MODE_AM, MODE_USB, MODE_LSB, MODE_CW, MODE_COUNT } demod_mode_t;

const char *mode_name(demod_mode_t m);
int mode_from_name(const char *s); /* −1 si no existe */
int mode_default_bw(demod_mode_t m);
int mode_max_bw(demod_mode_t m);

#define AUDIO_RATE 48000
#define CHAN_MAX_STAGES 6

/* Banderas que acompañan a cada trama de audio o de nivel. */
enum {
    CHAN_FLAG_SQL_OPEN = 1 << 0,  /* el squelch deja pasar audio */
    CHAN_FLAG_STEREO = 1 << 1,    /* trama con dos canales intercalados L,R */
    CHAN_FLAG_OUT_OF_BAND = 1 << 2, /* la frecuencia está fuera de la banda capturada */
    CHAN_FLAG_PILOT = 1 << 3,     /* piloto estéreo de 19 kHz enganchado */
};

/* Parámetros que controla el usuario. */
typedef struct {
    demod_mode_t mode;
    int64_t freq_hz;      /* frecuencia absoluta a escuchar */
    int bw_hz;            /* anchura del filtro de canal */
    float squelch_db;     /* umbral en dBFS; NAN = squelch desactivado */
    int deemph_us;        /* 50 (Europa) o 75 (América); 0 = sin deénfasis */
    bool stereo;          /* permitir estéreo en WFM */
    bool audio_on;        /* si es falso solo se calcula el nivel */
} chan_params_t;

/* Callback de salida. pcm == NULL indica una trama solo de nivel. */
typedef void (*chan_out_fn)(void *ctx, const int16_t *pcm, int nframes, float level_db, unsigned flags);

typedef struct channel channel_t;

channel_t *channel_create(void);
void channel_destroy(channel_t *c);
/* Reconfigura para una nueva tasa/centro del hardware o nuevos parámetros.
 * Solo reconstruye lo que haya cambiado (cambiar de frecuencia es casi gratis). */
void channel_configure(channel_t *c, double fs, int64_t center_hz, const chan_params_t *p);
/* Procesa n muestras IQ normalizadas. Llama a out() 0 o más veces. */
void channel_process(channel_t *c, const cf32 *in, int n, chan_out_fn out, void *ctx);
/* Igual que channel_process() pero sobre el bloque crudo Q11 (I,Q entrelazados,
 * ±2048 = fondo de escala): resta la continua dc_re/dc_im (en cuentas Q11) y
 * normaliza dentro del mezclador, sin pasar por un búfer float de todo el bloque. */
void channel_process_q11(channel_t *c, const int16_t *iq, int n, float dc_re, float dc_im, chan_out_fn out,
                         void *ctx);
/* Canal SIN audio: no se procesa ni una muestra IQ. El S-meter se calcula a partir
 * de una trama de espectro (dBFS por bin, ordenada de −fs/2 a +fs/2, escala del
 * módulo spectrum: tono de amplitud 1,0 = 0 dBFS en su bin):
 *     potencia del canal = Σ P_k / ENBW,   P_k = 10^(db_k/10),
 * con ENBW = N·Σw²/(Σw)² (anchura de ruido equivalente de la ventana, en bins;
 * 2,0044 para Blackman-Harris), por Parseval. Se integra la banda real del filtro
 * del canal (con bordes fraccionarios) y nunca menos de ENBW bins, porque un tono
 * se reparte en ~ENBW bins por la ventana. Con canales de < ~8 bins de ancho el
 * resultado difiere del método por muestras (ver tests/test_engine_perf.c).
 * dt = segundos desde la trama anterior (suavizado de ~50 ms, igual que
 * channel_process). Aplica squelch y emite la trama de nivel cada ≥ 0,1 s.
 * Solo vale si el canal no está en audio_on. */
void channel_feed_spectrum(channel_t *c, const float *db, int n, double enbw_bins, double dt, chan_out_fn out,
                           void *ctx);
/* Potencia (dBFS) de la banda [lo_hz, hi_hz] (relativa al centro de la captura) de
 * una trama de espectro; función pura que usa channel_feed_spectrum. */
float spectrum_band_dbfs(const float *db, int n, double fs, double lo_hz, double hi_hz, double enbw_bins);
/* ¿Este canal necesita muestras IQ ahora? (audio activo y dentro de la banda). */
bool channel_wants_samples(const channel_t *c);
/* Último nivel medido (dBFS) — para registros y pruebas. */
float channel_level(const channel_t *c);
double channel_rate(const channel_t *c);

#endif
