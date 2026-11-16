# Emisión (`src/tx/`)

Cada decodificador tiene un **modulador** gemelo (`mod_<id>.c`, contrato en
`src/tx/mod.h`) que produce banda base compleja centrada en 0 Hz, `|x| ≤ 0,9`, a la
tasa nativa del protocolo. Sirve para (1) probar decodificadores sin radio, (2)
generar ficheros SigMF de prueba y (3) emitir, solo si la política lo permite.

## Modos (`tx.c`)

| Modo | Requisito | Qué hace |
|---|---|---|
| `file` | ninguno | Escribe `recordings/tx_<proto>_<fecha>.sigmf-{data,meta}` (ci16, escala Q11) a `center`/`rate` con el canal desplazado a `freq`. Se reproduce con `--replay`. |
| `loopback` | `--enable-tx` | `BLADERF_LB_RFIC_BIST` (bucle interno del RFIC), ganancia fija de −20 dB (cerca del mínimo). Nada sale por la antena. Duración ≤ 30 s. |
| `air` | `--enable-tx-air` y todo lo siguiente | Radia por la antena. |

> Los modos `loopback` y `air` están implementados con `source_tx_*` pero **no se
> han ejecutado contra el hardware** en esta fase. Los tests (`tests/test_tx.c`)
> usan un sustituto del dispositivo (`tx_set_sink`) que nunca toca el bladeRF.

## Política de `air` (`tx_check_air`)

Se rechaza salvo que se cumpla **todo**:

1. `--enable-tx` y `--enable-tx-air`.
2. El modulador tiene `air_ok = true`. Nunca lo tienen: adsb, ais, dsc, acars, vdl2,
   rs41, dab, rds, lte, nr, gsm, wifi, dvbt, dmr, flex, pocsag. (Los esqueletos
   actuales marcan `true` en aprs, ism, lora y meshtastic.)
3. Ganancia ≤ `--tx-max-gain` (0 dB por defecto).
4. Duración total ≤ 5 s, calculada con las muestras realmente enviadas × `repeat`.
5. La señal **entera** (`freq` ± su ancho ocupado) cae dentro de UNA banda de la
   lista blanca, y el **centro del hardware** (donde sale la fuga de la portadora
   del oscilador local) cae en la misma banda:

   | Banda | Condición |
   |---|---|
   | ISM 433,050–434,790 MHz | — |
   | SRD 868,000–868,600 MHz | ciclo de trabajo ≤ 1 %/hora (36 s/h) |
   | SRD 869,400–869,650 MHz | — |
   | Radioaficionado 144–146 MHz | `--callsign` |
   | Radioaficionado 430–440 MHz | `--callsign` |

   El **ancho ocupado** se mide sobre la señal modulada entera: periodograma de
   Welch con FFT de 1024 y ventana Blackman-Harris; se toman los bordes que dejan el
   0,01 % de la potencia a cada lado (99,98 % ocupado). Además, la **imagen
   especular** del desequilibrio IQ (`2·centro − f`) y el centro (fuga del LO) deben
   caer en la misma banda.
6. En bandas de radioaficionado el indicativo se inserta en `params` como
   `"callsign"` (los protocolos que lo admiten, p. ej. APRS, lo meten en la trama).

Aviación, marítimo, GNSS, celular, radiodifusión y emergencias (406 MHz) quedan
fuera de la lista: no se pueden emitir ni cambiando el código del modulador.
Cada emisión se escribe en el log (protocolo, frecuencia, centro, tasa, ganancia,
duración, ancho ocupado). Solo hay una emisión a la vez; `tx_stop` la interrumpe.

## Colocación de la señal (`tx_render`)

Modulador → medida del ancho → remuestreo `cresamp_t` a `rate` (por defecto la del
modulador si ≥ 1 Msps, si no 1 Msps) → NCO con `freq − center`. Centro automático
(`center = 0`): en `file`, los decodificadores *narrow* se desplazan `−rate/4` (para
que no caigan sobre el pico de continua) y los *wide* quedan centrados; en
`loopback`/`air`, `center = freq`. Si `|freq − center| + borde > rate/2` se rechaza.

## Uso

```
# generar una señal y decodificarla
bladerf-lab --gen adsb --gen-freq 1090M --gen-center 1090M --gen-rate 2M --out /tmp/x
bladerf-lab --replay /tmp/x.sigmf-meta --decode adsb --print --duration 3

# desde la interfaz o un cliente WebSocket
{"cmd":"tx","proto":"adsb","mode":"file","freq":1090000000,"params":{"callsign":"TEST1"}}
```
La respuesta es siempre `{"type":"tx_result","ok","msg","file","seconds"}`.

## Revisión de seguridad (T1) de `tx.c`

Huecos encontrados en la versión inicial y cerrados (cada uno con test en `test_tx.c`):

| Hueco | Cierre |
|---|---|
| Señales de < 1024 muestras medían ancho 0 y pasaban cualquier banda | se rellenan a 1024 muestras |
| Señales largas: solo se medían los primeros ~2 M de muestras | se barre toda la señal |
| Fallo de memoria en la medida → ancho 0 → aceptada | la medida devuelve error y se rechaza |
| Imagen especular IQ (`2·centro − f`) sin comprobar | debe caer en la misma banda |
| Ciclo de trabajo: se anotaba al terminar y solo si iba bien; con 64 emisiones se dejaba de anotar; se reiniciaba al reiniciar el programa | se anota al lanzar, la tabla se funde (sobrestima) y se persiste en `recdir/tx_log.jsonl` |
| Dos órdenes simultáneas lanzaban dos hilos | cerrojo de `submit` |
| Indicativo sin validar; modo, tasa, frecuencia, ganancia, `repeat` sin acotar; conversión double→entero fuera de rango (UB) en `main.c` | validación en `tx.c` y `main.c` (`num_in`) |
| Fichero `file` con `repeat × señal` sin límite | máx. 10⁸ muestras |

Rutas: la única que llama a `source_tx_*` es `tx.c` (`hw_*`); WS (`tx`, `tx_play`,
`tx_cal`) y `--gen` pasan por él (`--gen` solo escribe ficheros). Solo `file` no pasa
por la lista blanca, porque no radia.

## Reproducción de grabaciones (`{"cmd":"tx_play","name":"…","mode":"file|loopback","repeat":N}`)

Lee `recordings/<name>.sigmf-{meta,data}` (ci16_le, cf32_le, ci8, cu8; el nombre solo
admite `[A-Za-z0-9._-]`, sin `..` ni `/`, y no se siguen enlaces simbólicos). `file`
escribe una copia `tx_play_*`; `loopback` (requiere `--enable-tx`) la emite por el
bucle interno con ganancia mínima, ≤ 30 s, al centro/tasa grabados. **`air` se
rechaza siempre** (riesgo de ataques de repetición): no hay ruta de código que
reproduzca una grabación por antena.

## Calibración del TX (`src/tx/cal.{h,c}`, `{"cmd":"tx_cal","freq":Hz,"rate":sps}`)

Modelo `y = x + q·x* + c` (q: desequilibrio ganancia/fase, c: fuga de LO). Ráfaga
`[ceros][preámbulo PN][tono +fs/16][tono −fs/16][ceros]`; el preámbulo da el retardo
entero y los dos tonos cancelan el fraccionario (fórmulas en `cal.h`, basadas en la
idea de tono desplazado de Hsu & Sheen / US 8290458). Precorrección exacta
`x = (w − q·w*)/(1 − |q|²)`, `w = z − c`, aplicada en `tx_job` antes de `source_tx_write`
si hay calibración para ese centro y tasa. Prueba contra un modelo con g = 1,12,
φ = 6°, DC (0,02, −0,015), retardo fraccionario y SNR 40–45 dB: error de q < 0,004 y
rechazo de imagen 22 → 92 dB.

La orden exige `--enable-tx`, **siempre** usa loopback con ganancia mínima, mide,
verifica con una segunda pasada precorregida y solo guarda si mejora. Responde con
`{"type":"tx_cal","ok","saved","msg","irr_before_db","irr_after_db","lo_before_dbc","lo_after_dbc"}`.

Pendiente para usarla en el equipo (decisión del usuario):
1. El motor debe instalar `tx_set_rx_capture()` (captura de N muestras del receptor
   sintonizado a freq/rate); sin él la orden responde con un error.
2. **Limitación física**: `BLADERF_LB_RFIC_BIST` es un bucle digital dentro del AD9361
   y no atraviesa el mezclador/LO del TX; en él la medida dará ~0 (la orden lo
   detecta y no aplica nada). La bladeRF 2.0 no ofrece bucle RF; para calibrar de
   verdad hace falta medir la salida por otra vía (receptor externo / atenuador).
   El algoritmo es el mismo.
3. q y c dependen de frecuencia y ganancia: la corrección es válida solo para el
   centro y tasa medidos.

## Simulador de canal (`tests/chan_sim.h`)

Cabecera reutilizable (prefijo `cs_`): desvanecimiento Rayleigh/Rice por suma de
sinusoides de Jakes (`cs_fader_*`, `cs_fading`), multitrayecto con retardos
fraccionarios (`cs_multipath`), ruido de fase de Wiener (`cs_phase_noise`), CFO,
desequilibrio IQ + DC (`cs_iq_imbalance`, `cs_iq_munu`), deriva de reloj
(`cs_clock_drift`), AWGN y la cadena completa `cs_apply(cs_cfg_t)`. Su test
(`test_chan_sim.c`) comprueba potencia, P(|h|²<0,1), autocorrelación J0, varianza de
Rice, potencias de caminos, varianza del ruido de fase, IRR teórico, deriva en ppm y SNR.
