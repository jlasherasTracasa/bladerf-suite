#!/usr/bin/env python3
"""gen_decoders.py — diagramas de la cadena de procesado de cada decodificador.

Escribe docs/img/src/decoders/<id>-cadena.html (SVG en línea, tema Material 3 de
lib.py). Luego: docs/img/render_decoders.sh -> docs/img/decoders/<id>-cadena-{light,dark}.png
Datos de las cadenas en dec_chains_{a,b,c}.py (salen del código de src/decoders/).
Uso: python3 docs/img/src/gen_decoders.py [id ...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import Canvas, ICONS, tw, icon, HERE  # noqa: E402
import dec_chains_a, dec_chains_b, dec_chains_c  # noqa: E402

OUT = os.path.join(HERE, "decoders")
D = {}
for m in (dec_chains_a, dec_chains_b, dec_chains_c):
    D.update(m.D)
GRP = {"adsb": "av", "acars": "av", "vdl2": "av", "rds": "bc", "dab": "bc", "dvbt": "bc", "tdt": "bc",
       "ais": "mar", "dsc": "mar", "aprs": "ham", "pocsag": "data", "flex": "data", "dmr": "data",
       "ism": "data", "rs41": "data", "lora": "data", "meshtastic": "data",
       "lte": "spec", "nr": "spec", "gsm": "spec", "wifi": "spec"}


def wrap(s, w, size):
    out, cur = [], ""
    for word in s.split(" "):
        t = (cur + " " + word).strip()
        if tw(t, size) <= w or not cur:
            cur = t
        else:
            out.append(cur)
            cur = word
    out.append(cur)
    return out


def build(pid):
    title, sub, stages, outp = D[pid]
    n = len(stages)
    cols = 4
    cw, ch, gx, gy = 268, 138, 46, 56
    rows = (n + cols - 1) // cols
    W = 96 + cols * cw + (cols - 1) * gx
    H = 150 + rows * ch + (rows - 1) * gy + 110
    c = Canvas(W, H, title + ": cadena de procesado", sub)
    g = "g-" + GRP[pid]
    c.add(f'<g class="{g}">')
    c.pill(48, 136, "IQ", 12.5, "gsolid", "f-onpri", h=28, padx=14, weight=600)
    c.add('</g>')
    c.text(106, 141, "→ DDC / host de decodificadores → decodificador", 12.5, 400, "f-var")
    for i, (t, s, ic) in enumerate(stages):
        r, k = divmod(i, cols)
        x = 48 + k * (cw + gx)
        y = 160 + r * (ch + gy)
        c.rect(x, y, cw, ch, 22, "card", True)
        c.add(f'<g class="{g}"><circle class="gtint" cx="{x + 34}" cy="{y + 34}" r="18"/>'
              f'<text x="{x + 34}" y="{y + 39.5}" text-anchor="middle" font-size="15" font-weight="700" class="gt">{i + 1}</text></g>')
        c.add(f'<g class="{g}">' + icon(ic if ic in ICONS else "wave", x + cw - 44, y + 22, 26, "var(--g)", 2) + '</g>')
        tl = wrap(t, cw - 112, 15)[:2]
        if len(tl) == 1:
            c.text(x + 62, y + 40, tl[0], 15, 600, "disp")
            yy = y + 76
        else:
            c.text(x + 62, y + 32, tl[0], 15, 600, "disp")
            c.text(x + 62, y + 50, tl[1], 15, 600, "disp")
            yy = y + 82
        for line in wrap(s, cw - 36, 12.5)[:3]:
            assert yy < y + ch - 6, (pid, t)
            c.text(x + 18, yy, line, 12.5, 400, "f-var")
            yy += 18
        if k < cols - 1 and i < n - 1:
            c.arrow([(x + cw + 4, y + ch / 2), (x + cw + gx - 4, y + ch / 2)], "pri", marker="ahp")
        elif k == cols - 1 and i < n - 1:
            ym = y + ch + gy / 2
            y2 = 160 + (r + 1) * (ch + gy)
            c.arrow([(x + cw / 2, y + ch + 3), (x + cw / 2, ym), (48 + cw / 2, ym), (48 + cw / 2, y2 - 3)], "pri", marker="ahp")
    yo = 160 + rows * ch + (rows - 1) * gy + 24
    c.add(f'<g class="{g}">')
    c.pill(48, yo + 14, "Salida", 12.5, "gsolid", "f-onpri", h=28, padx=14, weight=600)
    c.add('</g>')
    c.text(124, yo + 19, outp, 13.5, 500, "f-on")
    src = ("Dibujado a partir de src/decoders/adsb.c y docs/ADSB.md." if pid == "adsb" else
           "Dibujado a partir del código de src/ y de la sección «Cadena DSP» de docs/decoders/%s.md." % pid)
    c.text(48, yo + 52, src, 11.5, 400, "f-var")
    return c


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for pid in D:
        if sys.argv[1:] and pid not in sys.argv[1:]:
            continue
        c = build(pid)
        with open(os.path.join(OUT, pid + "-cadena.html"), "w", encoding="utf-8") as f:
            f.write(c.html(pid + " cadena"))
        print(pid, c.w, c.h)
