# WiFi (balizas 802.11b/g/a) — `wifi`

<!-- vision-rapida:inicio (generada por docs/img/src/doc_decoders.py) -->

## Visión rápida

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/decoders/wifi-cadena-dark.png">
  <img src="../img/decoders/wifi-cadena-light.png" alt="Cadena de procesado de WiFi">
</picture>

*Figura. Cadena de procesado de WiFi, dibujada a partir del código de `src/` y de la sección «Cadena DSP» de este documento.*

### Así es la señal

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/decoders/wifi-senal-dark.png">
  <img src="../img/decoders/wifi-senal-light.png" alt="Cascada y ráfagas de balizas 802.11b en la grabación real.">
</picture>

*Figura. Cascada y ráfagas de balizas 802.11b en la grabación real.*

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/decoders/wifi-estructura-dark.png">
  <img src="../img/decoders/wifi-estructura-light.png" alt="PPDU y trama Beacon.">
</picture>

*Figura. PPDU y trama Beacon.*

La procedencia de cada señal (grabación real o sintética) y qué parte del procesado es Python de ilustración y no el código C se indican en el pie de cada imagen.

### Ejemplo de evento y de estado

Procedencia: grabación IQ real (sigidwiki «Wi-Fi», balizas 802.11b DSSS). Sacado de ejecutar el decodificador, no escrito a mano (`bladerf-lab --replay wifi.sigmf-meta --decode wifi --print`).

Evento `dec` (campo `msg`, listas largas recortadas):

```json
{
  "text": "UPC5035941 38:43:7D:EF:D8:7C canal 1 WPA2+WPA n -54 dBFS",
  "kind": "beacon",
  "event": "new",
  "ssid": "UPC5035941",
  "bssid": "38:43:7D:EF:D8:7C",
  "channel": 1,
  "band": "2.4",
  "security": "WPA2+WPA",
  "std": "n",
  "phy": "DSSS 1M",
  "rates": "1,2,5.5,11,9,18,36,54,6,12,24,48",
  "rssi": -54.2,
  "beacon_ms": 102.4,
  "count": 1
}
```

Último `dec_state` recibido por WebSocket (`state`, listas recortadas):

```json
{
  "nets": [
    {
      "ssid": "UPC5035941",
      "bssid": "38:43:7D:EF:D8:7C",
      "channel": 1,
      "band": "2.4",
      "security": "WPA2+WPA",
      "std": "n",
      "phy": "DSSS 1M",
      "rates": "1,2,5.5,11,9,18,36,54,6,12,24,48",
      "rssi": -53.7,
      "beacon_ms": 102.4,
      "count": 2,
      "age": 3
    },
    {
      "ssid": "UPC8443073.2,4G",
      "bssid": "34:2C:C4:18:95:3B",
      "channel": 1,
      "band": "2.4",
      "security": "WPA2+WPA",
      "std": "n",
      "phy": "DSSS 1M",
      "rates": "1,2,5.5,11,9,18,36,54,6,12,24,48",
      "rssi": -36.0,
      "beacon_ms": 102.4,
      "count": 44,
      "age": 0
    },
    "… (11 más)"
  ],
  "bursts": 1496,
  "frames": 188,
  "fcs_err": 256,
  "skipped": 11,
  "noise_dbfs": -84.5
}
```

### En la web

![WiFi en la interfaz web](../img/decoders/wifi-web.png)

*Captura de la interfaz con un servidor `--replay` propio mostrando WiFi.*

<!-- vision-rapida:fin -->

Decodificador **wide** (20 Msps exactos, muestras Q11) que descubre redes WiFi a
partir de sus **balizas (Beacon)** y **respuestas de sondeo (Probe Response)**.

## Alcance ético (AGENTS.md §0.12)

- Solo tramas de gestión de difusión (tipo 0, subtipos 8 y 5). Del PSDU solo se
  usan el BSSID (dirección 3) y los elementos de información públicos.
- El receptor decodifica primero únicamente el **prefijo** del PSDU (≈ 8 bytes tras
  SERVICE) para leer el control de trama; datos, control, peticiones de sondeo
  (que llevan la MAC del cliente), etc. se **descartan ahí** (contador `skipped`)
  sin desentrelazar ni decodificar el resto.
- La dirección 1 de una respuesta de sondeo es la MAC de un cliente: **nunca se
  lee ni se emite**. Hay un test (`probe_resp` con cliente ficticio
  `02:AA:BB:CC:DD:EE`) que lo comprueba. No se descifra nada.

## Teoría y cadena DSP

0. **Detector de ráfagas** (reescrito en D4b con datos de aire, ver «Captura real en el aire»).
   Potencia por bloques de 1 µs (20 muestras). Suelo de ruido = media móvil exponencial
   (1/1024 por µs) actualizada solo cuando NO se abre ráfaga; arranca con la mediana de los 64
   primeros µs y baja de golpe si la media de 64 µs cae < 0,9× (cambio de ganancia).
   Apertura con dos criterios: media de 8 µs > 1,8× el suelo (+2,6 dB, señales fuertes) o media
   de 64 µs > 1,35× (+1,3 dB, señales débiles); pre-roll de 2048 muestras (102 µs: la ventana
   lenta se retrasa hasta ~40 µs). Cierre: 64 µs seguidos con la media de 16 µs < 1,2× (una
   baliza débil fluctúa ±40 % y se partía en 2-7 trozos) o, en ráfagas fuertes, 3 bloques bajo
   el 25 % de su nivel. Tras cerrar, 64 µs sin reabrir (la ventana de 64 µs aún contiene la
   cola). Ráfagas < 24 µs se descartan; las de > 10 ms no se procesan y re-aprenden el suelo.
   Potencia/SNR de la ráfaga = mediana de sus bloques. Todo el DSP posterior solo corre sobre
   ráfagas. Si la vía OFDM no produce nada (un tono estrecho también da correlación a retardo
   16) se prueba DSSS.
1. **OFDM 802.11a/g** (Bloessl et al., SRIF 2013; IEEE 802.11-2020 cl. 17, 6–54 Mb/s):
   - Schmidl-Cox sobre el STF: `P(n)=Σ x[n+k]·x*[n+k+16]`, `M=|P|²/R²` > 0,5
     sostenido 24 muestras → inicio; CFO grueso `f=−arg(P)·fs/(2π·16)` (±625 kHz).
   - Correlación cruzada con el LTF conocido (dos picos separados 64) → inicio de
     símbolo; CFO fino con el desfase entre los dos LTF (retardo 64).
   - Canal `H(k)=media(Y₁,Y₂)·L(k)`, ecualización `Z=Y·H*/|H|²` regularizada.
     Por símbolo, ajuste LS de la fase de los 4 pilotos `θ₀+b·k` (error común/CFO
     residual y deriva de reloj). Las ventanas FFT arrancan 4 muestras antes del
     final del GI para tolerar ecos.
   - SNR: de los dos LTF idénticos, `E|Y₁−Y₂|²=2N` y `E|Y₁+Y₂|²/4=|H|²+N/2`.
   - SIGNAL: BPSK 1/2 → Viterbi K=7 (133,171) → paridad par, RATE (Tabla 17-6, las
     8 tasas), LENGTH. Después, prefijo → control de trama → resto.
   - Demapeo blando (§17.3.5.8, Gray): por eje, bit 1 = nivel x, bit 2 = 2−|x| (16-QAM)
     o 4−|x| (64-QAM), bit 3 = 2−||x|−4|; **el peso de fiabilidad |H|²/media se aplica
     después** de estas no linealidades (aplicarlo antes deformaba los umbrales y dejaba
     un suelo de error con multitrayecto en 16/64-QAM).
   - Datos: desentrelazado (dos permutaciones), reinserción de borrados
     (punzonado 3/4: `A0 B0 A1 · · B2`; 2/3 de 48 Mb/s: `A0 B0 A1 ·`), Viterbi con cola, desaleatorizado
     (semilla de los 7 primeros bits de SERVICE, que son 0), FCS CRC-32.
2. **DSSS 802.11b 1 Mb/s** (si no hay STF): filtro adaptado al chip (coseno alzado α=0,5, 6
   muestras alrededor de la posición de chip `p+i·20/11`; pesos precalculados para las 44
   fracciones posibles) en vez de leer UNA muestra interpolada por chip (que tira 10·log10(20/11)
   = 2,6 dB de Es/N0). Fase de símbolo: ventanas de 50 símbolos (hasta 3 con señal, a +50 y
   +100), 20 fases de 1 muestra + afinado de ±0,75 en pasos de 0,25 sobre símbolos alternos;
   contraste `S_máx/media` ≥ 2,3 (ruido/OFDM: 1,2-2,1; balizas reales a −3 dB: 3,5-6);
   seguimiento temprano/tardío cada 4 símbolos (±100 ppm); DBPSK diferencial con CFO por
   `arg(Σd²)/2`; desaleatorizador autosincronizado `z⁻⁷+z⁻⁴+1`; SFD `0xF3A0` precedido de
   unos; cabecera PLCP con CRC-16 (solo SIGNAL = 0x0A, 1 Mb/s); FCS CRC-32.

## Parámetros y salida

`create` exige exactamente 20 Msps. Sin opciones. El canal se toma del IE DS
Parameter Set; si falta, del IE HT Operation o de la frecuencia central.

**Evento** (uno por red nueva o cuando cambian SSID/canal/seguridad/estándar):

```json
{"text":"BladeRF-Lab 02:00:5E:10:00:01 canal 6 WPA2 g -13 dBFS","kind":"beacon",
 "event":"new","ssid":"BladeRF-Lab","bssid":"02:00:5E:10:00:01","channel":6,
 "band":"2.4","security":"WPA2","std":"g","phy":"OFDM 6M","rates":"6,9,12,…",
 "rssi":-12.9,"beacon_ms":102.4,"count":1}
```
`security`: `open`, `WEP`, `WPA`, `WPA2`, `WPA2-EAP`, `WPA3`, `WPA2/WPA3`
(+`+WPA` si también hay IE de proveedor WPA); `std`: `b`, `g`/`a` (según banda),
`n`, `ac`, `ax` (por presencia de IEs HT/VHT/HE). `rssi` en dBFS (potencia de
ráfaga menos suelo); `snr` en dB (OFDM: de los LTF; DSSS: potencia de ráfaga/suelo);
`band` `"2.4"`/`"5"` según la frecuencia central; `freq` = frecuencia de captura (Hz);
`country` del IE 7; `last` = hora Unix de la última baliza. En 5 GHz no existe el IE DS
Parameter Set: el canal sale del IE HT Operation o, si no, de la frecuencia central
(5180 MHz → 36). **Posición** (`dec_emit_span`): `sample` = primer símbolo modulado (inicio
del STF; en DSSS, 128 µs de SYNC antes del SFD) y `sample_end` = último símbolo de datos.

**state_json**: `{"nets":[{ssid,bssid,channel,band,freq,security,std,phy,rates,rssi,
snr,beacon_ms,count,country?,last,age}],"bursts","frames","fcs_err","skipped","noise_dbfs"}`.
Las redes caducan a los 300 s; tabla de 64.

Modulador (`mod_wifi.c`, `--gen wifi`): dos redes de ejemplo (OFDM 6 Mb/s WPA2 y
DSSS 1 Mb/s abierta); `{"std":"g|a|b|gb" (a = 5 GHz, sin IE DS),"ssid","bssid","channel","security",
"rate":6|9|12|18|24|36|48|54,"ht","repeat","gap_us","frame":"beacon|probe_resp|data"}`.
`air_ok=false`.

## Pruebas (`tests/test_wifi.c`)

Independientes del modulador: secuencia PRBS del aleatorizador (56 primeros bits),
polaridad de pilotos p₀…p₁₅, CRC-32 de "123456789", posiciones de portadoras de
datos, entrelazador. De sistema (SNR de potencia media): OFDM y DSSS con
CFO ±100…150 kHz, ±100 ppm, 2 ecos (OFDM), 10–15 dB; **las 8 tasas OFDM con CFO de
±¼ del espaciado (78,1 kHz), ±100 ppm y 2 ecos (−6 y −10 dB, 150 y 350 ns)**; SNR de
prueba por tasa 10 dB (6/9/12), 12 (18), 14 (24), 22 (36), 26 (48), 28 (54). Barrido
medido (mismo canal, CFO 78 kHz, 100 ppm): 6–12 Mb/s decodifican desde 8 dB, 18–24 desde
10, 36 desde 18, 48 desde 20–24 y 54 desde 24 dB, coherente con la sensibilidad de la Tabla
17-18 de la norma; las tasas altas no pueden exigirse a 10 dB. También 5 GHz (canal 36 y
100), continua del receptor de 0,05 FS, posición `smp0`/`smp1`, seguridad; probe-resp y datos (ética);
solo ruido (también ruido cuantizado de 0,7/1,5/8 LSB con continua: 0 ráfagas); baliza DSSS a −2 dB de SNR por muestra (12/12) y la captura real propia (`tests/data/real/wifi_dsss_aire`, 1 baliza a −3,4 dB); coste: ~0,10 s de CPU/s con ≈25 % de ocupación del aire, ~0,45 s/s con
el canal 100 % saturado (peor caso no realista; domina el Viterbi de las balizas).

## Limitaciones y riesgos

- **OFDM sigue sin IQ real decodificable** (la captura propia de Pamplona solo trae DSSS: ver «Captura real en el aire») (pendiente de capturar con el bladeRF en el
  aire, ver más abajo): las tablas
  (STF, LTF, entrelazador, punzonado 3/4 y 2/3, polaridad, orden de bits del SIGNAL,
  CRC-16 PLCP transmitido MSB primero, orden de bits del SFD) están escritas de la
  norma de memoria y verificadas solo por coherencia con el modulador y por los
  vectores citados. **Riesgos principales**: punzonado 3/4 (9 y 18 Mb/s) y el orden
  de bits de la paridad CRC-16 DSSS. 6 Mb/s OFDM y la cadena BPSK/Viterbi son lo
  mejor contrastado (PRBS y polaridad coinciden con la norma).
- OFDM 6–54 Mb/s y DSSS 1 Mb/s; no 2/5,5/11 Mb/s (CCK). Solo se decodifican por completo
  balizas y respuestas de sondeo; HT/VHT/HE (802.11n/ac/ax) solo se identifican por sus IEs
  (el preámbulo legado de las balizas siempre es 802.11a/g).
- Se resta la continua del receptor con un seguidor de un polo (τ ≈ 0,2 ms).
- Ráfagas pegadas (SIFS) no se separan; ráfaga máxima de 10 ms (≈ 1200 bytes a
  1 Mb/s). Una ráfaga mezcla una baliza con lo que le siga a < 64 µs.
- Con interferencia continua que sube el suelo > 1,35× en < 1 ms el detector abre una ráfaga de 10 ms,
  la descarta y re-aprende el suelo; con Wi-Fi muy cargado el coste sube (Viterbi).
- La muestra Q11 debe llegar a 20 Msps exactos (el host lo exige).

## Referencias

- IEEE Std 802.11-2016, cl. 16 (DSSS), 17 (OFDM), 9.3.3.3 (Beacon), 9.4.2 (IEs).
- Bloessl, Segata, Sommer, Dressler, "An IEEE 802.11a/g/p OFDM Receiver for GNU
  Radio", SRIF 2013, DOI 10.1145/2491246.2491248; informe técnico CCS-2013-02.
- Schmidl & Cox, "Robust frequency and timing synchronization for OFDM", IEEE
  Trans. Commun. 45(12), 1997.

## Validación con señal real (V2, 2026-11-15)

Grabación: sigidwiki.com `Wi-Fi.zip` (IQ 8 bit a 18 MS/s, 0,3 s; routers UPC de operador), remuestreada a 20 MS/s. Decodifica **8 BSSID
reales** en 802.11b DSSS 1 Mb/s (SSID `UPC5035941`, `UPC8443073.2,4G`, `UPC Wi-Free`…, canal 1 del IE DS, seguridad WPA2/WPA2-EAP, 102,4 ms de intervalo):
**preámbulo/SFD, CRC-16 del PLCP, DBPSK/Barker, aleatorizador y FCS confirmados**. Fixture: `tests/data/wifi_sigid_beacon_20M.cs8` (152 kB).
Sin validar: **OFDM** (6 Mb/s, punzonado 3/4 y CRC de SIGNAL): esta grabación no contiene tramas OFDM decodificables.

## Captura en el aire (orden para el usuario)

20 Msps exactos, un canal por captura (la tasa fija el ancho). Canal 6 de 2,4 GHz y canal 36 /
100 de 5 GHz (ganancia manual ≈ 40 dB; con USB 3 no hay pérdidas a 20 Msps):

```
build/bladerf-lab --freq 2437000000 --rate 20000000 --decode wifi@2437000000 --print --duration 60
build/bladerf-lab --freq 5180000000 --rate 20000000 --decode wifi@5180000000 --print --duration 60
build/bladerf-lab --freq 5500000000 --rate 20000000 --decode wifi@5500000000 --print --duration 60
build/bladerf-lab --profile wifi          # servidor + web: 2437 MHz; luego cambiar a 5180 MHz en la web
```
Para validar OFDM con aire real: grabar con el botón de grabación de la web (SigMF, orden `record`) y repetir con `--replay`.
En 2,4 GHz esperar balizas DSSS (1 Mb/s) y OFDM (la mayoría de AP modernos); en 5 GHz solo OFDM
a 6 Mb/s (tasa básica): ahí `std` sale `a` (o `n/ac/ax` por los IEs) y el canal 36/100.

## Captura real en el aire (D4b, 2026-11-21, Pamplona)

Captura propia (bladeRF 2.0 micro, USB 3, canal 6 = 2437 MHz, 20 Msps, ganancia fija 35 dB,
analógico 18 MHz; 3,94 s, `ci16_le` Q11 crudo, fuera de git en `build/capturas/`). Solo se
analizan balizas.

**Niveles.** Ruido 23,5 LSB RMS = −38,9 dBFS (I: 305 LSB², Q: 249 LSB², desequilibrio ~0,9 dB,
correlación I/Q 0,005); continua < 0,001 LSB; sin saturación (pico 111 LSB = −25,3 dBFS); espectro
plano salvo +7 dB en ambos bordes de la banda analógica (±9 MHz). El ruido es estacionario entre
tramas (media de 1 ms: 1,00-1,05 del suelo).

**Qué hay.** Un solo AP en el canal: `MOVISTAR-WIFI6-D458` (44:3B:14:8F:D4:58, WPA2, canal 6),
balizas **DSSS 1 Mb/s** de 461 B cada 102,4 ms (3,9 ms de ráfaga), **SNR por muestra = −3,4 dB**
(potencia de señal 0,46× la de ruido en 20 MHz; Es/N0 tras Barker = −3,4 + 13 = 9,6 dB), CFO
−28,6 kHz (−11,7 ppm, estable). No hay ráfagas OFDM decodificables: tras filtrar a ±8,3 MHz la
autocorrelación del STF (retardo 16, ventana 144) solo llega a 0,38-0,56 en ~20 puntos (SNR ≲ 0
dB, por debajo de la sensibilidad de 6 Mb/s, ≈ 4-5 dB). Otras ráfagas: 14 de 375 µs cada
~307 ms (a veces en pares separados 0,84 ms) con la energía en +8,9…+9,4 MHz (+20 dB sobre el
suelo en 0,5 MHz, el resto de la banda +0,7 dB): un tono/estrecha banda en el borde del filtro,
no 802.11 de este canal (el estimador de CFO del STF da 480 kHz, el alias de +9,2 MHz a retardo 16).
**La señal es intrínsecamente débil** (antena/propagación): subir la ganancia no ayuda (el ruido
que se ve ya es el del propio receptor a 35 dB; a ganancias menores la cuantización del ADC solo empeora la SNR).

**Causas de que el decodificador no viera nada** (y arreglo), con la captura:

| causa | datos | arreglo |
|---|---|---|
| Umbral de apertura +6 dB × 2 bloques consecutivos | las balizas están a +1,6 dB sobre el suelo en 20 MHz | media de 64 µs > +1,3 dB (>12σ) y de 8 µs > +2,6 dB |
| Suelo sesgado −2 dB (seguidor «baja rápido, sube lento» = cuantil ~1 %) | `noise_dbfs` −41,2 con ruido de −39,0 | media exponencial actualizada solo sin ráfaga |
| Baliza troceada (la media de 16 µs fluctúa 0,7-1,7× sin señal) | 2-7 trozos por baliza, p. ej. 0,607 s en 6 ráfagas | cierre tras 64 µs seguidos bajo 1,2× |
| Correlador de Barker con 1 muestra por chip | −2,6 dB de Es/N0: con solo el detector nuevo, 22 PLCP válidos y **22 con FCS malo, 0 buenos** | filtro adaptado al chip (RC α=0,5, 6 muestras) |
| Reabrir con la cola de la ráfaga anterior (ventana de 64 µs) | ráfaga de 5 ms de ruido pegada a la trama | 64 µs de bloqueo tras cerrar |

Con 20 dB de ganancia (194 318 ráfagas en 30 s) no hay captura para reproducirlo; el ruido blanco
cuantizado (0,7, 1,5 y 8 LSB, continua de 2 LSB) da 0 ráfagas con el detector nuevo y los mínimos
de 1 µs ya no sesgan el suelo. Si vuelve a ocurrir hará falta la captura.

**Resultado sobre esta captura** (`build/d4b/run`, 3,94 s):

| | antes | después |
|---|---|---|
| ráfagas (≥ 24 µs) | 27 | 74 |
| cabeceras PLCP DSSS válidas | 0 | 46 |
| tramas con FCS correcto (BSSID) | **0** (1 red) | **43** (1 red; 38,5 balizas esperadas + respuestas) |
| tramas con FCS malo | 0 | 1 |
| OFDM | 0 | 0 (no hay) |

Sobre ruido real de la captura con balizas sintéticas inyectadas (18 por punto): DSSS 18/18 hasta
−3 dB de SNR (límite de energía −5 dB); OFDM 6 Mb/s 18/18 a 8 dB y 8-15/18 a 5 dB; 24 Mb/s desde 12 dB;
54 Mb/s desde ~20 dB. Coste en el banco (`tests/bench_decoders.c`, 10 s al 10 dB): 18 % de un núcleo (antes 11 %).
