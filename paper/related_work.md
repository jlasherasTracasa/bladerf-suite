# Trabajo relacionado y veredicto de novedad (P1)

Fecha de la revisión: 2026-11-20. Tesis evaluada: *etiquetado automático de capturas
reales de espectro de banda ancha usando decodificaciones verificadas por CRC/FEC de
muchos protocolos como verdad de terreno, para entrenar y evaluar detectores y
clasificadores de señales.*

**Regla de rigor aplicada.** Cada fila de la tabla se apoya en una página, resumen o
texto que se abrió de verdad; la URL es la abierta. Donde pone «n/d» significa que **lo
abierto no lo dice** (no que sea falso). En muchos casos solo se pudo abrir el resumen
de arXiv; ahí «n/d» es frecuente. Las referencias que solo vi citadas en otro texto o
en un fragmento de buscador están en §7 («sin verificar») y no se usan en ninguna
conclusión. Las entradas BibTeX están en `paper/referencias.bib` (clave entre
corchetes). **Limitación**: la búsqueda se hizo con WebSearch, WebFetch y alphaXiv; no
se accedió a IEEE Xplore, ScienceDirect ni Nature (bloqueados). Una ausencia de
resultados no prueba que no exista el trabajo.

## 1. Tabla comparativa (57 trabajos)

Columnas: año · datos (R = real, S = sintético, R+S = mixto) · nº de protocolos o
clases · origen de las etiquetas · ancho de banda de captura · etiquetas
tiempo-frecuencia (TF) · datos públicos.

| # | Referencia [clave] | Año | URL abierta | Datos | Protocolos / clases | Origen de etiquetas | Ancho de banda | TF | Públicos |
|---|---|---|---|---|---|---|---|---|---|
| 1 | O'Shea et al., Convolutional Radio Modulation Recognition Networks [oshea2016] | 2016 | https://arxiv.org/abs/1602.04105 | S (según CorteXforge §I, fila 10) | n/d | sintético (generación) | n/d | no | n/d |
| 2 | O'Shea et al., Over the Air DL Radio Signal Classification [oshea2018] | 2017 | https://arxiv.org/abs/1712.04578 | R+S (simulación y OTA de laboratorio) | 24 modulaciones (RadioML2018, según CorteXforge Tabla II) | sintético / generado en laboratorio | n/d | no | n/d |
| 3 | Boegner et al., Sig53 / TorchSig [boegner2022sig53] | 2022 | https://arxiv.org/abs/2207.09918 | S | 53 clases, 5 M muestras | sintético | n/d | no (clasificación de señal aislada, ver CorteXforge Tabla I) | TorchSig de código abierto; el conjunto se genera con él |
| 4 | West, O'Shea, Roy, A Wideband Signal Recognition Dataset [west2021wbsig] | 2021 | https://arxiv.org/abs/2110.00518 | S (suma de señales con símbolos aleatorios, sin canal) | 14 modulaciones, 130 registros SigMF | sintético | registros de 100 M muestras; tasa n/d | **sí** (segmentación en la rejilla TF) | **sí**, en SigMF junto al artículo |
| 5 | Boegner et al., WBSig53 [boegner2022wbsig53] | 2022 | https://arxiv.org/abs/2211.10335 | S | 53 clases; ≈550 k muestras, ≈2 M señales | sintético (generado con TorchSig) | n/d | **sí** (detección y reconocimiento; mAP y mAR) | n/d en el resumen; TorchSig abierto |
| 6 | TorchSig (repositorio) [torchsig] | — | https://github.com/TorchDSP/torchsig | S | n/d | sintético | n/d | n/d | sí (licencia MIT) |
| 7 | Nguyen et al., WRIST / SPREAD [nguyen2021wrist] | 2021 | https://arxiv.org/abs/2107.05114 | R (+ sintético autoetiquetado) | 5 tecnologías (WiFi, Bluetooth, ZigBee, Lightbridge, XPD) | **modelo iterativo + corrección manual** (el 41 % de las imágenes necesitó corrección) | 100 MHz (USRP X310) | **sí** | sí (≈1 M emisiones anotadas, según los autores) |
| 8 | Tu et al., Large-scale real-world radio signal recognition [tu2022] | 2022 | https://www.sciopen.com/article/10.1016/j.cja.2021.08.016 (texto: PDF del autor, verificado localmente) | R | 1 protocolo (ADS-B) | **decodificación + comprobación CRC; el ICAO etiqueta el avión** | muestreo hasta 50 MHz | no (etiqueta de dispositivo por ráfaga) | «se liberará» (no confirmado) |
| 9 | Chen et al., ACARS y ADS-B a gran escala [chen2019acars] | 2019 | https://arxiv.org/abs/1904.09425 | R | 2 protocolos; 3143 y 5157 clases (aviones) | n/d en el resumen | n/d | no | n/d |
| 10 | Joly et al., CorteXforge [joly2026] | 2026 | https://arxiv.org/abs/2610.26999 | R (banco de pruebas controlado) | 24 modulaciones | **escenario programado** (instantes de inicio y recuento anotados en SigMF) | 16,667 MHz | parcial (anotación temporal por transmisión) | sí (GitHub) |
| 11 | Chang et al., CSRD2025 [chang2025csrd] | 2025 | https://arxiv.org/abs/2508.19552 | S | 100 modulaciones | sintético; anotación COCO | n/d | **sí** | marco abierto; el conjunto de 200 TB no confirmado |
| 12 | Fonseca et al., RAT characterisation by object detection [fonseca2020] | 2020 | https://arxiv.org/abs/2007.13561 | R+S | 2 (WiFi, LTE) | generador propio (transmite y etiqueta) y conjunto comercial de Bélgica | espectrogramas de 20 MHz | **sí** (cajas VOC) | código en GitHub; conjunto de Bélgica en #56 |
| 13 | Peng et al., SigChord [peng2025sigchord] | 2025 | https://arxiv.org/abs/2504.06587 | R+S (OTA con transmisores propios) | WiFi y DVB-S2 (en las pruebas) | sintético / transmisores propios; la red decodifica cabeceras | 0,4–7 GHz y 4–40 GHz (submuestreo) | no | n/d |
| 14 | Peng et al., Sums [peng2024sums] | 2024 | https://arxiv.org/abs/2405.15705 | n/d | n/d | n/d | 70 MHz–8 GHz con 50 Msps | no | n/d |
| 15 | Rajendran et al., Electrosense [rajendran2017] | 2017 | https://arxiv.org/abs/1703.09989 | R | monitorización de ocupación | sin etiquetas de señal (n/d) | n/d | no | sí (datos abiertos como servicio) |
| 16 | Sankhe et al., ORACLE [sankhe2019oracle] | 2018 | https://arxiv.org/abs/1812.01124 | R | 16 USRP + >100 equipos WiFi | identidad de dispositivo | n/d | no | n/d en el resumen |
| 17 | Allahham et al., DroneRF [allahham2019] | 2019 | https://pmc.ncbi.nlm.nih.gov/articles/PMC6727013 | R | 3 drones + fondo; 4 modos de vuelo | identificador en el nombre de fichero (diseño del experimento) | ≈80 MHz (dos receptores de 40 MHz) | no | sí (Mendeley, >40 GB) |
| 18 | Agadakos et al., RF-DCN [agadakos2019] | 2019 | https://arxiv.org/abs/1909.08703 | R | n/d | identidad de dispositivo | n/d | no | n/d |
| 19 | Cekic et al., Wireless fingerprinting [cekic2020] | 2020 | https://arxiv.org/abs/2002.10791 | n/d | WiFi y ADS-B | identidad de dispositivo | n/d | no | n/d |
| 20 | Restuccia et al., DeepRadioID [restuccia2019deepradioid] | 2019 | https://arxiv.org/abs/1904.07623 | R (RFMLS de DARPA y 20 SDR) | 500 ADS-B y 500 WiFi | identidad de dispositivo | n/d | no | n/d |
| 21 | Restuccia et al., DeepFIR [restuccia2020deepfir] | 2020 | https://arxiv.org/abs/2005.04226 | R | 500 ADS-B, 500 WiFi, 24 modulaciones | identidad / modulación | n/d | no | n/d |
| 22 | Uvaydov et al., Stitching the Spectrum [uvaydov2024] | 2024 | https://arxiv.org/abs/2402.03465 | R de señales sueltas, **ensambladas** en banda ancha | 5 protocolos (WiFi, LTE, BLE, LoRa, ZigBee) | **por construcción (cosido)**; afirman que etiquetar banda ancha real es «hardly feasible» | 25 MHz de receptor; evalúan hasta 100 MHz | **sí** (segmentación) | «publicaremos» 17 GB (no confirmado) |
| 23 | Wicht et al., Spectrogram Data Set [wicht2022] | 2022 | https://ideas.repec.org/a/gam/jdataj/v7y2022i12p168-d981986.html | R (grabaciones) + aumento sintético | WiFi y Bluetooth | **pipeline de aumento** (etiquetar a mano es inviable, según el resumen) | n/d | **sí** (posición y tipo de trama) | n/d en el resumen |
| 24 | Wood et al., Object detection approaches [wood2022] | 2022 | https://arxiv.org/abs/2210.16173 | S (MATLAB) | 6 tipos (DSSS, BLE, QAM, AM, FM, WiFi) | sintético | 100 MHz | **sí** (cajas; mAP 0,906 con YOLOv5) | repositorio abierto |
| 25 | Chandhok et al., sub-Nyquist characterization [chandhok2019] | 2019 | https://arxiv.org/abs/1912.05255 | n/d | n/d | n/d | n/d | n/d | n/d |
| 26 | Xing et al., CRML23 [xing2024crml23] | 2024 | https://arxiv.org/abs/2405.00736 | S | n/d | sintético | n/d | cajas TF (según CorteXforge Tabla I) | sí (GitHub) |
| 27 | Zhang et al., DeepWiPHY [zhang2020deepwiphy] | 2020 | https://arxiv.org/abs/2010.09268 | R+S (110 M símbolos sintéticos, 14 M reales) | 802.11ax | n/d | n/d | no | n/d |
| 28 | Huang et al., RadDet [huang2025raddet] | 2025 | https://arxiv.org/abs/2501.10407 | n/d | 11 clases de radar | n/d | 500 MHz | **sí** | n/d |
| 29 | Rostami et al., CageDroneRF [rostami2026] | 2026 | https://arxiv.org/abs/2601.03302 | R+S | n/d | n/d | n/d | cajas (formato n/d) | herramientas abiertas |
| 30 | Shi et al., RFUAV [shi2025rfuav] | 2025 | https://arxiv.org/abs/2503.09033 | R | 37 drones | n/d | n/d | n/d | sí (GitHub, 1,3 TB) |
| 31 | Zheng et al., DRFF-R2 [zheng2026drff] | 2026 | https://arxiv.org/abs/2603.00106 | R | 26 unidades, 8 modelos | convención de nombres | n/d | n/d | n/d |
| 32 | Gallucci et al., SmartBAN [gallucci2026] | 2026 | https://arxiv.org/abs/2601.15836 | R+S | SmartBAN | n/d | n/d | n/d | marco abierto |
| 33 | Tung et al., RISE [tung2026rise] | 2026 | https://arxiv.org/abs/2603.20481 | R+S (OTA controlado) | n/d | n/d | 100 MHz | implícito (IoU) | n/d |
| 34 | Yang et al., ZoomSpec (sobre SpaceNet) [yang2026zoomspec] | 2026 | https://arxiv.org/abs/2604.13568 | R+S (SpaceNet: capturas de campo y mitad simulada) | 14 familias de modulación | señal suelta capturada y **composición controlada** | banda ISM de 2,4 GHz completa | **sí** (frecuencia central, ancho e intervalo activo en JSON) | SpaceNet «público» (según el texto) |
| 35 | Safak et al., Sub-6 GHz OTA AMC [safak2026] | 2026 | https://arxiv.org/abs/2609.07726 | R | 32 clases | n/d | n/d | no | n/d |
| 36 | Tapparel y Burg, LoRa UAV [tapparel2025] | 2025 | https://arxiv.org/abs/2512.15268 | R | LoRa | información de sincronización | ≈250 kHz de señal | n/d | publicado (sin enlace en el resumen) |
| 37 | Delplace et al., IQSim [delplace2026] | 2026 | https://arxiv.org/abs/2604.06408 | S | IoT sub-GHz | — | n/d | no | n/d |
| 38 | Bundscherer et al., modos de radioaficionado [bundscherer2025] | 2025 | https://arxiv.org/abs/2501.07337 | R (evaluación) + S (entrenamiento) | 17 modos, 98 variantes | señales propias | banda de 70 cm | no | n/d |
| 39 | Scholl, entrenamiento sintético y rendimiento real [scholl2022] | 2022 | https://arxiv.org/abs/2206.12967 | S (entrena), R (prueba) | 20 tipos de onda corta | n/d | n/d | no | n/d |
| 40 | Scholl, HF transmission modes [scholl2019] | 2019 | https://arxiv.org/abs/1906.04459 | n/d | 18 modos HF | n/d | n/d | no | n/d |
| 41 | Scholl, onda corta a gran escala [scholl2025] | 2025 | https://arxiv.org/abs/2504.05455 | R+S | 160 clases | n/d | n/d | no | n/d |
| 42 | Luostari et al., supervisión con ruido de entrada [luostari2026] | 2026 | https://arxiv.org/abs/2608.12918 | R+S | OFDM 5G/6G de laboratorio | **bits decodificados validados por CRC** («supervisión casi sin error») | n/d | no | n/d |
| 43 | Zhang et al., LTE RFF dataset [zhang2022xsrp] | 2022 | https://arxiv.org/abs/2205.12577 | R | LTE | identidad del transmisor | n/d | no | sí (GitHub) |
| 44 | Grimm et al., AIS + RF [grimm2026] | 2026 | https://arxiv.org/abs/2611.18563 | AIS real, detecciones RF sintéticas | AIS | n/d | — | no | n/d |
| 45 | Longo et al., Spectrum of Liability [longo2026] | 2026 | https://arxiv.org/abs/2610.25411 | n/d | AIS, GNSS | guarda IQ junto a salidas decodificadas de AIS/GNSS | multicanal | no | n/d |
| 46 | Cheraghinia et al., RFShape [cheraghinia2026] | 2026 | https://arxiv.org/abs/2611.03947 | S (preentrenamiento) | 10 tareas en 9 conjuntos | sin etiquetas en preentrenamiento | n/d | no | n/d |
| 47 | Milosheski et al., Deep clustering sin etiquetas [milosheski2025] | 2025 | https://arxiv.org/abs/2508.01709 | R (3 ciudades) | >10 RAT en 868 MHz, 2,4 GHz y 5,9 GHz | **ninguna**; clases asignadas a mano después | n/d | no | n/d |
| 48 | Alla y Lenders, WhiteNet [alla2026whitenet] | 2026 | https://arxiv.org/abs/2608.06581 | R+S | 802.11 solapado | n/d | n/d | no | usa datos OTA públicos |
| 49 | Chen et al., RFSS [chen2025rfss] | 2025 | https://arxiv.org/abs/2508.12106 | S | GSM, UMTS, LTE, 5G NR | referencias por fuente | 2,166–122,88 MHz de tasa | no | sí (no comercial) |
| 50 | Maeng et al., LTE I/Q UAV [maeng2023] | 2023 | https://arxiv.org/abs/2303.01635 | R | LTE (una estación) | n/d | n/d | no | «se proporciona» (sin enlace) |
| 51 | Baytekin et al., 5G NR neural receivers [baytekin2026] | 2026 | https://arxiv.org/abs/2603.09644 | R | 5G NR PUSCH | n/d | n/d | no | n/d |
| 52 | Kulin et al., End-to-end learning [kulin2018] | 2017 | https://arxiv.org/abs/1712.03987 | n/d | reconocimiento de modulación y de interferencia de tecnologías | n/d | n/d | no | n/d |
| 53 | Shen et al., LoRa RFFI [shen2021lora] | 2021 | https://arxiv.org/abs/2107.02867 y https://ieee-dataport.org/open-access/lorarffidataset | R | 60 dispositivos LoRa | identidad de dispositivo (preámbulo y etiqueta) | n/d | no | sí (IEEE DataPort, CC BY 4.0, 15,55 GB) |
| 54 | Schäfer et al., OpenSky [schafer2014opensky] | 2014 | https://www.cs.ox.ac.uk/files/7238/ipsn2014.pdf | R | ADS-B | red de receptores; mensajes de miles de millones (no IQ) | n/d | no | servicio para grupos académicos |
| 55 | rtl_433 [rtl433] | — | https://github.com/merbanan/rtl_433 | herramienta | ≈390 entradas de protocolo en el README | decodificador | 433, 868, 315, 345, 915 MHz | no | sí (código) |
| 56 | Technology-classification-dataset (Gante) [ewine_techclass] | — | https://github.com/ewine-project/Technology-classification-dataset | R | WiFi, LTE, DVB-T | tecnología por captura (diseño) | 1–10 MHz (el README es incoherente) | no | bajo petición (>5 GB) |
| 57 | NIST, Wi-Fi y Bluetooth I/Q [nist_wifi_bt] | — | https://data.nist.gov/od/id/mds2-2731 | R | WiFi y Bluetooth | n/d | n/d | n/d | repositorio público NIST |

**Lectura de la tabla.** De 57 trabajos solo uno (#8, Tu) obtiene etiquetas **por
decodificación con CRC** de señales reales a gran escala, y es de un solo protocolo y
etiqueta dispositivos, no cajas TF. #42 usa bits validados por CRC como supervisión,
pero para receptores neuronales, no para detección. Los conjuntos con etiquetas TF
(#4, #5, #7, #11, #22, #23, #24, #34) son sintéticos, ensamblados, aumentados o
etiquetados con ayuda de un modelo. Ninguno etiqueta con decodificadores de varios
protocolos una captura **de banda ancha no controlada**.

## 2. Los trabajos más cercanos a la tesis

**Tu et al. 2022 [tu2022] (#8).** Es el antecedente directo del mecanismo. Construyen
un sistema automático que captura ADS-B, busca la cabecera, decodifica, comprueba el
CRC y etiqueta cada ráfaga con el código ICAO del avión «sin participación humana»
(comprobado en el texto: «resampling, header search, information decoding, CRC check, and
positioning and labeling»). Muestrean hasta 50 MHz. **Diferencia**: un solo protocolo; la
etiqueta es la identidad del avión para huellas de dispositivo, no una caja
tiempo-frecuencia; ningún análisis de cobertura ni de sesgo de selección; datos no
liberados en la versión consultada. Es la referencia que obliga a presentar la tesis
como *generalización* y no como idea nueva.

**WRIST/SPREAD, Nguyen et al. 2021 [nguyen2021wrist] (#7).** Mismo problema (etiquetas
TF de emisiones reales en 100 MHz, cinco tecnologías) con otra solución: un modelo
entrenado iterativamente etiqueta, y las emisiones se graban por separado con cables y
cámara anecoica para evitar colisiones; el 41 % de las imágenes aún necesitó corrección
manual. **Diferencia**: allí el etiquetado depende de un modelo y de grabaciones
controladas; la tesis usa decodificación independiente del modelo y capturas en
ambiente real con señales coexistentes. Es el comparador natural: etiquetado
con modelo+humano frente a etiquetado por decodificación.

**Uvaydov et al. 2024 [uvaydov2024] (#22).** Dicen explícitamente que etiquetar
espectro real de banda ancha es «extremely challenging» por la coexistencia de señales
cuyo tipo y posición no se conocen a priori, y resuelven el problema cosiendo señales
reales sueltas en capturas sintéticas anchas. **Diferencia**: confirma la premisa de la
tesis (el problema es real y reconocido), pero evita la captura real ancha; sus etiquetas
valen por construcción, no por verificación.

**Fonseca et al. 2020 [fonseca2020] (#12).** Afirman que en transmisiones comerciales
capturadas «it is not possible to determine precisely the ground truth» y construyen un
generador que transmite y etiqueta con SDR propios (WiFi y LTE, cajas VOC). Cierran
diciendo que etiquetar a mano es lento y propenso a errores. **Diferencia**: de nuevo
control de la fuente en vez de verificación; solo dos tecnologías.

**Wicht et al. 2022 [wicht2022] (#23).** Conjunto de 20 000 segmentos de WiFi y
Bluetooth con posición y tipo de trama, creado con un pipeline de aumento porque etiquetar
a mano es inviable. **Diferencia**: etiquetas por aumento sobre grabaciones reales, dos
protocolos; no hay verificación por decodificación en capturas ambientales.

**CorteXforge, Joly et al. 2026 [joly2026] (#10).** Banco de pruebas SLICES-RI/CorteXlab
(sala apantallada), etiquetas derivadas del escenario con anotaciones SigMF, 24
modulaciones. Mide la brecha sintético→real: la precisión cae de 61,4 % (RadioML2018) a
23,2 % en datos reales. **Diferencia**: entorno controlado y sin interferencia externa
(lo reconocen en su §VI); la tesis trabaja con espectro abierto. Sus cifras sirven de
referencia para el experimento de brecha.

**SigChord y Sums, Peng et al. 2025/2024 [peng2025sigchord][peng2024sums] (#13, #14).**
Sniffing de bandas múltiples con submuestreo: la red decodifica cabeceras abiertas
(WiFi L-SIG/HT-SIG, DVB-S2). **Diferencia**: la decodificación es la *salida* del sistema
y se entrena con señales sintéticas y transmisores propios (texto de SigChord, §4.1–4.2);
no se usa como verdad de terreno para entrenar otros detectores en captura real.

**Luostari et al. 2026 [luostari2026] (#42).** Cercano en la idea: usan bits decodificados
validados por CRC como supervisión «casi sin error» y estudian cuándo la supervisión OTA
ayuda al receptor neuronal. **Diferencia**: objetivo (receptores) y un solo sistema OFDM;
no hay detección ni multiprotocolo. Su resultado (el ruido de etiqueta correlacionado con
la entrada limita el beneficio) es un argumento a considerar: las etiquetas por
decodificación están correlacionadas con la dificultad de la señal.

Menciones secundarias: Milosheski et al. [milosheski2025] reconocen tecnologías sin
etiquetas (agrupamiento) y asignan clases a mano; Longo et al. [longo2026] guardan IQ con
las salidas decodificadas de AIS/GNSS para análisis forense, lo cual es una forma de
verdad de terreno por decodificación sin finalidad de aprendizaje.

## 3. Veredicto de novedad

**Veredicto: incremental** (con potencial de «novedosa» si se acotan bien las
afirmaciones). No encontré, dentro de lo que pude abrir, un trabajo que ya haga
etiquetado por decodificación multiprotocolo sobre banda ancha real con etiquetas
tiempo-frecuencia, por lo que no es «ya hecha». Pero tampoco es una idea sin
antecedentes: el mecanismo (decodificar, comprobar CRC, usar el resultado como etiqueta
sin humano) ya está publicado para ADS-B (Tu 2022) y como supervisión para receptores
(Luostari 2026). La contribución diferencial real es la **combinación**: (i) muchos
protocolos a la vez, (ii) captura ancha en ambiente real con señales coexistentes,
(iii) etiquetas TF (instante, frecuencia, ancho, SNR) y no solo identidad, (iv) precisión
y cobertura de etiquetas *medidas*, (v) conjunto abierto y reproducible y (vi)
medición de la brecha sintético→real con esas etiquetas.

**Por qué no basta como «novedosa».** Un revisor citará Tu 2022 y WRIST y dirá que la
idea de etiquetado automático ya existe. Además hay un problema de fondo: la
decodificación solo etiqueta lo decodificable (alta SNR, protocolo conocido, sin
colisión). Las señales débiles o desconocidas quedan *sin etiqueta*, no son negativas, y
entrenar un detector con eso introduce un **sesgo de selección** hacia señales fáciles.
Quien lo lea como «verdad de terreno» completa lo rechazará.

**Qué cambiar en la tesis.**
1. Sustituir «etiquetado automático de espectro real» por *etiquetas verificadas y
   parciales* (alta precisión, cobertura limitada y medida) y presentarlas como
   conjunto de **evaluación con verdad verificada** más una fracción de entrenamiento.
2. Cuantificar la cobertura: fracción de señales/energía del espectro explicada por
   las decodificaciones, estimada frente a anotación manual en una muestra.
3. Tratar lo no etiquetado explícitamente (aprendizaje con positivos y no etiquetados o
   enmascarado de regiones sin etiqueta) y medir el sesgo por SNR.
4. Comparar de frente con etiquetado asistido por modelo (WRIST) y con datos sintéticos
   (WBSig53), y reproducir la brecha sintético→real de CorteXforge con detección y no
   solo clasificación.
5. Evitar la frase «la literatura dice que no se puede determinar la verdad de
   terreno» sin matiz: Fonseca [fonseca2020] y Uvaydov [uvaydov2024] lo dicen, pero Tu
   [tu2022] lo resuelve para ADS-B.
6. Plan B del PLAN §5.1 (artículo de sistema FPGA+GPU) sigue siendo menos novedoso;
   mantener solo como alternativa.

## 4. Qué exigiría un revisor

Métricas y comparativas habituales, extraídas de lo leído:

- **Detección TF**: mAP@[0,5:0,95] y mAR con IoU (WBSig53, Wood, CSRD, ZoomSpec usan
  mAP/mAR o COCO), mAP@0,5, curvas de precisión-recobrado y tasa de falsas alarmas.
  Resultados de referencia: YOLOv5 0,906 mAP en sintético [wood2022]; ZoomSpec 78,1
  mAP@0,5:0,95 en SpaceNet [yang2026zoomspec].
- **Clasificación**: precisión frente a SNR (RadioML, CorteXforge), matriz de confusión
  por clase.
- **Brecha sintético→real**: entrenar en sintético (WBSig53/CSRD/propio), probar en
  real etiquetado; afinado con fracciones del conjunto real (como en CorteXforge: 0,1 %
  a 100 %). Ver 61,4 % → 23,2 % como orden de magnitud.
- **Calidad de etiquetas**: error de tiempo y frecuencia con señales inyectadas de
  posición conocida, precisión y cobertura frente a anotación manual en una muestra y
  frente a decodificadores de referencia (por ejemplo rtl_433 [rtl433] para ISM),
  acuerdo entre anotadores (kappa).
- **Línea base clásica**: detector de energía/CFAR y U-Net de segmentación (WBSig53),
  YOLO/RetinaNet (Wood), RISE [tung2026rise] para tiempo real.
- **Robustez**: validación cruzada entre receptores y ubicaciones (CorteXforge usa
  receptor excluido; Uvaydov prueba con varias radios), varias semillas, intervalos de
  confianza y tests estadísticos.
- **Coste**: latencia y carga de CPU/GPU por protocolo (en el sistema propio).
- **Datos**: hoja de datos, licencia, DOI, anonimización (ya prevista en PLAN §5.3),
  límite legal de la captura.
- **Lo que no esté cubierto por decodificadores**: análisis de errores y de sesgo.

## 5. Revistas

El cuartil JCR **no se pudo confirmar en una fuente fiable** para ninguna: los datos
oficiales de Clarivate no son accesibles desde aquí. Se anota el factor de impacto
que dice la propia web cuando existe; los agregadores discrepan y no se usan.

| Revista | Alcance según la fuente abierta | Impacto citado | Cuartil | Trabajos parecidos verificados |
|---|---|---|---|---|
| IEEE Open Journal of the Communications Society (OJ-COMS) | Su web (https://www.comsoc.org/publications/journals/ieee-ojcoms) dice que «publishes original high-quality manuscripts on advances in the state of the art of telecommunications systems and networks» y cubre redes inalámbricas y aprendizaje automático para comunicaciones | 6,1 (JCR 2025, según su página) | sin confirmar | Milosheski et al. (RAT sin etiquetas), cuya referencia de revista en arXiv es OJ-COMS vol. 5, 2024 [milosheski2025] https://arxiv.org/abs/2508.01709 (dato de arXiv, no de la web de la revista) |
| IEEE Transactions on Instrumentation and Measurement (TIM) | Wikipedia (https://en.wikipedia.org/wiki/IEEE_Transactions_on_Instrumentation_and_Measurement): «the theory, design and use of electronic instrumentation and measurement techniques». No se abrió la web de IEEE (inaccesible) | 7 (JCR 2025 según Wikipedia; otros agregadores dan 5,9 a 7,5) | sin confirmar | ninguno encontrado |
| Scientific Data (Nature) | No se pudo abrir (redirecciones de autenticación). **Sin verificar** | n/d | sin confirmar | ninguno encontrado |
| Computer Networks (Elsevier) | La web de Elsevier/ScienceDirect devolvió 403. Solo agregadores (temas: arquitecturas, protocolos, seguridad, operación y gestión), **no usados** | los agregadores discrepan (3,0 a 5,6) | sin confirmar | ninguno encontrado |
| IEEE Transactions on Cognitive Communications and Networking (TCCN) | Su web (https://www.comsoc.org/publications/journals/ieee-tccn): comportamientos «cognitivos» en comunicaciones y redes; temas: aprendizaje automático para redes, radio cognitiva, acceso dinámico al espectro; exige entidad inteligente y contribución original en comunicaciones | no indicado | sin confirmar | ninguno verificado |
| Data (MDPI) | Revista de descriptores de datos; no abrí su alcance | n/d | sin confirmar | Wicht et al. 2022 [wicht2022] (confirmado: Data 7(12), 2022) https://ideas.repec.org/a/gam/jdataj/v7y2022i12p168-d981986.html |
| Data in Brief | No abrí su alcance | n/d | sin confirmar | DroneRF [allahham2019], Data in Brief vol. 26, 2019 https://pmc.ncbi.nlm.nih.gov/articles/PMC6727013 |

**Recomendación.** OJ-COMS es la que mejor encaja con lo que pude verificar: su alcance
incluye explícitamente aprendizaje para comunicaciones, tiene factor de impacto citado
por su propia web y hay un precedente directo de datos reales y reconocimiento de
tecnologías en esa revista. TIM encaja si el peso se pone en la **medición**
(incertidumbre de las etiquetas, calibración temporal y de frecuencia del sistema) y no
en el aprendizaje; no encontré trabajos parecidos en ella. Scientific Data solo si el
artículo se convierte en un descriptor de datos; no pude comprobar su alcance. El
cuartil de todas queda **sin confirmar** y debe comprobarse en el JCR vigente (tarea P0).

## 6. Siguientes pasos de la revisión (no hechos)

- Abrir los textos completos de Sums, SpaceNet y CSRD2025 para ver cómo etiquetan.
- Acceder a IEEE Xplore, ScienceDirect y Nature (cuenta institucional) para
  verificar alcance de revistas y los trabajos de §7.
- Buscar de nuevo trabajos de huella RF y de detección con etiquetas de
  decodificación en LoRa y AIS; mis búsquedas en alphaXiv no encontraron conjuntos IQ de
  AIS etiquetados por decodificación (solo trabajos de fusión AIS y forense, #44 y #45).

## 7. Referencias sin verificar (no usadas en ninguna conclusión)

Solo vistas citadas en otro texto o en un fragmento de buscador; no se abrió su
resumen o texto.

1. Jian et al., «Deep learning for RF fingerprinting: A massive experimental study»,
   IEEE Internet of Things Magazine 3(1), 2020 (cita vista en un resultado de
   búsqueda; es el artículo RFMLS pedido en la tarea).
2. O'Shea et al., «Learning robust general radio signal detection using computer vision
   methods», Asilomar 2017 (citado en Fonseca y Wood).
3. Vagollari et al., «Joint detection and classification of RF signals using deep
   learning», VTC2021-Spring (dblp en resultado de búsqueda; no se abrió).
4. Vagollari, Hirschbeck, Gerstacker, IEEE Access 2023 (sobre detección, localización y
   clasificación en banda ancha con detección de objetos; resultado de búsqueda;
   https://doaj.org/article/e529e3cb0f844cab9256ec1a07b1b912 devolvió 403).
5. Fontaine et al., «Towards low-complexity wireless technology classification across
   multiple environments», Ad Hoc Networks 91, 2019 (citado en Fonseca).
6. Selim et al., «Spectrum monitoring for radar bands using deep CNNs», GLOBECOM 2017
   (citado en Fonseca; el preprint arXiv 1705.00462 salió en un resultado de búsqueda).
7. Rajendran et al., «Deep learning models for wireless signal classification with
   distributed low-cost spectrum sensors», IEEE TCCN 2018 (citado en Fonseca).
8. DroneRFb-Spectra, «Radio-Frequency Control and Video Signal Recordings of Drones»
   (Zenodo 4264467), CRAWDAD up/rf_recordings, conjunto GNSS de la revista Data y
   arXiv 2512.12070 (RFFI de LoRa independiente del receptor): solo fragmentos de
   buscador.
9. DeepSig RadioML 2018.01A como fuente primaria (solo la vi descrita en CorteXforge).
10. Detalles de ORACLE como parte de «No Radio Left Behind» (TCCN): solo vi un
    fragmento de buscador.
11. Tamaños y tasas de WBSig53 (artículo 2022) más allá del resumen: no abrí el texto.
12. Todos los datos de revista (Scientific Data, Computer Networks, TIM web oficial,
    cuartiles JCR).

## 8. Fuentes de las citas textuales

- Fonseca [fonseca2020]: «In the case of commercial transmissions captured over the air,
  it is not possible to determine precisely the ground truth» (texto completo vía
  alphaXiv, §IV).
- Uvaydov [uvaydov2024]: «labeling real-world wideband spectrum is extremely challenging
  due to the coexistence of different signals in the same spectrum bands» (§I).
- Tu [tu2022]: pasos «resampling, header search, information decoding, CRC check, and
  positioning and labeling»; «we only label the airplane ID»; muestreo de 50 MHz (texto
  completo del PDF del autor, abierto localmente con pdftotext).
- WRIST [nguyen2021wrist]: «41 percent of RF images needed some manual corrections»;
  «approximately 800,000 curated, labeled RF emissions recorded using cables and EMI
  shielding» (texto vía alphaXiv).
- CorteXforge [joly2026]: 61,4 % → 23,2 % y «all acquisitions were performed within the
  same SLICES-RI/CorteXlab environment» (texto vía alphaXiv).
