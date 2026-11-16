# POCSAG

Decodificador `src/decoders/pocsag.c` (+ `pocsag.h`, `pocsag_fsk.h`), modulador `src/tx/mod_pocsag.c`,
test `tests/test_pocsag.c`. Narrow, 48 000 Hz, canal de 12,5 kHz.

## Teoría (ITU-R M.584, "Radio-paging code No. 1")

* FSK de 2 niveles, 512, 1200 o 2400 bit/s, desviación nominal ±4,5 kHz (la polaridad depende del equipo).
* Transmisión = preámbulo de ≥ 576 bits alternos (1010…) + lotes. Lote = palabra de sincronía
  `0x7CD215D8` + 16 palabras de 32 bits (8 tramas de 2 palabras). Palabra de reposo `0x7A89C197`.
* Palabra = 21 bits de datos + 10 bits BCH(31,21) (g(x) = x¹⁰+x⁹+x⁸+x⁶+x⁵+x³+1, `bch_init_pocsag`) +
  1 bit de paridad par. Corrige 2 errores (t = 2).
  * dirección: `0 | 18 bits | 2 bits de función`. RIC = (18 bits)·8 + índice de trama (posición/2): los 3 bits
    bajos del RIC no se transmiten.
  * mensaje: `1 | 20 bits`.
* Mensaje numérico: nibbles BCD enviados LSB primero (`0-9`, `*`/spare, `U`, espacio, `-`, `]`, `[`).
  Alfanumérico: ASCII de 7 bits, LSB primero, flujo continuo a través de las palabras de 20 bits, fin con EOT
  (0x04) o relleno. El mensaje continúa a través de los lotes hasta una nueva dirección o una palabra de reposo.

## Cadena DSP

`IQ → FIR de canal (41 coef., corte 9,5 kHz) → discriminador FM → 3 receptores paralelos (512/1200/2400 Bd)`;
cada receptor: filtro adaptado (media por bit) → reloj de Gardner (`pocsag_fsk.h`) → seguidores de pico que
quitan el desvío de frecuencia (CFO) y normalizan → bit. La velocidad se detecta porque solo el receptor a la
tasa correcta encuentra la sincronía (≤ 2 errores, patrón o su complemento → polaridad). Tras cada lote se
espera la sincronía siguiente (≤ 4 errores); si no aparece, termina la transmisión y se emite el mensaje pendiente.

Palabra: BCH con ≤ 2 correcciones; la paridad par se comprueba después de corregir (si falla se cuenta un
error más; con 2 correcciones + paridad mala la palabra se descarta). Un hueco (palabra no corregible) en
mitad de un mensaje lo descarta entero y se cuenta en `dropped` (no se emite nada con integridad dudosa).

## Eventos

```json
{"ric":1234567,"func":3,"kind":"alpha","msg":"HOLA","baud":1200,"corrected":0,
 "text":"POCSAG 1200 RIC 1234567 F3 alpha: HOLA"}
```
`kind` = `alpha` | `numeric` | `tone` (dirección sin palabras de mensaje). `corrected` = bits corregidos en las
palabras del mensaje. Función 0 → numérico, 3 → alfanumérico, 1/2 → alfanumérico si hay mensaje, si no tono:
convención habitual, no de la norma (M.584 deja el significado de los 2 bits al operador).

## state_json

```json
{"msgs":N,"batches":N,"bad_words":N,"dropped":N,"baud":1200,
 "pages":[{"ric","func","kind","baud","count","seen":s,"msg"}]}
```
`pages` = últimos 32 mensajes distintos (los repetidos incrementan `count`), caducan a los 600 s.

## Modulador

`{"baud":1200,"invert":false,"messages":[{"ric":1234567,"func":3,"text":"…"},{"ric":2001002,"func":0,"text":"01-23","numeric":true},{"ric":99,"func":1}]}`.
Sin parámetros: un alfanumérico, un numérico y un tono. `air_ok = false`.

## Verificación

* Sintético (modulador → AWGN 10 dB → CFO ±3125 Hz, ±100 ppm → decodificador) para los 3 baudios, polaridad
  invertida y un mensaje de 2 lotes. En barridos con otras semillas se pierde ≈ 1 % de los mensajes a 10 dB
  (2400 Bd, CFO negativo): ruido, no fallo lógico.
* Constantes publicadas: las palabras de sincronía y de reposo de M.584 son palabras BCH(31,21)+paridad
  válidas con el BCH de `fec.c` (comprobado en el test). No hay vectores IQ/audio públicos con licencia clara.
* Coste de CPU ≈ 0,005 s por s de señal.

## Limitaciones

* El orden de bits del BCD (LSB primero) y la tabla de símbolos 10-15 salen de la práctica común.
* Sin soporte de mensajes con bit de "secuencia"/fragmentos propios de algunos operadores.
* Sin detección de la lógica de grupo (RIC de grupo se trata como una dirección).

## Validación con señales reales (V1, 2026-11-15)

Las tres muestras públicas `POCSAG_sample_-_{512,1200,2400}_bps.flac` del repositorio de pruebas de
multimon-ng (grabaciones de aire; GPL-2.0, solo datos de prueba), remodulado en FM (±4,5 kHz) y
pasado por `--replay … --decode pocsag@466075000`: 512 bps → RIC 273040 F3 "512 B SIDE ZZZZZZ";
1200 bps → RIC 273040 F3 "+++TIME=0008300324+++TIME=0008300324"; 2400 bps → RIC 1022869 F1, mismo
texto. Coincide con multimon-ng. **Ninguna constante estaba mal** (BCH, trama, paridad, alfa de 7
bits, polaridad). Test: `tests/data/pocsag*_mm_phase.u8`.
