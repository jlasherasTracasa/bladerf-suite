"""Cadena TDT: OFDM 8k → TS → demux → H.264 (WebCodecs) + MP2 → AudioWorklet (docs/protocolos/tdt.md)."""
from lib import *


def stepcards(c, x0, y, w, gap, h, steps, ic_cls=None, arrows=True):
    for i, (t, s, ic) in enumerate(steps):
        x = x0 + i * (w + gap)
        c.rect(x, y, w, h, 22, "card", True)
        c.chip_icon(x + 14, y + 14, ic, 38, ic_cls)
        c.text(x + 14, y + 74, t, 14.5, 600, "disp")
        for j, l in enumerate(s):
            c.text(x + 14, y + 94 + j * 17, l, 11.5, 400, "f-var")
        if arrows and i < len(steps) - 1:
            c.arrow([(x + w + 3, y + h / 2), (x + w + gap - 3, y + h / 2)], "pri", marker="ahp")


def build():
    W, H = 1740, 860
    c = Canvas(W, H, "Cadena TDT: de la señal OFDM a la imagen y al sonido",
               "DVB-T 8 MHz, 8k (ETSI EN 300 744). Receptor, demux y audio propios; solo el vídeo H.264 lo decodifica el navegador (WebCodecs).")
    c.pill(48, 130, "Receptor dvbt_rx · src/tv/ · IQ a 64/7 MHz", 12.5, "pric", "f-onpric", h=28, padx=14)
    stepcards(c, 48, 156, 206, 34, 150, [
        ("Sincronía OFDM", ["8k, prefijo cíclico", "CFO y ventana"], "wave"),
        ("Ecualización", ["pilotos continuos y", "dispersos · TPS"], "sliders"),
        ("Demapeo blando", ["QPSK · 16 · 64-QAM", "bits blandos al Viterbi"], "grid"),
        ("Desentrelazados", ["de símbolo y de bit", "(interno)"], "repeat"),
        ("Viterbi AVX2", ["1/2 … 7/8 perforado", "≈ 100 Mb/s"], "zap"),
        ("Forney + RS", ["I = 12 · RS(204,188)", "externo"], "shield"),
        ("Desaleatorizado", ["PRBS x¹¹+x²+1", "→ paquetes TS 188 B"], "box"),
    ], "g-bc")
    # fila 2
    c.pill(48, 346, "Demux y reproductor · TV2", 12.5, "pric", "f-onpric", h=28, padx=14)
    r2 = [("ts.c", ["sync 0x47 ×4, continuidad", "PCR, TEI"], "list"),
          ("psi.c", ["PAT · PMT · SDT · NIT (LCN)", "CRC-32, texto DVB → UTF-8"], "database"),
          ("pes.c", ["reensamblado, PTS/DTS", "unidades de acceso H.264"], "layers"),
          ("tv.c", ["servicio activo, reloj", "JSON «tv» + binarios 5 y 6"], "tv")]
    stepcards(c, 48, 376, 240, 40, 128, r2, "g-bc")
    c.arrow([(1530, 306), (1530, 330), (48 + 103, 330), (48 + 103, 376)], "pri", marker="ahp", label="TS", lpos=.12) if False else None
    c.arrow([(1594 - 41, 306), (1594 - 41, 322), (278, 322), (278, 376)], "pri", marker="ahp", label="paquetes TS", lpos=.5)
    # nota honesta
    c.rect(1200, 376, 492, 128, 22, "card2", True)
    c.chip_icon(1216, 392, "check2", 38, "g-spec")
    c.text(1266, 417, "Estado de la validación", 14.5, 600, "disp")
    c.lines(1216, 454, ["gr-dtv: 4918 paquetes TS idénticos (64-QAM 2/3, 8k).",
                        "En el aire (Pamplona): TPS sí; TS no, la SNR con la",
                        "antena actual es ≈ −9 dB y 64-QAM pide ≈ 16 dB."], 12, 17)
    # paneles
    py, ph = 574, 190
    px1, px2, pw = 48, 884, 808
    for px, nm, ic, g in [(px1, "Vídeo", "video", "g-av"), (px2, "Audio", "volume", "g-ham")]:
        c.rect(px, py, pw, ph, 26, "card2", True)
        c.add(f'<g class="{g}">')
        c.pill(px + 24, py + 28, nm, 12.5, "gtint", "gt", h=28, padx=14, weight=600)
        c.add('</g>')
    stepcards(c, px1 + 24, py + 48, 230, 40, 120, [
        ("WS binario tipo 5", ["H.264 Annex B", "PTS/DTS, claves con SPS+PPS"], "send"),
        ("WebCodecs", ["VideoDecoder", "aceleración del navegador"], "cpu"),
        ("Canvas", ["se presenta cuando", "su PTS ≤ reloj de audio"], "monitor"),
    ], "g-av")
    stepcards(c, px2 + 24, py + 48, 230, 40, 120, [
        ("mp2.c", ["Layer II propio", "MPEG-1 y 2 LSF"], "wave"),
        ("WS binario tipo 6", ["PCM int16 + PTS", "de la 1.ª muestra"], "send"),
        ("AudioWorklet", ["tv-worklet.js", "devuelve el PTS que suena"], "volume"),
    ], "g-ham")
    c.arrow([(1008, 504), (1008, 540), (450, 540), (450, py)], "pri", marker="ahp", label="PES de vídeo", lpos=.5)
    c.arrow([(1008, 540), (1290, 540), (1290, py)], "pri", marker="ahp", label="PES de audio", lpos=.5)
    # reloj
    c.arrow([(px2 + 24 + 2 * 270 + 115, py + ph + 4), (px2 + 24 + 2 * 270 + 115, 800), (px1 + 24 + 2 * 270 + 115, 800), (px1 + 24 + 2 * 270 + 115, py + ph + 4)], "dash", marker="ah")
    c.label(1000, 800, "el audio es el reloj: sin audio, reloj propio con 120 ms de retardo", 12)
    c.text(48, 836, "Solo H.264 y MP2 se reproducen; MPEG-2, HEVC, AAC, AC-3 y E-AC-3 se listan sin decodificar. No se descifra ningún contenido (CA).", 12, 400, "f-var")
    return c
