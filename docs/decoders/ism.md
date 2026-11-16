# ISM genérico (OOK/FSK, 433/868 MHz) — `ism`

Decodificador estrecho (N) a 250 kHz complejos, canal de 200 kHz. Detecta ráfagas
OOK y FSK, extrae el tren de pulsos, lo pasa por protocolos conocidos con
validación de suma/CRC y, si ninguno encaja, por un analizador automático de
modulación estilo Universal Radio Hacker (Pohl & Noack, WOOT 2018).

Ficheros: `src/decoders/ism.c` (cadena DSP, eventos, tabla), `ism_protocols.{c,h}`
(protocolos, analizador, codificadores), `src/tx/mod_ism.c`, `tests/test_ism.c`.

## Teoría y cadena DSP

1. **Envolvente**: raíz de la potencia media de 8 muestras (32 µs).
2. **Umbral adaptativo**: se siguen el suelo de ruido `nf` (media lenta de lo que
   queda bajo el umbral, τ ≈ 8 ms) y el pico `pk` (ataque rápido, caída τ ≈ 200 ms).
   Hay señal si `pk > 2,5·nf`; umbral de subida `nf + 0,55(pk − nf)`, de bajada
   `nf + 0,40(pk − nf)`. No depende de la ganancia.
3. **Ráfaga**: empieza al superar el umbral y acaba tras 12 ms sin superarlo (o a
   1,5 s). Se guarda el IQ y se construye el tren OOK (marca = envolvente alta).
   Los saltos < 40 µs se funden con los vecinos.
4. **OOK o FSK**: si la envolvente no bajó nunca del umbral (portadora continua) la
   ráfaga es FSK y se demodula **al final**, con toda la ráfaga a la vista:
   desvío de portadora = arg Σ x[n]·x*[n−1]; paso a banda base; FIR paso bajo a
   ±55 kHz; discriminador de retardo 3 (alcance ±41 kHz, el ruido "de clic" del FM
   pesa menos); mediana de 5; umbral = media. Resultado: tren de marcas/espacios.
5. **Protocolos** (`ism_decode_train`) o **analizador genérico** (`ism_analyze`).

El analizador agrupa (1-D, ±25 %) las duraciones de marcas y espacios; separa
filas por el grupo de espacios ≥ 3× mayor que los principales; infiere:
`PWM` (2 grupos de marca), `PPM` (1 marca, 2 espacios), `Manchester` (duraciones 1×/2×
del tiempo de bit) o `NRZ`; extrae los bits de la fila más repetida y los da en
hexadecimal. Solo emite si la fila se repite ≥ 2 veces o tiene ≥ 32 bits.

## Protocolos (10 de sensores + trama de prueba)

| Modelo (`model`) | Línea | Validación |
|---|---|---|
| Oregon-THGR122N (0x1D20), Oregon-THN132N (0xEC40) | OOK Manchester 1024 Hz, preámbulo 16 unos | suma de nibbles |
| Oregon-THGR810 (0xF824) | idem, preámbulo 24 unos | suma de nibbles |
| LaCrosse-TX141B | OOK PWM 208/417 µs, 4 sincronías | CRC-8 poly 0x31 |
| Acurite-592TXR | OOK PWM 220/400 µs | suma 8 bits + paridad por octeto |
| Nexus-TH | OOK PPM 1000/2000 µs, 36 bits | 2 filas idénticas + nibble 0xF |
| FineOffset-WH1080 | OOK PWM 400/800 µs, preámbulo 0xFF | CRC-8 poly 0x31 |
| Ambient-F007TH | OOK Manchester 1000 bit/s, sync FFD45 | resumen LFSR 0x98/0x3E ^ 0x64 |
| TPMS-FSK (solo lectura) | FSK Manchester, sync 0x2DD4 | suma 8 bits |
| TPMS-OOK (solo lectura) | OOK Manchester, sync 0xA5 | CRC-8 poly 0x07 |
| BladeRFLab-test | OOK PWM 300/600 µs (trama propia) | CRC-16/CCITT-FALSE |

Los TPMS y cualquier mando/llavero/alarma **no tienen codificador** en el
modulador; el test los fabrica localmente.

## Formato de eventos

Sensor: `{"kind":"sensor","model","id","channel"?,"modulation":"OOK|FSK",
"temperature_c"?,"humidity"?,"pressure_kpa"?,"wind_avg_ms"?,"wind_gust_ms"?,
"wind_dir_deg"?,"rain_mm"?,"battery_low"?,"hex","text"}`.
Genérico: `{"kind":"generic","modulation","short_us","long_us","gap_us","bit_us",
"bits","repeats","rows","hex","text"}`. Un mensaje idéntico del mismo dispositivo
en < 1 s no se vuelve a emitir.

## `state_json`

`{"bursts","decoded","generic","devices":[{"model","id","channel",
"temperature_c","humidity","pressure_kpa","battery_low","count","age_s"}]}`.
Caducidad 900 s; máx. 64 dispositivos.

## Modulador (`mod_ism.c`, `air_ok = true`)

Solo sensores meteorológicos de la tabla y `BladeRFLab-test`; cualquier otro
`model` devuelve error. OOK a 250 kHz, amplitud 0,8. Parámetros: `model,id,
channel,temp,hum,batt_low,wind,gust,rain,dir,repeat`. Sin parámetros emite un
paquete de cada modelo.

## Referencias

- Pohl & Noack, "Universal Radio Hacker: A Suite for Analyzing and Attacking
  Stateless Wireless Systems", WOOT 2018.
- Reveng (Greg Cook), catálogo de CRC (CRC-8 poly 0x07: check 0xF4).
- Oregon Scientific, "Sensor protocols" (descripciones públicas v2.1/v3).

## Limitaciones y qué está verificado

- (Superado en parte: ver "Validación con señal real" más abajo.) Antes **no se había accedido a grabaciones** de
  ningún sensor. Los formatos se han implementado a partir de la memoria de
  descripciones públicas y **los desplazamientos de campo, tiempos exactos,
  polaridad y orden de nibbles pueden diferir de los sensores reales**. Los tests
  prueban la coherencia modulador↔decodificador (con AWGN 10 dB, CFO ±50 kHz,
  ±100 ppm), no la fidelidad con el hardware. Lo único contrastado con una fuente
  es el valor de comprobación del CRC-8 poly 0x07 (0xF4).
- Los TPMS son variantes genéricas (no corresponden a un fabricante concreto).
- Prologue, Hideki, Bresser 5/6/7-en-1 y LaCrosse TX29-IT+ no están implementados
  (sin fuente fiable de la que partir); el analizador genérico los cubre como
  trama repetida.
- La detección exige ráfagas con relación `pico/ruido` > 2,5 (≈ 8 dB); el FSK a
  SNR de banda completa de 10 dB con CFO de ±50 kHz decodifica ≈ 93 % de las
  tramas aisladas (los sensores repiten, así que en la práctica se recupera).
- Coste medido: ≈ 0,002 s de CPU por segundo de señal.

## Validación con señal real (V2, 2026-11-15)

Grabaciones: repositorio público `merbanan/rtl_433_tests` (RTL-SDR, cu8 a 250 kS/s, 433,92 MHz; solo como datos,
licencia GPL-2.0 del repositorio) y el decodificador de referencia instalado `/usr/bin/rtl_433` (solo se ejecuta,
no se lee su código). Fragmentos recortados en `tests/data/ism_*_rtl433.cu8` (< 200 kB) con la salida esperada
publicada en el `.json` de cada captura. Además, la salida de `--gen ism` (cu8, ×0,6, ruido) se pasó por `rtl_433 -r`.

| Modelo | Real (id, temp, hum…) | Antes | Después | Qué estaba mal |
|---|---|---|---|---|
| Acurite-592TXR (`Acurite_592TXR/…-003`, `two-sensors`) | id 12053, 26,7 °C, 74 % ; id 9884 canal B, batería baja | ok | ok | nada (batería: bit 6 de b2 = OK) |
| Nexus-TH (`nexus/01`, `/03`) | id 181 ch 2 19,0 °C 71 % ; id 201 ch 3 29,4 °C 30 % | ok | ok | nada |
| Ambient F007TH (`ambient_weather/02`) | id 169 ch 1, −4,6 °F (= −20,3 °C), 19 % | ok | ok | nada |
| Fine Offset WH1080 (`fineoffset_wh1080/01`, `/03`) | id 5, 7,4 °C 82 %, dir 315°, lluvia 183,9 mm | trama genérica | ok | **temporización** (marca corta ≈ 500 µs = 1, larga ≈ 1500 µs = 0, espacio 1000 µs; antes 400/800/400), **polaridad** (corta = 1), y la **última marca de la fila** (sin espacio propio) se perdía: 87 bits en vez de 88; lluvia = **12 bits** (el nibble alto de b6 es de estado) |
| LaCrosse TX141-B v2/v3 (`lacrosse/tx141-bv2`, `-bv3`) | id 117 20,1 °C ; id 33 20,3 °C | sin decodificar | ok | sincronía = 4 marcas de ≈ 960 µs; bits: marca ≈ 236 µs = 0, ≈ 480 µs = 1; filas de 37 (v2) y 33 (v3) bits, no 40; **no hay humedad**; batería OK = bit 7 de b1 (invertida); el resumen final **no coincide con ningún CRC-8 ni LFSR probado** (2 grabaciones): se exige que dos filas sean idénticas. Modulador con resto fijo copiado de la grabación |
| Oregon THN132N / THGR122N (`oregon_scientific/THN132N`, `thgr122n/01,02`) | id 231 18,0 °C ; id 248 18,8 °C 54 % ; id 187 23,0 °C 39 % | trama genérica | ok | **v2.1 envía cada bit seguido de su complemento** (el preámbulo de 16 unos lógicos es 1010… a 976 µs) y lleva 2 nibbles de cola; THGR122N tiene **17 nibbles** (nib 14 = 0, suma en 15-16), no 16 |

`rtl_433` decodifica ahora todos los modelos del modulador (`--gen ism`): Acurite-Tower, Ambientweather-F007TH,
Fineoffset-WHx080, LaCrosse-TX141Bv2, Nexus-TH, Oregon-THN132N, Oregon-THGR122N, Oregon-THGR810 (v3, Manchester
simple). Siguen **sin validar**: TPMS (variantes genéricas), BladeRFLab-test, resumen del TX141 y la batería del
WH1080 (bit sin contrastar).
