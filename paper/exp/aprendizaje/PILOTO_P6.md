# Piloto P6: aprender con etiquetas sesgadas por la detectabilidad

Pregunta: un detector entrenado con las etiquetas «observadas» del propio decodificador (que solo etiqueta
lo que decodifica, y por tanto pierde las señales débiles), ¿mejora si se corrige el sesgo con la propensión
e(SNR, protocolo) de la calibración por inyección (PLAN §5, contribución 3; `related_work.md` §9)?
Escala deliberadamente pequeña. No toca `src/`, `web/` ni el hardware.

**Resultado en una línea: con este montaje la corrección por propensión (nnPU ponderado por 1/e) NO supera
de forma clara a tratar lo no etiquetado como fondo; lo que sí ayuda mucho es (i) usar la clase `unknown`
y (ii) añadir datos sintéticos con verdad completa.**

## Reproducir

`paper/exp/aprendizaje/medir.sh` (≈ 25 min: 3 procesos de generación, después entrenamiento en GPU). Fases con
`SOLO=gen|learn|analisis`. Datos en `build/p6/` (fuera de git); binario `build/p6/bladerf-lab`
(`make BUILD=build/p6 CUDA=0`). Se usan Python 3.13, numpy 2.4, scikit-learn 1.8, PyTorch 2.10+cu128 (ya
instalados; sin venv ni paquetes nuevos). Salidas: `res_s{1,2,3}.json`, `resultados.json`, `tablas.md`,
`recall_snr.png`, `ap_proto.png`.

## Método

**Conjunto** (`gen.py`; por semilla 48 train, 20 test, 48 sintéticas, 20 calibración; 48 s a 48 kS/s):
* Fondo: el 60 % de las capturas usa fondo **real** (RS41 y 6 u 8 grabaciones ISM de un RTL de `tests/data/real`,
  recicladas con desplazamiento en frecuencia, conjugación/inversión temporal y ganancia aleatorios); el 40 %,
  ruido blanco con σ aleatoria. Solo se usan grabaciones de protocolos que NO se inyectan, para que la verdad
  no tenga positivos ocultos. Train y test usan ficheros de fondo distintos.
* Inyección (`calib.gen_copy` + `inyectar.local_noise`): 25 ráfagas por captura, 5 de cada protocolo
  (ais, aprs, pocsag, acars, flex), SNR local uniforme en [−5, 20] dB, CFO ±1 kHz, todas en el centro del canal.
* **Verdad** completa por construcción (protocolo, inicio, fin, SNR). **Observado**: `bladerf-lab --replay-fast
  --labels` con los cinco decodificadores: `verified` (evento con CRC), `unknown` (detector sin evento
  que lo explique), el resto `empty`.
* Unidad: ventana de 0,25 s (47 frames de 5,33 ms × 128 bins, dB, restando la mediana por bin de la captura),
  salto de media ventana en entrenamiento y sin solape en test. Ventana positiva de un protocolo si una ráfaga
  la solapa ≥ 50 % de min(duración de la ráfaga, ventana); la misma regla para verdad y para eventos verificados.
* Por semilla: 16 608 ventanas de entrenamiento, 3 540 de test; ≈ 8 580 positivos reales (ventana×protocolo) de
  los cuales solo ≈ 4 000 (47 %) están etiquetados `verified`; 46–59 % de las ventanas caen en una región `unknown`.

**Modelo**: CNN pequeña (3 bloques conv-BN-ReLU-pool, media en tiempo, FC 128, 5 salidas sigmoide), AdamW +
OneCycle, 12 épocas, lote 1 024, igual en todas las variantes. La semilla fija el conjunto y la inicialización.

**Variantes**
| | Entrenamiento |
|---|---|
| A | verified = 1, todo lo demás = 0 |
| A2 | como A, pero las ventanas `unknown` sin etiqueta se excluyen de la pérdida |
| B3 | PU con propensión: riesgo no sesgado de Horvitz-Thompson, `R = (1/N)[Σ_todas ℓ⁻ + Σ_etiquetadas (ℓ⁺−ℓ⁻)/e]` con corrección no negativa de nnPU (Kiryo et al. 2017; el marco de propensión es el de SAR-PU, Bekker et al. 2019/2020). e(SNR, protocolo) = curva de `calibracion/curvas.csv` (fondo real), SNR = la del host (`snr_db` del evento) |
| B3f2, B3f5 | B3 con suelo de e en 0,2 / 0,5 (peso máximo 5 / 2; sensibilidad a la varianza de 1/e) |
| Bc | e(SNR) ajustada en el propio entorno con las capturas «cal» (regresión logística), SNR del host corregida por su sesgo medido en «cal» |
| Bo | como Bc con la SNR verdadera (cota: sin error de SNR) |
| C | solo sintético: capturas con ruido blanco + inyección, verdad completa |
| E | A2 + las capturas sintéticas |
| D | verdad completa sobre las capturas reales (cota superior) |

ACARS no emite `snr_db`; para él (y solo para él) la SNR del evento se estima por exceso de potencia en la
banda central (grosera; el sesgo medio por protocolo se corrige con «cal»).

**Evaluación**: sobre la verdad completa del test; AP y AUC por protocolo (media de 5), recall por SNR verdadera,
precisión. Dos puntos de operación: FPR = 1 % (umbral fijado sobre los negativos del test, igual criterio para todas
las variantes) y umbral natural 0,5. Media ± desviación típica entre 3 semillas.

## Resultados (media de los 5 protocolos, 3 semillas)

| Variante | AP | AUC | prec@FPR1% | recall@FPR1% | recall@FPR1%, SNR<5 dB | prec@0,5 | recall@0,5 |
|---|---|---|---|---|---|---|---|
| A  no etiquetado = fondo | 0.693 ± 0.031 | 0.923 | 0.755 | 0.533 | 0.275 ± 0.022 | 0.663 | 0.269 |
| A2 A + excluir unknown | 0.702 ± 0.015 | 0.917 | 0.761 | 0.584 | 0.386 ± 0.021 | 0.789 | 0.518 |
| B3 PU-propensión (curva P3) | 0.673 ± 0.030 | 0.919 | 0.726 | 0.501 | 0.268 ± 0.014 | 0.817 | 0.381 |
| B3 suelo 0,2 | 0.708 ± 0.021 | 0.928 | 0.756 | 0.567 | 0.335 ± 0.004 | 0.836 | 0.370 |
| B3 suelo 0,5 | 0.724 ± 0.015 | 0.933 | 0.782 | 0.591 | 0.344 ± 0.009 | 0.856 | 0.337 |
| Bc curva propia | 0.647 ± 0.019 | 0.904 | 0.727 | 0.452 | 0.186 ± 0.022 | 0.755 | 0.373 |
| Bo SNR verdadera | 0.699 ± 0.035 | 0.924 | 0.743 | 0.548 | 0.311 ± 0.005 | 0.842 | 0.339 |
| C  solo sintético | 0.841 ± 0.007 | 0.967 | 0.816 | 0.773 | 0.671 ± 0.039 | 0.867 | 0.714 |
| E  A2 + sintético | 0.878 ± 0.006 | 0.982 | 0.829 | 0.823 | 0.722 ± 0.031 | 0.815 | 0.830 |
| D  verdad completa (cota) | 0.892 ± 0.027 | 0.983 | 0.835 | 0.850 | 0.760 ± 0.025 | 0.900 | 0.749 |

Tablas completas (recall por bin de SNR, por protocolo): `tablas.md`; figuras `recall_snr.png`, `ap_proto.png`.

## Lectura honesta

1. **¿B supera claramente a A en recall a SNR baja sin perder precisión? No.** A igual FPR, B3 (suelo 0,05) rinde
   como A (0,268 frente a 0,275 con SNR < 5 dB) y las versiones con pesos acotados ganan +6 a +7 puntos (0,335–0,344)
   con precisión igual o algo mejor, pero con ±0,02 entre semillas y sin curva de ajuste elegida a priori:
   indicio, no demostración. Con el pesado completo (1/e hasta 20) la varianza anula la ganancia. Que Bo (SNR
   exacta) no sea mejor que B3 indica que el cuello de botella no es el error de la SNR sino la varianza del
   estimador y la estructura de la propensión (ver 4).
2. Al umbral natural 0,5 sí cambia el compromiso: A casi no detecta (recall 0,27) porque su salida estima
   P(etiquetado | x), no P(señal | x); los PU dan salidas más próximas a la probabilidad real (precisión 0,82–0,86,
   recall 0,34–0,38), pero A2 llega a recall 0,52 con precisión 0,79. La corrección mejora la **calibración**
   de las salidas, no el **ranking** (AUC/AP iguales, ≤ ±0,03).
3. **Lo que más ayuda de lo no sintético es la clase `unknown`** (A2: +0,11 en recall a SNR < 5 dB frente a A,
   +0,25 en recall@0,5): excluir de los negativos las regiones donde el detector ve energía sin decodificar quita
   precisamente los falsos negativos más duros. Es coherente con el etiquetado en tres niveles de la tesis, y es
   un resultado propio del piloto, no una intención previa de la hipótesis.
4. **El sintético domina** (C 0,67 y E 0,72 de recall a SNR < 5 dB frente a 0,76 de la cota D). Esto es una
   advertencia sobre el montaje: la señal inyectada ya es sintética, así que el hueco sintético→real aquí solo
   es el del fondo; con señales reales y canal real el hueco será mayor y el argumento a favor de C/E se debilita.
   La propensión por SNR es además una aproximación: la detectabilidad también depende del fondo (la calibración
   P3 muestra desplazamientos de s50 de 5–10 dB entre fondo blanco y real; aprs y acars son los casos peores, y es
   donde las curvas P3 y la propia «cal» difieren más), de la frecuencia y del solapamiento.
5. AIS (ráfagas de 0,03 s, 1/8 de ventana) es el protocolo peor en todas las variantes (AP 0,2–0,5): la unidad
   «ventana» no es adecuada para ráfagas tan cortas.

## Limitaciones

* Fondo real mínimo: las grabaciones del repositorio suman unos 3 s a 48 kS/s; se reciclan, así que el «fondo real»
  repite las mismas ráfagas ISM (train y test no comparten fichero, pero sí tipo de señal).
* Todo en el centro del canal y a una sola tasa; el detector no tiene que localizar en frecuencia.
* Señales inyectadas = las del modulador propio, que es también el que usan los decodificadores (sesgo optimista
  de la detectabilidad, común a la calibración P3). Mensajes repetidos (24 por protocolo).
* Ventana de 0,25 s y regla de solape propias; `unknown` del detector tiene resolución de ±1 s.
* Propensión por ventana ≈ por ráfaga (una ráfaga larga, p. ej. FLEX de 2 s, cubre varias ventanas no independientes);
  los suelos de e (0,2 / 0,5) se probaron como sensibilidad, sin selección a priori; sin ajuste de hiperparámetros.
* SNR de ACARS estimada, no del host; 3 semillas (misma generación de datos por semilla, no réplicas independientes
  del experimento completo); sin intervalos formales.

## Qué haría falta para la versión completa

1. Campaña de capturas reales con el bladeRF (horas, varias bandas) para que el fondo y las señales no etiquetadas
   sean reales, con un subconjunto de verdad independiente (p. ej. ADS-B/AIS contra fuentes públicas, o inyección
   en aire con permiso) para evaluar sin depender de síntesis.
2. Propensión con más covariables (frecuencia, ocupación, fondo) además de la SNR, estimada por inyección en el
   mismo entorno; estimadores estables (clipping/estabilizado de pesos, doblemente robustos con un modelo de
   resultado) en vez de 1/e directo.
3. Detector con localización tiempo-frecuencia (segmentación) y varias frecuencias/tasas; más protocolos.
4. Selección de hiperparámetros con un conjunto de inyección de validación, más semillas independientes y pruebas
   de significación; comparar contra C/E de forma justa (hueco sintético→real medido).
