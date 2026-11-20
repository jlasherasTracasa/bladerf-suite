"""Política de emisión: file / loopback / air (src/tx/tx.c, docs/arquitectura/TX.md)."""
from lib import *


def build():
    W, H = 1740, 960
    c = Canvas(W, H, "Política de emisión",
               "La emisión está desactivada por defecto. Solo el modo «air» radia, y solo si se cumple todo a la vez.")
    # inicio
    c.node(48, 400, 236, 104, "Orden «tx»", None, "send", None, sublines=["WebSocket · proto, mode,", "freq, gain, params"])
    # carriles
    lx, lw = 380, 430
    def lane(y, h, title, sub, ic, g, pills):
        c.add(f'<g class="{g}">')
        c.rect(lx, y, lw, h, 26, "gtint", True)
        c.add('</g>')
        c.chip_icon(lx + 18, y + 18, ic, 44, g)
        c.add(f'<g class="{g}">')
        c.text(lx + 76, y + 46, title, 21, 700, "disp gt")
        c.add('</g>')
        c.text(lx + 76, y + 66, sub, 12.5, 400, "f-var")
        yy = y + 100
        for p in pills:
            c.text(lx + 24, yy, p, 12.5, 400, "f-on")
            yy += 19
    lane(150, 150, "file", "siempre permitido · no radia", "disk", "g-spec",
         ["Escribe recordings/tx_<proto>_<fecha>.sigmf-*", "a center/rate, con el canal desplazado a freq.", "No pasa por la lista blanca (no hay antena)."])
    lane(330, 150, "loopback", "requiere --enable-tx", "repeat", "g-data",
         ["BLADERF_LB_RFIC_BIST: bucle interno del RFIC.", "Ganancia fija de −20 dB, duración ≤ 30 s.", "Nada sale por la antena."])
    lane(510, 150, "air", "requiere --enable-tx-air (y --enable-tx)", "antenna", "g-ham",
         ["Radia por la antena. Antes de emitir se", "comprueban las seis condiciones de la", "derecha: basta que una falle para rechazar."])
    # flechas inicio → carriles
    c.arrow([(284, 452), (332, 452), (332, 225), (lx, 225)], "pri", marker="ahp", label="file", lpos=.9)
    c.arrow([(284, 452), (332, 452), (332, 405), (lx, 405)], "pri", marker="ahp", label="loopback", lpos=.85)
    c.arrow([(284, 452), (332, 452), (332, 585), (lx, 585)], "pri", marker="ahp", label="air", lpos=.9)
    # comprobaciones
    kx, ky, kw = 880, 330, 480
    rows = [("1", "air_ok del modulador", "sí: aprs · ism · lora · meshtastic. No: los otros 16"),
            ("2", "Ganancia ≤ --tx-max-gain", "0 dB por defecto"),
            ("3", "Duración ≤ 5 s", "muestras realmente enviadas × repeat"),
            ("4", "Toda la señal dentro de UNA banda", "freq ± ancho ocupado (99,98 %), centro y espejo IQ"),
            ("5", "Radioaficionado → --callsign", "el indicativo va dentro de la trama (APRS, CW)"),
            ("6", "Ciclo de trabajo ≤ 1 %/h", "solo en 868,0–868,6 MHz (36 s/h)")]
    rh = 62
    c.rect(kx, ky, kw, 68 + rh * len(rows), 28, "card2", True)
    c.text(kx + 24, ky + 40, "tx_check_air(): todo debe cumplirse", 17, 600, "disp")
    for i, (n, t, s) in enumerate(rows):
        yy = ky + 66 + i * rh
        c.add(f'<circle class="pri" cx="{kx + 40}" cy="{yy + 26}" r="15"/>')
        c.text(kx + 40, yy + 31, n, 14, 700, "f-onpri", "middle")
        c.text(kx + 68, yy + 22, t, 14.5, 600, "disp")
        c.text(kx + 68, yy + 41, s, 12, 400, "f-var")
    c.arrow([(lx + lw, 585), (kx, 585)], "pri", marker="ahp")
    # resultado
    rx = 1456
    c.add('<g class="g-spec">')
    c.rect(rx, 330, 240, 160, 26, "gtint", True)
    c.add('</g>')
    c.chip_icon(rx + 16, 346, "check2", 44, "g-spec")
    c.text(rx + 72, 374, "Se emite", 18, 700, "disp f-ok")
    c.lines(rx + 20, 420, ["Log: proto, freq, centro,", "tasa, ganancia, duración", "y ancho ocupado."], 12.5)
    c.rect(rx, 540, 240, 160, 26, "card", True)
    c.chip_icon(rx + 16, 556, "ban", 44, None, "terc", "var(--ter)") if False else None
    c.add(f'<circle cx="{rx + 38}" cy="578" r="22" style="fill:color-mix(in srgb,var(--err) 20%,var(--card))"/>')
    c.add(icon("ban", rx + 26, 566, 24, "var(--err)", 2))
    c.text(rx + 72, 584, "Se rechaza", 18, 700, "disp f-err")
    c.lines(rx + 20, 630, ["tx_result ok:false con el motivo.", "No se emite nada."], 12.5)
    c.arrow([(kx + kw, 400), (rx, 400)], "pri", marker="ahp", label="todo ok", lpos=.5)
    c.arrow([(kx + kw, 620), (rx, 620)], "err", marker="ahe", label="alguna falla", lpos=.5)
    # lista blanca
    wy = 700
    c.text(48, wy + 20, "Lista blanca de bandas (única vía para «air»)", 17, 600, "disp")
    bands = [("ISM 433", "433,050–434,790 MHz", "—", "g-data"),
             ("SRD 868", "868,000–868,600 MHz", "ciclo ≤ 1 %/h", "g-data"),
             ("SRD 869", "869,400–869,650 MHz", "—", "g-data"),
             ("Radioaficionado 2 m", "144–146 MHz", "--callsign", "g-ham"),
             ("Radioaficionado 70 cm", "430–440 MHz", "--callsign", "g-ham")]
    bw = 244
    for i, (n, f, cnd, g) in enumerate(bands):
        x = 48 + (i % 3) * (bw + 12)
        y = wy + 44 + (i // 3) * 92
        c.add(f'<g class="{g}">')
        c.rect(x, y, bw, 80, 20, "gtint", 2)
        c.text(x + 16, y + 28, n, 14, 600, "disp gt")
        c.add('</g>')
        c.text(x + 16, y + 49, f, 12.5, 500, "f-on")
        c.text(x + 16, y + 67, "condición: " + cnd, 11.5, 400, "f-var")
    x = 48 + 2 * (bw + 12)
    y = wy + 44 + 92
    c.rect(x, y, bw, 80, 20, "card2", 2)
    c.add(icon("ban", x + 14, y + 14, 20, "var(--err)", 2))
    c.text(x + 44, y + 29, "Nunca", 14, 700, "disp f-err")
    c.text(x + 16, y + 50, "aviación · marítimo · GNSS · celular", 11.5, 400, "f-var")
    c.text(x + 16, y + 67, "radiodifusión · emergencia 406 MHz", 11.5, 400, "f-var")
    c.text(kx, 910, "tx_play (reproducir una grabación) solo admite file y loopback; «air» se rechaza siempre.", 12, 400, "f-var")
    c.text(kx, 930, "loopback y air no se han ejecutado todavía contra el hardware; los tests usan un sustituto del dispositivo.", 12, 400, "f-var")
    return c
