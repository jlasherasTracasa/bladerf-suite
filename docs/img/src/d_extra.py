"""Diagramas adicionales: cadena de ADS-B (docs/protocolos/adsb.md)."""
from lib import *


def adsb():
    W, H = 1720, 640
    c = Canvas(W, H, "ADS-B: de las muestras a la tabla de aeronaves",
               "Decodificador WIDE a 2 Msps exactos: cada medio microsegundo es una muestra (docs/protocolos/adsb.md).")
    steps = [("Q11 a 2 Msps", ["muestras crudas", "wide_rate = 2 000 000"], "radio", "g-av"),
             ("Módulo |x|", ["no le afecta el", "desvío de frecuencia"], "bars", None),
             ("Preámbulo", ["8 µs: pulsos en las", "muestras 0, 2, 7 y 9"], "search", None),
             ("PPM", ["bit i: |x[16+2i]| frente", "a |x[17+2i]|"], "wave", None),
             ("CRC-24", ["polinomio 0xFFF409", "corrige 1–2 bits dudosos"], "shield", None),
             ("Campos ME", ["por type code (tc)", "y CPR global"], "list", None),
             ("Tabla", ["state_json · caduca a 60 s", "eventos new/callsign/squawk"], "plane", "g-av")]
    cw, gap = 200, 40
    for i, (t, ls, ic, g) in enumerate(steps):
        x = 48 + i * (cw + gap)
        y = 150
        c.rect(x, y, cw, 160, 24, "card", True)
        c.chip_icon(x + 16, y + 16, ic, 42, g)
        c.text(x + 16, y + 86, t, 15.5, 600, "disp")
        c.lines(x + 16, y + 110, ls, 12, 17)
        if i < len(steps) - 1:
            c.arrow([(x + cw + 3, y + 80), (x + cw + gap - 3, y + 80)], "pri", marker="ahp")
    c.text(48, 370, "Trama DF17 (112 bits):  DF(5) · CA(3) · ICAO(24) · ME(56) · PI(24)", 15, 600, "disp")
    fields = [("DF", 5, "g-av"), ("CA", 3, "g-bc"), ("ICAO", 24, "g-mar"), ("ME", 56, "g-data"), ("PI = CRC-24", 24, "g-ham")]
    x = 48
    total = W - 96
    for nm, b, g in fields:
        w = total * b / 112
        c.add(f'<g class="{g}">')
        c.rect(x, 392, w - 4, 58, 14, "gtint", 2)
        c.text(x + (w - 4) / 2, 426, nm if w > 60 else nm, 13 if w > 60 else 11.5, 600, "gt", "middle")
        c.add('</g>')
        x += w
    c.text(48, 476, "Type code (tc) del campo ME", 14, 600, "disp")
    tcs = [("1–4", "identificación"), ("5–8", "posición en superficie"), ("9–18", "posición en vuelo, altitud barométrica"),
           ("19", "velocidad"), ("20–22", "posición con altitud GNSS"), ("28", "emergencia / squawk")]
    x = 48
    for a, b in tcs:
        w = tw(a, 12.5, 700, True) + tw(b, 12.5, 500) + 44
        c.add(f'<rect class="secc" x="{x}" y="496" width="{w:.1f}" height="34" rx="17"/>')
        c.text(x + 16, 518, a, 12.5, 700, "f-pri", mono=True)
        c.text(x + 16 + tw(a, 12.5, 700, True) + 12, 518, b, 12.5, 500, "f-on")
        x += w + 10
    c.text(48, 580, "Altitud: alt = 25·N − 1000 ft (bit Q = 1). CPR de 17 bits con rejillas de 60 y 59 zonas; se descartan pares que cruzan de zona y saltos > 0,6 km/s.", 12, 400, "f-var")
    c.text(48, 602, "Estado de validación: vectores de The 1090 MHz Riddle y dump1090-fa sobre la salida del modulador; sin tráfico real con la antena actual.", 12, 400, "f-var")
    return c


def build_all():
    print(adsb().save("adsb-cadena", "Cadena ADS-B"))
