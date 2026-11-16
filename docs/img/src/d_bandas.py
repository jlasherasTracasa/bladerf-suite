"""Mapa de bandas 47 MHz – 6 GHz en escala logarítmica (AGENTS.md §5, src/profiles.c)."""
import math
from lib import *

ITEMS = [  # (frecuencia MHz, nombre, texto de frecuencia, grupo)
    (100.3, "FM + RDS", "100,3 MHz", "bc"),
    (127.5, "Banda aérea AM", "127,5 MHz", "av"),
    (131.7, "ACARS", "131,525–131,825", "av"),
    (136.85, "VDL Modo 2", "136,725–136,975", "av"),
    (144.8, "APRS", "144,8 MHz", "ham"),
    (156.525, "DSC · canal 16", "156,525 · 156,8", "mar"),
    (162.0, "AIS", "161,975 · 162,025", "mar"),
    (227.36, "DAB / DAB+", "227,36 MHz", "bc"),
    (403.0, "Radiosonda RS41", "~403 MHz", "data"),
    (433.92, "ISM 433", "433,92 MHz", "data"),
    (438.5, "DMR", "438,5 MHz", "ham"),
    (466.075, "POCSAG · FLEX", "466,075 MHz", "data"),
    (474.0, "TDT (DVB-T)", "474 MHz · UHF", "bc"),
    (806.0, "LTE banda 20", "806 MHz", "spec"),
    (868.3, "LoRa · ISM 868", "868,1–868,95", "data"),
    (869.525, "Meshtastic", "869,525 MHz", "data"),
    (942.5, "GSM 900", "942,5 MHz", "spec"),
    (1090.0, "ADS-B", "1090 MHz", "av"),
    (2437.0, "WiFi 2,4 GHz", "2437 MHz", "spec"),
    (3500.0, "5G NR n78", "3500 MHz", "spec"),
]


def build():
    W, H = 1760, 800
    c = Canvas(W, H, "Mapa de bandas: de 47 MHz a 6 GHz",
               "Escala logarítmica. Cada protocolo del catálogo en la frecuencia de su perfil; el color indica el grupo.")
    L, R = 60, W - 60
    lo, hi = math.log10(47), math.log10(6000)
    fx = lambda f: L + (math.log10(f) - lo) / (hi - lo) * (R - L)
    n = len(ITEMS)
    slot = (R - L) / n
    cw, chh = 150, 60
    ay = 540  # eje
    for i, (f, name, ftxt, g) in enumerate(ITEMS):
        cx = L + (i + .5) * slot
        row = i % 2
        y = 150 + row * 80
        mx = fx(f)
        lvl = 330 + i * 9
        c.add(f'<g class="g-{g}">')
        c.add(f'<path d="M{cx:.1f} {y + chh} V{lvl} H{mx:.1f} V{ay}" style="fill:none;stroke:var(--g);stroke-width:1.6;stroke-linejoin:round" />')
        c.add('</g>')
    for i, (f, name, ftxt, g) in enumerate(ITEMS):
        cx = L + (i + .5) * slot
        y = 150 + (i % 2) * 80
        c.add(f'<g class="g-{g}">')
        c.rect(cx - cw / 2, y, cw, chh, 16, "gtint", 2)
        c.add('</g>')
        c.text(cx, y + 26, name, 13.5, 600, "disp", "middle")
        c.add(f'<g class="g-{g}">')
        c.text(cx, y + 45, ftxt, 11.5, 500, "gt", "middle")
        c.add('</g>')
    # eje
    c.rect(L - 14, ay, R - L + 28, 22, 11, "card2", 2)
    for i, (f, name, ftxt, g) in enumerate(ITEMS):
        c.add(f'<g class="g-{g}"><circle class="gdot" cx="{fx(f):.1f}" cy="{ay + 11}" r="6.5" style="stroke:var(--bg);stroke-width:2"/></g>')
    for f in [50, 100, 200, 500, 1000, 2000, 5000]:
        x = fx(f)
        c.add(f'<line class="line" x1="{x:.1f}" y1="{ay + 28}" x2="{x:.1f}" y2="{ay + 40}"/>')
        c.text(x, ay + 60, f"{f} MHz" if f < 1000 else f"{f // 1000} GHz", 12.5, 500, "f-var", "middle")
        c.text(L - 14, ay + 96, "El eje cubre el alcance del bladeRF 2.0 micro en el barrido rápido: 47 MHz – 6 GHz.", 13, 500, "f-pri")
    # leyenda
    x = L - 14
    ly = 690
    c.text(x, ly - 4, "Grupos", 14, 600, "disp")
    for k, (nm, dk, lt) in GROUPS.items():
        c.add(f'<g class="g-{k}"><circle class="gdot" cx="{x + 7}" cy="{ly + 26}" r="7"/></g>')
        c.text(x + 22, ly + 31, nm, 13.5, 500, "f-on")
        x += 22 + tw(nm, 13.5, 500) + 34
    c.text(R + 14, ly + 31, "Frecuencias de src/profiles.c y del catálogo de AGENTS.md §5.", 12, 400, "f-var", "end")
    return c
