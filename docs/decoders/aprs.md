# APRS / AX.25 (AFSK 1200)

Fuentes: APRS Protocol Reference 1.0.1 (caps. 5-11, 15-16), AX.25 v2.2, Bell 202.
Vectores: ejemplos de APRS101 tal como los recoge el crate `aprs-decode`
(https://docs.rs/crate/aprs-decode/0.1.1/source/tests/integration.rs).

## Teoría

FM de banda estrecha (144,800 MHz en Europa). Audio AFSK Bell 202: 1200 Hz = 1,
2200 Hz = 0, 1200 Bd, fase continua. NRZI (0 = cambio de tono) + HDLC + FCS
CRC-16/X-25 (bytes LSB primero). Trama AX.25 UI: destino, origen, hasta 8
repetidores (7 bytes: carácter<<1, SSID en el 7º, bit 0 = fin), control 0x03,
PID 0xF0, información APRS.

## Cadena DSP (`aprs.c`, `afsk.c`)

1. `fmdisc` → audio; filtro paso alto de un polo (~70 Hz) que quita el CFO.
2. FIR paso bajo 3,3 kHz.
3. Tres demoduladores no coherentes por correladores (ventana de un símbolo a
   1200 y 2200 Hz, energía `e1 − g·e0`) con g = 0,6 / 1 / 1,7 para tolerar el
   preénfasis o de-énfasis (idea de Dire Wolf). Cada uno con su PLL de reloj de un
   símbolo (inercia 0,75, tolera ±1000 ppm), NRZI y HDLC.
4. Gana quien valida el CRC; duplicados en < 1 s se descartan por huella FNV.
5. Se aceptan solo tramas UI/PID F0 con direcciones válidas (A-Z, 0-9, espacio).

## Formatos APRS decodificados

`!` `=` `/` `@` posición sin comprimir (`DDMM.mmN/DDDMM.mmE`, ambigüedad con
espacios) y comprimida (base 91: lat = 90 − v/380926, lon = −180 + v/190463,
rumbo = c·4°, velocidad = 1,08^s − 1 kn, altitud 1,002^cs ft), rumbo/velocidad
`ccc/sss` y `/A=` altitud, Mic-E (latitud y bits de mensaje en el destino,
longitud/velocidad/rumbo en la información, altitud `xxx}`), `>` estado, `:`
mensajes (destinatario, texto, id `{`), `;` objetos, `)` ítems, meteorología (con
símbolo `_` o `_MMDDhhmm`: viento, ráfaga, temperatura °F, lluvia, humedad, presión).
No hay telemetría ni "user defined": se etiquetan `telemetry`/`other`.

## Eventos

`{"src","dst","path","kind","lat","lon","symbol","speed_kn","course","alt_m",
"object","live","to","message","msg_id","status","mice_msg","temp_f",...,"comment",
"info","text"}` (los no imprimibles de `info` salen como `.`).

## state_json

`{"packets":N,"count":N,"stations":[{"call","kind","object","lat","lon","symbol",
"dist_km","speed","course","alt","comment","path","pkts","age"}]}` Caducidad 1 h,
máximo 1024.

## Modulador (`mod_aprs.c`)

`air_ok = true` (2 m radioaficionado); `tx.c` exige `--callsign` y lo inserta en
`params.callsign`. El modulador rechaza un indicativo vacío o inválido; si falta
usa `N0CALL` (solo útil en modo file). Secuencia por defecto: posición,
comprimida, Mic-E, estado, mensaje (~2,8 s < 5 s). `params.type` selecciona
`position|compressed|mic-e|status|message|object|weather`; `info` da una cadena cruda.
FM de ±3 kHz de desviación (sin verificar contra ningún equipo concreto).

## Pruebas (`tests/test_aprs.c`)

Paquetes publicados (posición, timestamp+rumbo/velocidad+altitud, comprimida
`!/ABCD#$%^- sT`, APRS101 `/5L!!<*e7>7P[`, meteorología, Mic-E `PPPPPP` +
`` `(_fn"Oj/ `` → 20 kn, 251°, símbolo `/j`, mensaje, objeto, estado); AX.25 ida y
vuelta; 5 paquetes con AWGN 10 dB, CFO ±4 kHz, ±100 ppm: 45/45; objeto y
meteorología a 14 dB; ruido solo (20 s): 0; coste ~0,007 s/s.

## Limitaciones

Sin corrección de bits dudosos de la FCS ni "fix-bits"; sin AX.25 con PID distinto;
sin telemetría/mensajes de usuario; la posición Mic-E con ambigüedad (`K`, `L`, `Z`) se
trata como 0; el 1200 Bd a 9600 (G3RUH) no está soportado.

## Validación con señales reales (V1, 2026-11-15)

"AFSK1200 Sound" de la Signal Identification Wiki (audio FM, BBS FBB ZL1AB>FBB) remodulado en FM
(±3 kHz): el decodificador emite las mismas tramas AX.25 UI que multimon-ng y Dire Wolf
(`ZL1AB>FBB`, info "541973 P    439 ZL1BAV …"). **Ninguna constante estaba mal** (HDLC, NRZI,
CRC, tonos, polaridad). Las tramas no son APRS de posición, así que el análisis de posición/clima
sigue sin contrastarse con tráfico real. El IQ "APRS IQ 145070 kHz" de la misma wiki no contiene
paquetes que multimon-ng decodifique tampoco. Test: `tests/data/aprs_afsk1200_phase.u8`.
