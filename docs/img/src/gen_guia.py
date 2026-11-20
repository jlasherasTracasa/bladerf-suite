#!/usr/bin/env python3
"""gen_guia.py — diagramas de la guía, el protocolo, la arquitectura y la FPGA (DOC2b).

Reutiliza lib.py (tema Material 3, fuente Inter, claro y oscuro). Genera las fuentes
HTML en docs/img/src/ y, con --render, ejecuta docs/img/render.sh y mueve los PNG a
sus carpetas (docs/img/{guia,protocolo,arquitectura,fpga}/).

Uso:  python3 docs/img/src/gen_guia.py [--render] [nombre ...]
"""
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *  # noqa: E402,F401,F403

IMG = os.path.abspath(os.path.join(HERE, ".."))

# --------------------------------------------------------------- utilidades
def lifelines(c, items, y0, y1):
    """items = [(x, titulo, subtitulo, icono, clase)]; dibuja cabeceras y líneas de vida."""
    for x, t, s, ic, g in items:
        w = 250
        c.node(x - w / 2, y0 - 84, w, 84, t, None, ic, g, sublines=[s], iconsize=36)
        c.add(f'<path class="ghost" d="M{x} {y0 + 6} L{x} {y1}"/>')


def phase(c, y, h, num, title, x0=48, x1=None):
    x1 = x1 or c.w - 48
    c.add(f'<rect class="card2" x="{x0}" y="{y}" width="{x1 - x0}" height="{h}" rx="22" opacity=".55"/>')
    c.add(f'<circle class="pri" cx="{x0 + 30}" cy="{y + 30}" r="16"/>')
    c.text(x0 + 30, y + 36, str(num), 15, 700, "f-onpri", "middle")
    c.text(x0 + 56, y + 36, title, 15, 600, "disp")


def msg(c, x1, x2, y, text, sub=None, cls="", marker="ah"):
    c.arrow([(x1, y), (x2, y)], cls, marker=marker)
    mx = (x1 + x2) / 2
    c.text(mx, y - 9, text, 12.5, 500, "f-on", "middle")
    if sub:
        c.text(mx, y + 17, sub, 11.5, 400, "f-var", "middle")


def selfmsg(c, x, y, text, side=1, w=46):
    c.arrow([(x, y), (x + side * w, y), (x + side * w, y + 26), (x, y + 26)], "", marker="ah", r=8)
    if side > 0:
        c.text(x + w + 10, y + 17, text, 12.5, 500, "f-on")
    else:
        c.text(x - w - 10, y + 17, text, 12.5, 500, "f-on", "end")


# ------------------------------------------------------- 1. mensajes binarios
BYTE_COLS = ["g-av", "g-mar", "g-data", "g-ham", "g-spec", "g-bc"]


def byte_row(c, x0, y, cw, ncell, fields, data_label, hdr_len, tag, title, sub):
    """Fila de cabecera: ncell celdas de 1 byte; fields=(a,b,etiqueta,detalle,color)."""
    c.pill(x0, y + 14, tag, 13, "pri", "f-onpri", h=28, padx=13, weight=700)
    c.text(x0 + 66, y + 19, title, 16, 600, "disp")
    c.text(x0 + 66 + tw(title, 16, 600) + 14, y + 19, sub, 12.5, 400, "f-var")
    yy = y + 40
    for i in range(ncell):
        if i < hdr_len:
            c.text(x0 + i * cw + cw / 2, yy + 11, str(i), 10, 400, "f-var", "middle", mono=True)
    ry = yy + 20
    h = 58
    for a, b, lab, det, col in fields:
        x = x0 + a * cw
        w = (b - a + 1) * cw
        c.add(f'<g class="{col}"><rect class="gtint" x="{x + 1:.1f}" y="{ry}" width="{w - 2:.1f}" height="{h}" rx="10"/></g>')
        size = 12.5 if w > 70 else 11
        c.text(x + w / 2, ry + 24, lab, size, 600, "", "middle")
        if det:
            c.text(x + w / 2, ry + 43, det, 10.5, 400, "f-var", "middle", mono=True)
    if data_label:
        x = x0 + hdr_len * cw
        w = (ncell - hdr_len) * cw
        c.add(f'<rect class="ghost" x="{x + 1:.1f}" y="{ry}" width="{w - 2:.1f}" height="{h}" rx="10"/>')
        c.text(x + w / 2, ry + 35, data_label, 12.5, 500, "f-var", "middle")
    return ry + h


def build_msg_binarios():
    cw, nc = 38, 40
    W = 48 + 28 + nc * cw + 48
    H = 1030
    c = Canvas(W, H, "Mensajes binarios del WebSocket (servidor → cliente)",
               "Byte 0 = tipo. Todo en little-endian. Cada casilla es un byte; las cifras de arriba son su desplazamiento.")
    x0 = 76
    y = 120
    spec = [(0, 0, "tipo", "=1", "g-bc"), (1, 1, "GPU", "0|1", "g-av"), (2, 3, "reservado", "", "g-data"),
            (4, 7, "n", "u32", "g-mar"), (8, 15, "centro Hz", "f64", "g-ham"), (16, 23, "tasa Hz", "f64", "g-spec"),
            (24, 27, "dbmin", "f32", "g-av"), (28, 31, "dbstep", "f32", "g-mar")]
    y = byte_row(c, x0, y, cw, nc, spec, "n bytes v", 32, "tipo 1", "Espectro", "dBFS = dbmin + v · dbstep  (hoy −145 + 0,6·v)") + 30
    aud = [(0, 0, "tipo", "=2", "g-bc"), (1, 1, "flags", "", "g-av"), (2, 2, "canal", "1|2", "g-mar"),
           (3, 3, "res.", "", "g-data"), (4, 7, "nivel dBFS", "f32", "g-ham")]
    y = byte_row(c, x0, y, cw, nc, aud, "PCM int16 · 48 kHz · intercalado (L,R,L,R…)", 8, "tipo 2", "Audio",
                 "flags: 1 squelch abierto · 2 estéreo · 4 fuera de banda · 8 piloto 19 kHz") + 30
    niv = [(0, 0, "tipo", "=3", "g-bc"), (1, 1, "flags", "", "g-av"), (2, 2, "canal", "", "g-mar"),
           (3, 3, "res.", "", "g-data"), (4, 7, "nivel dBFS", "f32", "g-ham")]
    y = byte_row(c, x0, y, cw, nc, niv, "sin datos", 8, "tipo 3", "Nivel", "igual que el audio, sin PCM: S-meter de quien no escucha") + 30
    bar = [(0, 0, "tipo", "=4", "g-bc"), (1, 1, "0", "", "g-data"), (2, 2, "paso", "bit 0", "g-av"), (3, 3, "%", "", "g-mar"),
           (4, 7, "n", "u32", "g-mar"), (8, 15, "centro Hz", "f64", "g-ham"), (16, 23, "tasa = fin − inicio", "f64", "g-spec"),
           (24, 27, "dbmin", "f32", "g-av"), (28, 31, "dbstep", "f32", "g-mar")]
    y = byte_row(c, x0, y, cw, nc, bar, "n bytes v", 32, "tipo 4", "Barrido", "bin i en inicio + i · tasa / n  (aquí i = inicio del bin)") + 30
    vid = [(0, 0, "tipo", "=5", "g-bc"), (1, 1, "clave", "bit 0", "g-av"), (2, 2, "códec", "", "g-mar"), (3, 3, "sec.", "", "g-data"),
           (4, 11, "PTS (90 kHz)", "i64", "g-ham"), (12, 19, "DTS (90 kHz)", "i64", "g-spec")]
    y = byte_row(c, x0, y, cw, nc, vid, "unidad de acceso H.264 en Annex B", 20, "tipo 5", "Vídeo TV",
                 "códec: 1 H.264 · 2 MPEG-2 · 3 HEVC; las claves llevan SPS+PPS") + 30
    atv = [(0, 0, "tipo", "=6", "g-bc"), (1, 1, "canal", "=2", "g-mar"), (2, 3, "res.", "", "g-data"),
           (4, 7, "tasa Hz", "u32", "g-ham"), (8, 15, "PTS 1.ª muestra", "i64", "g-spec")]
    y = byte_row(c, x0, y, cw, nc, atv, "PCM int16 intercalado", 16, "tipo 6", "Audio TV", "el PTS está desenrollado a 64 bit y nunca retrocede") + 20
    c.text(48, H - 24, "Cabeceras: tipo 1 y 4 → 32 B; tipo 2 y 3 → 8 B; tipo 5 → 20 B; tipo 6 → 16 B. Fuente: docs/arquitectura/WEBSOCKET.md.", 12.5, 400, "f-var")
    return c


# --------------------------------------------------------- 2. secuencia WS
def build_secuencia_ws():
    W, H = 1640, 1640
    c = Canvas(W, H, "Sesión WebSocket de principio a fin",
               "Conexión, instantánea inicial, órdenes, flujo continuo y reconexión (web/js/ws.js, src/net/server.c, src/main.c).")
    xs = (330, 820, 1310)
    top = 190
    lifelines(c, [(xs[0], "Navegador", "web/js/ws.js", "monitor", "g-bc"),
                  (xs[1], "Servidor", "hilo de red · main.c", "server", "g-av"),
                  (xs[2], "Motor y host", "hilo DSP · dec_host", "cpu", "g-spec")], top, H - 40)
    y = top + 24
    phase(c, y, 150, 1, "Conexión")
    msg(c, xs[0], xs[1], y + 78, "GET /ws  ·  Upgrade: websocket", "Origin debe coincidir con Host")
    msg(c, xs[1], xs[0], y + 128, "101 Switching Protocols", None, "dash")
    y += 170
    phase(c, y, 290, 2, "Instantánea al conectar")
    msg(c, xs[1], xs[0], y + 80, "status · signals · recording", None, "pri", "ahp")
    msg(c, xs[1], xs[0], y + 124, "decoders · profiles · tx_policy · dec_list", None, "pri", "ahp")
    msg(c, xs[1], xs[0], y + 168, "dec_state (último conocido)", None, "pri", "ahp")
    msg(c, xs[1], xs[0], y + 212, "últimos 100 dec con \"replay\": true", None, "pri", "ahp")
    c.text(xs[1] + 24, y + 262, "el cliente ya puede pintar sin pedir nada", 12, 400, "f-var")
    y += 310
    phase(c, y, 330, 3, "Órdenes")
    msg(c, xs[0], xs[1], y + 82, "{\"cmd\":\"profile\",\"id\":\"fm\"}", "o config, channel, dec_add, record, tx…")
    msg(c, xs[1], xs[2], y + 136, "engine_request() · dec_host_add()", None)
    msg(c, xs[2], xs[1], y + 186, "aplicado (el DSP pausa el lector si hace falta)", None, "dash")
    msg(c, xs[1], xs[0], y + 236, "status nuevo + dec_list", "a todos los clientes", "pri", "ahp")
    msg(c, xs[1], xs[0], y + 290, "{\"type\":\"error\",\"msg\":…}", "si la orden no es válida", "dash")
    y += 350
    phase(c, y, 330, 4, "Flujo continuo")
    msg(c, xs[2], xs[1], y + 78, "bloque de IQ → espectro, audio, eventos", None)
    msg(c, xs[1], xs[0], y + 128, "binario tipo 1 · espectro", "a la tasa de fps")
    msg(c, xs[1], xs[0], y + 186, "binario tipo 2 · audio 48 kHz", "solo al cliente que escucha; tipo 3 al resto")
    msg(c, xs[1], xs[0], y + 244, "signals ~2 Hz · dec_state ~1 Hz · status ~2 s", None)
    msg(c, xs[1], xs[0], y + 296, "dec (un evento por mensaje válido, ≤ 200/s)", None)
    y += 350
    phase(c, y, 200, 5, "Reconexión")
    c.text(xs[0] + 24, y + 78, "✕ se corta la red o el servidor se reinicia", 12.5, 500, "f-err")
    selfmsg(c, xs[0], y + 96, "espera 0,5 s × 1,7 por intento (máx. 6 s)", 1)
    msg(c, xs[0], xs[1], y + 160, "GET /ws otra vez", "y se repite la instantánea del paso 2")
    return c


# ------------------------------------------------- 3. secuencia del bloque
def build_secuencia_bloque():
    W, H = 1760, 1010
    c = Canvas(W, H, "Recorrido de un bloque de muestras",
               "Del lector al navegador: lo que ocurre con cada bloque de IQ (src/engine.c, src/decoders/host.c, src/net/server.c).")
    xs = (190, 470, 760, 1050, 1340, 1620)
    top = 200
    lifelines(c, [(xs[0], "Lector", "reader_main", "thread", "g-data"),
                  (xs[1], "Hilo DSP", "dsp_main", "cpu", "g-spec"),
                  (xs[2], "Host", "dec_host_feed", "layers", "g-bc"),
                  (xs[3], "Instancia", "dec-<id>", "thread", "g-av"),
                  (xs[4], "Servidor", "server_send_*", "server", "g-mar"),
                  (xs[5], "Navegador", "WebSocket", "monitor", "g-ham")], top, H - 40)
    y = top + 40
    selfmsg(c, xs[0], y - 12, "source_read()", 1, 40)
    y += 56
    msg(c, xs[0], xs[1], y, "ring_push → ring_pop", "anillo de 0,25–0,5 s")
    y += 66
    selfmsg(c, xs[1], y - 12, "espectro (CPU o GPU)", 1, 40)
    msg(c, xs[1], xs[4], y + 40, "tipo 1 · espectro por difusión", None, "pri", "ahp")
    msg(c, xs[4], xs[5], y + 40, "", None, "pri", "ahp")
    y += 92
    msg(c, xs[1], xs[2], y, "dec_host_feed(iq, buf, n)", "copia UNA vez; contador de referencias")
    y += 66
    msg(c, xs[2], xs[3], y, "encola el puntero", "cola de 16 bloques; llena → drops")
    y += 66
    selfmsg(c, xs[3], y - 12, "DDC → decoder", 1, 40)
    y += 52
    msg(c, xs[3], xs[4], y, "emit(): JSON dec", "límite 200 eventos/s")
    msg(c, xs[4], xs[5], y, "", None)
    y += 66
    selfmsg(c, xs[1], y - 12, "canales de audio (Q11)", 1, 40)
    msg(c, xs[1], xs[4], y + 46, "tipo 2 · audio al cliente que escucha", None, "pri", "ahp")
    msg(c, xs[4], xs[5], y + 46, "", None, "pri", "ahp")
    y += 100
    msg(c, xs[1], xs[2], y, "dec_host_periodic()", "dec_state y dec_list ~1 Hz")
    msg(c, xs[2], xs[4], y + 54, "dec_state · dec_list", None, "dash")
    msg(c, xs[4], xs[5], y + 54, "", None, "dash")
    return c


# ----------------------------------------------------- 4. hilos y cerrojos
def build_hilos():
    W, H = 1700, 980
    c = Canvas(W, H, "Hilos y cerrojos",
               "Qué hilos existen, qué cerrojos toman y en qué orden. El hilo DSP solo mantiene cerrojos durante microsegundos.")
    threads = [("Lector", "reader_main · source_read", "thread", "g-data", ["rg_m"]),
               ("Hilo DSP", "dsp_main · engine.c", "cpu", "g-spec", ["rg_m", "e->lock", "tap_lock", "h->lock"]),
               ("Hilo de red", "server_run · poll", "server", "g-av", ["srv->lock", "e->lock"]),
               ("dec-<id> × N", "una por instancia", "layers", "g-bc", ["inst->m", "h->lock"]),
               ("Emisión", "tx.c · job_main", "send", "g-ham", ["tx->lock", "tx->submit"]),
               ("TV", "pub · scan · file", "tv", "g-mar", ["tv->mu"]),
               ("Escritor", "recorder · SigMF", "disk", "g-data", ["rec->lock"])]
    locks = {"rg_m": ("rg_m", "anillo lector → DSP"), "e->lock": ("e->lock", "configuración del motor"),
             "tap_lock": ("tap_lock", "toma de IQ de tx_cal"), "h->lock": ("h->lock", "tabla de instancias y eventos"),
             "inst->m": ("inst->m", "cola y contadores"), "srv->lock": ("srv->lock", "clientes y envío"),
             "tx->lock": ("tx->lock", "estado de la emisión"), "tx->submit": ("tx->submit", "serializa órdenes tx"),
             "tv->mu": ("tv->mu", "servicios y PES"), "rec->lock": ("rec->lock", "cola de escritura")}
    tw_, th_ = 200, 96
    gap = (W - 96 - 7 * tw_) / 6
    tx0 = {}
    for i, (t, s, ic, g, lk) in enumerate(threads):
        x = 48 + i * (tw_ + gap)
        tx0[t] = x + tw_ / 2
        c.node(x, 130, tw_, th_, t, None, ic, g, sublines=[s], iconsize=34, tsize=15, ssize=11.5)
    order = ["rg_m", "e->lock", "tap_lock", "srv->lock", "h->lock", "inst->m", "tx->lock", "tx->submit", "tv->mu", "rec->lock"]
    lw, lh = 150, 66
    lgap = (W - 96 - 10 * lw) / 9
    lx = {}
    ly = 600
    for i, k in enumerate(order):
        x = 48 + i * (lw + lgap)
        lx[k] = x + lw / 2
        c.rect(x, ly, lw, lh, 18, "card", 2)
        c.text(x + lw / 2, ly + 28, locks[k][0], 13.5, 600, "", "middle", mono=True)
        c.text(x + lw / 2, ly + 48, locks[k][1], 10.5, 400, "f-var", "middle")
    # aristas hilo → cerrojo (escalonadas para que no se solapen)
    k = 0
    for i, (t, s, ic, g, lk) in enumerate(threads):
        for j, l in enumerate(lk):
            xs_ = tx0[t] - 40 + j * 26
            ymid = 280 + (k % 7) * 36
            k += 1
            c.arrow([(xs_, 226), (xs_, ymid), (lx[l], ymid), (lx[l], ly)], "", marker="ah", r=10)
    c.rect(48, 800, W - 96, 130, 24, "card2", True)
    c.chip_icon(68, 818, "lock", 40)
    c.text(124, 846, "Reglas de orden", 16, 600, "disp")
    c.lines(124, 876, ["h->lock → inst->m: nunca al revés. Un hilo de instancia no toma h->lock con inst->m cogido.",
                       "El hilo DSP no espera por e->lock cuando arma la toma de tx_cal: usa tap_lock propio.",
                       "dec_host_feed() copia el bloque una vez y encola punteros: no bloquea al hilo DSP."], 12.5, 22)
    return c


# ------------------------------------------------------------ 5. FPGA
def build_fpga_motor():
    W, H = 1700, 760
    c = Canvas(W, H, "Motor de espectro en la FPGA (imagen lab)",
               "Ventana, FFT de flujo, potencia y acumulación de K tramas sin huecos; la salida sustituye al ADC con rx_mux = 3.")
    bw, bh = 330, 150
    xs = [60, 460, 860, 1260]
    row1 = [("Entrada", ["adc_streams · canal activo", "12 bit, 1 muestra cada 2 ciclos", "habilitador ce"], "fpga", "g-data"),
            ("Ventana", ["Blackman-Harris Q15 en ROM", "N = 1024 · 2048 · 4096 · 8192", "xw = (x·w + 1024) >> 11"], "wave", "g-av"),
            ("spec_fft", ["SDF radix-2², 13 etapas", "X_hw = 0,5 · FFT(x·w)", "salida 25 bit, bit-invertida"], "bars", "g-spec"),
            ("|X|² y acumulador", ["re² + im² exacto (50 bit)", "K tramas en 64 bit por bin", "RAM M10K × 2 en ping-pong"], "calc", "g-mar")]
    for x, (t, ls, ic, g) in zip(xs, row1):
        c.node(x, 130, bw, bh, t, None, ic, g, sublines=ls, iconsize=38)
    for a, b in zip(xs[:-1], xs[1:]):
        c.arrow([(a + bw, 205), (b, 205)], "pri", marker="ahp")
    row2 = [("Lectura fftshift", ["N bins, de −N/2 a N/2−1", "1 palabra por ciclo"], "list", "g-ham"),
            ("Formato {E, M}", ["M · 2^E, 26 bit de mantisa", "E = máx(0, msb(acc) − 25)"], "gauge", "g-av"),
            ("FIFO + rx_mux = 3", ["sustituye al flujo del ADC", "trama SC16, sin metadatos"], "layers", "g-data"),
            ("FX3 · USB 3 → host", ["lab_probe / fpga_lab_parser", "(w0, w1) sincronizan la trama"], "monitor", "g-bc")]
    y2 = 380
    for x, (t, ls, ic, g) in zip(xs[::-1], row2):
        c.node(x, y2, bw, 130, t, None, ic, g, sublines=ls, iconsize=38)
    c.arrow([(xs[3] + bw / 2, 280), (xs[3] + bw / 2, y2)], "pri", marker="ahp")
    for a, b in zip(xs[::-1][:-1], xs[::-1][1:]):
        c.arrow([(a, y2 + 65), (b + bw, y2 + 65)], "pri", marker="ahp")
    # banco de registros
    c.rect(60, 590, 1530, 120, 24, "card2", True)
    c.chip_icon(80, 608, "sliders", 40)
    c.text(136, 636, "Banco de registros Wishbone (lab_regs)", 16, 600, "disp")
    c.text(136, 660, "SPEC_LOG2N (N) · SPEC_K (K) · SPEC_CTRL (relleno) configuran el motor; los contadores CNT_* lo vigilan.", 12.5, 400, "f-var")
    c.text(136, 682, "Maestro Wishbone del Nios, a 80 MHz, accesible con bladerf_wishbone_master_read/write (libbladeRF 2.6.1, sin parches).", 12.5, 400, "f-var")
    c.text(W - 48, H - 18, "Fuente: docs/hardware/FIRMWARE.md §10 y §11.", 12, 400, "f-var", "end")
    return c


REGS = [(0, "ID", "RO", "0x4C414231 («LAB1»)"), (1, "VERSION", "RO", "0x00010001"), (2, "FEATURES", "RO", "motor de espectro; log2 N de 10 a 13 (0x0DA1)"),
        (3, "SCRATCH", "RW", "32 bit de prueba"), (4, "SCRATCH_N", "RO", "~SCRATCH"), (5, "SPEC_LOG2N", "RW", "[3:0] log2 N, 10…13 (defecto 12)"),
        (6, "SPEC_K", "RW", "[15:0] tramas promediadas (defecto 16)"), (7, "SPEC_CTRL", "RW", "bit 0 PAD: relleno a múltiplos de 2048 palabras"),
        (8, "CNT_SAMPLES", "RO", "muestras RX del canal activo"), (9, "CNT_FIFO_OVF", "RO", "desbordes de la FIFO"),
        (10, "CNT_FFT", "RO", "tramas de FFT procesadas"), (11, "CNT_SPEC", "RO", "tramas de espectro entregadas"),
        (12, "CNT_LATE", "RO", "tramas cortadas por lectura tardía"), (13, "CNT_WORDS", "RO", "palabras de espectro emitidas"),
        (14, "STATUS", "RO", "bit 0: motor activo (rx_mux = 3 y RX activo)"), (15, "CLEAR", "WO", "cualquier escritura pone a cero 8…13")]


def build_fpga_regs():
    W, H = 1500, 1000
    c = Canvas(W, H, "Mapa de registros Wishbone de la imagen lab",
               "Dirección de byte = 4 × índice (la que se pasa a libbladeRF). Direcciones sin mapear devuelven 0xBAD0BAD0.")
    x0, y0, rh = 48, 124, 44
    cols = [(x0, "Índice"), (x0 + 90, "Byte"), (x0 + 190, "Nombre"), (x0 + 400, "Acceso"), (x0 + 500, "Descripción")]
    c.rect(x0, y0, W - 96, 40, 14, "card3", False)
    for x, t in cols:
        c.text(x + 16, y0 + 26, t, 12.5, 600, "f-var")
    cls = {"RO": "g-av", "RW": "g-mar", "WO": "g-ham"}
    for i, (idx, nm, acc, desc) in enumerate(REGS):
        y = y0 + 50 + i * rh
        if i % 2 == 0:
            c.add(f'<rect class="card" x="{x0}" y="{y}" width="{W - 96}" height="{rh - 4}" rx="12"/>')
        c.text(x0 + 16, y + 27, str(idx), 14, 500, "", mono=True)
        c.text(x0 + 106, y + 27, f"0x{4 * idx:02X}", 14, 500, "f-var", mono=True)
        c.text(x0 + 206, y + 27, nm, 14, 600, "", mono=True)
        c.add(f'<g class="{cls[acc]}">')
        c.pill(x0 + 416, y + 20, acc, 11.5, "gtint", "f-on", h=24, padx=11)
        c.add("</g>")
        c.text(x0 + 516, y + 27, desc, 13.5, 400, "f-on")
    c.text(48, H - 22, "Los registros de configuración no se reinician con sys_reset. Detección segura: marca 0xA en config_gpio[27:24] y luego lectura de ID. Fuente: docs/hardware/FIRMWARE.md §11.", 12, 400, "f-var")
    return c


def build_fpga_trama():
    W, H = 1640, 920
    c = Canvas(W, H, "Formato de la trama de espectro de la FPGA",
               "Palabras de 32 bit; en el flujo SC16 el host ve cada una como (I = bits 15:0, Q = bits 31:16). Sin marca de fin: se sincroniza por (w0, w1).")
    x0, cw = 48, 1544 / 9
    words = [("w0", "0x4C415031", "«LAP1»", "g-bc"), ("w1", "0xB3BEAFCE", "~w0", "g-bc"), ("w2", "[15:0] N · [31:16] K", "", "g-av"),
             ("w3", "contador", "de trama", "g-mar"), ("w4", "índice bajo", "1.ª muestra", "g-ham"), ("w5", "índice alto", "avanza K·N", "g-ham"),
             ("w6", "[7:0] = 8", "[15:8] = 1 · [16] pérdida", "g-data"), ("w7", "palabras", "totales", "g-spec")]
    y = 140
    c.text(x0, y - 10, "Cabecera (8 palabras)", 15, 600, "disp")
    cw = 1544 / 9.5
    for i, (w, a, b, g) in enumerate(words):
        x = x0 + i * cw
        c.add(f'<g class="{g}"><rect class="gtint" x="{x + 2:.1f}" y="{y + 6}" width="{cw - 4:.1f}" height="110" rx="14"/></g>')
        c.text(x + cw / 2, y + 36, w, 16, 700, "", "middle", mono=True)
        c.text(x + cw / 2, y + 66, a, 11.5 if len(a) < 14 else 10.5, 500, "", "middle", mono=True)
        c.text(x + cw / 2, y + 90, b, 10.5, 400, "f-var", "middle")
    x = x0 + 8 * cw
    c.add(f'<rect class="ghost" x="{x + 2:.1f}" y="{y + 6}" width="{cw * 1.5 - 4:.1f}" height="110" rx="14"/>')
    c.text(x + cw * 0.75, y + 56, "w8 … w8+N−1", 12.5, 600, "", "middle", mono=True)
    c.text(x + cw * 0.75, y + 80, "potencias", 12, 400, "f-var", "middle")
    # cuerpo
    y2 = 330
    c.text(x0, y2 - 14, "Cuerpo: N potencias en orden fftshift, y relleno hasta múltiplo de 2048 palabras (PAD = 1)", 15, 600, "disp")
    c.rect(x0, y2, 1544, 90, 18, "card", 2)
    labels = [("bin −N/2", "−fs/2"), ("…", ""), ("bin 0", "centro"), ("…", ""), ("bin N/2−1", "")]
    for i, (a, b) in enumerate(labels):
        xx = x0 + 30 + i * 300
        c.text(xx, y2 + 40, a, 13, 600, "", mono=True)
        if b:
            c.text(xx, y2 + 62, b, 11.5, 400, "f-var")
    # palabra de potencia
    y3 = 500
    c.text(x0, y3 - 14, "Palabra de potencia {E[5:0], M[25:0]}", 15, 600, "disp")
    mw = 1544
    c.add('<g class="g-spec"><rect class="gtint" x="%d" y="%d" width="%d" height="64" rx="14"/></g>' % (x0, y3, 6 / 32 * mw))
    c.add('<g class="g-av"><rect class="gtint" x="%d" y="%d" width="%d" height="64" rx="14"/></g>' % (x0 + 6 / 32 * mw + 6, y3, 26 / 32 * mw - 6))
    c.text(x0 + 3 / 32 * mw, y3 + 38, "E  [31:26]", 14, 600, "", "middle", mono=True)
    c.text(x0 + (6 + 13) / 32 * mw + 3, y3 + 38, "M  [25:0]  mantisa de 26 bit", 14, 600, "", "middle", mono=True)
    c.text(x0, y3 + 100, "valor = M · 2^E  ·  E = máx(0, msb(acc) − 25)  ·  rango de 2^64 con acumulación exacta de K tramas", 13, 400, "f-var")
    # escala
    y4 = 660
    c.rect(x0, y4, 1544, 190, 24, "card2", True)
    c.chip_icon(x0 + 20, y4 + 18, "calc", 42)
    c.text(x0 + 78, y4 + 46, "De la palabra a dBFS", 16, 600, "disp")
    c.text(x0 + 24, y4 + 90, "dBFS[k] = 10·log10( 4·acc[k] / K ) − 20·log10( 2048 · Σw )", 18, 500, "", mono=True)
    c.text(x0 + 24, y4 + 124, "Σw = suma de la ventana Q15 (fpga_lab_window_sum(N)). Un tono de fondo de escala da 0 dBFS, como en src/dsp/spectrum.c.", 12.5, 400, "f-var")
    c.text(x0 + 24, y4 + 148, "Comprobado con el simulador del HDL: tono de amplitud 1000 → −6,47 dBFS (teórico −6,23); suelo de ruido −68,60 dBFS por bin (teórico −68,71).", 12.5, 400, "f-var")
    c.text(x0 + 24, y4 + 172, "Si llega un nuevo (w0, w1) antes de completar w7 palabras, la trama anterior se descarta.", 12.5, 400, "f-var")
    return c


# ---------------------------------------------- 6. diagramas de la guía
def build_flujo_sin_antena():
    W, H = 1640, 520
    c = Canvas(W, H, "Probar sin antena: generar, reproducir y decodificar",
               "Cada protocolo trae su modulador: --gen crea un SigMF, --replay lo reproduce en bucle y el decodificador lo lee como si fuera radio.")
    bw, bh, y = 300, 170, 150
    xs = [48, 400, 752, 1104]
    items = [("--gen rds", ["modulador del protocolo", "ruido, desvío y reloj opcionales"], "wave", "g-bc"),
             ("/tmp/rds.sigmf-*", ["IQ cs16 + metadatos SigMF", "centro, tasa y frecuencia"], "file", "g-data"),
             ("--replay … --profile fm", ["fuente de fichero en bucle", "mismo motor que con la placa"], "play", "g-av"),
             ("Navegador o --print", ["espectro, audio y eventos", "http://127.0.0.1:8090"], "monitor", "g-spec")]
    for x, (t, ls, ic, g) in zip(xs, items):
        c.node(x, y, bw, bh, t, None, ic, g, sublines=ls, iconsize=38, tsize=15)
    for a, b in zip(xs[:-1], xs[1:]):
        c.arrow([(a + bw, y + bh / 2), (b, y + bh / 2)], "pri", marker="ahp")
    c.rect(48, 370, 1356, 100, 22, "card2", True)
    c.text(72, 406, "bladerf-lab --gen rds --gen-freq 100300000 --gen-center 100000000 --gen-rate 2400000 --out RDS", 13, 500, "", mono=True)
    c.text(72, 434, "bladerf-lab --replay RDS.sigmf-meta --profile fm       (o: --decode ais@161975000 --print)", 13, 500, "", mono=True)
    return c


def build_decodificar_como():
    W, H = 1640, 560
    c = Canvas(W, H, "Decodificar como…: de una señal detectada a un decodificador",
               "El detector OS-CFAR marca las portadoras; un clic crea una instancia sobre esa frecuencia (dec_add).")
    bw, bh, y = 330, 200, 150
    xs = [48, 440, 832, 1224]
    items = [("1  Señal detectada", ["barra amarilla sobre el espectro", "frecuencia, ancho y SNR"], "search", "g-spec"),
             ("2  Clic en la marca", ["menú: sintonizar o", "«decodificar como…»"], "eye", "g-av"),
             ("3  Elegir protocolo", ["catálogo con su tasa y ancho", "narrow o wide"], "list", "g-bc"),
             ("4  Instancia en marcha", ["pestaña Decodificadores", "activo · esperando · error"], "layers", "g-mar")]
    for x, (t, ls, ic, g) in zip(xs, items):
        c.node(x, y, bw, bh, t, None, ic, g, sublines=ls, iconsize=40, tsize=16)
    for a, b in zip(xs[:-1], xs[1:]):
        c.arrow([(a + bw, y + bh / 2), (b, y + bh / 2)], "pri", marker="ahp")
    c.pill(1389, 410, "esperando = el canal queda fuera de la banda capturada", 12, "secc", "f-on", anchor="middle")
    c.text(48, 500, "Orden enviada: {\"cmd\":\"dec_add\",\"proto\":\"…\",\"freq\":Hz}. Con un perfil, el host puede crear las instancias solo (auto:true).", 12.5, 400, "f-var")
    return c


def build_problemas():
    W, H = 1700, 760
    c = Canvas(W, H, "Solución de problemas",
               "De síntoma a causa y remedio: equipo ocupado, overruns, sin señal y antena.")
    cols = [("Equipo ocupado", "alert", "g-ham", ["El servidor no abre el bladeRF:", "otro programa lo tiene (p. ej. el", "servicio OpenWebRX)."],
             ["Libéralo antes de arrancar.", "No hay dos procesos a la vez.", "Prueba con --replay para descartar", "el resto del montaje."]),
            ("Overruns", "gauge", "g-data", ["stats.overruns sube: el USB o", "libbladeRF perdieron muestras.", "stats.drops: el DSP no da abasto."],
             ["Baja la tasa (122,88 Msps es el", "límite del USB, no del motor).", "Puerto USB 3 directo, cable corto.", "Libera CPU o activa la GPU."]),
            ("Sin señal", "search", "g-av", ["Cascada plana o solo ruido.", "Decodificadores en «esperando».", "Audio mudo."],
             ["Comprueba centro y tasa: el canal", "debe caer dentro de la banda.", "Sube la ganancia o activa el AGC.", "Mira el bias-tee solo si el LNA lo pide."]),
            ("Antena", "radio", "g-spec", ["Con la antena actual no llega ADS-B", "(1090 MHz) ni UHF suficiente para", "la TDT (SNR ≈ −9 dB frente a ≈ 16 dB)."],
             ["ADS-B: monopolo de ¼ de onda", "(≈ 6,9 cm), alto y despejado.", "TDT: toma de antena de TV de casa", "con adaptador F/IEC a SMA."])]
    cw = 380
    for i, (t, ic, g, sym, fix) in enumerate(cols):
        x = 48 + i * (cw + 24)
        c.rect(x, 130, cw, 560, 26, "card", True)
        c.chip_icon(x + 20, 150, ic, 46, g)
        c.text(x + 80, 182, t, 19, 600, "disp")
        c.text(x + 24, 250, "SÍNTOMA", 11.5, 700, "f-var", spacing=1)
        c.lines(x + 24, 278, sym, 13.5, 24, cls="f-on")
        c.add(f'<path class="line" d="M{x + 24} 390 L{x + cw - 24} 390"/>')
        c.text(x + 24, 424, "REMEDIO", 11.5, 700, "f-pri", spacing=1)
        c.lines(x + 24, 452, fix, 13.5, 24, cls="f-on")
    return c


BUILDS = {
    # nombre: (carpeta, función, título)
    "msg-binarios": ("protocolo", build_msg_binarios, "Mensajes binarios"),
    "secuencia-ws": ("protocolo", build_secuencia_ws, "Sesión WebSocket"),
    "secuencia-bloque": ("arquitectura", build_secuencia_bloque, "Recorrido de un bloque"),
    "hilos-cerrojos": ("arquitectura", build_hilos, "Hilos y cerrojos"),
    "fpga-motor-espectro": ("fpga", build_fpga_motor, "Motor de espectro FPGA"),
    "fpga-registros": ("fpga", build_fpga_regs, "Registros FPGA"),
    "fpga-trama": ("fpga", build_fpga_trama, "Trama FPGA"),
    "flujo-sin-antena": ("guia", build_flujo_sin_antena, "Sin antena"),
    "decodificar-como": ("guia", build_decodificar_como, "Decodificar como"),
    "solucion-problemas": ("guia", build_problemas, "Solución de problemas"),
}

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    render = "--render" in sys.argv
    names = args or list(BUILDS)
    for n in names:
        folder, fn, title = BUILDS[n]
        print(fn().save(n, title))
    if render:
        subprocess.run([os.path.join(IMG, "render.sh")] + names, check=True)
        for n in names:
            folder = BUILDS[n][0]
            os.makedirs(os.path.join(IMG, folder), exist_ok=True)
            for th in ("light", "dark"):
                src = os.path.join(IMG, f"{n}-{th}.png")
                if os.path.exists(src):
                    shutil.move(src, os.path.join(IMG, folder, f"{n}-{th}.png"))
