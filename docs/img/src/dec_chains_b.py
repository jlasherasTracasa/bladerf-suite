"""dec_chains_b.py — cadenas de procesado (B): pocsag, flex, dmr, acars, vdl2, ism, rs41."""
D = {}
D["pocsag"] = ("POCSAG", "src/decoders/pocsag.c + pocsag_fsk.h · NARROW · 48 000 muestras/s", [
    ("FIR de canal", "41 coeficientes, corte 9,5 kHz", "filter"),
    ("Discriminador FM", "fmdisc", "wave"),
    ("3 receptores", "512 / 1200 / 2400 bd en paralelo", "layers"),
    ("Filtro adaptado", "media por bit y reloj de Gardner", "clock"),
    ("Sincronía", "≤ 2 errores, patrón o complemento = polaridad", "search"),
    ("BCH(31,21)", "corrige ≤ 2 bits y paridad par", "check2"),
    ("Lotes y direcciones", "8 tramas × 2 palabras; RIC y función", "list"),
    ("Mensaje", "numérico o alfanumérico; fin al faltar la sincronía", "tag"),
], "evento por mensaje (RIC, función, texto)  +  dec_state")
D["flex"] = ("FLEX", "src/decoders/flex.c · NARROW · 48 000 muestras/s · 1600/3200/6400 bd", [
    ("FIR de canal", "corte 10,5 kHz", "filter"),
    ("Discriminador FM", "±4800 Hz = ±1", "wave"),
    ("2 receptores", "1600 y 3200 bd, filtro boxcar + Gardner", "layers"),
    ("SEARCH", "palabra A = código + 0xA6C6, ≤ 3 errores", "search"),
    ("FIW", "32 bits: BCH, suma y ciclo ≤ 14", "check2"),
    ("DATA", "símbolos blandos del modo; margen para Sync 2", "repeat"),
    ("Desentrelazado", "mejor desplazamiento: Σ(10 − errores), ≥ 6 válidas", "sliders"),
    ("BIW, direcciones, vectores", "solo mensajes con todas las palabras válidas", "list"),
], "evento por mensaje alfanumérico  +  dec_state")
D["dmr"] = ("DMR (solo metadatos)", "src/decoders/dmr.c + dmr_fec.c · NARROW · 48 000 muestras/s · 4FSK 4800 bd", [
    ("FIR de canal", "corte 8,5 kHz", "filter"),
    ("Discriminador FM", "±1944 Hz = ±1", "wave"),
    ("RRC β = 0,2", "span 8; adaptado al pulso", "sliders"),
    ("Reloj de Gardner", "sps = 10; adquisición dirigida por la sincronía", "clock"),
    ("Dibits", "umbrales 0 y ±2/3 con seguidores de pico", "box"),
    ("Sincronía", "ventana de 24 símbolos contra 8 patrones (≤ 5 bits)", "search"),
    ("CACH, tipo, BPTC", "ranura, tipo de ranura, FEC y CRC/RS", "check2"),
    ("LC", "origen, destino, CC; sin voz", "tag"),
], "evento por ráfaga (CC, ranura, LC)  +  dec_state")
D["acars"] = ("ACARS", "src/decoders/acars.c + acars_blk.c · NARROW · 48 000 muestras/s · AM + MSK 2400", [
    ("Envolvente AM", "a = |x|/media − 1, inmune al CFO", "bars"),
    ("Mezcla −1800 Hz", "FIR de 41 coef., corte 1500 Hz: MSK ≈ FSK ±600 Hz", "filter"),
    ("Reloj de bit", "20 productos y[n]·conj(y[n−1]) + DLL (0,15)", "clock"),
    ("MSDD", "16 hipótesis de 4 bits, coherente; gana 2-3 dB", "sliders"),
    ("Diferencial", "2400 Hz = bit igual, 1200 Hz = cambia", "branch"),
    ("Sincronía", "+* SYN SYN SOH, 40 bits, ≤ 3 errores", "search"),
    ("BCS y paridad", "caracteres LSB primero hasta ETX/ETB", "check2"),
    ("Campos del bloque", "modo, aeronave, etiqueta, n.º de mensaje, texto", "list"),
], "evento por bloque  +  dec_state")
D["vdl2"] = ("VDL Modo 2", "src/decoders/vdl2*.c · NARROW · 105 000 muestras/s · D8PSK 10 500 bd", [
    ("Detector de energía", "bloques de 256 muestras, suelo de ruido adaptativo", "scan"),
    ("CFO grueso", "centroide espectral (±6,25 kHz)", "gauge"),
    ("RRC adaptado", "10 muestras por símbolo", "sliders"),
    ("Correlación con la UW", "diferencial, independiente de CFO y fase; fija el tiempo", "search"),
    ("CFO fino", "fase de esa correlación (±5,25 kHz)", "gauge"),
    ("Gardner + Farrow", "cúbico, absorbe ±100 ppm", "clock"),
    ("D8PSK diferencial", "3 bits por símbolo con seguimiento de fase", "layers"),
    ("Cabecera, RS, HDLC", "aleatorizador x¹⁵+x¹⁴+1; Reed-Solomon; AVLC", "check2"),
], "evento AVLC / ACARS sobre AVLC  +  dec_state")
D["ism"] = ("ISM 433/868 MHz", "src/decoders/ism.c + ism_protocols.c · NARROW · 250 000 muestras/s", [
    ("Envolvente", "potencia media de 8 muestras", "bars"),
    ("Umbral adaptativo", "suelo y pico; histéresis 0,55/0,40; pk > 2,5·nf", "sliders"),
    ("Ráfaga", "empieza al superar el umbral; termina tras 12 ms", "scan"),
    ("OOK o FSK", "FSK: CFO, paso bajo ±55 kHz, discriminador retardo 3", "branch"),
    ("Tren de pulsos", "marcas y espacios, mediana de 5", "wave"),
    ("Protocolos conocidos", "PWM / PPM / Manchester; suma o CRC", "check2"),
    ("Analizador genérico", "estilo URH si se repite y nada encaja", "search"),
    ("Dispositivo", "modelo, id, temperatura, humedad; tabla con TTL", "database"),
], "evento sensor|generic  +  dec_state (tabla de dispositivos)")
D["rs41"] = ("Radiosonda Vaisala RS41", "src/decoders/rs41.c + rs41_frame.c · NARROW · 48 000 muestras/s · GFSK 4800 bd", [
    ("FIR ±9 kHz", "paso bajo", "filter"),
    ("Discriminador FM", "fmdisc", "wave"),
    ("Filtro adaptado", "media de 10 muestras (NRZ)", "sliders"),
    ("Umbral móvil", "media móvil: absorbe el CFO", "gauge"),
    ("Cabecera de 64 bits", "10 fases a la vez, ≤ 10 errores, también invertida", "search"),
    ("312 octetos", "lazo temprano/tardío; XOR de desaleatorizado", "clock"),
    ("CRC y Reed-Solomon", "bloques con CRC; si fallan, RS y se revalida", "check2"),
    ("Subbloques", "posición GPS, altura, PTU, número de serie", "list"),
], "evento por trama  +  dec_state")
