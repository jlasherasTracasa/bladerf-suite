"""Cadena de un canal de audio (src/dsp/channel.h)."""
from lib import *


def build():
    W, H = 1720, 700
    c = Canvas(W, H, "Cadena de un canal de audio",
               "Cada navegador tiene su propio canal: escucha otra frecuencia dentro de la misma banda capturada (channel_t).")
    items = [
        ("IQ @ fs", "bloque Q11 crudo", "radio", "ENTRADA", ["±2048 = fondo de escala", "se resta la continua"]),
        ("NCO", "desplaza la emisora", "wave", "1", ["freq − centro → 0 Hz", "mezclador complejo"]),
        ("Diezmado", "en etapas", "layers", "2", ["÷8, ÷8, ÷6…", "hasta la tasa fch"]),
        ("Filtro de canal", "ancho elegido", "filter", "3", ["FIR complejo", "bw del usuario"]),
        ("Nivel y squelch", "S-meter en dBFS", "gauge", "4", ["umbral en dBFS", "NAN = desactivado"]),
        ("Demodulador", "según el modo", "activity", "5", ["WFM · NFM · AM", "USB · LSB · CW"]),
        ("48 kHz → PCM", "remuestreo + audio", "volume", "SALIDA", ["filtro de audio, deénfasis", "int16 mono o estéreo"]),
    ]
    cw, gap, x0, y0, h = 196, 40, 48, 150, 190
    for i, (t, s, ic, tag, ls) in enumerate(items):
        x = x0 + i * (cw + gap)
        gc = "g-bc" if i in (0, 6) else None
        c.rect(x, y0, cw, h, 24, "card", True)
        c.chip_icon(x + 16, y0 + 16, ic, 44, "g-spec" if i == 5 else gc)
        if tag.isdigit():
            c.pill(x + cw - 16, y0 + 38, tag, 12, "pric", "f-onpric", anchor="end", padx=9)
        else:
            c.pill(x + cw - 16, y0 + 38, tag, 10, "secc", "f-var", anchor="end", padx=8)
        c.text(x + 16, y0 + 98, t, 16, 600, "disp")
        c.text(x + 16, y0 + 118, s, 12.5, 500, "f-pri")
        c.lines(x + 16, y0 + 146, ls, 12, 17)
        if i < 6:
            c.arrow([(x + cw + 4, y0 + h / 2), (x + cw + gap - 4, y0 + h / 2)], "pri", marker="ahp")
    # tasas
    ty = 400
    c.text(48, ty, "Tasa de canal mínima y anchura máxima por modo", 17, 600, "disp")
    modes = [("WFM", "≥ 250 kHz", "hasta 220 kHz", "estéreo y RDS caben", "g-bc"),
             ("NFM", "≥ 48 kHz", "hasta 40 kHz", "voz y datos de 12,5 kHz", "g-ham"),
             ("AM", "≥ 24 kHz", "hasta 20 kHz", "banda aérea", "g-av"),
             ("USB · LSB · CW", "≥ 12 kHz", "banda lateral / telegrafía", "voz de radioaficionado", "g-data")]
    mw = (W - 96 - 3 * 24) / 4
    for i, (m, a, b, d, g) in enumerate(modes):
        x = 48 + i * (mw + 24)
        c.add(f'<g class="{g}">')
        c.rect(x, ty + 22, mw, 112, 22, "gtint", 2)
        c.add('</g>')
        c.add(f'<g class="{g}">')
        c.text(x + 20, ty + 56, m, 19, 700, "disp gt")
        c.add('</g>')
        c.text(x + 20, ty + 82, "fch " + a, 14, 600, "f-on")
        c.text(x + 20, ty + 102, "filtro " + b, 12.5, 400, "f-var")
        c.text(x + 20, ty + 120, d, 12.5, 400, "f-var")
    fy = 590
    c.text(48, fy, "Banderas de cada trama", 15, 600, "disp")
    x = 48
    for t in ["1 · squelch abierto", "2 · estéreo", "4 · fuera de banda", "8 · piloto de 19 kHz"]:
        x += c.pill(x, fy + 36, t, 12.5, "secc", "f-on", h=30) + 10
    c.text(W - 48, fy + 41, "Sin audio: el S-meter sale de la trama de espectro (cero trabajo por muestra).", 12.5, 400, "f-var", "end")
    return c
