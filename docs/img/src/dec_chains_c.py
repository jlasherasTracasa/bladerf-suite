"""dec_chains_c.py — cadenas de procesado (C): lora, meshtastic, lte, nr, gsm, wifi, dvbt, tdt."""
D = {}
D["lora"] = ("LoRa PHY / LoRaWAN", "src/decoders/lora.c + lora_phy.c · NARROW · 500 000 muestras/s · SF7-12, BW 125/250", [
    ("Canal a 2·BW", "FIR Kaiser de diezmado", "filter"),
    ("Dechirp + FFT 2N", "detector por SF; bins k y k+N", "layers"),
    ("Preámbulo", "≥ 4 ventanas con el mismo bin, pico/media ≥ 9", "search"),
    ("SFD", "chirp descendente; ε = (q_up+q_down)/2", "scan"),
    ("Sincronía fina ×3", "CFO < 5 Hz, SFO ±5 ppm", "gauge"),
    ("Símbolos", "ventana −m, FFT N, Jacobsen, lazo PI", "clock"),
    ("Gray + Hamming", "desentrelazado diagonal, codeword más cercano", "repeat"),
    ("Cabecera, blanqueo, CRC", "LFSR 0xFF; CRC-16; LoRaWAN MHDR/DevAddr", "check2"),
], "evento por paquete (SF, BW, CR, CRC, carga)  +  dec_state")
D["meshtastic"] = ("Meshtastic", "src/decoders/meshtastic.c + aes.c · NARROW · 500 000 muestras/s · LoRa + sync 0x2B", [
    ("Receptor LoRa", "el de lora.md con SF y BW del preset", "wave"),
    ("Cabecera 16 B", "to, from, id, flags, hash de canal, next_hop, relay", "tag"),
    ("Hash de canal", "XOR(nombre) ⊕ XOR(clave) = 8 para LongFast", "check2"),
    ("AES-128-CTR", "solo con la clave pública por defecto", "key"),
    ("Protobuf Data", "válido o se emite con encrypted:true", "lock"),
    ("portnum", "texto, posición, nodeinfo, telemetría", "list"),
    ("Tabla de nodos", "nombre, posición, batería, entorno", "database"),
], "evento por paquete  +  dec_state.nodes[]")
D["lte"] = ("LTE (búsqueda de celda)", "src/decoders/lte.c + lte_phy.c · NARROW · 1 920 000 muestras/s", [
    ("Ventana 48 ms", "92 160 muestras cada period s", "scan"),
    ("PSS", "ZC 62, raíces 25/29/34; NCC plegada a 5 ms", "search"),
    ("CFO grueso", "barrido ±10 kHz", "gauge"),
    ("SSS", "168 N_ID1 × hipótesis FDD/TDD; PCI = 3·N_ID1 + N_ID2", "calc"),
    ("CRS", "RSRP, SINR, puertos; pendiente de fase", "bars"),
    ("CFO fino", "correlación del CP, ±50 Hz", "gauge"),
    ("PBCH", "QPSK, Gold, Viterbi tail-biting, CRC-16 por puertos", "check2"),
    ("MIB", "ancho de banda, PHICH, SFN", "list"),
], "evento por celda (PCI, RSRP, CFO, MIB)  +  dec_state")
D["nr"] = ("5G NR (SSB)", "src/decoders/nr.c + nr_phy.c + nr_corr.c · WIDE · ≥ 15 360 000 muestras/s", [
    ("Rejilla GSCN", "posiciones candidatas dentro de la banda capturada", "grid"),
    ("Diezmado a 7,68 Msps", "mezcla y FIR Kaiser 70 dB", "filter"),
    ("PSS por FFT", "6 plantillas: N_ID2 × SCS 30/15; R = pico/mediana ≥ 45", "search"),
    ("CFO grueso", "barrido ±15 kHz", "gauge"),
    ("SSS", "336 hipótesis; umbral 0,45; valida la celda", "calc"),
    ("CFO fino", "fase entre PSS y SSS", "gauge"),
    ("Potencia", "RSRP por RE y SNR aproximados", "bars"),
    ("PCI", "3·N_ID1 + N_ID2", "tag"),
], "evento por celda (PCI, GSCN, SCS, RSRP)  +  dec_state")
D["gsm"] = ("GSM (difusión)", "src/decoders/gsm.c + gsm_phy.c · NARROW · 1 083 333 muestras/s · GMSK", [
    ("FIR ±170 kHz", "33 coeficientes", "filter"),
    ("FCCH", "tono a +67 708 Hz; M = |Σz|/Σ|z| ≈ 1", "search"),
    ("CFO", "lag 4 grueso, lag 64 fino (±8 kHz)", "gauge"),
    ("SCH", "1 trama (5000 muestras) después; posición por LS", "scan"),
    ("MLSE 16 estados", "max-log-MAP sobre la secuencia de entrenamiento", "sliders"),
    ("Viterbi K=5", "78 bits + paridad de 10 bits", "check2"),
    ("BSIC y FN", "T1, T2, T3' → número de trama", "tag"),
    ("BCCH SI3/SI4", "4 ráfagas, código Fire; MCC, MNC, LAC, Cell ID", "list"),
], "evento por celda (BSIC, FN, SI3)  +  dec_state")
D["wifi"] = ("WiFi 802.11 (balizas)", "src/decoders/wifi.c + wifi_phy.c · WIDE · 20 000 000 muestras/s exactas", [
    ("Detector de energía", "bloques de 1 µs; +6 dB sobre el suelo", "scan"),
    ("Schmidl-Cox (STF)", "M = |P|²/R² > 0,5; CFO grueso ±625 kHz", "search"),
    ("LTF", "correlación cruzada y CFO fino (retardo 64)", "gauge"),
    ("Canal y pilotos", "H(k), ecualización, fase θ₀ + b·k", "sliders"),
    ("SIGNAL", "BPSK 1/2, Viterbi, paridad, RATE y LENGTH", "check2"),
    ("Prefijo del PSDU", "si no es baliza ni probe-resp se descarta", "ban"),
    ("Viterbi K=7 + FCS", "desentrelazado, desaleatorizado, CRC-32", "branch"),
    ("IEs públicos", "BSSID, SSID, canal, tasas, RSN; DSSS 1 Mb/s sin STF", "list"),
], "evento por baliza  +  dec_state.aps[]")
D["dvbt"] = ("DVB-T (TPS y diagnóstico)", "src/decoders/dvbt.c + dvbt_phy.c · NARROW · 9 142 857 muestras/s (64/7 MHz)", [
    ("Segmento 1,5 Msps", "≈ 164 ms cada 0,5 s", "scan"),
    ("Modo y guarda", "correlación del CP (van de Beek), 8 combinaciones", "search"),
    ("CFO fraccional", "−arg(Σ x[n]·x*[n+N])", "gauge"),
    ("CFO entero y reloj", "pilotos continuos ±571 kHz; remuestreo > 20 ppm", "clock"),
    ("TPS", "DBPSK sobre portadoras TPS; fase común e inclinación", "layers"),
    ("Sincronía + BCH", "≤ 4 errores en la palabra; BCH corrige ≤ 2", "check2"),
    ("Parámetros", "constelación, tasa, guarda, modo, cell id", "tag"),
    ("MER", "ecualización con pilotos dispersos (diagnóstico)", "bars"),
], "evento por múltiplex (TPS, MER)  +  dec_state")
D["tdt"] = ("TDT: del IQ a la imagen", "src/decoders/tdt.c + src/tv/ · DVB-T 8k 8 MHz · receptor propio", [
    ("Sincronía OFDM 8k", "CP, CFO, ventana", "wave"),
    ("Pilotos y TPS", "ecualización con continuos y dispersos", "sliders"),
    ("Demapeo blando", "QPSK / 16 / 64-QAM → LLR", "grid"),
    ("Desentrelazados", "de símbolo y de bit", "repeat"),
    ("Viterbi + Forney + RS", "1/2…7/8; RS(204,188), I = 12", "check2"),
    ("TS 188 B", "desaleatorizado PRBS; ts.c, continuidad", "box"),
    ("PSI", "PAT, PMT, SDT, NIT (LCN), CRC-32", "database"),
    ("PES, mp2, tv.c", "H.264 a WebCodecs; MP2 propio a PCM", "video"),
], "JSON «tv» + binarios 5 (vídeo) y 6 (audio)  →  navegador")
