# DSP: filtros, remuestreo y DDC

## Figuras

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/canal-audio-dark.png">
  <img src="../img/canal-audio-light.png" alt="Cadena de un canal de audio">
</picture>

*Figura. Cadena de un canal de audio (`src/dsp/channel.h`): NCO, diezmado, filtro de canal, nivel y squelch, demodulador y remuestreo a 48 kHz.*


Este documento resume lo que añade la base (A1). Los bloques anteriores (FFT,
canal de audio, detector) están explicados en las cabeceras de `src/dsp/`.

## Diseño FIR por ventana de Kaiser (`filter.c`)

Un paso bajo ideal de corte `fc` (normalizado a fs) es `h[n] = 2fc·sinc(2fc·n)`.
Se trunca a N coeficientes y se multiplica por la ventana de Kaiser
`w[n] = I0(β·√(1−r²)) / I0(β)`, `r = (n − m)/m`. Las fórmulas empíricas de Kaiser
dan, para una atenuación A (dB) y una transición Δf/fs:

```
β = 0,1102·(A − 8,7)            (A > 50)
N ≈ (A − 8) / (2,285·2π·Δf/fs)
```

Todos los filtros se normalizan para ganancia 1 en continua.

## Diezmador `firdec_t`

Solo calcula una de cada D salidas: el coste es proporcional a la tasa de
**salida**. La línea de retardo está duplicada (cada muestra se escribe en `i` e
`i+N`) para que las N últimas muestras sean contiguas y el producto escalar se
vectorice. Con paso `pass_hz` la banda de rechazo empieza en `fs_out − pass_hz`:
lo que aliasee dentro de la banda útil queda ≥ 70 dB abajo.

## Remuestreador fraccionario `resamp_t` / `cresamp_t`

Para obtener `y(τ)` entre muestras se evalúa `Σ x[k]·h(τ − k)` con `h` un paso
bajo continuo. Se precalcula en **P = 256 fases**: `bank[p][j] = h(L/2−1−j+p/P)`,
cada fase normalizada a ganancia 1. La salida usa la fase más cercana a la
fracción de `τ` (error temporal ≤ 1/512 de muestra). El avance por salida es
`step = fs_in/fs_out` y la fracción `frac` se acumula en `double`: la **tasa de
salida es exacta** a largo plazo (error < 1e-12). `cresamp_t` es idéntico pero la
línea de retardo guarda muestras `cf32`; comparte el banco de coeficientes con
`resamp_init`. Las pruebas (`tests/test_dsp.c`) miden `n·fout/fin ± 1,5` salidas y
conservación de frecuencia y amplitud del tono.

## DDC (`ddc.c`)

```
x[n] ─► NCO e^{-jωn} ─► firdec ÷D1 ─► firdec ÷D2 … ─► cfir ±bw/2 ─► cresamp ─► y[m]
```

1. **Mezcla.** `ω = 2π(f_canal − f_centro)/fs_in` con signo negativo: el canal queda
   en 0 Hz. El NCO es un rotor (`fasor·e^{jω}`), renormalizado por bloque.
2. **Diezmado en etapas.** `M = ⌊fs_in/fs_out⌋` se reduce al mayor `M' ≤ M` con
   factores primos {2,3,5,7} y se reparte en etapas ≤ 8 (8,7,6,5,4,3,2). Cada etapa
   `k` con tasa de salida `r` usa `pass = bw/2 + ¼(r − bw)`, transición `½(r − bw)`.
   Las primeras etapas, con tasas altas, tienen transiciones anchas → pocos taps.
3. **Filtro de canal.** `cfir_t` paso banda `[−bw/2, +bw/2]` (transición el 20 % de
   `bw`, mín. 2 % de la tasa intermedia), 65 dB.
4. **Remuestreo.** `cresamp_t` de la tasa intermedia (entre `fs_out` y ~1,5·`fs_out`)
   a `fs_out` exacto, con corte en `bw/2`.

`ddc_retune` solo cambia el paso del NCO y conserva su fase: la salida no tiene
saltos. El trabajo se hace en bloques internos de 8192 muestras (cabe en caché
aunque la entrada sea de 61 Msps). Resultado medido (`test_dsp`): un interferente a
+30 kHz de un canal de 16 kHz queda a −93 dB.

## Respuestas medidas con nuestro código

Las gráficas siguientes no son esquemas: salen de pequeños programas en C que enlazan con `src/dsp/*` y vuelcan los coeficientes y las medidas reales (`build/doc2b/*.c`), y se dibujan con `docs/img/src/plots_tecnicos.py`. Los datos se regeneran con `build/doc2b/build.sh` y `python3 docs/img/src/plots_tecnicos.py`.

### Diezmador `firdec_t`

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/dsp/firdec-respuesta-dark.png">
  <img src="../img/dsp/firdec-respuesta-light.png" alt="Respuesta del diezmador FIR de Kaiser">
</picture>

*Figura. Etapa ÷8 de un DDC con canal de 200 kHz, a 2,4 Msps (209 taps) y a 61,44 Msps (73 taps). La banda rosa es la que, al diezmar, aliasea sobre la banda útil: queda por debajo de −68 dB, como pide el diseño de Kaiser (atenuación de 65 dB y más).*

### Filtro de canal `cfir_t`

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/dsp/cfir-canal-dark.png">
  <img src="../img/dsp/cfir-canal-light.png" alt="Respuesta de los filtros de canal complejos">
</picture>

*Figura. Paso banda de coeficientes complejos de `channel.c` a 48 kHz. USB y LSB dejan pasar solo un lado (200–2700 Hz y −2700…−200 Hz); AM y NFM son simétricos. Nota: `cfir_process` guarda los coeficientes en orden inverso (el coeficiente `i` multiplica a `x[n−(N−1−i)]`), por lo que la respuesta al impulso es el vector invertido.*

### Remuestreador polifásico

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/dsp/resamp-respuesta-dark.png">
  <img src="../img/dsp/resamp-respuesta-light.png" alt="Respuesta del remuestreador polifásico">
</picture>

*Figura. Prototipo del banco de 256 fases (WFM, 240 → 48 kHz con corte de 15 kHz; NFM, 52 083 → 48 kHz con corte de 3 kHz). La línea punteada marca fs_out/2; lo que queda por encima de `fs_out − corte` aliasearía sobre la banda de audio.*

### Ventana Blackman-Harris

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/dsp/ventana-blackman-harris-dark.png">
  <img src="../img/dsp/ventana-blackman-harris-light.png" alt="Ventana Blackman-Harris y su espectro">
</picture>

*Figura. `window_blackman_harris()` con N = 1024: lóbulos laterales a −93 dB (el primero de la rectangular está a −13 dB y el de Hann a −31 dB) a cambio de un lóbulo principal más ancho (ENBW = 2,006 bins).*

### Calibración del espectro

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/dsp/calibracion-espectro-dark.png">
  <img src="../img/dsp/calibracion-espectro-light.png" alt="Calibración del espectro">
</picture>

*Figura. Un tono complejo de amplitud conocida frente al pico medido por `spectrum.c` (ruta CPU; la ruta GPU da el mismo resultado en `tests/test_gpu.c`, error máximo 0,09 dB). Con el tono entre dos bins la ventana pierde 0,8 dB (festoneo); por debajo de −70 dBFS el tono se acerca a la cuantización de 12 bits.*

### BER con y sin Viterbi

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/dsp/ber-viterbi-dark.png">
  <img src="../img/dsp/ber-viterbi-light.png" alt="BER con y sin Viterbi">
</picture>

*Figura. Código K = 7 (0171, 0133) de `fec.c`, BPSK sobre AWGN. A 4 dB de Eb/N0 el Viterbi blando de tasa 1/2 llega a ≈ 10⁻⁵ mientras que el duro ronda 5·10⁻³ y la señal sin codificar 1,3·10⁻²; el blando cuantizado a 8 bits (int8) es indistinguible del de coma flotante. La curva 3/4 es el punzonado de DVB.*
