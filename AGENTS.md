# AGENTS.md — Guía para agentes que trabajan en BladeRF Lab

Este fichero es **el contrato** entre el orquestador (que planifica y verifica) y
los agentes que escriben el código. Si algo de aquí choca con tu intuición, manda
este fichero. Si choca con la realidad del código, dilo en tu informe final.

---

## 0. Reglas obligatorias (léelas antes de tocar nada)

1. **No leas conversaciones ajenas.** Prohibido abrir el directorio de historiales del asistente en tu HOME,
   historiales `.jsonl` o transcripciones de otras sesiones. La única fuente de
   verdad es este fichero, el código de `src/`, `docs/` y lo que te diga tu tarea.
2. **Lenguaje**: C11 + POSIX (`-D_GNU_SOURCE`). CUDA solo dentro de `src/gpu/`.
   JavaScript/HTML/CSS sin frameworks ni paso de compilación en `web/`.
3. **Sin dependencias de terceros.** Permitido: libbladeRF, libm, pthread, CUDA
   runtime (opcional). Prohibido: liquid-dsp, FFTW, VOLK, GNU Radio, cuFFT,
   bibliotecas JS, CDNs, mapas o fuentes externas. Todo se implementa aquí.
4. **Comentarios en español**, explicando el *porqué* y la teoría (fórmulas,
   referencia al estándar o al paper con sección/tabla). Mira `src/decoders/adsb.c`
   como modelo de estilo. Identificadores en `snake_case`.
5. **Propiedad de ficheros.** Solo creas o modificas los ficheros asignados a tu
   tarea (§8). ¿Necesitas un cambio en un fichero ajeno? No lo hagas: descríbelo en
   tu informe final. Si tu tarea permite editar un fichero compartido (p. ej.
   `main.c`), vuelve a leerlo justo antes de cada edición y haz cambios pequeños y
   localizados.
6. **Compilación aislada.** Usa siempre tu propio directorio: `make BUILD=build/<tu-agente>`.
   Para iterar sobre tu módulo compila tu test con una línea `gcc` explícita (§7):
   así no te rompe el trabajo a medias de otro agente. No borres `build/` ajenos.
7. **Nunca termines con un fichero que no compile.** Objetivo: 0 avisos con
   `-Wall -Wextra`.
8. **Pruebas obligatorias** (§7): cada decodificador viene con su modulador y un
   test sintético (modular → canal con ruido, desvío de frecuencia y de reloj →
   decodificar → comprobar campos) y, si existen, vectores reales publicados (cita
   la fuente en el test).
9. **Hardware.** Hay un bladeRF 2.0 micro conectado (FPGA 0.16.0, FX3 2.6.0,
   libbladeRF 2.6.1). El servicio `openwebrx` puede tenerlo abierto. **No pares
   servicios, no uses sudo, no instales paquetes del sistema.** No abras el
   hardware salvo que tu tarea lo diga expresamente.
10. **Transmisión**: ver §6. Los agentes **nunca** radian por antena ni activan TX
    en el hardware; prueban la emisión solo en modo `file`.
11. **Firmware**: ningún agente flashea ni carga nada en el equipo
    (`bladeRF-cli -f`, `-L`, `-l`, `bladerf_flash_*`, `bladerf_load_fpga`). El
    código oficial está en `~/bladerf-firmware/bladeRF` y es **solo lectura**; el
    trabajo de FPGA se hace en la copia `firmware/bladeRF` (agente F1). Cargar una
    imagen en la RAM de la FPGA (volátil, se pierde al desconectar) solo lo hace
    el orquestador con permiso explícito del usuario. **Nunca** se escribe la
    flash SPI (`-L`/autoload) ni el firmware del FX3.
12. **Ética de decodificación**: solo información de difusión o de sistema (ID de
    celda, SSID de beacons, posiciones AIS/ADS-B…). Nunca extraer identidades de
    abonados (IMSI/TMSI), descifrar tráfico cifrado (excepción: la clave pública
    por defecto de Meshtastic, que es pública por diseño) ni decodificar voz.
13. **Informe final** (≤ 250 palabras): ficheros creados o tocados, qué funciona
    (con la salida resumida de los tests), qué falta y qué cambios necesitas en
    ficheros ajenos.

---

## 1. Qué es BladeRF Lab

Un servidor SDR propio, escrito desde cero en C, para el bladeRF 2.0 micro:

- Captura IQ (bladeRF o fichero `cs16`/`cs8`/`cu8`/SigMF en bucle).
- Espectro y cascada (FFT propia en CPU o en CUDA) difundidos por WebSocket.
- Audio multi-cliente: cada navegador escucha su propia frecuencia
  (WFM estéreo, NFM, AM, USB, LSB, CW).
- Detector automático de señales (OS-CFAR).
- Grabación IQ en SigMF.
- **Decodificadores de protocolos** (ADS-B ya hecho; el resto, §5).
- **Emisión** de las mismas tramas (modo `file` siempre; `loopback`/`air` con la
  política de §6).
- Interfaz web servida por el propio binario (`web/`).

Sustituye al montaje antiguo con OpenWebRX+ que hay en `legacy/` (solo referencia).

---

## 2. Estado (2026-11-15)

| Módulo | Ficheros | Estado |
|---|---|---|
| Tipos, log | `src/common.h`, `src/util/log.c` | ✔ |
| JSON, cadenas | `src/util/json.*`, `src/util/strbuf.*` | ✔ |
| FFT, ventanas | `src/dsp/fft.*` | ✔ |
| Filtros (Kaiser, FIR diezmador, FIR complejo, remuestreador real, NCO) | `src/dsp/filter.*` | ✔ |
| Canal de audio | `src/dsp/channel.*` | ✔ |
| Espectro CPU/GPU | `src/dsp/spectrum.*`, `src/gpu/*` | ✔ |
| Detector de señales | `src/dsp/detector.*` | ✔ |
| Fuente bladeRF/fichero (+ TX básico) | `src/sdr/source.*` | ✔ |
| HTTP + WebSocket | `src/net/server.*`, `src/net/sha1.*` | ✔ |
| Grabador SigMF | `src/util/recorder.*` | ✔ |
| Motor DSP | `src/engine.*` | ✔ (ADS-B cableado: hay que migrarlo al host de decodificadores) |
| ADS-B / Mode S | `src/decoders/adsb.*` | ✔ |
| `main.c`, `Makefile`, perfiles | — | ✘ (fase 1, A1) |
| Marco de decodificadores, DDC, TX | — | ✘ (fase 1, A1) |
| Bloques DSP comunes y FEC | — | ✘ (fase 1, A3) |
| Interfaz web | `web/` vacío | ✘ (fase 1, A2) |
| Documentación | `docs/` vacío | ✘ |
| Tests | `tests/` vacío | ✘ |

---

## 3. Arquitectura

```
                 ┌────────────── hilo de red (server_run, poll) ──────────────┐
 navegador ⇄ WS ─┤ main.c: callbacks on_open/on_text/on_close → órdenes        │
                 └───────┬───────────────────────┬────────────────┬───────────┘
                         │engine_request()        │dec_host_*()     │tx_*()
                         ▼                        ▼                 ▼
 bladeRF/fichero ─► hilo DSP (engine.c) ───────► dec_host ───► hilos de TX
   source_read      1 espectro → difusión       1 hilo por        (tx.c)
                    2 detector                  instancia:
                    3 grabador ─► hilo escritor  cola de bloques
                    4 canales de audio           refcontados →
                    5 dec_host_feed() (no        DDC → decoder →
                      bloquea nunca)             eventos JSON → WS
```

- **El hilo DSP no se bloquea nunca.** `dec_host_feed()` solo encola un puntero a
  un bloque compartido (copiado una vez, con contador de referencias). Si una
  instancia va retrasada se le descarta el bloque y se cuenta en `drops`.
- **Una instancia de decodificador = un hilo** con su propio DDC (si es *narrow*).
  Así 20 decodificadores aprovechan los 20 núcleos.
- Solo el hilo DSP llama a funciones de **recepción** de libbladeRF; TX usa su
  propio hilo (libbladeRF serializa el acceso al dispositivo).

---

## 4. Contratos (interfaces que todos respetan)

### 4.1 Decodificadores — `src/decoders/decoder.h`

```c
typedef enum { DEC_NARROW = 0, DEC_WIDE = 1 } dec_kind_t;

/* json: objeto JSON completo y válido. Debe incluir "text": resumen de una
 * línea para la interfaz. Resto de campos: propios del protocolo. */
typedef void (*dec_emit_fn)(void *ctx, const char *json, size_t len);

typedef struct {
    double rate;        /* tasa de las muestras que recibirá process/process_raw */
    int64_t freq_hz;    /* NARROW: frecuencia del canal; WIDE: = center_hz */
    int64_t center_hz;  /* frecuencia central del hardware */
    bool loc_valid;
    double lat, lon;    /* posición del receptor */
    const char *opts;   /* objeto JSON de opciones propias, o NULL */
} dec_cfg_t;

typedef struct {
    const char *id;        /* "ais": minúsculas, único, = id del modulador */
    const char *name;      /* "AIS" */
    const char *group;     /* "Marítimo", "Aviación"… (agrupa en la interfaz) */
    const char *desc;      /* una frase */
    dec_kind_t kind;
    double rate;           /* NARROW: tasa compleja EXACTA que entrega el DDC */
    int bw_hz;             /* NARROW: anchura del filtro de canal del DDC */
    uint32_t wide_rate;    /* WIDE: tasa exacta exigida al hardware (0 = libre) */
    uint32_t wide_min_rate;/* WIDE: tasa mínima si wide_rate == 0 */
    int64_t freqs[4];      /* frecuencias típicas (0 = fin de lista) */
    void *(*create)(const dec_cfg_t *cfg, char *err, size_t errlen);
    void (*destroy)(void *st);
    /* NARROW: muestras complejas normalizadas (±1,0 = fondo de escala) a 'rate'. */
    void (*process)(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx);
    /* WIDE: muestras Q11 crudas (I,Q,I,Q…; ±2048) a la tasa del hardware. */
    void (*process_raw)(void *st, const int16_t *iq, int n, double now, dec_emit_fn emit, void *ctx);
    void (*state_json)(void *st, sb_t *sb, double now);              /* opcional, ~1 Hz */
    void (*set_location)(void *st, bool valid, double lat, double lon); /* opcional */
} decoder_desc_t;

extern const decoder_desc_t *const DECODERS[];   /* registry.c, acaba en NULL */
const decoder_desc_t *decoder_find(const char *id);
```

- Cada decodificador vive en `src/decoders/<id>.c` y exporta
  `const decoder_desc_t DEC_<ID>` (id en mayúsculas: `DEC_AIS`, `DEC_RS41`).
- `now` = hora Unix (s) de la primera muestra del bloque.
- `process*` se llama siempre desde el mismo hilo; no hace falta cerrojo interno.
- Presupuesto: tiempo real con holgura (≤ 30 % de un núcleo a su tasa).
  Nada de `malloc` por muestra; reserva en `create` o al crecer.
- Se emite un evento por mensaje válido (con CRC/FEC correctos). Lo que no pase la
  comprobación de integridad no se emite (como mucho, se cuenta).
- Los que mantienen una tabla (aviones, barcos, celdas, redes WiFi…) la exponen
  con `state_json` (objeto JSON) y caducan entradas viejas.

### 4.2 Host de decodificadores — `src/decoders/host.{h,c}` (A1)

```c
dec_host_t *dec_host_create(server_t *srv);
void dec_host_destroy(dec_host_t *h);
/* Devuelve el nº de instancia (>0) o −1 con err. owner: 0 manual, 1 perfil. */
int  dec_host_add(dec_host_t *h, const char *proto, int64_t freq, const char *opts, int owner, char *err, size_t errlen);
int  dec_host_remove(dec_host_t *h, int inst);
void dec_host_remove_owner(dec_host_t *h, int owner);
int  dec_host_retune(dec_host_t *h, int inst, int64_t freq);
void dec_host_set_location(dec_host_t *h, bool valid, double lat, double lon);
/* Hilo DSP. iq: Q11 crudo; buf: complejo normalizado sin continua. No bloquea. */
void dec_host_feed(dec_host_t *h, const int16_t *iq, const cf32 *buf, unsigned n, double fs, int64_t center, double now);
void dec_host_periodic(dec_host_t *h, double now);      /* dec_state ~1 Hz, dec_list */
void dec_host_list_json(dec_host_t *h, sb_t *sb);       /* mensaje "dec_list" */
void dec_host_catalog_json(sb_t *sb);                   /* mensaje "decoders" */
void dec_host_send_snapshot(dec_host_t *h, int client); /* lista + últimos 100 eventos */
void dec_host_on_event(dec_host_t *h, void (*fn)(void *ctx, const char *line), void *ctx); /* --print / --log-events */
```

Reglas del host: instancia *narrow* fuera de la banda capturada → estado
`"waiting"` (no error); *wide* con tasa incompatible → `"error"` con mensaje
claro ("requiere exactamente 2 Msps"). Límite de 200 eventos/s por instancia
(el exceso se descarta y se cuenta). Si `create` devuelve NULL, la instancia
queda en `"error"` con el texto de `err`.

### 4.3 DDC — `src/dsp/ddc.{h,c}` (A1)

```c
ddc_t *ddc_create(double fs_in, int64_t center_hz, int64_t freq_hz, double fs_out, double bw_hz);
void   ddc_destroy(ddc_t *d);
void   ddc_retune(ddc_t *d, int64_t center_hz, int64_t freq_hz);   /* sin clics de fase */
int    ddc_process(ddc_t *d, const cf32 *in, int n, cf32 *out, int out_cap); /* devuelve nº de salidas */
size_t ddc_max_out(const ddc_t *d, int n);
```
Cadena: NCO → `firdec_t` en etapas (÷8 máx.) hasta [fs_out, 2·fs_out) →
`cfir_t` de ancho `bw_hz` → remuestreador **complejo** fraccionario
(`cresamp_t`, nuevo en `filter.{h,c}`, mismo diseño polifásico que `resamp_t`)
hasta `fs_out` exacto.

### 4.4 Moduladores — `src/tx/mod.h` (A1)

```c
typedef struct { cf32 *s; int n, cap; double rate; } iqbuf_t;
void iqbuf_reserve(iqbuf_t *b, int extra);
void iqbuf_push(iqbuf_t *b, cf32 v);
void iqbuf_free(iqbuf_t *b);

typedef struct {
    const char *id;     /* = id del decodificador */
    const char *name;
    bool air_ok;        /* true SOLO si puede radiarse legalmente (§6) */
    /* params: objeto JSON (NULL → mensaje de ejemplo). Escribe banda base
     * compleja centrada en 0 Hz, |x| ≤ 0,9, y fija out->rate. */
    int (*modulate)(const char *params, iqbuf_t *out, char *err, size_t errlen);
} modulator_desc_t;

extern const modulator_desc_t *const MODULATORS[];  /* tx/modreg.c, acaba en NULL */
const modulator_desc_t *modulator_find(const char *id);
```
Cada modulador vive en `src/tx/mod_<id>.c` y exporta `const modulator_desc_t MOD_<ID>`.

### 4.5 Esqueletos

A1 crea **stubs compilables** de los 20 decodificadores y moduladores de §5
(`create` devuelve NULL con `err = "no implementado todavía"`; `modulate`
devuelve −1). Cada agente de fase 2 **sustituye** el stub de sus ids.

### 4.5b Bloques DSP comunes (A3, verificados)

Úsalos antes de reescribir nada; la documentación está en cada cabecera
(convenciones de signo del soft, polinomios en octal, orden de bits).

- `src/dsp/bits.h`: `bits_pack/unpack_{msb,lsb}`, `bits_get/set/flip`,
  `bits_reverse8/reverse`, `bits_popcount32/64/buf`, `bits_parity32`,
  `bits_hamming_dist`, `bits_find_sync`, `nrzi_encode/decode`,
  `lfsr_init` (+ `_wifi/_dvb/_dab/_g3ruh`), `lfsr_prbs_bit/bit/process/process_bytes`.
- `src/dsp/crc.h`: `crc_init`, `crc_preset("CRC-16/X-25")` (55 presets de
  reveng), `crc_start/update/update_bit/finish`, `crc_compute`,
  `crc_compute_bits`, `crc24_modes`.
- `src/dsp/fec.h`: convolucionales `conv_nasa_k7/ccsds_k7/gsm_k5/dab_k7/lte_k7_r3/k9_r2/k9_r3`,
  `conv_punct_dvb`, `conv_encode`, `viterbi_create/destroy/decode_i8/decode_f`
  (terminado y tail-biting); Reed-Solomon `rs_init/encode/decode` (con borrados),
  `rs_init_ccsds/rs41/vdl2/dvb/dab`; BCH `bch_init`, `bch_init_pocsag`,
  `bch_encode/decode(_u32)`; Golay `golay23/24_encode/decode`; Hamming
  `hamming_init/_cols/_7_4/_13_9/_15_11/_16_11`, `hamming_encode/decode/data`;
  bloque lineal `linblock_init/encode/decode`, `linblock_init_qr16_7`.
- `src/dsp/sync.h`: `agc_*`, `fmdisc_*`, `pll_*`, `costas_*`, `fll_*`
  (borde de banda: grueso, ~1 % de la tasa de símbolo), `cfo_estimate_mpower`,
  `farrow3_f/c`, `symsync_*` (Gardner/M&M + Farrow, real y complejo, PAM4),
  `corr_*` (correlador con umbral normalizado).
- `src/dsp/pulse.h`: `pulse_rrc/rc/gaussian/rect/gmsk`, `pulse_ntaps`,
  `fir_f_*` (datos float), `fir_cf_*` (cf32 × coeficientes reales), `pulse_shape_f/cf`.

Advertencias de A3 que debes resolver con datos reales: los parámetros RS del
RS41 (0x11D, fcr 0) no están confirmados; la matriz de `linblock_init_qr16_7`
y las columnas de Hamming de DMR pueden no coincidir con ETSI TS 102 361-1
(pasa las de la norma con `linblock_init`/`hamming_init_cols`); `crc24_modes`
reconstruye su tabla en cada llamada (en bucles usa `crc_t`).

### 4.6 Protocolo WebSocket (`GET /ws`)

Todo en little-endian. A1 lo documenta ampliado en `docs/arquitectura/WEBSOCKET.md`.

**Binario servidor → cliente** (byte 0 = tipo):

| Tipo | Cabecera | Datos |
|---|---|---|
| 1 espectro | `[1]`=1 si GPU, `[4..7]` u32 n, `[8..15]` f64 centro Hz, `[16..23]` f64 tasa, `[24..27]` f32 dbmin, `[28..31]` f32 dbstep | n bytes `v`: dBFS = dbmin + v·dbstep |
| 2 audio | `[1]` flags, `[2]` canales (1/2), `[4..7]` f32 nivel dBFS | PCM int16 48 kHz intercalado |
| 3 nivel | igual que audio | — |

Flags: 1 squelch abierto, 2 estéreo, 4 fuera de banda, 8 piloto 19 kHz.

**JSON cliente → servidor** (campo `cmd`; todos los campos salvo `cmd` son opcionales salvo que se indique):

```
{"cmd":"config","center":Hz,"rate":Hz,"rf_bw":Hz,"gain_mode":"auto|manual","gain":dB,
 "biastee":bool,"fft":N,"fps":N,"det_snr":dB,"loc":{"lat":..,"lon":..}|null}
{"cmd":"channel","freq":Hz,"mode":"wfm|nfm|am|usb|lsb|cw","bw":Hz,"squelch":dBFS|null,
 "deemph":50|75|0,"stereo":bool,"audio":bool}
{"cmd":"profile","id":"ais"}
{"cmd":"dec_add","proto":"ais","freq":Hz,"opts":{...}}        (proto y freq obligatorios)
{"cmd":"dec_remove","inst":N}
{"cmd":"dec_tune","inst":N,"freq":Hz}
{"cmd":"record","on":bool}
{"cmd":"rec_delete","name":"fichero"}
{"cmd":"tx","proto":"pocsag","mode":"file|loopback|air","freq":Hz,"gain":dB,
 "center":Hz,"rate":Hz,"repeat":N,"params":{...}}
{"cmd":"tx_stop"}
```

**JSON servidor → cliente** (campo `type`):

```
status      (ya existe; quitar adsb/adsb_active; añadir "profile":id|null)
signals, recording, error         (ya existen)
decoders    {"list":[{"id","name","group","desc","kind":"narrow|wide","rate","bw",
             "wide_rate","wide_min_rate","freqs":[..],"tx":bool,"air_ok":bool}]}
profiles    {"list":[{"id","name","group","center","rate","mode","chan_freq","bw",
             "decoders":[{"proto","freq","follow":bool,"auto":bool}],"spectrum_only":bool}]}
dec_list    {"list":[{"inst","proto","freq","rate","owner","state":"ok|waiting|error",
             "err","events","drops","load"}]}
dec         {"inst","proto","freq","t","msg":{...,"text":"…"}}
dec_state   {"inst","proto","t","state":{...}}   (ADS-B: {"msgs","rate","rx","ac":[...]}
             con los mismos campos que hoy genera adsb_json)
tx_policy   {"enabled":bool,"air":bool,"callsign":"","max_gain":dB,"bands":[[lo,hi,"nombre"],..]}
tx_result   {"ok":bool,"msg":"…","file":"…"|null,"seconds":s}
```

Al conectar, el servidor envía: `status`, `signals`, `recording`, `decoders`,
`profiles`, `dec_list`, `tx_policy` y los últimos 100 `dec` (con `"replay":true`).
`follow:true` en un perfil = la interfaz mueve ese decodificador (`dec_tune`)
cuando el usuario cambia la frecuencia de audio (RDS). `auto:true` = el host crea
instancias sobre las señales que detecta el detector dentro de la banda del
perfil (máx. 8; opcional en fase 1).

---

## 5. Catálogo de protocolos

`N` = narrow (DDC a `rate`), `W` = wide (muestras crudas). Las frecuencias
centrales de los perfiles se desplazan a propósito para que ningún canal caiga en
el pico de continua del centro.

| id | Protocolo | Tipo / tasa | Alcance mínimo exigido | Agente |
|---|---|---|---|---|
| adsb | ADS-B / Mode S 1090 | W, 2 000 000 exacto | ya hecho; migrar al host | A1 |
| rds | RDS en FM comercial | N, 250 000 | PI, PS, PTY, RT, CT, AF; grupos 0A/0B/2A/2B/4A | D1 |
| dab | DAB / DAB+ | W, 2 048 000 exacto | sincronía OFDM modo I, FIC: conjunto, servicios, subcanales (sin audio) | D1 |
| ais | AIS | N, 48 000 | GMSK 9600, NRZI/HDLC, tipos 1-3, 4, 5, 18, 19, 21, 24; tabla de barcos | D2 |
| dsc | DSC VHF (canal 70) | N, 48 000 | FSK 1200, formatos de socorro/rutina, MMSI | D2 |
| aprs | APRS / AX.25 | N, 48 000 | AFSK 1200 Bell 202 sobre FM, posición, estado, mensajes | D2 |
| pocsag | POCSAG | N, 48 000 | 512/1200/2400 bps, BCH(31,21) con corrección, numérico y alfanumérico | D3 |
| flex | FLEX | N, 48 000 | 1600/3200/6400, 2/4-FSK, BCH, mensajes alfanuméricos | D3 |
| dmr | DMR (solo metadatos) | N, 48 000 | 4FSK 4800, sincronías, CC, slot, LC: origen/destino; **sin voz** | D3 |
| acars | ACARS | N, 48 000 | AM + MSK 2400, paridad, CRC, campos de la trama | D4 |
| vdl2 | VDL Modo 2 | N, 105 000 | D8PSK 10 500 Bd, RS, AVLC, ACARS sobre AVLC | D4 |
| ism | ISM OOK/FSK genérico | N, 250 000 | analizador de pulsos (PWM/PPM/Manchester) + ≥ 10 protocolos comunes (sensores meteo, TPMS, mandos) | D5 |
| rs41 | Radiosonda Vaisala RS41 | N, 48 000 | GFSK 4800, desaleatorizado, RS, posición/altura/PTU | D5 |
| lora | LoRa PHY / LoRaWAN | N, 500 000 | CSS SF7–12, BW125/250, cabecera explícita, CRC, LoRaWAN MHDR/DevAddr | D6 |
| meshtastic | Meshtastic | N, 500 000 | LoRa + AES-CTR con clave por defecto, texto, posición, nodeinfo | D6 |
| lte | LTE (búsqueda de celda) | N, 1 920 000 | PSS/SSS → PCI, CFO, potencia; MIB si da tiempo | D7 |
| nr | 5G NR (SSB) | W, mín. 15 360 000 | PSS/SSS → PCI en la rejilla GSCN | D7 |
| gsm | GSM | N, 1 083 333 | FCCH, SCH → BSIC y FN; SI3 (MCC/MNC/LAC/CellID) si da tiempo | D7 |
| wifi | WiFi 2,4/5 GHz beacons | W, 20 000 000 exacto | 802.11b DSSS 1 Mb/s y 802.11a/g OFDM 6 Mb/s: SSID, BSSID, canal | D8 |
| dvbt | DVB-T | N, 9 142 857 (64/7 MHz) | 2k/8k, TPS: modulación, tasa, guarda, cell ID | D8 |

**Perfiles** (`src/profiles.c`, A1). Formato: centro / tasa / audio / decodificadores.

| id | Nombre | Centro | Tasa | Audio | Decodificadores |
|---|---|---|---|---|---|
| fm | FM comercial | 100,0 MHz | 2,4 M | WFM 100,3 MHz | rds@100,3 (follow) |
| airband | Banda aérea | 127,2 MHz | 2,4 M | AM 127,5 MHz | — |
| adsb | ADS-B | 1090 MHz | 2 M | — | adsb |
| maritimo | VHF marina | 159,3 MHz | 8 M | NFM 156,8 MHz (canal 16) | ais@161,975, ais@162,025, dsc@156,525 |
| acars | ACARS | 131,3 MHz | 1 M | AM 131,725 | acars@131,525/131,725/131,825 |
| vdl2 | VDL2 | 136,6 MHz | 1 M | — | vdl2@136,975/136,875/136,775/136,725 |
| dab | DAB / DAB+ | 227,36 MHz | 2,048 M | — | dab |
| busca | Buscapersonas | 466,3 MHz | 1 M | NFM 466,075 | pocsag@466,075, flex@466,075 |
| aprs | APRS 2 m | 145,0 MHz | 1 M | NFM 144,8 | aprs@144,8 |
| ham2m | 2 m | 145,0 MHz | 2,4 M | NFM 145,5 | aprs@144,8 |
| ham70 | 70 cm | 433,0 MHz | 2,4 M | NFM 433,5 | — |
| dmr | DMR 70 cm | 438,5 MHz | 4 M | — | (añadir desde señales detectadas) |
| ism433 | ISM 433 | 433,42 MHz | 2 M | — | ism@433,92 |
| ism868 | ISM 868 | 868,0 MHz | 2 M | — | ism@868,3, ism@868,95 |
| lora | LoRaWAN EU868 | 867,7 MHz | 2 M | — | lora@868,1/868,3/868,5 |
| meshtastic | Meshtastic EU | 869,2 MHz | 1 M | — | meshtastic@869,525 |
| sonda | Radiosondas | 403,0 MHz | 6 M | — | rs41 (auto) |
| lte | LTE banda 20/3 | 806,0 MHz | 20 M | — | (desde señales detectadas) |
| nr | 5G n78 | 3500 MHz | 30,72 M | — | nr |
| gsm | GSM 900 | 942,5 MHz | 20 M | — | (desde señales detectadas) |
| wifi | WiFi 2,4 GHz | 2437 MHz | 20 M | — | wifi |
| dvbt | TDT | 474 MHz | 20 M | — | dvbt@474 |

---

## 6. Política de transmisión (src/tx/tx.c)

Modos:

- **file** (siempre permitido): escribe la señal en SigMF en `recordings/`
  (`tx_<proto>_<fecha>.sigmf-*`) a la `center`/`rate` pedidas con el canal
  desplazado a `freq`. Sirve para pruebas de extremo a extremo con `--replay`.
- **loopback**: requiere `--enable-tx`. Usa `BLADERF_LB_RFIC_BIST` (bucle interno
  del RFIC) con la ganancia de TX al mínimo. Nada sale por la antena.
- **air**: requiere `--enable-tx-air` **y** que se cumpla todo:
  - el modulador tiene `air_ok = true`;
  - la frecuencia ± ancho ocupado cae entera dentro de una banda permitida:
    ISM 433,050–434,790 MHz; SRD 868,000–868,600 y 869,400–869,650 MHz;
    radioaficionado 144–146 y 430–440 MHz **solo** con `--callsign` (y el
    indicativo va dentro de la trama cuando el protocolo lo permite: APRS, CW);
  - ganancia ≤ `--tx-max-gain` (por defecto 0 dB);
  - duración ≤ 5 s por orden y ciclo de trabajo ≤ 1 % por hora en 868,0–868,6;
  - registro en el log de cada emisión (frecuencia, protocolo, duración).
- Moduladores con `air_ok = false` siempre: adsb, ais, dsc, acars, vdl2, rs41,
  dab, rds, lte, nr, gsm, wifi, dvbt, dmr, flex, pocsag. Su emisión es solo
  `file` y `loopback`.
- Jamás se transmite en bandas de aviación, marítimas, GNSS, celulares, de
  radiodifusión ni de emergencia (406 MHz). La lista blanca lo garantiza.

---

## 7. Compilar y probar

```
make                       # build/bladerf-lab (CUDA automático si existe nvcc)
make CUDA=0                # sin GPU (usa src/gpu/*_stub.c)
make BUILD=build/a1        # directorio propio por agente
make test                  # compila y ejecuta tests/test_*.c
make test T=ais            # solo tests/test_ais.c
make SAN=1 test            # AddressSanitizer + UBSan
make check                 # -Werror + todos los tests
```

CUDA: `nvcc` está en `/usr/local/cuda/bin` (CUDA 13.2, no está en el PATH);
GPU RTX 4080 Laptop → `-arch=sm_89`. El Makefile la busca ahí.

FPGA: la placa es **bladeRF 2.0 micro xA9** (Cyclone V 5CEBA9, 301 kLE, flash
128 Mbit). Quartus Prime Lite **20.1.1** en `~/altera_lite/20.1/quartus/bin`
(con Cyclone V y Nios II EDS en `~/altera_lite/20.1/nios2eds`), que es la versión
que exige el HDL oficial; hay también una 25.1std con Questa FSE
(`~/altera_lite/25.1std/questa_fse/bin/vsim`, sin licencia configurada). Compilar:
`firmware/bladeRF/hdl/quartus/build_bladerf.sh -b bladeRF-micro -r hosted -s A9`
con el entorno de `firmware/env.sh`.

Iteración aislada de un decodificador (no depende del resto del árbol):

```
gcc -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Isrc -Itests \
    tests/test_ais.c src/decoders/ais.c src/decoders/hdlc.c src/tx/mod_ais.c \
    src/tx/iqbuf.c src/dsp/*.c src/util/*.c src/gpu/spectrum_gpu_stub.c \
    -lm -lpthread -o build/d2/test_ais && build/d2/test_ais
```

`tests/test_util.h` (A1) ofrece: `CHECK`, `CHECK_EQ`, `CHECK_NEAR`,
`TEST_SUMMARY()`, canal simulado (`chan_awgn`, `chan_cfo`, `chan_resample`,
`chan_drift`), `run_narrow()`/`run_wide()` que alimentan un decodificador y
recogen sus eventos, y `ev_has(evs, "\"mmsi\":224")`.

Requisito de cada test de decodificador: decodificación correcta con SNR ≥ 10 dB,
desvío de frecuencia de ±(¼ del ancho del canal) y error de reloj de ±100 ppm.

Prueba de extremo a extremo (orquestador):
`bladerf-lab --gen ais --gen-freq 161975000 --gen-center 162000000 --gen-rate 1000000 --out /tmp/x`
y luego `bladerf-lab --replay /tmp/x.sigmf-meta --decode ais@161975000 --print`.

Opciones de línea de órdenes de `main.c` (A1): `--host`, `--port` (por defecto
8090), `--serial`, `--replay <fichero>`, `--rate`, `--freq`, `--no-gpu`, `--web`,
`--recdir`, `--config`, `--profile <id>`, `--decode <id>[@Hz]` (repetible),
`--print` (eventos JSONL por stdout), `--log-events <dir>`, `--duration <s>`,
`--gen <id>` + `--gen-params/--gen-freq/--gen-center/--gen-rate/--out`,
`--enable-tx`, `--enable-tx-air`, `--callsign`, `--tx-max-gain`, `-v`.

---

## 8. Plan y tablero

**Fase 1** (en paralelo): A0, A1, A2, A3, F1. **Fase 2** (cuando A1 y A3 terminen):
D1–D9, C1, H1, T1, W2 y F1 (implementación). **Fase 3**: integración y verificación del orquestador.

| Agente | Tarea | Ficheros que posee |
|---|---|---|
| A0 investigación | Papers implementables (decodificación, emisión, CUDA, firmware) | `docs/investigacion/INVESTIGACION.md` |
| A1 base | Makefile, main.c, perfiles, host, DDC, `cresamp_t`, TX (tx.c, iqbuf, política), stubs, migrar ADS-B, test_util, docs de arquitectura | `Makefile`, `src/main.c`, `src/profiles.*`, `src/engine.*`, `src/decoders/{decoder.h,host.*,registry.c}`, `src/decoders/adsb.*` (envoltorio), `src/dsp/ddc.*`, `src/dsp/filter.*`, `src/tx/{mod.h,iqbuf.c,modreg.c,tx.*}`, stubs `src/decoders/<id>.c` y `src/tx/mod_<id>.c`, `tests/{test_util.h,run_tests.sh,test_dsp.c,test_json.c,test_adsb.c,test_host.c}`, `docs/{ARQUITECTURA,PROTOCOLO,DSP,ADSB,TX}.md`, `.gitignore` |
| A2 web | Interfaz completa según §4.6 | `web/**` |
| A3 DSP común | FEC (Viterbi, RS, BCH, Golay, Hamming), CRC genérico, sincronía (Gardner, M&M, Costas, PLL, FLL, Farrow), pulsos (RRC, gaussiano), FIR real/complejo con coeficientes propios, utilidades de bits | `src/dsp/{fec,crc,sync,pulse,bits}.*`, `tests/{test_fec,test_crc,test_sync}.c` |
| D1 radiodifusión | rds, dab | `src/decoders/{rds,dab*}.c/h`, `src/tx/mod_{rds,dab}.c`, `tests/test_{rds,dab}.c`, `docs/protocolos/{rds,dab}.md` |
| D2 marítimo/AX.25 | ais, dsc, aprs | `src/decoders/{ais,dsc,aprs,hdlc}.*`, `src/tx/mod_{ais,dsc,aprs}.c`, tests, docs |
| D3 busca/DMR | pocsag, flex, dmr | `src/decoders/{pocsag,flex,dmr}.*`, mods, tests, docs |
| D4 aviación | acars, vdl2 | `src/decoders/{acars,vdl2}.*`, mods, tests, docs |
| D5 ISM/telemetría | ism, rs41 | `src/decoders/{ism*,rs41}.*`, mods, tests, docs |
| D6 LoRa | lora, meshtastic | `src/decoders/{lora*,meshtastic,aes}.*`, mods, tests, docs |
| D7 celular | lte, nr, gsm | `src/decoders/{lte,nr,gsm}*.*`, mods, tests, docs |
| D9 ADS-B+ | Mejoras de docs/investigacion/INVESTIGACION.md E1/E2: confianza por bit, corrección CRC de 1–2 bits en bits dudosos, entramado por CRC como modo alternativo, superposición de tramas | `src/decoders/adsb.*`, `tests/test_adsb.c` (ampliar), `docs/protocolos/adsb.md` |
| D8 WiFi/TDT | wifi, dvbt | `src/decoders/{wifi,dvbt}*.*`, mods, tests, docs |
| C1 CUDA | Canalizador polifásico en GPU para alimentar muchos decodificadores, mejoras de espectro, lo que salga de A0 | `src/gpu/**`, integración en `host.c` y en `spectrum.*`, `docs/arquitectura/CUDA.md`, `tests/test_gpu.c` |
| H1 hardware (host) | Funciones de la FPGA 0.16 desde el host (oversample 122,88 Msps SC8, AGC rápida/lenta, calibración de ganancia, quick-tune y barrido), y la parte de host de lo que haga F1 | `src/sdr/**`, `engine_sanitize`/`alloc_block` en `engine.c`, órdenes nuevas en `main.c`, `docs/hardware/HARDWARE.md`, `tests/test_source.c` |
| F1 FPGA | Copia del árbol oficial, compilación reproducible con Quartus 20.1, simulación, bloques DSP nuevos en la FPGA (fase 1: cadena de herramientas y análisis; fase 2: implementación según A0) | `firmware/**`, `docs/hardware/FIRMWARE.md` |
| T1 emisión | Mejoras de TX según A0 (calibración IQ/LO por bucle interno, reproducción de SigMF, simulador de canal) | `src/tx/tx.*`, `src/tx/cal*.c`, `tests/test_tx.c`, `docs/arquitectura/TX.md` |
| W2 web 2 | Vistas específicas: mapa (ADS-B, AIS, APRS, Meshtastic, RS41), RDS en el panel de audio, listas de buscas, celdas y redes | `web/**` |

`README.md` y `AGENTS.md` solo los edita el orquestador.

**Traspasos al empezar la fase 2** (A1, A2 y A3 han terminado y están verificados):
`src/decoders/adsb.*`, `tests/test_adsb.c`, `tests/test_host.c`, `docs/protocolos/adsb.md` → D9 ·
`src/tx/tx.*`, `docs/arquitectura/TX.md` → T1 · `src/decoders/host.c` (solo el enganche de
GPU) → C1 · `src/net/server.c` (guardia de `_GNU_SOURCE` y avisos de truncado)
y `web/**` → W2 · `src/main.c`: H1 y T1 añaden órdenes (relee antes de cada
edición). `Makefile`, `profiles.c`, `ddc.*`, `filter.*`: sin dueño; pide cambios
en el informe.

Campos fijos de `dec_state` que la web (W2) pinta de forma específica:
RDS `{"pi","ps","rt","pty","pty_name","tp","ta","ms","af":[Hz],"ct"}` ·
cualquier array de objetos con `lat`/`lon` se dibuja en el mapa · cualquier
array de objetos se muestra como tabla.

---

## 8b. Pendientes de integración (fase 3)

Recogidos por el orquestador de los informes verificados de fase 2:

1. `docs/arquitectura/WEBSOCKET.md` + web: mensaje binario tipo 4 (barrido), órdenes
   `sweep`/`sweep_stop`/`tx_play`/`tx_cal`, campos nuevos de `status`
   (`sweep`, `format`, `oversample`, `dbm_offset`, `gain_mode` fast/slow/hybrid,
   `device.rate_max_oversample`) y de `dec_state` de ADS-B (`fps`, `fix1`,
   `fix2`, `crc`, `rej`), campo `kind` de los eventos ADS-B.
2. `main.c`: `--no-gpu` debe llamar también a `dec_host_set_gpu(host, false)`.
3. Motor: instalar `tx_set_rx_capture()` para `tx_cal` (aviso: el bucle
   `RFIC_BIST` es digital y no pasa por el LO de TX; la calibración real exige
   cable TX→RX con atenuador ≥ 30 dB y permiso del usuario).
4. Host: perfiles con `auto:true` (crear instancias sobre señales detectadas).
5. `tests/test_util.h`: `chan_drift` desborda (reserva `n+8`) con ppm positivos
   y señales largas.
6. `Makefile`: quitar `-Wno-format-truncation` y la excepción de
   `_GNU_SOURCE` (ya corregidos en `server.c` y `source.c`).
7. Espectro a 122,88 Msps: la CPU no lo sostiene; usar GPU o el motor de
   espectro de la FPGA (F1) e integrarlo en el motor.
8. Validación con señales reales (grabaciones públicas y capturas con el
   bladeRF cuando el usuario lo autorice) de todos los decodificadores; en
   especial constantes no contrastadas: VDL2 (cabecera, aleatorizador, orden
   RS), ACARS (polaridad, BCS), DSC (bits de comprobación, categorías), RS41
   (parámetros RS), DVB-T (pilotos/TPS), LoRa (tablas).
9. AIS: mejora PLVA (Kanaan et al.) no hecha.
10. ✔ Detector: bordes excluidos, fusión por ancho ocupado e histéresis (N1, 2026-11-20).
11. ✔ Web: RDS `af` en MHz y «Ancho RF» con fichero (L1).
12. Enlace USB: el 2026-11-16 la placa estaba en USB 2.0 Hi-Speed (≈ 45 MB/s,
    ≈ 11 Msps). Volver a medir el caudal (RX normal, 61,44 Msps y el modo espectro
    de la imagen lab) con USB 3 SuperSpeed (`bladeRF-cli -e info`).
13. ✔ Host + GPU: SIGSEGV corregido (G1); `make webtest` incluye la prueba de
    estrés y debe pasarse siempre también con CUDA.
14. ✔ Imagen FPGA «lab» integrada como fuente de espectro (`spectrum_src`, F1) y
    probada en placa el 2026-11-20. Pendiente F1b (CPU < 5 % y error de lectura
    al reconfigurar). La lab se carga solo en RAM; tras probar, se vuelve a la
    imagen de serie.

## 8c. TDT: ver y oír la televisión (fase 3b)

Petición del usuario: ver el vídeo y oír el audio de la TDT española (DVB-T,
8 MHz, 8k; desde 2024 todos los canales son HD en H.264, audio MPEG-1 Layer II
y, a veces, AC-3/E-AC-3). Arquitectura:

```
bladeRF ─► host (DDC a 64/7 MHz) ─► decodificador "tdt" ─► dvbt_rx (TV1) ─► paquetes TS de 188 B
            ─► tv.c (TV2): demux TS, PSI/SI (PAT, PMT, SDT, NIT), PES con PTS
                 ├─ vídeo H.264 (Annex B) ──► WS binario tipo 5 ──► navegador: WebCodecs VideoDecoder → canvas
                 └─ audio MP2 ─► mp2.c (decodificador propio) ─► PCM ─► WS binario tipo 6 ─► AudioWorklet (sincronía por PTS)
```

**Excepción a §0.3, solo para el vídeo**: un decodificador H.264 propio no es
razonable; el vídeo lo decodifica el navegador (WebCodecs, aceleración por
hardware). Todo lo demás (receptor DVB-T, Viterbi, Reed-Solomon, demux, PSI,
audio MP2) es propio. Ningún contenido cifrado (CA) se descifra: la TDT española
es en abierto. `ffmpeg` y `dvbsnoop`/herramientas del sistema solo como
generadores de datos de prueba o referencia, nunca enlazados.

**API de TV1** — `src/tv/dvbt_rx.h`:
```c
typedef void (*ts_sink_fn)(void *ctx, const uint8_t *pkts, int npkts); /* npkts × 188 B, alineados a 0x47 */
typedef struct {
    bool lock;                 /* sincronía OFDM + TPS + RS estables */
    int mode_k, guard_den;     /* 2/8, 4/8/16/32 */
    int qam, cr_num, cr_den;   /* 4/16/64, 1/2…7/8 (HP) */
    int cell_id;               /* −1 si no se conoce */
    float cfo_hz, mer_db, snr_db;
    double ber_pre_viterbi, ber_pre_rs;
    unsigned long rs_ok, rs_corrected, rs_failed;
    double ts_bitrate;         /* b/s útiles */
} dvbt_rx_stats_t;
typedef struct dvbt_rx dvbt_rx_t;
dvbt_rx_t *dvbt_rx_create(double rate_in, char *err, size_t errlen);   /* rate_in = 64e6/7 */
void dvbt_rx_destroy(dvbt_rx_t *r);
void dvbt_rx_reset(dvbt_rx_t *r);
void dvbt_rx_process(dvbt_rx_t *r, const cf32 *iq, int n, ts_sink_fn sink, void *ctx);
void dvbt_rx_get_stats(dvbt_rx_t *r, dvbt_rx_stats_t *st);
```
Debe sostener en tiempo real 64-QAM 2/3 (≈ 20–24 Mb/s de TS) en ≤ 1,5
núcleos: Viterbi con AVX2/SSE (o GPU si hiciera falta) y nada de `malloc` por
símbolo.

**Protocolo de TV2** (ampliar `docs/arquitectura/WEBSOCKET.md`):
- Órdenes: `{"cmd":"tv","freq":Hz}` (sintoniza un múltiplex: centro y tasa
  del hardware y crea la instancia "tdt"), `{"cmd":"tv_service","sid":N}`,
  `{"cmd":"tv_stop"}`, `{"cmd":"tv_scan","start":Hz,"stop":Hz}` (recorre los
  canales 21–48 y lista los que enganchan).
- JSON `tv`: `{"freq","lock","stats":{…dvbt_rx_stats_t…},"services":[{"sid","name",
  "provider","type","video":{"pid","codec","w","h"},"audio":[{"pid","codec","lang"}]}],
  "current":sid|null}` (~1 Hz) y `tv_scan` con los múltiplex encontrados.
- Binario tipo 5 (vídeo): `[0]`=5, `[1]` bit0 = fotograma clave,
  `[2]` códec (1 H.264, 2 MPEG-2, 3 HEVC), `[4..11]` i64 PTS (90 kHz),
  `[12..19]` i64 DTS, después la unidad de acceso completa en Annex B.
- Binario tipo 6 (audio TV): `[0]`=6, `[1]` canales, `[4..7]` u32 tasa,
  `[8..15]` i64 PTS (90 kHz) de la primera muestra, después PCM int16 intercalado.
- WebCodecs exige contexto seguro (`localhost` o HTTPS). Desde otra máquina por
  HTTP la web lo detecta y lo explica (alternativa futura: remux a fMP4 + MSE).

| Agente | Tarea | Ficheros |
|---|---|---|
| TV1 receptor | DVB-T completo hasta TS (sincronía, ecualización, demapeo blando, desentrelazado interno y externo, Viterbi rápido, RS(204,188), desaleatorizado), validado con IQ real de esta ubicación | `src/tv/dvbt_rx.*`, `src/tv/dvbt_*.{h,c}`, `src/decoders/dvbt*.{c,h}` (absorbe el de D8), `src/tx/mod_dvbt.c` (modulador completo de TS para tests), `tests/test_dvbt*.c`, `docs/protocolos/dvbt.md`, `docs/protocolos/tdt.md` (parte del receptor) |
| TV2 TS y reproductor | Demux TS, PSI/SI con nombres de canal, PES/PTS, decodificador MP2 propio, decodificador "tdt" en el host, órdenes y mensajes, pestaña TV en la web con WebCodecs y sincronía A/V | `src/tv/{ts,psi,pes,mp2,tv}.{h,c}`, `src/decoders/tdt.c` (+ registro en `registry.c`), órdenes `tv*` en `main.c`, `web/js/tv.js` + pestaña en `web/`, `tests/test_{ts,mp2,tv}.c`, `docs/protocolos/tdt.md` (parte de TS/reproductor), `docs/arquitectura/WEBSOCKET.md` (sección TV) |

## 9. Estilo

- Cabecera de cada fichero: qué hace y la teoría en 10–40 líneas (ver `adsb.h`).
- Funciones cortas, `static` todo lo que no sea API. Sin variables globales
  mutables salvo tablas constantes inicializadas una vez.
- Errores: devolver < 0 y texto en un `char *err` del llamador; `LOG_*` para el
  registro. Nada de `exit()` fuera de `main.c`.
- JSON siempre con `sb_t` + `sb_json_str`/`sb_json_num` (escapado correcto).
- Unidades en el nombre cuando no sean obvias: `freq_hz`, `gain_db`, `gs_kt`.
