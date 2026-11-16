"""Banner del README: nombre, lema y una cascada estilizada."""
import random, math
from lib import *


def build():
    W, H = 1280, 400
    c = Canvas(W, H)
    # fondo con degradado tonal (primario → terciario muy suave)
    c.add('<defs><linearGradient id="bgg" x1="0" y1="0" x2="1" y2="1">'
          '<stop offset="0" style="stop-color:var(--pric);stop-opacity:.55"/>'
          '<stop offset=".55" style="stop-color:var(--bg);stop-opacity:0"/>'
          '<stop offset="1" style="stop-color:var(--terc);stop-opacity:.5"/></linearGradient></defs>')
    c.add(f'<rect width="{W}" height="{H}" fill="url(#bgg)"/>')
    # --- cascada (derecha)
    px, py, pw, ph = 700, 48, 532, 304
    c.rect(px, py, pw, ph, 28, "card", True)
    rnd = random.Random(11)
    cols, rows = 66, 22
    cw = (pw - 32) / cols
    sx, sy = px + 16, py + 128
    ch = (ph - 128 - 16) / rows
    sigs = [(8, 6, .9), (19, 3, .7), (27, 9, 1.0), (38, 2, .55), (46, 5, .8), (55, 3, .65), (60, 2, .5)]
    levels = []
    for r in range(rows):
        row = []
        for k in range(cols):
            v = rnd.random() * 1.6
            for (c0, wd, a) in sigs:
                d = abs(k - c0 - math.sin(r * .35 + c0) * .6)
                if d < wd:
                    on = (r * 7 + c0 * 3) % 11 > 2 or wd > 5
                    if on:
                        v += a * (1 - d / wd) * (4.2 + rnd.random() * 1.6)
            row.append(min(7, int(v)))
        levels.append(row)
    for r in range(rows):
        for k in range(cols):
            lv = levels[r][k]
            c.add(f'<rect class="w{lv}" x="{sx + k * cw:.1f}" y="{sy + r * ch:.1f}" width="{cw + .4:.1f}" height="{ch + .4:.1f}"/>')
    # curva del espectro (última fila suavizada)
    base = py + 112
    pts = []
    last = levels[0]
    for k in range(cols):
        v = (last[max(0, k - 1)] + last[k] * 2 + last[min(cols - 1, k + 1)]) / 4
        pts.append((sx + k * cw + cw / 2, base - v * 11))
    d = "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in pts)
    c.add(f'<path d="{d} L{pts[-1][0]:.1f} {base} L{pts[0][0]:.1f} {base}Z" style="fill:var(--pri);fill-opacity:.18"/>')
    c.add(f'<path d="{d}" style="fill:none;stroke:var(--pri);stroke-width:2.2;stroke-linejoin:round"/>')
    for g in range(0, 5):
        gx = sx + g * (pw - 32) / 4
        c.add(f'<line x1="{gx:.1f}" y1="{py + 26}" x2="{gx:.1f}" y2="{base}" style="stroke:var(--outlv);stroke-width:1"/>')
    for i, t in enumerate(["47 MHz", "", "", "", "6 GHz"]):
        if t:
            c.text(sx + i * (pw - 32) / 4 + (0 if i == 0 else -0), py + 40, t, 12, 500, "f-var", "start" if i == 0 else "end")
    # --- texto (izquierda)
    c.add(icon("radio", 48, 70, 40, "var(--pri)", 1.9))
    c.text(100, 100, "SDR · bladeRF 2.0 micro", 15, 600, "f-pri", spacing=1.2)
    c.add(f'<text class="disp" x="44" y="186" font-size="84" font-weight="700" letter-spacing="-2.5">BladeRF Lab</text>')
    c.text(48, 232, "Del IQ al protocolo, desde cero en C.", 25, 500, "disp f-on")
    c.text(48, 264, "Espectro, audio, 21 decodificadores y emisión controlada,", 16.5, 400, "f-var")
    c.text(48, 288, "todo en un solo binario y manejado desde el navegador.", 16.5, 400, "f-var")
    x = 48
    for t, cls in [("C11 + POSIX", "secc"), ("CUDA opcional", "secc"), ("sin dependencias de terceros", "secc")]:
        x += c.pill(x, 332, t, 13, cls, "f-on", h=32) + 8
    return c
