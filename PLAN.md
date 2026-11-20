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

**Cerrada el 2026-11-20.** `make ci` da los 9 pasos en verde en ~15 min. Queda
anotado: `test_auto` falla de vez en cuando bajo TSan (sensibilidad de tiempos
de `dec_host_auto_sync`, sin informe de carrera); se revisa en N2.

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

#### Fase 4b — Experiencia de uso (UI/UX)

Añadida el 2026-11-20. W1–W5 añaden funciones; estas tareas hacen que la
aplicación se entienda y se maneje bien. Empieza por W7: su resultado ordena el
resto. Todo sigue sin bibliotecas ni recursos externos.

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| W7 | Auditoría de uso: 12 tareas reales («escuchar una emisora», «ver aviones en el mapa», «grabar y reproducir», «decodificar esta señal»…) guionizadas en las pruebas de la web, midiendo clics, tiempo y errores; lista priorizada de fricciones | **Sonnet** (guiones) + **Opus** (revisión con capturas) | `docs/calidad/UX.md` con la línea base y los objetivos por tarea; las medidas se repiten en `make webtest` |
| W8 | Manipulación directa del espectro: clic para sintonizar, rueda con pasos por modo, arrastrar los bordes del filtro, zoom y desplazamiento con inercia, doble clic centra; atajos de teclado y paleta de órdenes (Ctrl+K) | **Sonnet** | Todas las tareas de W7 de sintonía se hacen sin tocar un campo de texto; hoja de atajos en la ayuda |
| W9 | «¿Qué es esta señal?»: al pulsar una señal detectada se proponen decodificadores por banda, ancho y forma espectral, con un clic para añadirlos; eventos con búsqueda, filtros, fijar, panel de detalle y copiar/exportar | **Sonnet** | De señal detectada a primer mensaje decodificado en ≤ 2 clics |
| W10 | Estado y errores comprensibles: barra única (fuente, enlace USB 2/3, pérdidas, GPU, carga por decodificador), avisos no bloqueantes con la acción que los resuelve, `waiting`/`error` explicados, reconexión que restaura sesión y audio | **Sonnet** | Ningún error se muestra solo como texto crudo del servidor; cortar y volver la red no pierde el estado |
| W11 | Primer uso: recorrido guiado de 6 pasos, estados vacíos que dicen qué hacer, glosario emergente (dBFS, squelch, CFAR…) enlazado a la guía, modo demostración con una grabación incluida cuando no hay placa | **Sonnet** | Una persona sin experiencia en radio completa 3 tareas de W7 sin leer la documentación (lo valida el usuario) |
| W12 | Fluidez: cascada y espectro en WebGL dentro de un *worker* (OffscreenCanvas) con alternativa 2D, tablas virtualizadas con 10 000 eventos, memoria acotada tras 8 h | **Sonnet** | 60 imágenes/s a 4K con CPU del navegador < 25 % de un núcleo; sin crecimiento de memoria en la prueba larga |
| W13 | Accesibilidad: todo manejable con teclado, foco visible, roles ARIA, respeto de «reducir movimiento», paletas de cascada aptas para daltonismo, zoom al 200 % sin solapes | **Sonnet** | Recorrido completo con teclado en las pruebas; contraste AA medido |
| W14 | Disposición: paneles redimensionables y plegables, disposiciones guardadas por perfil, mapa y TV en ventana aparte, pantalla completa de la cascada | **Sonnet** | La disposición sobrevive a recargar; pruebas a 360, 768, 1440 y 3840 px |
| W15 | Asistente de emisión seguro: explica la política antes de pedir datos, muestra el espectro previsto de la señal y la banda permitida, deja claro `file`/`loopback`/`air`, confirmación explícita para aire | **Sonnet** | Imposible llegar a «emitir» sin ver modo, frecuencia, duración y motivo de un rechazo |
| W16 | Panel de hardware y FPGA: imagen cargada y capacidades, fuente del espectro (CPU, GPU, FPGA), contadores de la FPGA (F17), temperatura y caudal USB en una gráfica | **Sonnet** | Refleja en vivo los registros de la imagen lab; con imagen de serie lo indica sin errores |
| W17 | Idiomas: textos en un diccionario (`web/i18n/es.json`, `en.json`), selector y formato local de números y fechas | **Haiku** | Ningún texto fijo en el HTML/JS; inglés completo |
| W18 | Sistema de diseño: variables de color, tipografía y espaciado únicas, componentes (botón, tabla, pestaña, aviso) documentados en una página `web/estilo.html` | **Haiku** (inventario) + **Sonnet** (unificación) | Sin colores ni tamaños sueltos en el CSS; capturas de referencia para regresión visual |

### Fase 5 — Firmware (FPGA)

Todo HDL: simulación contra el modelo C, temporización con setup ≥ +0,1 ns y
hold > 0 en los 4 modelos, compilación determinista y parche regenerado. La
prueba en placa la hace Opus en RAM.

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| F1 | Integrar el espectro de la FPGA en el motor y la web como fuente alternativa (`rx_mux = 3`, secuencia «late-mux»), con conmutación en caliente y vuelta automática si la imagen no es la lab | **Sonnet** | Cascada a 30 imágenes/s a 61,44 Msps con CPU del host < 5 % |
| F1✔ | Probado en placa el 2026-11-20 (RAM, USB 2, 30,72 Msps): 20 imágenes/s, 0 huecos, nivel medio igual que la CPU, FFT y fps reconfigurables, vuelta a CPU correcta. CPU del servidor 146 % → 10 % de un núcleo | — | — |
| F1b | Bajar de 10 % a < 5 % de CPU en modo FPGA (perfilar lectura USB y conversión de tramas) y evitar el `sync_rx: Operation timed out` que aparece al reconfigurar fps | **Sonnet** + **Opus** (placa) | < 5 % medido en placa; 10 reconfiguraciones seguidas sin error |
| F1b✔ | Parcial (2026-11-20, placa): CPU 10 % → 7 %; `sync_rx` al reconfigurar pasa de 1 de cada 2 a 1 de cada 30. El intérprete de tramas ya cuesta 0,1 %: el resto está en libusb/libbladeRF | — | — |
| F1c | Lectura del modo espectro con búferes y transferencias dimensionados al caudal real (~0,5 MB/s, no 30 Msps), y reconfiguración de fps escribiendo solo el registro K sin parar el flujo | **Sonnet** + **Opus** (placa) | < 5 % de CPU; 30 reconfiguraciones sin `sync_rx` |
| F2 | Trama robusta: CRC por trama e indicador de saturación del ADC; el host descarta tramas corruptas | **Sonnet** | Test con errores inyectados en simulación |
| F3 | Espectro y muestras a la vez: canal de espectro diezmado multiplexado con el flujo IQ (hoy es uno u otro) | **Sonnet** | Decodificar y ver el espectro de la FPGA al mismo tiempo en placa |
| F4 | DDC en la FPGA: NCO + CIC + FIR de compensación, 4–8 canales estrechos configurables por Wishbone, entregados empaquetados por USB | **Sonnet** | Los decodificadores estrechos funcionan con el ancho de banda completo sin pasar todo por USB; AIS + ACARS + POCSAG a la vez desde 61,44 Msps |
| F5 | Detector en la FPGA: umbral sobre el espectro acumulado y marca de tiempo de ráfagas, para disparar capturas | **Sonnet** | Ráfagas ISM marcadas con ±1 trama de error |
| F6 | Modo de 122,88 Msps (sobremuestreo, 8 bits) con el motor de espectro y dos canales RX | **Sonnet** | Espectro de 122,88 MHz sin pérdidas con USB 3 |
| F7 | Margen de temporización: segmentar los caminos con menos de +0,2 ns y fijar restricciones; informe de recursos | **Sonnet** | Setup y hold ≥ +0,1 ns en los 4 modelos |
| F8 | Automatizar: `firmware/ci.sh` lanza todas las simulaciones y compara sumas; tabla de registros generada desde un único fichero para HDL, C y documentación | **Haiku** | Una orden; registros sin duplicar a mano |
| F9 | `docs/hardware/FIRMWARE.md`, figuras y módulo del curso al día | **Haiku** | Coincide con el HDL y con las medidas |

#### Fase 5b — Firmware: más funciones en la FPGA

Añadida el 2026-11-20. Mismas reglas que la fase 5 (simulación contra modelo C,
temporización, solo RAM, nunca la flash). Orden sugerido: F10 y F11 primero
(dan seguridad y caudal a todo lo demás); F14 y F15 dependen de F5.

| ID | Tarea | Modelo | Terminado cuando |
|---|---|---|---|
| F10 | Registro de capacidades y telemetría: versión, mapa de bits de funciones, contadores (desbordes de FIFO, saturación del ADC, tramas emitidas y perdidas); el host negocia y funciona igual con la imagen de serie | **Sonnet** | `lab_probe --regs` y el motor leen las capacidades; ninguna función se activa a ciegas |
| F11 | Empaquetado de muestras: modo de 12 bits empaquetados (3 bytes por muestra IQ en vez de 4) y modo de 8 bits con ganancia digital, para sacar más ancho de banda por el mismo USB | **Sonnet** | +33 % de Msps sostenidos medidos en placa (≈ 11 → ≈ 15 Msps en USB 2); decodificadores sin pérdida de sensibilidad medible |
| F12 | Motor de espectro configurable: tamaño de FFT 256–8192, ventana (Hann, Blackman-Harris) por ROM, media exponencial, retención de máximo y de mínimo por bin, tasa de imágenes programable | **Sonnet** | Cada modo coincide bit a bit con el modelo C; selector en la web (W16) |
| F13 | Corrección en la FPGA antes de todo lo demás: bloqueo de continua, corrección de desequilibrio IQ con coeficientes del host y ganancia digital con detección de saturación | **Sonnet** | Pico de continua e imagen ≥ 20 dB más bajos en placa que sin corregir |
| F14 | Captura con predisparo: memoria circular en BRAM de las últimas N muestras; al dispararse el detector (F5) se envía la ráfaga completa con su marca de tiempo, sin transmitir el resto | **Sonnet** | Ráfagas ISM de 433 MHz capturadas enteras desde 61,44 Msps con USB 2 |
| F15 | Barrido dirigido por la FPGA: el secuenciador cambia de frecuencia con los perfiles de quick-tune y etiqueta cada trama de espectro con su frecuencia; el host solo compone el panorama | **Sonnet** + **Opus** (placa) | 47 MHz–6 GHz en < 1 s por pasada, sin tramas mal etiquetadas |
| F16 | Demodulador ADS-B en la FPGA: detección de preámbulo y recorte de bits a 2 Msps equivalentes, entrega de tramas de 112 bits con nivel y marca de tiempo; el CRC y lo demás siguen en el host | **Sonnet** | Mismas tramas que el decodificador de CPU en la grabación real ±1 %; CPU del host ≈ 0 |
| F17 | Emisión desde la FPGA: reproductor en bucle de una forma de onda guardada en BRAM, generador de tono y de barrido para calibrar (T1), y emisión programada por marca de tiempo. Solo se prueba en `loopback` o por cable con permiso | **Sonnet** + **Opus** (placa) | Tono y barrido usados por la calibración de TX; nada sale por antena en las pruebas |
| F18 | Verificación más dura: bancos autoverificables con estímulos aleatorios y semilla, aserciones en las interfaces (válido/listo, cruces de reloj), cobertura de registros y revisión de cruces de dominio | **Sonnet** | Cada bloque nuevo con ≥ 1 000 tramas aleatorias sin discrepancias; informe de cruces de reloj sin avisos sin justificar |
| F19 | Compilación y carga cómodas: `firmware/build.sh` reproducible con suma publicada, imagen comprimida, tiempo de carga medido y orden `--fpga-lab` del servidor que la carga en RAM solo con confirmación | **Haiku** (guiones) + **Sonnet** (servidor) | Dos compilaciones dan la misma suma; la web muestra qué imagen hay y nunca escribe la flash |

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
6. Experiencia de uso: W7 (auditoría) en cuanto cierre la fase 1; después W8,
   W9 y W10, que son las que más cambian el manejo diario.
7. FPGA: F10 y F11 justo después de F1; el resto de la fase 5b tras F5.

## 4. Riesgos

| Riesgo | Mitigación |
|---|---|
| F4 (DDC en FPGA) no cierra temporización o no cabe | Empezar por 4 canales; hay holgura de recursos (9,5 k ALM usados de 113 k) |
| Protocolos nuevos sin grabaciones públicas | No se dan por validados: quedan como «sintético» en la matriz |
| Carreras difíciles de reproducir | Q2 (TSan) antes de tocar más el host |
| F11 (12 bits empaquetados) exige tocar el camino FX3/libbladeRF, que no se modifica | Empaquetar dentro del flujo de muestras de la FPGA y desempaquetar en `src/sdr/`; si no es viable, queda solo el modo de 8 bits |
| F16/F17 amplían mucho el HDL y pueden romper temporización | Cada bloque detrás de su bit de capacidad (F10) y desactivable en síntesis |
| Las mejoras de UX se juzgan a ojo | W7 fija medidas (clics, tiempo, errores) y se repiten en `make webtest` |
| Deriva entre código, documentación y curso | Tareas Haiku de cierre en cada fase (D12, W6, F9, Z1) |

---

## 5. Fase P — Artículo para una revista Q1

Objetivo: un artículo publicable en una revista del primer cuartil (JCR). Corre
en paralelo a las fases 2–5 y reutiliza su trabajo.

### 5.1 Qué se publica (tesis revisada el 2026-11-20 tras la revisión P1)

P1 dejó la idea original («etiquetar con decodificaciones verificadas») como
incremental: el mecanismo ya existe para un protocolo (Tu et al. 2022) y su
punto débil es el sesgo de selección. La tesis nueva convierte ese punto débil
en la contribución.

**Tesis: verdad de terreno calibrada y consciente del sesgo para espectro real.**
Una etiqueta por decodificación solo dice «aquí hay X»; no dice cuánto se ha
quedado sin etiquetar. Este proyecto puede medirlo porque tiene, para los
mismos 21 protocolos, **el modulador y el decodificador emparejados**: se
inyectan señales conocidas sobre capturas reales y se mide cuándo el
etiquetador las encuentra.

Contribuciones que el artículo debe sostener con medidas:

1. **Etiquetado en tres niveles**: positivo verificado (CRC/FEC correcto),
   energía detectada sin decodificar («desconocido», no negativo) y vacío
   verificado (por debajo del umbral del detector). Ningún conjunto de datos
   revisado en P1 distingue el segundo nivel.
2. **Calibración por inyección**: probabilidad de etiquetado de cada protocolo
   en función de la SNR, de la interferencia y de la ocupación, medida
   inyectando señales de los moduladores propios en fondo real; con ella cada
   captura lleva una estimación de su cobertura, y cada etiqueta su error de
   tiempo y de frecuencia.
3. **Aprendizaje con el sesgo corregido**: usar esa probabilidad como
   propensión (aprendizaje positivo-no etiquetado, ponderación inversa) al
   entrenar un detector, y demostrar que mejora frente a tratar lo no
   etiquetado como fondo, evaluado sobre verdad de terreno inyectada y sobre
   otro receptor.

**Veredicto P1b (2026-11-20, `paper/related_work.md` §9):** (1) novedosa con
cautela: no hay conjunto de RF con «energía sin decodificar» como nivel propio;
(2) incremental: la inyección y recuperación ya existe en SETI (Margot 2023,
Choza 2023), lo nuevo es llevarla a comunicaciones con 21 moduladores
emparejados; (3) PU con propensión es un método conocido (Bekker 2019) y nuevo
en RF, así que el artículo **necesita una ganancia empírica clara** frente al
entrenamiento ingenuo. Pendiente: búsqueda en IEEE Xplore y verificar 7 citas.
4. **Sistema y datos abiertos**: binario único reproducible sobre un SDR de
   menos de 1000 USD y conjunto de datos con DOI, con las tres clases de
   etiqueta y las curvas de calibración.

Trabajos que hay que diferenciar: los de `paper/related_work.md` (Tu 2022,
WRIST/SPREAD, Uvaydov 2024, Fonseca 2020, Luostari 2026, WBSig53, CorteXforge).
**Pendiente de comprobar (P1b)**: si ya hay trabajos de aprendizaje
positivo-no etiquetado, de sesgo de selección de etiquetas o de calibración por
inyección aplicados a conjuntos de datos de RF. Si los hay, la contribución 3
se ajusta; las 1 y 2 se sostienen por los moduladores emparejados.

Alternativa de reserva: artículo de sistema sobre la arquitectura FPGA + GPU.

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
| P1b | Búsqueda dirigida: aprendizaje positivo-no etiquetado, sesgo de selección de etiquetas y calibración por inyección en RF; mismo rigor que P1 | **Sonnet** | Sección nueva en `paper/related_work.md` con veredicto sobre la tesis revisada |
| P3a | Piloto sin tocar `src/`: curvas de probabilidad de decodificación frente a SNR para 4–5 protocolos, inyectando `--gen` sobre ruido y sobre una grabación real | **Sonnet** | `paper/exp/piloto/` con guion reproducible, curvas y conclusión de viabilidad |
| P3a✔ | Resultado (2026-11-20, `paper/exp/piloto/PILOTO.md`): viable. Punto del 50 % con ±0,2–0,5 dB; el fondo real desplaza las curvas de forma distinta por protocolo (AIS +5,7 dB, APRS +7,4 dB, POCSAG −4,8 dB). 0 falsos positivos sobre ruido salvo POCSAG con copias presentes. Defectos de método a corregir en P3: SNR medida por copia y en local (no global sobre un fondo corto en bucle, que causa el suelo del 17–22 % de AIS/ADS-B y probablemente la mejora aparente de POCSAG), fondos reales largos y varias semillas | — | — |
| P3c | POCSAG: techo del ~94 % de recuperación a 25 dB (fallos deterministas) y mensajes con contenido erróneo que pasan el BCH cuando hay varias copias; causa raíz y arreglo en el decodificador, con test | **Sonnet** | ≥ 99,5 % a 15 dB en el test y 0 mensajes erróneos en la rejilla del piloto |
| P2 | Exportar etiquetas en tres niveles (requisitos de P3a: índice de muestra en `--print`, aviso de pérdida por `drops`, SNR estimada por el decodificador): evento verificado con muestra inicial y final, frecuencia, ancho y SNR; regiones del detector sin decodificar como «desconocido»; resto como vacío. Anotaciones SigMF | **Sonnet** | Los 21 decodificadores lo emiten; test con señales inyectadas en posición conocida |
| P2b | Inyector: herramienta que suma a una captura real señales de los moduladores con SNR, instante y frecuencia sorteados y guarda su verdad de terreno | **Sonnet** | Test: lo inyectado se recupera donde se puso |
| P3 | Calibración completa: probabilidad de etiquetado por protocolo frente a SNR, interferencia y ocupación; error de tiempo y frecuencia; falsos positivos sobre ruido; contraste con decodificadores de referencia | **Sonnet** | Curvas y tabla por protocolo con intervalos de confianza |
| P4 | Campaña de captura: bandas y horarios planificados, con USB 3 y las antenas adecuadas; registro de condiciones | **Opus** (placa) + usuario (antenas) | ≥ 20 h repartidas en ≥ 8 bandas |
| P5 | Anonimización y empaquetado del conjunto de datos según 5.3, con ficha de datos y suma de comprobación | **Sonnet** | Revisión de Opus sin identificadores en claro |
| P6 | Experimentos: detector entrenado (a) tratando lo no etiquetado como fondo, (b) con corrección por propensión, (c) solo con sintético; evaluado en verdad inyectada y en otro receptor (fuera del binario; aquí sí se usan bibliotecas de aprendizaje), con semillas y guiones reproducibles | **Sonnet** | Resultados repetidos con 3 semillas |
| P7 | Figuras y tablas del artículo, generadas por guion | **Haiku** | Una orden las regenera todas |
| P8 | Redacción en LaTeX con la plantilla de la revista | **Opus** | Borrador completo; cada cifra enlazada al guion que la produce |
| P9 | Revisión interna simulando tres revisores y corrección | **Sonnet** (revisores) + **Opus** | Lista de objeciones cerrada |
| P10 | Publicar datos y código con DOI, y enviar | **Usuario** | Enviado |

Dependencias: P1b y P3a pueden empezar ya; P2 y P2b necesitan la fase 1 cerrada; P4 necesita USB 3 (H2) y gana
con F1, F3 y N4; P6 necesita P4 y P5. P1 puede empezar ya.
