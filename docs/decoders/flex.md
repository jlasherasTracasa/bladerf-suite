# FLEX

Decodificador `src/decoders/flex.c` (+ `flex.h`, `pocsag_fsk.h`), modulador `src/tx/mod_flex.c`, test
`tests/test_flex.c`. Narrow, 48 000 Hz.

## IMPORTANTE: qué está confirmado y qué no

La especificación de FLEX (Motorola) **no es pública** y no he podido consultarla. La fuente del proyecto
(`docs/INVESTIGACION.md`, E17) ya avisaba: "documentación pública escasa". Lo que sigue es una implementación
a partir de descripciones públicas **recordadas**. **Actualización V1 (2026-11-15):** el modo 1600/2 se
ha contrastado con tráfico real (ver "Validación con señales reales" al final); 3200/2, 3200/4 y
6400/4 siguen sin decodificar.

| Elemento | Estado |
|---|---|
| Palabra de 32 bits = 21 datos + BCH(31,21) + paridad par; mismo BCH que POCSAG | descripción pública (coherente con M.584 y con los decodificadores abiertos) |
| 1600/3200/6400 bit/s con 2 y 4 niveles; 1, 2 o 4 fases (A–D) | descripción pública |
| Trama de 1,875 s, 11 bloques de 8 palabras por fase, entrelazado 8×32 | descripción pública |
| Palabra A = código de 16 bits + complemento: 0x870C, 0xB068, 0xDEA0, 0x4C7C | **recordado; la asignación código→modo no está confirmada** |
| Relleno "bit sync 1" (32 bits 1010…) delante de A | **supuesto** (el decodificador solo exige A) |
| FIW: bits 0-3 suma de nibbles = 0xF, 4-7 ciclo, 8-14 trama | recordado; el algoritmo de suma está **sin confirmar** |
| Sync 2: longitud y contenido | **desconocidos**: el decodificador busca el desplazamiento (0-96 símbolos) por validez BCH del bloque 0 |
| BIW: bits 8-9 `aoffset`, 10-15 `voffset` | recordado, sin confirmar |
| Dirección corta: 0x8001 ≤ w ≤ 0x1E0000, capcode = w − 0x8000; direcciones largas **no soportadas** (se cuentan) | recordado |
| Vector: bits 0-3 suma, 4-6 tipo (2 tono, 3 numérico, 5 alfa), 7-13 primera palabra, 14-20 longitud | recordado |
| Alfanumérico: cabecera de 7 bits + 2 caracteres en la 1ª palabra, 3 por palabra después | recordado, sin confirmar |
| Numérico: cabecera de 10 bits, nibbles LSB primero, tabla `0123456789 U -][` | **muy poco seguro** |
| Orden de bits dentro de la palabra (el primero transmitido = LSB de los datos) | supuesto |
| Mapeo de dibits de 4 niveles (Gray, +3 = 01) y reparto de bits entre fases | **supuesto** |
| Desviaciones: ±4800 Hz (2 niveles), ±1600/±4800 (4 niveles) | descripción pública |

**Consecuencia:** el test comprueba la autoconsistencia modulador↔decodificador, el BCH y el procesado de
señal (ruido, CFO, reloj), pero **no garantiza decodificar un transmisor FLEX real**. Con una captura real
habría que ajustar las constantes marcadas (todas están en `flex.h` y en los comentarios de `flex.c`).

## Cadena DSP

`IQ → FIR de canal (corte 10,5 kHz) → discriminador FM (±4800 Hz = ±1) →` dos receptores con reloj propio
(1600 Bd y 3200 Bd) con filtro adaptado boxcar y reloj de Gardner (`pocsag_fsk.h`). Máquina de estados:
`SEARCH` (palabra A en el receptor de 1600 Bd, ≤ 3 errores, ambas polaridades) → `FIW` (32 bits; BCH, suma,
ciclo ≤ 14) → `DATA` (símbolos blandos a la velocidad del modo, 11·256·fases bits + 96 símbolos de margen para
Sync 2) → decodificación: se elige el desplazamiento que maximiza Σ(10 − errores) de las 8 palabras del primer
bloque de la fase A (exige ≥ 6 válidas), se desentrelaza cada fase, se corrige el BCH y se recorren BIW,
direcciones y vectores. Solo se emiten mensajes con dirección, vector (suma de nibbles) y todas las palabras
del mensaje válidas.

## Eventos

```json
{"capcode":123456,"phase":"A","cycle":3,"frame":42,"kind":"alpha","msg":"…","mode":"3200/2",
 "baud":3200,"levels":2,"corrected":0,"text":"FLEX 3200/2 capcode 123456 fase A alpha: …"}
```
`kind` = `alpha` | `numeric` | `tone`. `baud` = bit/s del modo.

## state_json

`{"frames","bad_frames","msgs","long_addr_skipped","mode":"1600/2"|null,"cycle","frame","pages":[…]}` con
los últimos 32 mensajes (`capcode`,`phase`,`kind`,`mode`,`count`,`seen`,`msg`), caducidad 600 s.

## Modulador

Una trama por orden: `{"mode":"3200/4","cycle":3,"frame":42,"messages":[{"capcode":123456,"phase":"A","type":"alpha","text":"…"},…]}`
(`type`: `alpha`, `numeric`, `tone`; capcode 1…1 933 312). Sin parámetros, mensajes de ejemplo en todas las
fases del modo. `air_ok = false`.

## Limitaciones

* Direcciones largas, vectores de instrucción corta/seguro/binario/numerado, mensajes fragmentados y grupos
  no se interpretan.
* Solo se decodifica una trama por detección de Sync 1 (las tramas consecutivas se encadenan por la
  máquina de estados, pero con la longitud de relleno del Sync 2 desconocida podría perderse la siguiente).
* Coste de CPU ≈ 0,004 s por s de señal.

## Validación con señales reales (V1, 2026-11-15)

Fuentes: repositorio de pruebas de multimon-ng (`FLEX_1600_2fsk_P2000_proef_alarm.flac`, P2000
Países Bajos; GPL-2.0, solo datos de prueba) y la Signal Identification Wiki (FLEX 1600 de un
hospital de EE. UU., 3200 y 3200/4). multimon-ng (FLEX_NEXT) como referencia. Cambios:

| Constante | Antes | Real |
|---|---|---|
| Palabra A de Sync 1 | código + complemento (`870C 78F3`) | **código + 0xA6C6** (`870C A6C6`); los códigos 0x870C (1600/2), 0xB068 y 0xDEA0 (3200/4) salen tal cual de las grabaciones |
| Estructura de Sync 1 | A + FIW | `[BS1][~A (32)][puntos 16][A (32)][FIW]`: la 1ª copia (~A) se confundía con A invertida y su "FIW" era basura; ahora se sigue buscando |
| FIW | bits 0-3 suma, 4-7 ciclo, 8-14 trama | **confirmado** (suma de nibbles ≡ 0xF, ciclo 14, tramas 114, 115, 116…), BCH(31,21) con el primer bit transmitido como grado más alto |
| Sync 2 | 0-96 símbolos buscados | ≈ 40 símbolos tras el FIW (la búsqueda por validez BCH lo encuentra) |
| Entrelazado 8×32, palabra idle 0x1FFFFF/0x0 alternadas | supuesto | **confirmado** |
| Dirección corta capcode = w − 0x8000, vector (suma/tipo 5 alfa/mw1/len) | supuesto | **confirmado** (capcode 1123201, vector 0x281D7: alfa, 1ª palabra 1, 10 palabras) |
| Texto alfanumérico | 2 caracteres útiles en la 1ª palabra | la cabecera de fragmento ocupa una palabra pequeña (< 2¹³) y **los 7 primeros bits de la primera palabra de datos no son ASCII**; el texto empieza en el 2º carácter |

Resultado en el P2000 (1600/2): 20 mensajes con texto idéntico al de multimon-ng ("Test: Proefalarm
Ochtend Brandweer Veiligheidsregio Rotterdam Rijnmond.", "B2 Eindhoven Rit: 105012", …); en la
muestra hospitalaria "CLEAN [T]: AVILES, MERCEDES 77 Medical #: …". **Sigue sin funcionar:**
(1) 3200/2, 3200/4 y 6400/4: el Sync 1 y el FIW salen bien pero el BIW sale ilegible (las palabras
de los bloques no son válidas): el mapeo de dibitos de 4 niveles o el reparto de fases es otro; no
resuelto. (2) Direcciones largas (capcodes > 1 933 312, "LS"/"LN" en multimon-ng): se cuentan y
no se emiten; la fórmula de la dirección larga no está confirmada. Test:
`tests/data/flex_p2000_phase.u8` en `tests/test_flex.c`.

### Alta velocidad (V1, 2026-11-16)

Muestras reales a 3200/4 hay dos (sigidwiki "FLEX" y "FLEX 6400bps": ambas llevan el código A 0xDEA0 y
`multimon-ng` las decodifica como 3200/4; la de "3200" con A = 0xB068 no la decodifica ni la referencia).
No hay muestras de 6400/4 (código 0x4C7C) ni de 3200/2. Lo que se pudo medir en 3200/4 con
FLEX.mp3: el Sync 1 y el FIW salen bien (`[A][puntos][~A][FIW]` con la polaridad de esa grabación),
los datos empiezan ≈ 41 símbolos tras el FIW y dos flujos de bits —el indicador "nivel exterior"
(|v| > 2/3) y el signo— contienen palabras BCH válidas intercaladas 8×32 en 15 de 16 palabras
de una trama con tráfico, así que el reparto A/B con 2 bits por símbolo es del tipo implementado,
pero el decodificador sigue sacando direcciones sin sentido (casi todo "dirección larga") por la
tasa de error de símbolo y la falta de las direcciones largas (todos los capcodes de esa
grabación son largos, `LS` en multimon-ng). **Sin resolver:** el mapeo exacto nivel → bits por fase y
la fórmula de la dirección larga.
