# PLAN.md — Hoja de ruta de BladeRF Lab (desde el 2026-11-20)

Plan para mejorar a fondo la aplicación y el firmware. Lo ejecuta un
**orquestador Opus** que reparte el trabajo entre subagentes **Sonnet** y
**Haiku**. `AGENTS.md` sigue siendo el contrato de los agentes; este fichero
dice qué se hace, en qué orden, quién lo hace y cómo se da por bueno.

Punto de partida: commit `42eabeb`, árbol limpio, `make check` 42/42.
Las tareas G1 (cuelgue con GPU) y R1 (reordenar `docs/`) se interrumpieron sin
dejar cambios: se rehacen en la fase 0.

---

## 1. Quién hace qué

| Papel | Modelo | Qué hace | Qué no hace |
|---|---|---|---|
| Orquestador | **Opus** | Planifica, redacta los encargos, verifica cada entrega (compilar, tests, leer el diff, probar la web), hace **todas** las pruebas con la placa, los commits y la actualización de `AGENTS.md`, `README.md` y este plan | No escribe código del proyecto |
| Agente complejo | **Sonnet** | Diseño y código difícil: DSP, concurrencia, CUDA, HDL, decodificadores, depuración con causa desconocida | No toca la placa salvo RX si el encargo lo dice; nunca carga FPGA ni transmite |
| Agente mecánico | **Haiku** | Trabajo acotado y verificable por orden: renombrados, rutas, limpieza, tablas de documentación, capturas, ejecutar baterías y resumir, cambios de una línea ya especificados | Nada que exija decidir un diseño o depurar |

Regla para elegir: si el encargo cabe en «haz exactamente esto y comprueba con
esta orden», va a **Haiku**. Si hay que investigar, diseñar o hay riesgo de
romper tiempo real o concurrencia, va a **Sonnet**. Si un Haiku falla dos veces
la verificación, la tarea sube a Sonnet. Si un Sonnet no encuentra la causa en
dos rondas, Opus revisa el planteamiento antes de una tercera.

### Reglas del orquestador

1. **Puerta de calidad antes de cada commit**: `make check` (con CUDA),
   `make CUDA=0 BUILD=build/orqc test`, `make SAN=1 BUILD=build/orqs test` y
   `make webtest` con el binario **con CUDA**. Todo en verde y 0 avisos.
2. **Un commit por tarea verificada**, mensaje en español, sin menciones al
   asistente ni líneas de coautoría, sin rutas del HOME. `git push` tras cada fase.
3. **Placa**: solo Opus. Imagen FPGA solo en RAM (`bladeRF-cli -l`), nunca la
   flash; restaurar la de serie al terminar. Transmitir por antena o por cable
   exige permiso expreso del usuario en ese momento.
4. **Paralelismo**: máximo 4 agentes a la vez y nunca dos sobre el mismo
   fichero. Cada agente usa `BUILD=build/<id>` y su propio rango de puertos.
5. **Encargos**: cada uno lleva ficheros permitidos, criterio de terminado con
   órdenes exactas e informe ≤ 250 palabras. Opus no se fía del informe: repite
   las órdenes.
6. Al cerrar cada fase: actualizar `AGENTS.md` §2 y §8b, el estado del
   `README.md`, `docs/calidad/VALIDACION.md` y regenerar el curso si cambian cifras.

### Decisiones y acciones que dependen del usuario

| Qué | Para qué | Bloquea |
|---|---|---|
| Conectar la placa a un puerto y cable **USB 3** | Pasar de 11 a 61 Msps | H2, F6, D4, D5 |
| Conectar la **antena de TV** de casa | TDT en el aire (hoy SNR −9 dB) | D6 |
| Antena para 1090 MHz / VHF aérea en mejor sitio | ADS-B y VDL2 en el aire | D1 |
| Permiso y cable TX→RX con atenuador ≥ 30 dB | Calibración real de emisión | T1 |
| ¿Audio de DAB+ con el decodificador del navegador (como el vídeo de la TDT)? | Oír DAB+ sin escribir un AAC propio | D7 |

---

## 2. Fases

Orden: 0 → 1 → (2, 3 y 4 en paralelo) → 5 → 6. La fase 0 es obligatoria antes
de nada; la 1 protege todo lo que viene después.

### Fase 0 — Cerrar lo que quedó a medias

| ID | Tarea | Modelo | Ficheros | Terminado cuando |
|---|---|---|---|---|
| G1 | SIGSEGV con DDC de GPU al añadir, mover y quitar instancias deprisa. Traza: `nco_mix` ← `ddc_process` ← `process_block` (`host.c:533`): un bloque compartido se libera mientras una instancia de CPU lo lee | **Sonnet** | `src/decoders/host.{c,h}`, `src/gpu/ddc_gpu*`, `src/gpu/pfb_gpu*`, `tests/test_host.c` | Reproductor de estrés (dec_add ×17, dec_tune, dec_remove en bucle por WebSocket) 3 × 200 rondas sin caída; `make webtest` con CUDA sin «PROCESO CAÍDO»; causa raíz explicada con fichero:línea; el hilo DSP sigue sin bloquearse |
| G1b | Guardar el reproductor de estrés en `tests/stress_host.mjs` y engancharlo a `make webtest` | **Haiku** | `tests/stress_host.mjs`, `Makefile` | El objetivo lo ejecuta y falla si el servidor muere |
| R1 | Reordenar `docs/` por temas (`uso/`, `arquitectura/`, `hardware/`, `protocolos/`, `calidad/`, `investigacion/`), `PROTOCOLO.md` → `arquitectura/WEBSOCKET.md`, índice `docs/README.md`, con `git mv` | **Haiku** | `docs/**`, rutas en `README.md`, `AGENTS.md`, comentarios de `src/`, generadores de `docs/img/src/` y del curso | Comprobador de enlaces: 0 rotos y ≥ 386 revisados; `git grep` de rutas viejas vacío; movimientos como renombrados |
| L1 | Limpieza: quitar las trazas DBG de `dvbt.c` y `wifi.c`; quitar `-Wno-format-truncation` y la excepción de `_GNU_SOURCE` del `Makefile`; `--no-gpu` llama a `dec_host_set_gpu(host,false)`; RDS `af` en MHz y «Ancho RF» con fichero en la web | **Haiku** | los citados, `src/main.c`, `web/js/rds.js`, `web/js/hardware.js` | `make check` y `make webtest` en verde |

### Fase 1 — Red de seguridad

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| Q1 | `make ci`: encadena las cuatro puertas de calidad, las simulaciones de la FPGA que no necesitan Quartus (modelo C) y el comprobador de enlaces; resumen final de una pantalla | **Haiku** | Una orden da verde o rojo con la lista de fallos |
| Q2 | ThreadSanitizer para host, motor y servidor (`make TSAN=1 test`), y corregir las carreras que aparezcan | **Sonnet** | 0 informes de TSan en `test_host`, `test_engine*`, `test_tv` |
| Q3 | Pruebas de entradas hostiles (fuzz propio, sin bibliotecas): JSON y órdenes WebSocket, cabeceras HTTP, SigMF, TS/PSI/PES, y cada decodificador con ruido y tramas truncadas, bajo ASan/UBSan | **Sonnet** | 10 min por objetivo sin fallos; los casos que rompían quedan como tests |
| Q4 | Presupuesto de tiempo real: test que mide CPU por decodificador a su tasa y falla si supera el 30 % de un núcleo; tabla en `docs/arquitectura/RENDIMIENTO.md` | **Haiku** (la medida) / **Sonnet** (si hay que optimizar) | Los 21 dentro de presupuesto o con excepción documentada |
| Q5 | Batería de regresión con grabaciones reales: fijar en `tests/data/` (o descarga con suma de comprobación) un fragmento por protocolo con el número de mensajes esperado | **Sonnet** | `make realtest` compara con la referencia y falla si baja |

### Fase 2 — Aplicación: núcleo y hardware

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| N1 | Detector: excluir el ~5 % de cada borde, fusionar detecciones contiguas dentro del ancho ocupado, histéresis temporal; quitar los parches de la web que lo ocultaban | **Sonnet** | Con la FM sintética y la real: un marcador por emisora, 0 en los bordes; test nuevo |
| N2 | Perfiles `auto:true`: el host crea y retira instancias sobre señales detectadas (máx. 8), para sondas, DMR, LTE y GSM | **Sonnet** | Test de host con señales que aparecen y desaparecen; web muestra «auto» |
| N3 | Barrido: adaptar el adelanto de quick-tune a la latencia medida para no caer al modo lento | **Sonnet** | Opus mide en placa: 47 MHz–6 GHz sin pasar a modo lento |
| H2 | Repetir con USB 3 todas las medidas de caudal (RX 30,72 / 61,44 / 122,88 Msps y modo espectro) | **Opus** (placa) + **Haiku** (tablas) | `docs/arquitectura/RENDIMIENTO.md`, `docs/hardware/HARDWARE.md` y `docs/hardware/FIRMWARE.md` con cifras USB 3 |
| N4 | Segundo canal de recepción (2×2): fuente con RX1+RX2, espectro por canal y selector en la web; base para diversidad y medida de dirección | **Sonnet** | Dos espectros simultáneos en placa; decodificadores eligen canal |
| N5 | Persistencia: marcadores de frecuencia con nombre, sesión guardada (perfil, canal, decodificadores) y registro de eventos consultable por fecha y protocolo (JSONL rotado, API de consulta paginada) | **Sonnet** | Reiniciar conserva el estado; la web busca en el histórico |
| N6 | Grabación mejor: disparo por squelch o por decodificador, límite de disco, reproducir una grabación desde la web con barra de tiempo | **Sonnet** | Test de extremo a extremo; captura en la guía |
| N7 | Acceso remoto seguro: TLS propio no (sin terceros); documentar proxy inverso y añadir autenticación por testigo y límite de clientes; reempaquetado de vídeo a fMP4 + MSE para ver la TV sin contexto seguro | **Sonnet** | TV visible desde otra máquina por HTTP; órdenes de emisión exigen testigo |

### Fase 3 — Decodificadores

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| D1 | VDL2: ecualizador adaptativo (la grabación real tiene mucha interferencia entre símbolos) y validación contra `dumpvdl2` | **Sonnet** | ≥ 90 % de las tramas que saca la referencia |
| D2 | DMR: columnas de Hamming y matriz QR de la norma ETSI, validación con grabación pública | **Sonnet** | Origen y destino coinciden con la referencia |
| D3 | FLEX 3200/6400 (4-FSK y fases múltiples) | **Sonnet** | Igual que `multimon-ng` en grabación real |
| D4 | WiFi OFDM 6–54 Mb/s en el aire y 5 GHz (necesita USB 3) | **Sonnet** | SSID reales en canal 6 y en un canal de 5 GHz |
| D5 | LTE: SIB1 (operador, banda, TAC) y 5G NR: MIB del SSB en n78 (necesita USB 3) | **Sonnet** | Celdas reales con operador identificado |
| D6 | TDT en el aire con la antena de casa: enganche, vídeo y audio estables 10 min; AC-3 y E-AC-3 por WebCodecs; lista de canales por exploración | **Sonnet** + **Opus** (placa) | 10 min sin cortes, desfase A/V < 40 ms |
| D7 | DAB+: audio (según la decisión del usuario), texto DLS y carátulas | **Sonnet** | Se oye una emisora de la grabación real |
| D8 | GSM: BCCH con SI3 (MCC, MNC, LAC, celda). Solo difusión, nada de abonados | **Sonnet** | Operador correcto en grabación real |
| D9 | AIS: mejora PLVA y recepción de los dos canales con un solo DDC ancho | **Sonnet** | Más mensajes que hoy a SNR baja, medido en test |
| D10 | ADS-B: MLAT no; sí CPR local con la posición del receptor, tabla con procedencia y pista en el mapa | **Sonnet** | Posición con un solo mensaje cuando hay ubicación |
| D11 | Protocolos nuevos, uno por agente, con modulador y test como exige `AGENTS.md`: satélites meteorológicos Meteor-M LRPT (imagen), TETRA (solo metadatos de difusión), radiosondas DFM/M10/M20, Iridium (anillo de difusión), NAVTEX | **Sonnet** | Cada uno con test sintético y una grabación real |
| D12 | Fichas de protocolo, capturas y matriz de validación al día tras cada decodificador | **Haiku** | Enlaces sin romper; pies con procedencia real o sintética |

### Fase 4 — Interfaz web

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| W1 | Mapa propio con costa y fronteras vectoriales empaquetadas (sin teselas externas), pistas, filtros por capa y distancia | **Sonnet** | Se reconoce la geografía sin conexión a internet |
| W2 | Vista de señal por decodificador: constelación, ojo y espectro del canal en directo (mensaje binario nuevo, documentado) | **Sonnet** | Visible para al menos PSK, FSK y OFDM |
| W3 | Cascada con historia desplazable, cursores de medida (Δf, ΔdB), marcadores con nombre y exportar PNG | **Sonnet** | Pruebas de la web nuevas en verde |
| W4 | Tema claro, aplicación instalable (PWA sin servicios externos) y disposición móvil con el espectro plegable | **Sonnet** | Contraste AA en los dos temas; pruebas a 360 px |
| W5 | Paneles específicos: celdas (LTE/NR/GSM), redes WiFi, buscas, radiosondas con perfil de altura | **Sonnet** | Cada tabla ordenable, con exportación CSV |
| W6 | Capturas de la guía y del curso regeneradas; `QA_WEB.md` con el estado final | **Haiku** | Sin capturas antiguas |

### Fase 5 — Firmware (FPGA)

Todo HDL: simulación contra el modelo C, temporización con setup ≥ +0,1 ns y
hold > 0 en los 4 modelos, compilación determinista y parche regenerado. La
prueba en placa la hace Opus en RAM.

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| F1 | Integrar el espectro de la FPGA en el motor y la web como fuente alternativa (`rx_mux = 3`, secuencia «late-mux»), con conmutación en caliente y vuelta automática si la imagen no es la lab | **Sonnet** | Cascada a 30 imágenes/s a 61,44 Msps con CPU del host < 5 % |
| F2 | Trama robusta: CRC por trama e indicador de saturación del ADC; el host descarta tramas corruptas | **Sonnet** | Test con errores inyectados en simulación |
| F3 | Espectro y muestras a la vez: canal de espectro diezmado multiplexado con el flujo IQ (hoy es uno u otro) | **Sonnet** | Decodificar y ver el espectro de la FPGA al mismo tiempo en placa |
| F4 | DDC en la FPGA: NCO + CIC + FIR de compensación, 4–8 canales estrechos configurables por Wishbone, entregados empaquetados por USB | **Sonnet** | Los decodificadores estrechos funcionan con el ancho de banda completo sin pasar todo por USB; AIS + ACARS + POCSAG a la vez desde 61,44 Msps |
| F5 | Detector en la FPGA: umbral sobre el espectro acumulado y marca de tiempo de ráfagas, para disparar capturas | **Sonnet** | Ráfagas ISM marcadas con ±1 trama de error |
| F6 | Modo de 122,88 Msps (sobremuestreo, 8 bits) con el motor de espectro y dos canales RX | **Sonnet** | Espectro de 122,88 MHz sin pérdidas con USB 3 |
| F7 | Margen de temporización: segmentar los caminos con menos de +0,2 ns y fijar restricciones; informe de recursos | **Sonnet** | Setup y hold ≥ +0,1 ns en los 4 modelos |
| F8 | Automatizar: `firmware/ci.sh` lanza todas las simulaciones y compara sumas; tabla de registros generada desde un único fichero para HDL, C y documentación | **Haiku** | Una orden; registros sin duplicar a mano |
| F9 | `docs/hardware/FIRMWARE.md`, figuras y módulo del curso al día | **Haiku** | Coincide con el HDL y con las medidas |

### Fase 6 — Emisión y cierre

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| T1 | Calibración real IQ/LO de TX por cable con atenuador (con permiso del usuario) e instalar `tx_set_rx_capture()` en el motor | **Sonnet** + **Opus** (placa) | Rechazo de imagen y de portadora medidos y guardados |
| T2 | Bucle completo por cable: emitir cada modulador y decodificarlo con la propia placa (solo protocolos y bandas de la lista blanca por aire; el resto, por cable) | **Sonnet** + **Opus** | Tabla de los 21 en `docs/calidad/VALIDACION.md` |
| T3 | Revisión de seguridad de la emisión: lista blanca, ciclo de trabajo, ganancia máxima y registro, con tests que intenten saltárselos | **Sonnet** | Ningún test consigue emitir fuera de política |
| Z1 | Curso, README, guía y validación finales; etiqueta de versión `v2.1.0` | **Haiku** (regenerar) + **Opus** (revisar y etiquetar) | Todo coherente con el código |

---

## 3. Orden recomendado de las primeras dos semanas

1. G1 (Sonnet) y R1 + L1 (Haiku) en paralelo → commit y push.
2. Q1 (Haiku), Q2 y Q3 (Sonnet) → a partir de aquí todo pasa por `make ci`.
3. En paralelo: N1 (detector), F1 (espectro de la FPGA en la web), D1 (VDL2), W3 (cascada).
4. Cuando el usuario conecte USB 3: H2 y después D4, D5 y F6.
5. Cuando conecte la antena de TV: D6.

## 4. Riesgos

| Riesgo | Mitigación |
|---|---|
| F4 (DDC en FPGA) no cierra temporización o no cabe | Empezar por 4 canales; hay holgura de recursos (9,5 k ALM usados de 113 k) |
| Protocolos nuevos sin grabaciones públicas | No se dan por validados: quedan como «sintético» en la matriz |
| Carreras difíciles de reproducir | Q2 (TSan) antes de tocar más el host |
| Deriva entre código, documentación y curso | Tareas Haiku de cierre en cada fase (D12, W6, F9, Z1) |

---

## 5. Fase P — Artículo para una revista Q1

Objetivo: un artículo publicable en una revista del primer cuartil (JCR). Corre
en paralelo a las fases 2–5 y reutiliza su trabajo.

### 5.1 Qué se publica (propuesta tras una primera búsqueda bibliográfica)

**Tesis: etiquetado automático de espectro real verificado por decodificación.**
Los conjuntos de datos con los que se entrenan los detectores y clasificadores
de señales son casi todos sintéticos (RadioML, Sig53, WBSig53, CSRD2025) y la
literatura reconoce que con capturas reales «no se puede determinar la verdad
de terreno». Este proyecto puede darla: 21 decodificadores propios que solo
emiten un evento cuando el CRC o el FEC cuadran, sobre una captura de banda
ancha. Cada evento es una etiqueta exacta (protocolo, instante, frecuencia,
ancho, SNR) sin anotación humana.

Contribuciones que el artículo debe sostener con medidas:

1. **Método**: etiquetas tiempo-frecuencia derivadas de decodificaciones
   verificadas, con su precisión y su cobertura medidas (señales inyectadas de
   posición conocida y contraste con decodificadores de referencia).
2. **Sistema abierto y reproducible**: un único binario sin dependencias de
   DSP, con DDC por lotes en GPU y motor de espectro en la FPGA, sobre un SDR
   de menos de 1000 USD.
3. **Conjunto de datos**: capturas reales de varias bandas con etiquetas
   verificadas, publicado con DOI.
4. **Evaluación**: detectores entrenados con datos sintéticos frente a datos
   reales etiquetados así; cuantificar la diferencia.

Trabajos cercanos que hay que diferenciar (encontrados en la primera búsqueda;
P1 debe leerlos y ampliar): WBSig53 y Sig53 (sintéticos), WRIST/SPREAD
(5 tecnologías, etiquetado iterativo con modelo), Tu et al. 2022 (solo ADS-B,
etiquetado por decodificación), RFMLS (WiFi y ADS-B), CorteXforge 2026 (bancos
de pruebas controlados), SigChord/Sums (inspección de cabeceras multibanda).
**Veredicto de P1 (2026-11-20): incremental.** Etiquetar por decodificación con CRC ya está publicado para un solo protocolo (Tu et al. 2022, ADS-B) y como supervisión en un OFDM de laboratorio (Luostari et al. 2026). Lo nuevo es la combinación: muchos protocolos coexistiendo en banda ancha real, etiquetas tiempo-frecuencia con precisión y cobertura medidas, y la brecha sintético→real. El artículo debe matizar que la verdad de terreno real «no se puede» obtener (Fonseca 2020, Uvaydov 2024) citando a Tu, y tratar de frente el **sesgo de selección**: solo se etiqueta lo decodificable, y lo no decodificado no es un negativo. Revista con mejor encaje según lo verificado: IEEE OJ-COMS (cuartil pendiente de confirmar en JCR). Detalle en `paper/related_work.md`.

**La novedad no está confirmada del todo**: si P1 encuentra un trabajo que ya hace
etiquetado por decodificación multiprotocolo en banda ancha, se replantea.

Alternativa de reserva (menos novedosa): artículo de sistema sobre la
arquitectura FPGA + GPU para decodificación multiprotocolo simultánea.

### 5.2 Revistas candidatas

| Revista | Encaje | Cuartil |
|---|---|---|
| IEEE Transactions on Instrumentation and Measurement | Sistema de medida + método de etiquetado | Q1 según la fuente consultada (FI 5,9) |
| IEEE Open Journal of the Communications Society | Datos y aprendizaje para comunicaciones | Probable Q1, sin confirmar |
| Scientific Data | Si el peso está en el conjunto de datos | Sin confirmar |
| Computer Networks | Monitorización de espectro | Sin confirmar |

IEEE Access queda descartada: es Q2. **El cuartil de cada candidata se
comprueba en el JCR vigente antes de elegir** (P0); las cifras de arriba vienen
de páginas de terceros.

### 5.3 Límites éticos y legales (condicionan el diseño)

- Solo metadatos de difusión. No se publica contenido de mensajes privados:
  los textos de buscapersonas (POCSAG, FLEX) y de ACARS se excluyen o se
  sustituyen; de ellos solo queda la etiqueta de protocolo y su posición.
- Identificadores: BSSID y SSID de WiFi son datos personales (RGPD) y se
  anonimizan con una función resumen con sal; MMSI e ICAO son públicos, pero
  se decide en P0 si también se seudonimizan.
- Las muestras IQ de bandas con contenido privado no se publican en bruto si
  permiten recuperar ese contenido: se publica el espectrograma o se excluye
  la banda. Hay que confirmar el marco legal español de captación antes de
  publicar (tarea del usuario; el orquestador no da asesoría legal).
- Uso de IA: casi todas las revistas exigen declarar la asistencia de IA en la
  redacción y el código, y no admiten una IA como autora. La declaración y la
  autoría son decisión y responsabilidad del usuario.

### 5.4 Tareas

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| P0 | Decisiones del usuario: revista (cuartil comprobado en JCR), autoría y afiliación, política de IA, alcance de los datos publicables | **Usuario** + Opus | Decisiones anotadas aquí |
| P1 | Revisión bibliográfica sistemática: 40–60 referencias leídas, tabla comparativa (datos reales o sintéticos, nº de protocolos, origen de las etiquetas, ancho de banda, disponibilidad) y veredicto de novedad | **Sonnet**; Opus comprueba cada cita contra su fuente | `paper/related_work.md` sin citas sin verificar |
| P2 | Exportar etiquetas: cada evento con instante de muestra inicial y final, frecuencia central, ancho ocupado y SNR, escrito como anotaciones SigMF junto a la grabación | **Sonnet** | Los 21 decodificadores lo emiten; test con señales inyectadas en posición conocida |
| P3 | Validar las etiquetas: error de tiempo y frecuencia con señales sintéticas inyectadas sobre ruido real, cobertura frente a decodificadores de referencia, tasa de etiquetas falsas sobre ruido | **Sonnet** | Tabla de precisión y cobertura por protocolo |
| P4 | Campaña de captura: bandas y horarios planificados, con USB 3 y las antenas adecuadas; registro de condiciones | **Opus** (placa) + usuario (antenas) | ≥ 20 h repartidas en ≥ 8 bandas |
| P5 | Anonimización y empaquetado del conjunto de datos según 5.3, con ficha de datos y suma de comprobación | **Sonnet** | Revisión de Opus sin identificadores en claro |
| P6 | Experimentos: detector de referencia entrenado con sintético frente a real (fuera del binario; aquí sí se usan bibliotecas de aprendizaje), con semillas y guiones reproducibles | **Sonnet** | Resultados repetidos con 3 semillas |
| P7 | Figuras y tablas del artículo, generadas por guion | **Haiku** | Una orden las regenera todas |
| P8 | Redacción en LaTeX con la plantilla de la revista | **Opus** | Borrador completo; cada cifra enlazada al guion que la produce |
| P9 | Revisión interna simulando tres revisores y corrección | **Sonnet** (revisores) + **Opus** | Lista de objeciones cerrada |
| P10 | Publicar datos y código con DOI, y enviar | **Usuario** | Enviado |

Dependencias: P2 necesita la fase 1 cerrada; P4 necesita USB 3 (H2) y gana
con F1, F3 y N4; P6 necesita P4 y P5. P1 puede empezar ya.
