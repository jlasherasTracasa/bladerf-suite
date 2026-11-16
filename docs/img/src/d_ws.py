"""Protocolo WebSocket (docs/PROTOCOLO.md)."""
from lib import *


def chips(c, x, y, maxw, items, cls_bg="secc", cls_fg="f-on", size=12.5, mono=True, h=28, gap=8):
    cx, cy = x, y
    for t in items:
        w = tw(t, size, 500, mono) + 22
        if cx + w > x + maxw:
            cx = x
            cy += h + gap
        c.pill(cx, cy + h / 2, t, size, cls_bg, cls_fg, h=h, mono=mono)
        cx += w + gap
    return cy + h


def build():
    W, H = 1740, 1080
    c = Canvas(W, H, "Protocolo WebSocket  ·  GET /ws",
               "Todo en little-endian. Mensajes binarios para los datos y JSON para el control; un único socket por navegador.")
    # extremos
    c.node(48, 128, 250, 84, "Navegador", None, "monitor", "g-bc", sublines=["web/js · sin dependencias"])
    c.node(1442, 128, 250, 84, "Servidor", None, "server", "g-bc", sublines=["src/net/server.c · main.c"])
    c.arrow([(298, 156), (1442, 156)], "pri", marker="ahp", label='JSON {"cmd": …}', lpos=.5)
    c.arrow([(1442, 188), (298, 188)], "", marker="ah", label="binario 1–6 · JSON {\"type\": …}", lpos=.5)
    # ---- órdenes
    x, y, w, h = 48, 250, 520, 790
    c.rect(x, y, w, h, 28, "card2", True)
    c.chip_icon(x + 20, y + 18, "send", 44)
    c.text(x + 76, y + 46, "Cliente → servidor", 19, 600, "disp")
    c.text(x + 76, y + 66, "JSON con campo cmd", 12.5, 400, "f-var")
    groups = [("Receptor", ["config", "channel", "profile"]),
              ("Decodificadores", ["dec_add", "dec_remove", "dec_tune"]),
              ("Grabación", ["record", "rec_delete"]),
              ("Barrido 47 MHz–6 GHz", ["sweep", "sweep_stop"]),
              ("Emisión", ["tx", "tx_stop", "tx_play", "tx_cal"]),
              ("Televisión", ["tv", "tv_service", "tv_stop", "tv_scan"])]
    yy = y + 104
    for gname, its in groups:
        c.text(x + 24, yy, gname, 13.5, 600, "disp")
        yy = chips(c, x + 24, yy + 12, w - 48, its) + 26
    c.text(x + 24, yy + 4, "Un mensaje no válido responde «error». «channel» actúa", 12, 400, "f-var")
    c.text(x + 24, yy + 22, "solo sobre el canal del cliente que lo envía.", 12, 400, "f-var")
    hy = 840
    c.rect(x + 20, hy, w - 40, 170, 22, "card", 2)
    c.text(x + 40, hy + 34, "HTTP (mismo puerto)", 15, 600, "disp")
    c.lines(x + 40, hy + 62, ["GET /  y ficheros de web/", "GET /api/status · GET /api/recordings", "GET /recordings/<fichero>", "La cabecera Origin debe coincidir con Host.", "Por defecto solo 127.0.0.1 (--host 0.0.0.0 para la LAN)."], 12.5, 21, mono=False)
    # ---- binarios
    bx, by, bw = 600, 250, 1092
    c.rect(bx, by, bw, 500, 28, "card2", True)
    c.chip_icon(bx + 20, by + 18, "box", 44)
    c.text(bx + 76, by + 46, "Servidor → cliente · binario", 19, 600, "disp")
    c.text(bx + 76, by + 66, "byte 0 = tipo", 12.5, 400, "f-var")
    rows = [("1", "espectro", "[1]=GPU · [4..7] n u32 · [8..15] centro f64 · [16..23] tasa f64 · [24..31] dbmin, dbstep f32", "n bytes v: dBFS = dbmin + v·dbstep", "g-spec"),
            ("2", "audio", "[1] flags · [2] canales (1/2) · [4..7] nivel dBFS f32", "PCM int16 a 48 kHz intercalado", "g-ham"),
            ("3", "nivel", "igual que audio", "sin datos (canal sin audio)", "g-ham"),
            ("4", "barrido", "cabecera de 32 B como el tipo 1 · [2] pasada completa · [3] progreso 0–100", "n bytes v sobre [inicio, fin]", "g-data"),
            ("5", "vídeo TV", "[1] clave · [2] códec (1 H.264) · [3] sec. · [4..11] PTS i64 · [12..19] DTS i64", "unidad de acceso Annex B (SPS+PPS en claves)", "g-av"),
            ("6", "audio TV", "[1] canales · [4..7] tasa u32 · [8..15] PTS i64 (90 kHz)", "PCM int16 intercalado", "g-bc")]
    for i, (n, nm, hdr, data, g) in enumerate(rows):
        yy = by + 92 + i * 61
        c.add(f'<g class="{g}">')
        c.add(f'<circle class="gsolid" cx="{bx + 44}" cy="{yy + 26}" r="19"/>')
        c.add('</g>')
        c.text(bx + 44, yy + 32, n, 16, 700, "f-bg", "middle") if False else None
        c.add(f'<text x="{bx + 44}" y="{yy + 32}" text-anchor="middle" font-size="16" font-weight="700" style="fill:var(--bg)">{n}</text>')
        c.text(bx + 80, yy + 22, nm, 15, 600, "disp")
        c.text(bx + 190, yy + 22, hdr, 11.5, 400, "f-var", mono=True)
        c.text(bx + 190, yy + 42, "→ " + data, 12, 500, "f-on")
        if i < 5:
            c.add(f'<line class="line" x1="{bx + 24}" y1="{yy + 57}" x2="{bx + bw - 24}" y2="{yy + 57}"/>')
    c.text(bx + 24, by + 478, "Flags del audio: 1 squelch abierto · 2 estéreo · 4 fuera de banda · 8 piloto 19 kHz. PTS desenrollado, nunca retrocede.", 11.5, 400, "f-var")
    # ---- JSON servidor → cliente
    jy = 774
    c.rect(bx, jy, bw, 266, 28, "card2", True)
    c.chip_icon(bx + 20, jy + 18, "list", 44)
    c.text(bx + 76, jy + 46, "Servidor → cliente · JSON", 19, 600, "disp")
    c.text(bx + 76, jy + 66, "campo type", 12.5, 400, "f-var")
    j = ["status", "signals", "recording", "error", "decoders", "profiles", "dec_list", "dec", "dec_state",
         "tx_policy", "tx_result", "tx_cal", "tv", "tv_scan"]
    e = chips(c, bx + 24, jy + 92, bw - 48, j, "pric", "f-onpric")
    c.text(bx + 24, e + 34, "Orden al conectar", 13.5, 600, "disp")
    seq = ["status", "signals", "recording", "decoders", "profiles", "tx_policy", "dec_list", "dec_state", "100 × dec (replay)"]
    xx = bx + 24
    yy = e + 62
    for i, s in enumerate(seq):
        w = tw(s, 12, 500, True) + 22
        if xx + w + 20 > bx + bw - 24:
            xx = bx + 24
            yy += 36
        c.pill(xx, yy, s, 12, "secc", "f-on", h=26, mono=True)
        xx += w
        if i < len(seq) - 1:
            c.add(f'<text x="{xx + 8}" y="{yy + 5}" font-size="14" class="f-var" text-anchor="middle">›</text>')
            xx += 18
    return c
