"""Matriz de validación de los 21 decodificadores (docs/VALIDACION.md)."""
from lib import *

ST = {"aire": ("En el aire (Pamplona)", "g-spec", "antenna"),
      "real": ("Datos reales", "g-av", "check"),
      "sint": ("Sintético / referencia", "g-bc", "flask"),
      "pend": ("Pendiente", "g-data", "clock")}

DEC = [  # id, nombre, grupo, estado, nota (2 líneas)
    ("rds", "RDS", "bc", "aire", ["Radio Pamplona 100,4 MHz", "PI E239, PS «SER»"]),
    ("lte", "LTE", "spec", "aire", ["4 celdas con MIB en", "Pamplona (PCI + PBCH)"]),
    ("adsb", "ADS-B", "av", "real", ["vectores 1090 Riddle,", "dump1090-fa · sin tráfico real"]),
    ("ais", "AIS", "mar", "real", ["sigidwiki: MMSI y posiciones", "tipos 5/18/19/21/24 pendientes"]),
    ("acars", "ACARS", "av", "real", ["11/11 bloques = acarsdec", "multibloque sin contrastar"]),
    ("flex", "FLEX", "data", "real", ["1600/2 = multimon-ng", "3200 y 6400 pendientes"]),
    ("pocsag", "POCSAG", "data", "real", ["512/1200/2400 bps", "= multimon-ng"]),
    ("aprs", "APRS", "ham", "real", ["tramas AX.25 = multimon-ng", "y Dire Wolf"]),
    ("ism", "ISM OOK/FSK", "data", "real", ["8 modelos reales", "= rtl_433"]),
    ("rs41", "RS41", "data", "real", ["38/38 tramas, 2 sondas", "posiciones coherentes"]),
    ("lora", "LoRa", "data", "real", ["SF7–12, CR 1–4, BW 125/250", "con loratx y lorarx"]),
    ("dab", "DAB / DAB+", "bc", "real", ["ensemble 5001: 36/36 FIB", "servicio «Radio Slovensko»"]),
    ("gsm", "GSM", "spec", "real", ["SCH: BSIC 23, 211/211", "BCCH/SI3 pendiente"]),
    ("wifi", "WiFi", "spec", "real", ["8 BSSID en DSSS 1 Mb/s", "OFDM pendiente"]),
    ("dsc", "DSC", "mar", "real", ["tablas ITU-R M.493-15", "sin grabación ni aire"]),
    ("dvbt", "DVB-T (TPS)", "bc", "real", ["referencia gr-dtv y TPS", "en el aire (C32, 8k 64-QAM)"]),
    ("meshtastic", "Meshtastic", "data", "real", ["cabecera 16 B y clave", "en el aire: pendiente"]),
    ("tdt", "TDT (TS)", "bc", "sint", ["gr-dtv: 4918 paquetes TS", "TS en el aire: no (SNR)"]),
    ("vdl2", "VDL Modo 2", "av", "pend", ["0 tramas reales: ISI", "falta traza de referencia"]),
    ("nr", "5G NR", "spec", "pend", ["sin celda real hallada", "(centro desconocido)"]),
    ("dmr", "DMR", "ham", "pend", ["1 sincronía, 0 ráfagas", "solo tablas ETSI"]),
]


def build():
    W, H = 1740, 700
    c = Canvas(W, H, "Matriz de validación: 21 decodificadores",
               "Qué se ha contrastado con señales reales y qué sigue pendiente. Todos pasan sus pruebas sintéticas (modulador → canal → decodificador).")
    counts = {k: sum(1 for d in DEC if d[3] == k) for k in ST}
    # leyenda con contadores
    x = 48
    for k, (nm, g, ic) in ST.items():
        c.add(f'<g class="{g}"><rect class="gtint" x="{x}" y="128" width="{tw(nm, 13, 600) + 100}" height="38" rx="19"/>'
              f'<circle class="gdot" cx="{x + 19}" cy="147" r="11"/></g>')
        c.add(icon(ic, x + 12.5, 140.5, 13, "var(--bg)", 2.4))
        c.text(x + 38, 152, nm, 13, 600, "f-on")
        c.add(f'<g class="{g}">')
        c.text(x + 38 + tw(nm, 13, 600) + 10, 152, f"{counts[k]}", 14, 700, "gt")
        c.add('</g>')
        x += tw(nm, 13, 600) + 118
    cols = 7
    gap = 16
    cw = (W - 96 - gap * (cols - 1)) / cols
    ch = 128
    for i, (did, nm, grp, st, note) in enumerate(DEC):
        r, cc = divmod(i, cols)
        x = 48 + cc * (cw + gap)
        y = 196 + r * (ch + gap)
        sname, sg, sic = ST[st]
        c.add(f'<g class="{sg}">')
        c.rect(x, y, cw, ch, 22, "gtint", 2)
        c.add(f'<circle class="gdot" cx="{x + 26}" cy="{y + 30}" r="13"/>')
        c.add('</g>')
        c.add(icon(sic, x + 18, y + 22, 16, "var(--bg)", 2.6))
        c.text(x + 48, y + 36, nm, 16, 650 if False else 600, "disp")
        c.add(f'<g class="g-{grp}"><circle class="gdot" cx="{x + cw - 22}" cy="{y + 30}" r="6"/></g>')
        c.add(f'<g class="{sg}">')
        c.text(x + 18, y + 62, sname, 11.5, 600, "gt")
        c.add('</g>')
        c.lines(x + 18, y + 84, note, 11.5, 17)
        c.text(x + 18, y + ch - 12, "docs/decoders/" + did + ".md" if did not in ("tdt",) else "docs/TDT.md", 10, 400, "f-var", mono=True) if False else None
    # pie: leyenda de puntos
    ly = 196 + 3 * (ch + gap) - 6
    x = 48
    c.text(x, ly + 14, "Punto de la esquina = grupo:", 12.5, 600, "f-var")
    x += tw("Punto de la esquina = grupo:", 12.5, 600) + 16
    for k, (nm, dk, lt) in GROUPS.items():
        c.add(f'<g class="g-{k}"><circle class="gdot" cx="{x + 6}" cy="{ly + 10}" r="6"/></g>')
        c.text(x + 18, ly + 15, nm, 12.5, 500, "f-on")
        x += 18 + tw(nm, 12.5, 500) + 22
    c.text(W - 48, ly + 15, "Detalle por protocolo: docs/VALIDACION.md y docs/decoders/<id>.md", 12, 400, "f-var", "end")
    return c
