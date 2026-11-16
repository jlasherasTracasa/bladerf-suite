# DMR (solo metadatos)

Decodificador `src/decoders/dmr.c` (+ `dmr.h`, `dmr_fec.{h,c}`, `pocsag_fsk.h`), modulador `src/tx/mod_dmr.c`, test
`tests/test_dmr.c`. Narrow, 48 000 Hz. Fuente: **ETSI TS 102 361-1 V2.5.1 (2017-10)**; las matrices,
polinomios, máscaras y el entrelazado se han tomado del texto de la norma (anexo B, 9.1, 9.2, 10.2), no de
memoria. **NO se decodifica, extrae ni reproduce voz** (AGENTS.md §0.12): las tramas de voz solo sirven
para saber que hay una llamada y para leer su LC.

## Teoría

* 4FSK, 4800 símb/s, h = 0,27: dibit 01 → +3 (+1944 Hz), 00 → +1 (+648), 10 → −1, 11 → −3 (tabla 10.3).
  Filtro de TX RRC con roll-off 0,2 (§10.2.2.2).
* TDMA de 2 ranuras, trama de 30 ms = CACH (24 bits, solo en el canal de bajada) + ráfaga de 264 bits.
  Ráfaga de datos: info(98) | tipo de ranura(10) | SYNC(48) | tipo de ranura(10) | info(98). Ráfaga de voz:
  voz(108) | SYNC(48) o EMB(8)+incrustado(32)+EMB(8) | voz(108).
* Sincronías (tabla 9.2, 8 de 9 usadas; la de canal inverso RC no): BS/MS/directo-S1/directo-S2 × voz/datos.
  Solo contienen símbolos ±3; voz y datos son complementarios.
* CACH (fig. B.9): bits temporales 0,4,8,12,14,18,22 = AT, TC, LCSS(1), LCSS(0), H2, H1, H0; el resto, los 17
  bits de carga (LC corto, no decodificado). TACT = Hamming(7,4,3); TC 0 → ranura 1, 1 → ranura 2.
* Tipo de ranura: CC(4) + tipo(4) + paridad Golay(20,8) (en realidad d = 8, ver abajo).
* BPTC(196,96) (B.1.1): matriz 13×15 (9 filas de datos con Hamming(15,11,3) y 4 de paridad de columna
  Hamming(13,9,3)), R(3) + 195 bits; bit de índice *i* de la matriz va a la posición temporal (i·181) mod 196.
* LC de cabecera/terminador: 96 bits = LC(9 octetos) + RS(12,9) sobre GF(2⁸) (0x11D, raíces α¹..α³, g = x³+0E x²+38 x+40)
  con la paridad XOR 0x969696 (cabecera) o 0x999999 (terminador). LC: PF, FLCO(6), FID, y para FLCO 0 (grupo)
  y 3 (privada): opciones de servicio (bit 7 emergencia, bit 6 privacidad), destino(24), origen(24).
* CSBK: 96 bits = 80 de señalización + CRC-CCITT (x¹⁶+x¹²+x⁵+1, registro inicial 0, resultado invertido,
  B.3.8) XOR 0xA5A5. Cabecera de datos: igual con máscara 0xCCCC.
* LC incrustado (B.2.1): 4 fragmentos de 32 bits en las tramas B..E del supertrama (LCSS 01, 11, 11, 10),
  matriz 8×16: 7 filas Hamming(16,11,4) con 72 bits de LC + CS de 5 bits (suma de los 9 octetos mod 31) y
  una fila de paridad por columna; transmisión por columnas. EMB = QR(16,7,6): CC(4), PI(1), LCSS(2) + 9.

## Verificación de las matrices

Transcritas de las tablas B.11-B.17 y comprobadas de forma independiente contra los polinomios que la norma da
para cada código (`g = 0xC75` Golay acortado + paridad global; `0x139` QR; `0x13` Hamming 15/13/16; `0xB`
Hamming 7): todas las filas de P coinciden con x^k mod g. El test comprueba además las filas de G de RS(12,9)
(tabla B.18), las distancias mínimas (Golay(20,8) = 8 —la norma lo llama (20,8,7), pero con la paridad global
es d = 8—, QR 6, Hamming 3/3/3/4) y CRC-CCITT = ~CRC-16/XMODEM (check 0x31C3 de reveng). **No** se han usado
`hamming_init`/`linblock_init_qr16_7` de A3.

## Cadena DSP

`IQ → FIR de canal (corte 8,5 kHz) → discriminador FM (±1944 Hz = ±1) → RRC β = 0,2, span 8 → reloj de Gardner
(sps = 10, kp = 0,02) → dibits (umbrales 0 y ±2/3 de la amplitud externa) → ventana de 24 símbolos vs. 8 patrones
(≤ 5 bits)`. El centro/amplitud salen de seguidores de pico (absorben el CFO).

**Adquisición de reloj dirigida por la sincronía**: una transmisión nueva trae solo 54 símbolos antes de su
sincronía y el lazo de Gardner puede tardar cientos desde una mala fase. Mientras no ha habido sincronía
reciente se mantiene, por cada una de las 10 fases de muestreo, un registro con los signos de los últimos
24 símbolos y se compara en O(1) con las máscaras de signo de las sincronías; en una coincidencia (≤ 2) se
toma el centro de la meseta, se reconstruyen los símbolos con esa fase, se confirma con los 48 bits y se
reajusta el reloj y el anillo de símbolos (así se recupera incluso la primera ráfaga). Con ella, 20 de 20 semillas de
ruido a 10 dB enganchan la primera ráfaga (sin ella, 2 de 16 tardaban ≥ 9 tramas).

Al llegar los 54 símbolos posteriores a la sincronía se procesa la ráfaga (CACH → ranura; tipo de ranura;
BPTC; RS/CRC). Una sincronía de voz programa las 5 tramas siguientes de la misma ranura (+288 símbolos cada
una) para leer EMB y LC incrustado (llegada tarde a una llamada).

## Eventos

* `voice_header`, `terminator`, `embedded_lc`: `{"burst","slot","cc","pf","flco","fid","call":"group|private",
  "dst","src","svc","emergency","privacy","corrected","text"}`; para FLCO distintos de 0 y 3, `"data":"hex"`.
* `csbk`: `{"burst":"csbk","slot","cc","lb","pf","csbko","fid","data":"8 octetos hex","corrected","text"}`.
* `data_header`: `{"burst":"data_header","slot","cc","dpf","dpf_name","sap","call","dst","src","corrected","text"}`.

`slot` = 1/2 (de TACT en BS, de la sincronía en modo directo), 0 = desconocida (MS sin CACH). Solo se emite
tras pasar FEC y RS/CRC; las repeticiones idénticas (< 3 s) se descartan; la cabecera repetida de una llamada
ya conocida y el LC incrustado de una llamada ya identificada no generan evento.

## state_json

`{"bursts","syncs","bad_slot","lc_ok","lc_bad","csbk","data_headers","emb","cc","calls":[{"slot","cc","via":
"header|embedded|voice","age","dur","call","dst","src","svc","emergency"}],"recent":[{…}]}`.
`calls` = llamadas activas por ranura (sin actividad > 3 s → pasan a `recent`; una llamada vista solo por
sincronías de voz tiene `via:"voice"` y sin IDs); `recent` = últimas 24 llamadas con IDs, caducidad 600 s.

## Modulador

Flujo continuo de BS con las dos ranuras: reposo, [CSBK, cabecera de datos], cabecera de voz LC, N supertramas
(carga "de voz" = bits pseudoaleatorios fijos, NO audio), terminador, reposo. Parámetros:
`{"cc":1,"csbk":true,"data_header":true,"calls":[{"slot":1,"group":true,"dst":91,"src":2001234,"svc":0,"superframes":2,"header":true}, …]}`
(`header:false` simula la llegada tarde). `air_ok = false`.

## Limitaciones / no verificado

* **Sin capturas reales**: el test es sintético; la concordancia sobre el aire con equipos reales no está
  comprobada (sí lo están FEC, máscaras, entrelazado y sincronías frente al texto de la norma).
* Polaridad: una señal invertida se ve como "datos" donde era "voz" (los patrones son complementarios); no se corrige.
* No se decodifican: LC corto del CACH, ráfagas de canal inverso (RC), PI header, MBC, datos 1/2, 3/4 y 1
  (solo se cuentan por tipo), CSBKO concretos (la semántica está en ETSI TS 102 361-2, que no tengo), alias
  de locutor/GPS (FLCO 4-8), FID de fabricante (solo se informan). El nombre de FLCO 0 = grupo y 3 = privada y
  el diseño de las opciones de servicio vienen de TS 102 361-2 (no consultada).
* Hasta ≈ 1 % de las ráfagas se pierden por ruido a 10 dB; un solo fallo de CRC/RS descarta la ráfaga.
* Coste de CPU ≈ 0,006 s por s de señal.

## Intento de validación (V2, 2026-11-15) — sin validar

sigidwiki.com `DMR.zip` (`SDRSharp_20160101_231914Z_12kHz_IQ.wav`, cabecera 48 kS/s, 34 s): con tasa 48 kS/s y con la hipótesis de 12 kS/s
(×4) el decodificador ve 1 sincronía y 0 ráfagas; no hay evidencia de que la señal sea DMR estándar (espectro de ≈ 15 kHz con una
envolvente de potencia periódica). No hay decodificador DMR instalado para contrastar (`multimon-ng` no lo incluye). **Sin validar.**
