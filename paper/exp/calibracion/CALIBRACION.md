# Calibración por inyección (P3)

Mide, con verdad de terreno, qué probabilidad tiene el decodificador de etiquetar
una señal en función de su **SNR local**, y con qué error de posición, frecuencia y
SNR. No toca `src/`. Sustituye al piloto (`../piloto/PILOTO.md`), que usaba SNR
nominal.

## Reproducir

`paper/exp/calibracion/medir.sh` (compila en `build/p3`, ≤ 4 procesos en paralelo,
`nice 10`). Es **reanudable**: una serie con `datos/<proto>_<fondo>_<cond>_s<semilla>.csv`
y `.json` se salta (bórrense para repetirla). `SOLO=series|ocup|fp|analisis` ejecuta
una fase. Las semillas son 1, 2 y 3 (la aleatoriedad de cada serie sale de
`[semilla, crc32(proto), crc32(fondo), crc32(condición)]`). Las pasadas IQ son
temporales (`build/p3/work`). Total medido: ≈ 130 series, ≈ 70 min con 4 procesos.
Salidas: `resultados.json`, `curvas.csv`, `resumen.csv`, `curvas.png`,
`curvas_emparejados.png`, `ocupacion.png`, `errores.png`, `datos/` (por copia).

## Método

1. Cada pasada es un SigMF (ci16) = fondo **sin bucle** + k copias generadas por
   `bladerf-lab --gen`. En ais, aprs, pocsag, lora y adsb cada copia lleva un
   identificador único en el mensaje, así que el emparejado con el evento es exacto.
   En acars, rs41, flex, dsc e ism no se puede: se empareja **por tiempo** (la marca
   `sample` del evento dentro de la ventana de la copia; en ism además por el valor de
   temperatura). Están marcados «por tiempo» y su error de posición no es fiable.
2. **SNR local por copia**: SNR = P_señal(B) / P_fondo(B), con P_fondo medida con Welch
   en la ventana de tiempo y en la banda B de *esa* copia (`inyectar.local_noise`).
   Un fondo no estacionario ya no desplaza la curva por nivel medio: lo que se
   mide es lo que el decodificador ve en ese instante. Niveles −10…+25 dB, 1 dB
   (2 dB en ocupación y en los emparejados por tiempo), 34 copias por punto y semilla
   (10–20 en los de paso 2): 1 224 copias/semilla/serie principal, 3 672 con 3 semillas.
   Orden de los niveles barajado; desvío de frecuencia ±1 kHz (±5 kHz LoRa, 0 ADS-B).
3. Fondos: **blanco** (σ = 60 LSB Q11 por muestra compleja) y **real** de
   `tests/data/real` (sin bucle, concatenado; 3 s de calentamiento sin copias al principio
   de cada pasada, con la grabación en palíndromo): `real_rf` (ruido y ráfagas ISM de un
   RTL, remuestreado a 48 kS/s) y `real_rec` (grabaciones de otros protocolos a 48 kS/s,
   excluida la propia) para los de 48 kS/s; lte/gsm/wifi remuestreados a 0,5 y 2 MS/s
   para lora (sin la captura propia de LoRa) y adsb.
4. Reproducción con `--replay-fast` (sin bucle, sin descartar bloques). Las copias a
   ±1 s de un `gap` del reproductor (bloque perdido) se excluyen del cálculo (columna
   `excluidas_en_hueco` de `resultados.json`: 0–33 de 540–3 672).
5. Ajuste por máxima verosimilitud binomial de `p = g + (c−g)·sigmoide((SNR−s50p)/w)`
   (suelo g, techo c). **SNR50/SNR90** = SNR donde la curva ajustada vale 0,5/0,9
   en términos absolutos (NA si el techo no llega). IC 95 % por bootstrap de copias
   (200 réplicas); en las barras de las gráficas, Wilson 95 % por punto.
6. Errores (con SNR ≥ SNR90, o ≥ 15 dB si no hay curva): posición = `sample` del evento −
   inicio de la ráfaga activa inyectada, en símbolos; SNR = `snr_db` del evento − SNR
   local inyectada (regresión lineal); frecuencia = `cfo` del evento − desvío
   inyectado (solo LoRa lo exporta).
7. Ocupación: serie con un interferente del mismo protocolo (ID propio, desvío y
   posición aleatorios, solapado en tiempo): `adj` canal adyacente (25 kHz; 250 kHz en
   LoRa) +10 dB sobre el objetivo; `co0` cocanal a la misma potencia; `co-6` cocanal 6 dB
   por debajo. `ctl` = control sin interferente a la misma tasa. ADS-B solo cocanal.
8. Falsos positivos: ruido blanco sin copias (1 h en los de 48 kS/s, 15 min ism y lora,
   10 min adsb) y cada grabación real sin inyectar.

## Resultados

SNR local en dB; [IC 95 %]. Techo = P con SNR ≥ 20 dB. «real» = `real_rf`
(ais, aprs, pocsag) o lte/gsm/wifi (lora, adsb).

| Proto | Fondo | SNR50 | SNR90 | Techo | Suelo (≤ −5 dB) |
|---|---|---|---|---|---|
| ais | blanco | 10,2 [10,1; 10,4] | 11,8 [11,7; 12,1] | 1,00 | 0,00 |
| ais | real | 12,2 [12,0; 12,5] | 16,3 [15,7; 18,0] | 0,93 | 0,00 |
| adsb | blanco | 6,7 [6,6; 6,8] | 8,2 [8,0; 8,4] | 1,00 | 0,00 |
| adsb | real | 3,9 [3,9; 4,2] | 8,0 [7,9; 8,5] | 1,00 | 0,015 |
| aprs | blanco | 9,3 [9,2; 9,4] | 10,2 [10,0; 10,4] | 1,00 | 0,00 |
| aprs | real | 13,9 [13,6; 14,2] | 18,8 [18,4; 19,2] | 0,97 | 0,00 |
| lora | blanco | −4,8 [−4,9; −4,6] | −3,2 [−3,3; −3,0] | 1,00 | 0,11 (*) |
| lora | real | −5,6 [−5,8; −5,4] | −3,7 [−4,0; −3,4] | 1,00 | 0,23 (*) |
| pocsag | blanco | 0,9 [0,8; 1,0] | 1,8 [1,6; 1,9] | 1,00 | 0,00 |
| pocsag | real_rf | −1,4 [−1,6; −1,1] | 3,0 [2,6; 3,3] | 1,00 | 0,06 |

(*) LoRa SF7 con el suelo ya en −5 dB: son puntos de −5 dB (justo en el SNR50), no un
suelo de falsa detección.

Con grabaciones de otros protocolos como fondo (`real_rec`): ais SNR50 14,1 con techo 0,60;
aprs 21,8 con techo 0,68; pocsag −13,7 con «suelo» 0,23. **No son curvas de
sensibilidad** (ver Limitaciones).

Emparejados por tiempo (marcados; SNR50/SNR90 en blanco → real_rf o real_rec entre
paréntesis): acars 13,4/14,2 (real_rf 11,7/14,4; real_rec techo 0,89, SNR50 26);
dsc 6,0/6,4 (real_rec 18,2); flex 0,6/1,7 (real_rec −1,8/5,5, suelo 0,12);
rs41 5,0/5,3 (real_rec 3,3/8,1); ism 9,2/10,0 (real: techo 0,57, SNR50 22). El IC
de rs41 y dsc es optimista: la curva es más estrecha que la rejilla de 2 dB.

### Error de posición, SNR y frecuencia (ruido blanco, SNR ≥ SNR90)

| Proto | Sesgo de `sample` (símbolos) | Dispersión (p95 de \|e − sesgo\|, símbolos) | Sesgo / σ residual de `snr_db` (dB) |
|---|---|---|---|
| ais | 24,4 (2,55 ms) | 0,14 | −0,5 / 0,18 |
| aprs | 120,5 (100 ms) | 0,02 | −1,1 / 1,4 |
| pocsag | 121,1 (50 ms) | 0,00 | −0,7 / 7,0 |
| lora | 0,04 | 0,00 | −3,7 / 0,16 |
| adsb | 0 | 0 | +1,7 / 0,22 |

El `sample` del evento no marca el inicio de la ráfaga, sino un punto fijo del
protocolo: el sesgo es constante (se resta una vez) y la dispersión
real es < 0,2 símbolos en todos. Es la propiedad que necesita el etiquetado. La
pendiente de `snr_db` frente a la SNR inyectada es 1,0 ± 0,03 (ais, aprs, lora,
adsb): el estimador del decodificador tiene un desplazamiento constante respecto a
nuestra definición (otra banda de ruido), no distorsión; pocsag y aprs saturan a SNR
alta (σ residual grande). Con fondo real la σ residual sube a 3–7 dB.
Frecuencia (solo LoRa exporta `cfo`): sesgo ≈ 0 Hz, σ = 21 Hz, p95 = 47 Hz
con desvío inyectado de ±5 kHz.

### Falsos positivos

* Ruido blanco sin copias: **0** eventos en 1 h por protocolo (ais, aprs, pocsag, acars,
  rs41, flex, dsc), 15 min (ism, lora) y 10 min (adsb): ≤ 3 /h (≤ 12 /h ism y lora,
  ≤ 18 /h adsb) al 95 %.
* Grabaciones reales sin inyectar (2–17 s por protocolo): 0 eventos salvo ism (7 en 2 s),
  que son transmisiones reales de sensores (sus propias grabaciones). Con tan pocos
  segundos la cota superior es > 600 /h: **no hay medida útil de falsos positivos en
  fondo real**, falta una grabación larga sin señal.
* Eventos ajenos con copias presentes (superan el CRC/BCH y no son una copia): 0 en
  todos salvo ism con fondo real (135, decodificaciones legítimas del fondo).
  El piloto contó 114/245 en POCSAG; ya no aparecen.

### Ocupación (ruido blanco, copias solapadas)

| Proto | adyacente +10 dB: ΔSNR50 | cocanal −6 dB | cocanal 0 dB |
|---|---|---|---|
| ais | 0,0 | no llega al 50 % (techo 0,40) | 0 |
| aprs | −0,0 | +4,4 dB, techo 1,0 | 0 |
| pocsag | −0,2 | +2,3 dB, techo 1,0 | 0 |
| lora | −0,4 | ±0 (techo 0,76) | techo 0,40 |
| adsb | — | +0,6 (techo 0,98) | ±0 (techo 0,90) |

Δ respecto al control `ctl` (sin interferente, misma tasa). Un canal adyacente a
+10 dB no afecta. Con solape cocanal, los de modulación angular (ais, aprs, pocsag)
se pierden si el interferente tiene la misma potencia (captura al más fuerte: ni
siquiera el interferente se decodifica de forma fiable); LoRa y ADS-B toleran mejor
la colisión (SF7 casi ortogonal al propio con desfase; ADS-B por la mezcla de pulsos).
Estas curvas dan la «tasa de pérdida por ocupación» para el artículo.

## Comparación con el piloto: ¿desaparece el suelo del 17–22 %?

**Sí.** Con SNR local, el suelo de ADS-B pasa de 0,22 a **0,01** (P con SNR ≤ −5 dB:
1,5 %, IC95 % [0,8; 2,8], n = 612) y el de AIS de 0,17 a **0,00**. El suelo era un artefacto de
la SNR nominal: en un fondo no estacionario parte de las copias cae en huecos de
potencia. Cambios del resto de curvas (piloto → ahora, SNR50): ais real 15,7 → 12,2;
adsb real 7,7 → 3,9 (SNR90 11,7 → 8,0); aprs real 16,4 → 13,9; pocsag real −4,3 →
−1,4; lora real −4,0 → −5,6; blanco: ais 10,0 → 10,2, aprs 9,0 → 9,3, pocsag 0,5 → 0,9,
lora −5,0 → −4,8, adsb 6,6 → 6,7. El blanco apenas se mueve (el método es coherente);
el fondo real ya no empeora la sensibilidad de forma sistemática (ADS-B y LoRa
mejoran 1–3 dB; no se ha investigado por qué, una hipótesis es que el fondo lte/gsm
remuestreado es más llano en la banda de la señal que un blanco de igual potencia). El techo de POCSAG del piloto (0,94) es ahora 1,00: lo corrigió el commit del
decodificador (`1b89e2d`), no este método. En su lugar, con fondos reales aparece
otra limitación: un **techo < 1** en ais (0,93), aprs (0,97) y, con grabaciones, 0,5–0,7:
la SNR local es la media en la ventana de la copia, y una ráfaga ISM o una portadora
que cubre solo parte de la copia da menos SNR instantánea en esa parte.

## Limitaciones

* La SNR local mide **potencia**, no SINR efectiva. Con `real_rec` el fondo contiene
  portadoras y tonos intensos (potencia local 10³ veces la del blanco) que el
  decodificador elimina (continua, filtros, limitador FM): pocsag se decodifica con
  −10 dB (P = 0,85 a ≤ −5 dB) y ais falla con las grabaciones de APRS y ACARS de fondo (P con SNR ≥ 20 dB:
  0,18 y 0,28) y aprs con las de ACARS, flex y pocsag (0,29–0,56). El eje no es comparable con el blanco. Para el
  artículo usar blanco y `real_rf`.
* Los fondos reales son cortos (0,03–3 s; ISM 2 s a 48 kS/s tras remuestrear), sin
  bucle pero con palíndromo en el calentamiento, y de pocos tipos: no hay grabación
  larga sin señal de VHF/UHF, ADS-B ni LoRa. Los falsos positivos en real son una cota
  débil.
* ADS-B: una trama por copia (P por trama, no por avión); sin CPR ni filtrado por
  ICAO conocido.
* Emparejados por tiempo: la ventana de coincidencia (−0,05 s … +0,5 s tras la copia)
  hace que el error de posición sea de ±1 000–3 000 símbolos y no se interprete.
  Un evento ajeno dentro de la ventana contaría como acierto.
* Solo LoRa exporta frecuencia: para el resto no hay error de frecuencia que medir.
* SNR90 con curva más estrecha que la rejilla (rs41, dsc): IC subestimado.
* Los ficheros se reproducen con `--replay-fast` sin la carga de la interfaz ni de
  otros decodificadores: la P es la del decodificador aislado, no del sistema.
* En ocupación, el interferente es del mismo protocolo y solo hay solape temporal
  completo; no cubre ráfagas parciales (±0,5 L de desfase aleatorio sí).
