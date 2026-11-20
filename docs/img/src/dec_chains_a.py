"""dec_chains_a.py — cadenas de procesado (A): adsb, rds, dab, ais, dsc, aprs.
Sale de src/decoders/<id>*.c y de la sección «Cadena DSP» de docs/protocolos/<id>.md.
Formato: id: (título, subtítulo, [(etapa, texto, icono)], salida)."""
D = {}
D["adsb"] = ("ADS-B / Mode S", "src/decoders/adsb.c · WIDE · 2 000 000 muestras/s exactas (½ µs por muestra)", [
    ("Módulo |x|", "Q11 → magnitud; PPM no depende del CFO", "bars"),
    ("Preámbulo", "pulsos en 0; 1; 3,5; 4,5 µs = muestras 0, 2, 7, 9", "search"),
    ("Bits PPM", "bit i: |x[16+2i]| frente a |x[17+2i]|, con confianza", "wave"),
    ("CRC-24", "polinomio 0xFFF409; síndrome 0 = trama válida", "check2"),
    ("Corrección", "1 y 2 bits dudosos; DF nunca se toca; CRC deslizante opcional", "wrench"),
    ("Campos ME", "type code: ident., posición, velocidad, emergencia", "list"),
    ("CPR", "par par/impar → lat/lon globales; filtro de saltos", "map"),
    ("Tabla de aeronaves", "caduca a 60 s; evento solo en novedad", "database"),
], "evento new|callsign|squawk|emerg  +  dec_state.ac[]")
D["rds"] = ("RDS (FM comercial)", "src/decoders/rds.c · NARROW · 250 000 muestras/s", [
    ("Discriminador FM", "±75 kHz = ±1; el CFO pasa a continua en el MPX", "wave"),
    ("Mezcla −57 kHz", "FIR Kaiser 60 dB (±2,4 kHz) y diezmado ÷10 → 25 kS/s", "filter"),
    ("Costas BPSK", "lazo de portadora, Bn = 40 Hz", "repeat"),
    ("Filtro adaptado", "un periodo de seno por bit (1187,5 bit/s)", "sliders"),
    ("Reloj de Gardner", "21,05 muestras/bit; tolera ±100 ppm", "clock"),
    ("Diferencial", "decisión dura, bit = cambio de signo", "branch"),
    ("Sincronía de bloque", "síndrome con offsets A, B, C/C', D en 26 bits", "search"),
    ("Grupos", "0A/0B PS, 2A/2B RT, 4A CT, 10A PTYN; texto 2 veces seguido", "list"),
], "evento (PI, PS, PTY, RT, CT, AF)  +  dec_state")
D["dab"] = ("DAB / DAB+", "src/decoders/dab.c · WIDE · 2 048 000 muestras/s · modo I", [
    ("Símbolo nulo", "suma móvil de |x|² < 40 % de la media", "scan"),
    ("Prefijo cíclico", "sincronía fina ±700 muestras, 4 símbolos", "search"),
    ("CFO", "fraccional con el CP; entero con el PRS", "gauge"),
    ("FFT 2048 + D-QPSK", "d = R_l·conj(R_{l-1}); quita el giro conocido", "layers"),
    ("Desentrelazado", "tiempo y frecuencia; decisiones blandas por amplitud", "repeat"),
    ("Viterbi K=7", "despunzonado con borrados y desaleatorizado del FIC", "branch"),
    ("CRC-16 por FIB", "cada FIB se valida por separado", "check2"),
    ("FIG 0/0, 0/1, 0/2, 1/x", "conjunto, subcanales, servicios y etiquetas", "list"),
], "evento ensemble/servicio  +  dec_state (sin audio)")
D["ais"] = ("AIS", "src/decoders/ais.c · NARROW · 48 000 muestras/s · GMSK 9600 bd", [
    ("Discriminador FM", "fmdisc, ±1 = ±2400 Hz (GMSK es FM)", "wave"),
    ("FIR paso bajo", "25 coeficientes, corte 6,5 kHz", "filter"),
    ("Quita CFO", "media móvil de 48 símbolos ponderada por |x|²", "sliders"),
    ("5 fases de reloj", "cada una con NRZI + HDLC en paralelo", "clock"),
    ("HDLC", "bandera 0x7E, relleno tras cinco 1", "box"),
    ("FCS CRC-16/X-25", "gana la fase que la valida; duplicados FNV < 0,25 s", "check2"),
    ("Tipo y longitud", "1-3, 4, 5, 9, 11, 18, 19, 21, 24, 27 coherentes", "list"),
    ("Campos M.1371", "MMSI, posición 1/10000 min, rumbo; NMEA !AIVDM", "map"),
], "evento por mensaje (+ nmea)  +  dec_state.ships[]")
D["dsc"] = ("DSC VHF canal 70", "src/decoders/dsc.c · NARROW · 48 000 muestras/s · FSK 1200 bd", [
    ("Discriminador FM", "fmdisc", "wave"),
    ("Paso alto + FIR 2,7 kHz", "quita el CFO y el ruido fuera de banda", "filter"),
    ("Correladores AFSK", "1300 / 2100 Hz, 1200 bd, ganancias 0,7/1,4", "sliders"),
    ("20 seguidores", "10 alineaciones de símbolo × 2 polaridades", "clock"),
    ("Fase de entrada", "símbolo 104 con ≥ 3 de 111…105 y ≥ 3 × 125", "search"),
    ("Símbolos DX/RX", "hueco DX; si falla su comprobación, su copia RX", "repeat"),
    ("ECC y EOS", "el ECC debe cuadrar; sin él no se emite", "check2"),
    ("Formato y MMSI", "socorro, rutina…; posición y naturaleza del socorro", "list"),
], "evento por llamada  +  dec_state")
D["aprs"] = ("APRS / AX.25", "src/decoders/aprs.c + afsk.c · NARROW · 48 000 muestras/s · Bell 202", [
    ("Discriminador FM", "audio; paso alto ~70 Hz quita el CFO", "wave"),
    ("FIR 3,3 kHz", "paso bajo del audio", "filter"),
    ("3 demoduladores", "correladores 1200/2200 Hz con g = 0,6 / 1 / 1,7", "sliders"),
    ("PLL de reloj", "por demodulador, inercia 0,75, ±1000 ppm", "clock"),
    ("NRZI + HDLC", "bandera 0x7E y relleno de ceros", "box"),
    ("CRC AX.25", "gana quien la valida; duplicados FNV < 1 s", "check2"),
    ("Cabecera AX.25", "destino, origen, digipeaters; solo UI / PID F0", "tag"),
    ("Información APRS", "posición, estado, mensajes, meteo", "map"),
], "evento por trama  +  dec_state.stations[]")
