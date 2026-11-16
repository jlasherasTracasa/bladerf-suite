#!/usr/bin/env python3
"""doc_decoders.py — inserta (o actualiza) la sección «Visión rápida» al principio de cada
docs/decoders/<id>.md (ADS-B: docs/ADSB.md, TDT: docs/TDT.md): diagrama de la cadena,
gráficas de la señal, ejemplo de evento y de dec_state (sacados de `--replay … --print` y del
WebSocket, guardados en build/doc2a/events/) y, si existe, la captura de la web.
La sección va entre marcas HTML y se regenera sin tocar el resto del documento.
Uso: python3 -I docs/img/src/doc_decoders.py"""
import json, os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
EV = os.path.join(ROOT, "build", "doc2a", "events")
IMG = os.path.join(ROOT, "docs", "img", "decoders")
INI = "<!-- vision-rapida:inicio (generada por docs/img/src/doc_decoders.py) -->"
FIN = "<!-- vision-rapida:fin -->"

# procedencia del ejemplo de evento
PROC = {
    "adsb": "señal sintética de `--gen adsb` (no hay grabación real a 1090 MHz)",
    "rds": "señal sintética de `--gen rds` (RDS real validado aparte con `redsea`, sin grabación IQ)",
    "dab": "grabación IQ real (sigidwiki «DAB», ensemble 5001)",
    "ais": "audio FM real (sigidwiki «AIS Sound», Long Beach) remodulado en FM",
    "dsc": "señal sintética de `--gen dsc` (no hay grabación real de DSC)",
    "aprs": "señal sintética de `--gen aprs`",
    "pocsag": "audio FM real de multimon-ng (POCSAG 1200 bps) remodulado en FM",
    "flex": "audio FM real de multimon-ng (FLEX 1600, P2000) remodulado en FM",
    "dmr": "señal sintética de `--gen dmr` (la grabación real de sigidwiki no se valida)",
    "acars": "grabación IQ real (sigidwiki «ACARS IQ»)",
    "vdl2": "señal sintética de `--gen vdl2` (la grabación real sigue sin decodificarse)",
    "ism": "señal sintética de `--gen ism` (los .cu8 reales de rtl_433_tests dan los mismos modelos)",
    "rs41": "audio FM real de rs1729/RS (sonda RS41, 2014) remodulado",
    "lora": "referencia generada con la herramienta externa loratx (no es una captura en el aire)",
    "meshtastic": "señal sintética de `--gen meshtastic`",
    "lte": "grabación IQ real (sigidwiki «LTE 1,4 MHz»)",
    "nr": "grabación IQ real (sigidwiki «NR 10 MHz»): no aparece ninguna celda, así que no hay evento",
    "gsm": "grabación IQ real (sigidwiki «GSM downlink BCCH»)",
    "wifi": "grabación IQ real (sigidwiki «Wi-Fi», balizas 802.11b DSSS)",
    "dvbt": "grabación real del C32 de Pamplona: con la SNR de ≈ −9 dB el receptor engancha pero en el `--replay` del fragmento no se emite evento",
    "tdt": "sin evento JSON propio: el decodificador `tdt` publica el mensaje `tv` y los binarios 5 y 6 por WebSocket",
}
CMD = {
    "adsb": "bladerf-lab --replay adsb.sigmf-meta --decode adsb --print",
    "rds": "bladerf-lab --replay rds.sigmf-meta --decode rds@100300000 --print",
    "dab": "bladerf-lab --replay dab.sigmf-meta --decode dab --print",
    "ais": "bladerf-lab --replay ais.sigmf-meta --decode ais@161975000 --print",
    "dsc": "bladerf-lab --replay dsc.sigmf-meta --decode dsc@156525000 --print",
    "aprs": "bladerf-lab --replay aprs.sigmf-meta --decode aprs@144800000 --print",
    "pocsag": "bladerf-lab --replay pocsag.sigmf-meta --decode pocsag@466075000 --print",
    "flex": "bladerf-lab --replay flex.sigmf-meta --decode flex@466075000 --print",
    "dmr": "bladerf-lab --replay dmr.sigmf-meta --decode dmr@438500000 --print",
    "acars": "bladerf-lab --replay acars.sigmf-meta --decode acars@131550000 --print",
    "vdl2": "bladerf-lab --replay vdl2.sigmf-meta --decode vdl2@136975000 --print",
    "ism": "bladerf-lab --replay ism.sigmf-meta --decode ism@433920000 --print",
    "rs41": "bladerf-lab --replay rs41.sigmf-meta --decode rs41@402500000 --print",
    "lora": "bladerf-lab --replay lora.sigmf-meta --decode lora@868100000 --print",
    "meshtastic": "bladerf-lab --replay meshtastic.sigmf-meta --decode meshtastic@869525000 --print",
    "lte": "bladerf-lab --replay lte.sigmf-meta --decode lte@800000000 --print",
    "nr": "bladerf-lab --replay nr.sigmf-meta --decode nr --print",
    "gsm": "bladerf-lab --replay gsm.sigmf-meta --decode gsm@947000000 --print",
    "wifi": "bladerf-lab --replay wifi.sigmf-meta --decode wifi --print",
    "dvbt": "bladerf-lab --replay c32_frag.cs8 --rate 9142857.142857 --freq 562000000 --decode dvbt@562000000 --print",
}

# gráficas: (fichero sin sufijo, pie corto)
FIG = {
    "adsb": [("adsb-trama", "Módulo de una trama DF17 a 2 Msps: preámbulo y 112 bits PPM. Señal sintética."), ("adsb-estructura", "Campos de la trama DF17 y del ME de posición.")],
    "rds": [("rds-senal", "Espectro MPX con piloto y subportadora de 57 kHz, y constelación BPSK. Señal sintética."), ("rds-estructura", "Estructura de los grupos 0A y 2A.")],
    "dab": [("dab-senal", "Símbolo nulo, espectro y constelación D-QPSK de una grabación real."), ("dab-estructura", "Trama de transmisión del modo I y FIB.")],
    "ais": [("ais-ojo", "Cascada de la grabación (audio real remodulado) y diagrama de ojo GMSK de una señal generada."), ("ais-estructura", "Ráfaga AIS y mensaje 1/2/3.")],
    "dsc": [("dsc-senal", "Cascada y frecuencia instantánea FSK de 1200 bd. Señal sintética."), ("dsc-estructura", "Secuencia de símbolos de una llamada.")],
    "aprs": [("aprs-senal", "Cascada y audio con los tonos Bell 202. Señal sintética."), ("aprs-estructura", "Trama AX.25 UI e información de posición.")],
    "pocsag": [("pocsag-senal", "Cascada y diagrama de ojo FSK a 1200 bd (audio real remodulado)."), ("pocsag-estructura", "Transmisión, lote y palabra POCSAG.")],
    "flex": [("flex-senal", "Cascada y histograma FSK de 2 niveles (audio real remodulado)."), ("flex-estructura", "Trama FLEX.")],
    "dmr": [("dmr-senal", "Cascada de la grabación real y niveles 4FSK de una señal generada."), ("dmr-estructura", "Ráfaga TDMA y metadatos.")],
    "acars": [("acars-senal", "Cascada y envolvente AM de la grabación real."), ("acars-estructura", "Bloque ACARS.")],
    "vdl2": [("vdl2-senal", "Cascada real y constelación D8PSK diferencial, sintética y real."), ("vdl2-estructura", "Ráfaga D8PSK y trama AVLC.")],
    "ism": [("ism-senal", "Envolvente OOK real y un tren de pulsos con el umbral adaptativo."), ("ism-estructura", "Esquema de un sensor meteorológico OOK.")],
    "rs41": [("rs41-senal", "Cascada y diagrama de ojo GFSK 4800 bd de la grabación real."), ("rs41-estructura", "Trama de 312 octetos.")],
    "lora": [("lora-senal", "Cascada de chirps y picos tras el dechirp (referencia de loratx)."), ("lora-estructura", "Paquete LoRa y cabecera explícita.")],
    "meshtastic": [("meshtastic-senal", "Cascada y picos tras el dechirp (SF11, BW 250 kHz). Señal sintética."), ("meshtastic-estructura", "Cabecera de 16 bytes y nonce de AES-CTR.")],
    "lte": [("lte-pss", "Espectro y picos de correlación con la PSS cada 5 ms (grabación real)."), ("lte-estructura", "Posición de PSS, SSS y PBCH.")],
    "nr": [("nr-pss", "Espectro de la grabación real y correlación con la PSS en una señal generada."), ("nr-estructura", "Bloque SSB e identidad de celda.")],
    "gsm": [("gsm-fcch", "Cascada y detector de FCCH sobre la grabación real."), ("gsm-estructura", "Multitrama de 51 y ráfaga normal.")],
    "wifi": [("wifi-senal", "Cascada y ráfagas de balizas 802.11b en la grabación real."), ("wifi-estructura", "PPDU y trama Beacon.")],
    "dvbt": [("dvbt-senal", "Espectro y correlación del prefijo cíclico reales del C32, y por qué 64-QAM a −9 dB no demodula (simulada)."), ("dvbt-estructura", "Símbolo OFDM 8k y bits TPS.")],
    "tdt": [("dvbt-senal", "La señal real del C32 de Pamplona (misma figura que en dvbt.md)."), ("tdt-estructura", "Paquete TS y pila de tablas."), ("tdt-estructura2", "Capas de la TDT.")],
}
NAMES = {"adsb": "ADS-B", "rds": "RDS", "dab": "DAB", "ais": "AIS", "dsc": "DSC", "aprs": "APRS", "pocsag": "POCSAG", "flex": "FLEX",
         "dmr": "DMR", "acars": "ACARS", "vdl2": "VDL2", "ism": "ISM", "rs41": "RS41", "lora": "LoRa", "meshtastic": "Meshtastic",
         "lte": "LTE", "nr": "NR", "gsm": "GSM", "wifi": "WiFi", "dvbt": "DVB-T", "tdt": "TDT"}


def pic(prefix, name, alt):
    return (f'<picture>\n  <source media="(prefers-color-scheme: dark)" srcset="{prefix}{name}-dark.png">\n'
            f'  <img src="{prefix}{name}-light.png" alt="{alt}">\n</picture>\n')


def trim(o, depth=0):
    if isinstance(o, dict):
        return {k: trim(v, depth + 1) for k, v in o.items()}
    if isinstance(o, list):
        r = [trim(v, depth + 1) for v in o[:2]]
        if len(o) > 2:
            r.append(f"… ({len(o) - 2} más)")
        return r
    if isinstance(o, str) and len(o) > 90:
        return o[:90] + "…"
    return o


def first_event(pid):
    p = os.path.join(EV, pid + ".jsonl")
    if not os.path.exists(p):
        return None
    for ln in open(p, encoding="utf-8"):
        try:
            m = json.loads(ln)
        except Exception:
            continue
        if m.get("type") == "dec":
            return m
    return None


def section(pid, prefix):
    n = NAMES[pid]
    out = [INI, "", "## Visión rápida", ""]
    out.append(pic(prefix, f"{pid}-cadena", f"Cadena de procesado de {n}"))
    out.append(f"*Figura. Cadena de procesado de {n}, dibujada a partir del código de `src/` y de la sección «Cadena DSP» de este documento.*\n")
    out.append("### Así es la señal\n")
    for name, cap in FIG[pid]:
        if os.path.exists(os.path.join(IMG, name + "-light.png")):
            out.append(pic(prefix, name, cap))
            out.append(f"*Figura. {cap}*\n")
    out.append("La procedencia de cada señal (grabación real o sintética) y qué parte del procesado es Python de ilustración y no el código C se indican en el pie de cada imagen.\n")
    out.append("### Ejemplo de evento y de estado\n")
    out.append(f"Procedencia: {PROC[pid]}. Sacado de ejecutar el decodificador, no escrito a mano" + (f" (`{CMD[pid]}`)." if pid in CMD else ".") + "\n")
    ev = first_event(pid)
    if ev:
        out.append("Evento `dec` (campo `msg`, listas largas recortadas):\n")
        out.append("```json\n" + json.dumps(trim(ev["msg"]), ensure_ascii=False, indent=2) + "\n```\n")
    else:
        out.append("Esta grabación no produce ningún evento `dec`.\n")
    sp = os.path.join(EV, pid + ".state.json")
    if os.path.exists(sp):
        st = json.load(open(sp, encoding="utf-8"))
        out.append("Último `dec_state` recibido por WebSocket (`state`, listas recortadas):\n")
        out.append("```json\n" + json.dumps(trim(st.get("state", st)), ensure_ascii=False, indent=2) + "\n```\n")
    web = os.path.join(IMG, pid + "-web.png")
    if os.path.exists(web):
        out.append("### En la web\n")
        out.append(f'![{n} en la interfaz web]({prefix}{pid}-web.png)\n')
        out.append(f"*Captura de la interfaz con un servidor `--replay` propio mostrando {n}.*\n")
    out.append(FIN)
    return "\n".join(out) + "\n"


def target(pid):
    if pid == "adsb":
        return os.path.join(ROOT, "docs", "ADSB.md"), "img/decoders/"
    if pid == "tdt":
        return os.path.join(ROOT, "docs", "TDT.md"), "img/decoders/"
    return os.path.join(ROOT, "docs", "decoders", pid + ".md"), "../img/decoders/"


def main():
    for pid in NAMES:
        path, prefix = target(pid)
        s = open(path, encoding="utf-8").read()
        sec = section(pid, prefix)
        if INI in s:
            s = re.sub(re.escape(INI) + r".*?" + re.escape(FIN) + r"\n", lambda m: sec, s, flags=re.S)
        else:
            i = s.index("\n") + 1
            s = s[:i] + "\n" + sec + "\n" + s[i:].lstrip("\n")
        open(path, "w", encoding="utf-8").write(s)
        print("ok", path)


if __name__ == "__main__":
    main()
