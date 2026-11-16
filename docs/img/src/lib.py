#!/usr/bin/env python3
"""lib.py — pequeña biblioteca para dibujar los diagramas de la documentación.

Cada diagrama es una página HTML con un único SVG en línea. Los colores salen de
variables CSS (paleta tonal de Material Design 3, la misma que la web de la app):
la misma fuente se renderiza en claro y en oscuro (render.sh añade ?dark).

Las anchuras de texto se miden con las fuentes Inter reales (PIL) para que los
rótulos, las píldoras y las flechas queden donde deben sin solaparse.
"""
import html
import os
import re
from PIL import ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

_FONTS = {}


def _font(weight, size, mono=False):
    key = (weight, size, mono)
    if key not in _FONTS:
        if mono:
            path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
            if not os.path.exists(path):
                path = "/usr/share/fonts/opentype/inter/Inter-Regular.otf"
        else:
            name = {400: "Regular", 500: "Medium", 600: "SemiBold", 700: "Bold"}[weight]
            path = f"/usr/share/fonts/opentype/inter/Inter-{name}.otf"
        _FONTS[key] = ImageFont.truetype(path, size * 4)
    return _FONTS[key]


def tw(s, size=14, weight=400, mono=False):
    """Anchura en px de un texto."""
    return _font(weight, size, mono).getlength(s) / 4.0


def esc(s):
    return html.escape(s, quote=False)


# ---------------------------------------------------------------- paleta
GROUPS = {
    # clave: (nombre, oscuro, claro)
    "bc": ("Radiodifusión", "#D0BCFF", "#6A4FB3"),
    "av": ("Aviación", "#8ECDF7", "#0A6AA6"),
    "mar": ("Marítimo", "#7ED6C4", "#08695B"),
    "data": ("Datos / ISM", "#F7C873", "#8A5700"),
    "ham": ("Radioaficionado", "#F2A7C3", "#B0286A"),
    "spec": ("Espectro / telefonía", "#A6E3A1", "#26702B"),
}

CSS = """
:root{
  --bg:#FEF7FF; --card:#F3EDF7; --card2:#ECE6F0; --card3:#E6E0E9;
  --on:#1D1B20; --var:#49454F; --outline:#CAC4D0; --outlv:#E0D9E3;
  --pri:#6750A4; --onpri:#FFFFFF; --pric:#EADDFF; --onpric:#21005D;
  --sec:#625B71; --secc:#E8DEF8; --terc:#FFD8E4; --ter:#7D5260;
  --arrow:#6F6A78; --sh:.16; --ok:#26702B; --warn:#8A5700; --err:#B3261E;
  --w0:#F3EDF7; --w1:#E8DEF8; --w2:#D9C9F3; --w3:#C4B0EA; --w4:#A58BDB; --w5:#8467C8; --w6:#6750A4; --w7:#4F378B;
"""
for k, (_, dk, lt) in GROUPS.items():
    CSS += f"  --{k}:{lt};\n"
CSS += "}\n"
CSS += """
:root[data-theme=dark]{
  --bg:#141218; --card:#211F26; --card2:#2B2930; --card3:#36343B;
  --on:#E6E0E9; --var:#CAC4D0; --outline:#49454F; --outlv:#3A3741;
  --pri:#D0BCFF; --onpri:#381E72; --pric:#4F378B; --onpric:#EADDFF;
  --sec:#CCC2DC; --secc:#4A4458; --terc:#633B48; --ter:#EFB8C8;
  --arrow:#A29DAB; --sh:.5; --ok:#A6E3A1; --warn:#F7C873; --err:#F2B8B5;
  --w0:#1D1B20; --w1:#2B2336; --w2:#3A2D55; --w3:#4F378B; --w4:#6B4FB0; --w5:#8E6FD6; --w6:#B79CF5; --w7:#E8DDFF;
"""
for k, (_, dk, lt) in GROUPS.items():
    CSS += f"  --{k}:{dk};\n"
CSS += "}\n"
CSS += """
*{box-sizing:border-box}
html,body{margin:0;padding:0;background:var(--bg)}
body{font-family:'Inter','Inter Display',system-ui,sans-serif;-webkit-font-smoothing:antialiased}
svg{display:block}
text{font-family:'Inter',system-ui,sans-serif;fill:var(--on)}
.disp{font-family:'Inter Display','Inter',sans-serif}
.mono{font-family:'DejaVu Sans Mono',ui-monospace,monospace}
.f-var{fill:var(--var)} .f-pri{fill:var(--pri)} .f-sec{fill:var(--sec)} .f-on{fill:var(--on)}
.f-ok{fill:var(--ok)} .f-warn{fill:var(--warn)} .f-err{fill:var(--err)} .f-onpri{fill:var(--onpri)} .f-onpric{fill:var(--onpric)}
.bg{fill:var(--bg)}
.card{fill:var(--card);stroke:var(--outlv);stroke-width:1}
.card2{fill:var(--card2);stroke:var(--outlv);stroke-width:1}
.card3{fill:var(--card3)}
.pri{fill:var(--pri)} .pric{fill:var(--pric)} .secc{fill:var(--secc)} .terc{fill:var(--terc)}
.ghost{fill:none;stroke:var(--outline);stroke-width:1.5;stroke-dasharray:5 5}
.line{stroke:var(--outline);stroke-width:1;fill:none}
.arr{stroke:var(--arrow);stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}
.arr.pri{stroke:var(--pri);stroke-width:2.5}
.arr.dash{stroke-dasharray:6 6}
.arr.err{stroke:var(--err)}
.mk{fill:var(--arrow)} .mk-pri{fill:var(--pri)} .mk-err{fill:var(--err)}
.ico{fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.sh{filter:url(#sh)}
.sh2{filter:url(#sh2)}
.lbl{fill:var(--bg);stroke:var(--outlv)}
"""
for k in GROUPS:
    CSS += f".g-{k}{{--g:var(--{k})}}\n"
CSS += """
.gt{fill:var(--g)} .gs{stroke:var(--g);fill:none}
.gtint{fill:color-mix(in srgb,var(--g) 17%,var(--card));stroke:color-mix(in srgb,var(--g) 45%,transparent);stroke-width:1.2}
.gsolid{fill:var(--g)}
.gdot{fill:var(--g)}
.w0{fill:var(--w0)}.w1{fill:var(--w1)}.w2{fill:var(--w2)}.w3{fill:var(--w3)}.w4{fill:var(--w4)}.w5{fill:var(--w5)}.w6{fill:var(--w6)}.w7{fill:var(--w7)}
"""

# ---------------------------------------------------------------- iconos
ICONS = {}


def _load_material_icons():
    p = os.path.join(ROOT, "legacy", "openwebrx-plugin", "plugins", "receiver", "material", "material.js")
    try:
        src = open(p, encoding="utf-8").read()
    except OSError:
        return
    for m in re.finditer(r"^\s{4}([a-z0-9_]+):\s*'(.*)',?\s*$", src, re.M):
        ICONS[m.group(1)] = m.group(2)


_load_material_icons()
ICONS.update({
    "activity": '<path d="M22 12h-4l-3 9L9 3l-3 9H2"/>',
    "server": '<rect width="20" height="8" x="2" y="2" rx="2"/><rect width="20" height="8" x="2" y="14" rx="2"/><path d="M6 6h.01M6 18h.01"/>',
    "monitor": '<rect width="20" height="14" x="2" y="3" rx="2"/><path d="M8 21h8M12 17v4"/>',
    "file": '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/>',
    "filter": '<polygon points="22 3 2 3 10 12.46 10 19 14 21 14 12.46 22 3"/>',
    "layers": '<polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/>',
    "volume": '<polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14"/>',
    "check": '<path d="M20 6 9 17l-5-5"/>',
    "check2": '<path d="M18 6 7 17l-5-5"/><path d="m22 10-7.5 7.5L13 16"/>',
    "clock": '<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>',
    "lock": '<rect width="18" height="11" x="3" y="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/>',
    "shield": '<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>',
    "database": '<ellipse cx="12" cy="5" rx="9" ry="3"/><path d="M3 5v14a9 3 0 0 0 18 0V5"/><path d="M3 12a9 3 0 0 0 18 0"/>',
    "mic": '<path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2M12 19v3"/>',
    "map": '<polygon points="1 6 1 22 8 18 16 22 23 18 23 2 16 6 8 2 1 6"/><line x1="8" y1="2" x2="8" y2="18"/><line x1="16" y1="6" x2="16" y2="22"/>',
    "globe": '<circle cx="12" cy="12" r="10"/><path d="M2 12h20"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/>',
    "disk": '<line x1="22" x2="2" y1="12" y2="12"/><path d="M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"/><line x1="6" x2="6.01" y1="16" y2="16"/>',
    "alert": '<path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3z"/><path d="M12 9v4M12 17h.01"/>',
    "play": '<polygon points="5 3 19 12 5 21 5 3"/>',
    "wave": '<path d="M2 13a2 2 0 0 0 2-2V7a2 2 0 0 1 4 0v13a2 2 0 0 0 4 0V4a2 2 0 0 1 4 0v13a2 2 0 0 0 4 0v-4a2 2 0 0 1 2-2"/>',
    "box": '<path d="M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><path d="m3.3 7 8.7 5 8.7-5M12 22V12"/>',
    "video": '<path d="m22 8-6 4 6 4V8z"/><rect width="14" height="12" x="2" y="6" rx="2"/>',
    "send": '<path d="m22 2-7 20-4-9-9-4z"/><path d="M22 2 11 13"/>',
    "key": '<circle cx="7.5" cy="15.5" r="5.5"/><path d="m21 2-9.6 9.6M15.5 7.5l3 3L22 7l-3-3"/>',
    "list": '<path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/>',
    "flask": '<path d="M10 2v7.5L4.5 19a2 2 0 0 0 1.8 3h11.4a2 2 0 0 0 1.8-3L14 9.5V2"/><path d="M8.5 2h7M7 16h10"/>',
    "sliders": '<path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/>',
    "search": '<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',
    "repeat": '<path d="m17 2 4 4-4 4"/><path d="M3 11v-1a4 4 0 0 1 4-4h14M7 22l-4-4 4-4"/><path d="M21 13v1a4 4 0 0 1-4 4H3"/>',
    "bars": '<path d="M3 20V10M9 20V4M15 20v-7M21 20V8"/>',
    "plug": '<path d="M12 22v-5M9 8V2M15 8V2M18 8v5a4 4 0 0 1-4 4h-4a4 4 0 0 1-4-4V8z"/>',
    "thread": '<circle cx="6" cy="6" r="3"/><circle cx="18" cy="18" r="3"/><path d="M6 9v3a3 3 0 0 0 3 3h6"/>',
    "image": '<rect width="18" height="18" x="3" y="3" rx="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-3.1-3.1a2 2 0 0 0-2.8 0L6 21"/>',
    "ban": '<circle cx="12" cy="12" r="10"/><path d="m4.9 4.9 14.2 14.2"/>',
    "gauge": '<path d="m12 14 4-4"/><path d="M3.34 19a10 10 0 1 1 17.32 0"/>',
    "eye": '<path d="M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7z"/><circle cx="12" cy="12" r="3"/>',
    "tag": '<path d="M12.6 2.6a2 2 0 0 0-1.4-.6H4a2 2 0 0 0-2 2v7.2a2 2 0 0 0 .6 1.4l8.7 8.7a2.4 2.4 0 0 0 3.4 0l6.6-6.6a2.4 2.4 0 0 0 0-3.4z"/><circle cx="7.5" cy="7.5" r=".5"/>',
    "branch": '<path d="M6 3v12"/><circle cx="18" cy="6" r="3"/><circle cx="6" cy="18" r="3"/><path d="M18 9a9 9 0 0 1-9 9"/>',
    "scan": '<path d="M3 7V5a2 2 0 0 1 2-2h2M17 3h2a2 2 0 0 1 2 2v2M21 17v2a2 2 0 0 1-2 2h-2M7 21H5a2 2 0 0 1-2-2v-2M7 12h10"/>',
    "calc": '<rect width="16" height="20" x="4" y="2" rx="2"/><path d="M8 6h8M16 14v4M16 10h.01M12 10h.01M8 10h.01M12 14h.01M8 14h.01M12 18h.01M8 18h.01"/>',
    "user": '<path d="M19 21v-2a4 4 0 0 0-4-4H9a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/>',
    "fpga": '<rect x="5" y="5" width="14" height="14" rx="2"/><path d="M9 9h6v6H9zM9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"/>',
    "wrench": '<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94z"/>',
})


def icon(name, x, y, size=24, color="currentColor", sw=2):
    """Icono de línea (rejilla de 24) con su esquina superior izquierda en (x, y)."""
    body = ICONS.get(name)
    if body is None:
        raise KeyError(name)
    s = size / 24.0
    return (f'<g transform="translate({x:.1f} {y:.1f}) scale({s:.4f})" class="ico" '
            f'style="color:{color};stroke-width:{sw}">{body}</g>')


# ---------------------------------------------------------------- lienzo
class Canvas:
    def __init__(self, w, h, title=None, subtitle=None):
        self.w, self.h = w, h
        self.parts = []
        self.defs = (
            '<filter id="sh" x="-20%" y="-20%" width="140%" height="150%">'
            '<feDropShadow dx="0" dy="3" stdDeviation="6" flood-color="#2a1a4a" style="flood-opacity:var(--sh)"/></filter>'
            '<filter id="sh2" x="-20%" y="-20%" width="140%" height="150%">'
            '<feDropShadow dx="0" dy="1" stdDeviation="2" flood-color="#2a1a4a" style="flood-opacity:calc(var(--sh)*.7)"/></filter>'
            '<marker id="ah" viewBox="0 0 10 10" refX="8.5" refY="5" markerUnits="userSpaceOnUse" markerWidth="12" markerHeight="12" orient="auto-start-reverse">'
            '<path class="mk" d="M1 1.5 8.5 5 1 8.5z" stroke-linejoin="round" stroke="var(--arrow)" stroke-width="1"/></marker>'
            '<marker id="ahp" viewBox="0 0 10 10" refX="8.5" refY="5" markerUnits="userSpaceOnUse" markerWidth="12" markerHeight="12" orient="auto-start-reverse">'
            '<path class="mk-pri" d="M1 1.5 8.5 5 1 8.5z" stroke-linejoin="round" stroke="var(--pri)" stroke-width="1"/></marker>'
            '<marker id="ahe" viewBox="0 0 10 10" refX="8.5" refY="5" markerUnits="userSpaceOnUse" markerWidth="12" markerHeight="12" orient="auto-start-reverse">'
            '<path class="mk-err" d="M1 1.5 8.5 5 1 8.5z" stroke-linejoin="round" stroke="var(--err)" stroke-width="1"/></marker>'
        )
        self.add(f'<rect class="bg" width="{w}" height="{h}"/>')
        if title:
            self.header(title, subtitle)

    def add(self, s):
        self.parts.append(s)

    def header(self, title, subtitle=None, x=48, y=64):
        self.add(f'<text class="disp" x="{x}" y="{y}" font-size="30" font-weight="600" letter-spacing="-0.4">{esc(title)}</text>')
        if subtitle:
            self.add(f'<text class="f-var" x="{x}" y="{y + 28}" font-size="15">{esc(subtitle)}</text>')

    # ---- primitivas
    def rect(self, x, y, w, h, r=20, cls="card", shadow=True, extra=""):
        sh = " sh" if shadow is True else (" sh2" if shadow == 2 else "")
        self.add(f'<rect class="{cls}{sh}" x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{r}" {extra}/>')

    def text(self, x, y, s, size=14, weight=400, cls="", anchor="start", mono=False, spacing=None):
        c = ("mono " if mono else "") + cls
        a = f' text-anchor="{anchor}"' if anchor != "start" else ""
        ls = f' letter-spacing="{spacing}"' if spacing is not None else ""
        self.add(f'<text class="{c.strip()}" x="{x:.1f}" y="{y:.1f}" font-size="{size}" font-weight="{weight}"{a}{ls}>{esc(s)}</text>')

    def lines(self, x, y, ls, size=13, lh=None, weight=400, cls="f-var", anchor="start", mono=False):
        lh = lh or size * 1.45
        for i, s in enumerate(ls):
            self.text(x, y + i * lh, s, size, weight, cls, anchor, mono)

    def pill(self, x, y, s, size=12, cls_bg="secc", cls_fg="f-on", h=None, padx=11, weight=500, mono=False, anchor="start", ic=None):
        """Píldora centrada verticalmente en y. Devuelve su anchura."""
        h = h or size + 12
        w = tw(s, size, weight, mono) + 2 * padx + (size + 5 if ic else 0)
        x0 = x if anchor == "start" else (x - w / 2 if anchor == "middle" else x - w)
        self.add(f'<rect class="{cls_bg}" x="{x0:.1f}" y="{y - h / 2:.1f}" width="{w:.1f}" height="{h}" rx="{h / 2}"/>')
        tx = x0 + padx
        if ic:
            self.add(icon(ic, tx, y - size / 2 - .5, size + 1, "currentColor", 2.2).replace('class="ico"', 'class="ico ' + cls_fg.replace("f-", "c-") + '"'))
            tx += size + 5
        self.text(tx, y + size * 0.36, s, size, weight, cls_fg, mono=mono)
        return w

    def chip_icon(self, x, y, name, d=40, gcls=None, cls_bg="pric", color="var(--onpric)"):
        """Círculo tonal con un icono dentro (x,y = esquina sup. izq.)."""
        g = f' class="{gcls}"' if gcls else ""
        if gcls:
            self.add(f'<g class="{gcls}"><circle class="gtint" cx="{x + d / 2}" cy="{y + d / 2}" r="{d / 2}"/>'
                     + icon(name, x + d * 0.25, y + d * 0.25, d * 0.5, "var(--g)", 2) + '</g>')
        else:
            self.add(f'<circle class="{cls_bg}" cx="{x + d / 2}" cy="{y + d / 2}" r="{d / 2}"/>'
                     + icon(name, x + d * 0.25, y + d * 0.25, d * 0.5, color, 2))

    def node(self, x, y, w, h, title, sub=None, ic=None, gcls=None, r=20, cls="card", tsize=16, ssize=12.5, shadow=True, sublines=None, iconsize=40):
        """Tarjeta con icono, título y subtítulo(s)."""
        self.rect(x, y, w, h, r, cls, shadow)
        tx = x + 18
        if ic:
            self.chip_icon(x + 16, y + 16, ic, iconsize, gcls)
            tx = x + 16 + iconsize + 12
        self.text(tx, y + 16 + (iconsize / 2 - 2 if ic else 18) + (0 if (sub or sublines) else 5), title, tsize, 600, "disp")
        yy = y + 16 + iconsize / 2 + 16 if ic else y + 42
        for i, s in enumerate(sublines or ([sub] if sub else [])):
            self.text(tx, yy + i * ssize * 1.45, s, ssize, 400, "f-var")

    def arrow(self, pts, cls="", r=12, marker="ah", label=None, lpos=0.5, lsize=11.5, start_marker=False):
        """Flecha poligonal con esquinas redondeadas. pts = [(x,y),…]."""
        d = f"M{pts[0][0]:.1f} {pts[0][1]:.1f}"
        for i in range(1, len(pts) - 1):
            (x0, y0), (x1, y1), (x2, y2) = pts[i - 1], pts[i], pts[i + 1]
            l1 = max(abs(x1 - x0), abs(y1 - y0))
            l2 = max(abs(x2 - x1), abs(y2 - y1))
            rr = min(r, l1 / 2, l2 / 2)
            sx = (x1 - x0) / l1 if l1 else 0
            sy = (y1 - y0) / l1 if l1 else 0
            ex = (x2 - x1) / l2 if l2 else 0
            ey = (y2 - y1) / l2 if l2 else 0
            d += f" L{x1 - sx * rr:.1f} {y1 - sy * rr:.1f} Q{x1:.1f} {y1:.1f} {x1 + ex * rr:.1f} {y1 + ey * rr:.1f}"
        d += f" L{pts[-1][0]:.1f} {pts[-1][1]:.1f}"
        mk = f' marker-end="url(#{marker})"' if marker else ""
        ms = f' marker-start="url(#{marker})"' if start_marker else ""
        self.add(f'<path class="arr {cls}" d="{d}"{mk}{ms}/>')
        if label:
            # punto medio por longitud de segmento
            segs = [(pts[i], pts[i + 1], abs(pts[i + 1][0] - pts[i][0]) + abs(pts[i + 1][1] - pts[i][1])) for i in range(len(pts) - 1)]
            tot = sum(s[2] for s in segs)
            tgt = tot * lpos
            acc = 0
            for a, b, l in segs:
                if acc + l >= tgt and l > 0:
                    f = (tgt - acc) / l
                    mx, my = a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f
                    break
                acc += l
            self.label(mx, my, label, lsize)

    def label(self, cx, cy, s, size=11.5, weight=500):
        w = tw(s, size, weight) + 14
        self.add(f'<rect class="lbl" x="{cx - w / 2:.1f}" y="{cy - size / 2 - 5:.1f}" width="{w:.1f}" height="{size + 10:.1f}" rx="{(size + 10) / 2:.1f}"/>')
        self.text(cx, cy + size * 0.36, s, size, weight, "f-var", "middle")

    def svg(self):
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}">'
                f'<defs>{self.defs}</defs>' + "".join(self.parts) + "</svg>")

    def html(self, title):
        return (f'<!doctype html><html lang="es"><head><meta charset="utf-8"><title>{esc(title)}</title>'
                f'<style>{CSS}</style></head><body>{self.svg()}'
                '<script>if(/[?&]dark/.test(location.search))document.documentElement.dataset.theme="dark";</script>'
                '</body></html>')

    def save(self, name, title):
        path = os.path.join(HERE, name + ".html")
        with open(path, "w", encoding="utf-8") as f:
            f.write(self.html(title))
        # tamaño para render.sh
        return path
