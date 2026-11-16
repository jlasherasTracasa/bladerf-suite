"""Marco de decodificadores (src/decoders/decoder.h, host.c)."""
from lib import *


def build():
    W, H = 1740, 840
    c = Canvas(W, H, "Marco de decodificadores",
               "Un descriptor por protocolo; el host crea un hilo por instancia y se ocupa del DDC, de las colas y de los eventos.")
    # ---- descriptor
    x, y, w, h = 48, 150, 400, 600
    c.rect(x, y, w, h, 28, "card2", True)
    c.chip_icon(x + 20, y + 18, "file", 44)
    c.text(x + 76, y + 44, "decoder_desc_t", 19, 600, "disp mono")
    c.text(x + 76, y + 64, "src/decoders/<id>.c → DEC_<ID>", 12.5, 400, "f-var")
    groups = [("Identidad", ["id · name · group · desc"]),
              ("Tipo", ["kind: DEC_NARROW | DEC_WIDE", "rate · bw_hz  (narrow)", "wide_rate · wide_min_rate  (wide)", "freqs[4]  (frecuencias típicas)"]),
              ("Funciones", ["create(cfg) · destroy(st)", "process(st, cf32 *iq, n, now, emit)", "process_raw(st, int16 *iq, n, …)", "state_json(st, sb, now)  ~1 Hz", "set_location(st, lat, lon)"])]
    yy = y + 100
    for gname, ls in groups:
        c.pill(x + 24, yy, gname, 11.5, "pric", "f-onpric", padx=10)
        yy += 24
        for l in ls:
            c.text(x + 28, yy + 12, l, 12.5, 400, "f-on", mono=True)
            yy += 21
        yy += 22
    c.rect(x + 24, y + h - 84, w - 48, 60, 16, "card", 2)
    c.text(x + 44, y + h - 58, "registry.c · DECODERS[]", 13.5, 600, "disp")
    c.text(x + 44, y + h - 38, "21 decodificadores, acaba en NULL", 12.5, 400, "f-var")
    # ---- flujo central
    mx = 540
    c.node(mx, 150, 250, 92, "dec_host_feed()", None, "cpu", None, sublines=["hilo DSP · no bloquea nunca"])
    c.arrow([(790, 196), (850, 196)], "pri", marker="ahp")
    c.rect(850, 150, 260, 92, 22, "card", True)
    c.text(872, 186, "Cola de bloques", 15.5, 600, "disp")
    c.text(872, 206, "16 bloques refcontados (~0,3 s)", 12.5, 400, "f-var")
    c.text(872, 224, "llena → drop de ese bloque", 12.5, 400, "f-var")
    # instancia
    ix, iy, iw, ih = 540, 290, 570, 460
    c.rect(ix, iy, iw, ih, 28, "card2", True)
    c.chip_icon(ix + 20, iy + 18, "thread", 44)
    c.text(ix + 76, iy + 44, "Hilo de instancia  dec-<id>", 18, 600, "disp")
    c.text(ix + 76, iy + 64, "configuración perezosa: compara (fs, center, freq) de cada bloque", 12.5, 400, "f-var")
    # narrow
    c.add('<g class="g-spec">')
    c.rect(ix + 24, iy + 100, 255, 230, 22, "gtint", 2)
    c.add('</g>')
    c.pill(ix + 44, iy + 128, "NARROW", 12, "pri", "f-onpri", padx=12)
    ds = ["NCO", "FIR ÷8 máx. por etapa", "cfir_t · ancho bw_hz", "cresamp_t → rate exacto"]
    for i, d in enumerate(ds):
        yy = iy + 160 + i * 34
        c.rect(ix + 44, yy, 215, 26, 13, "card", 2)
        c.text(ix + 151, yy + 18, d, 12, 500, "f-on", "middle")
        if i < 3:
            c.arrow([(ix + 151, yy + 26), (ix + 151, yy + 34)], "", marker="ah", r=1)
    c.text(ix + 44, iy + 316, "process(cf32 normalizado)", 12, 600, "f-on", mono=True)
    c.text(ix + 44, iy + 334, "", 12)
    # wide
    c.add('<g class="g-data">')
    c.rect(ix + 291, iy + 100, 255, 230, 22, "gtint", 2)
    c.add('</g>')
    c.pill(ix + 311, iy + 128, "WIDE", 12, "pri", "f-onpri", padx=12)
    c.lines(ix + 311, iy + 168, ["Sin DDC: muestras Q11 crudas", "(I,Q,I,Q…; ±2048)", "a la tasa del hardware.", "", "exige fs == wide_rate", "o fs ≥ wide_min_rate"], 12.5)
    c.text(ix + 311, iy + 316, "process_raw(int16 Q11)", 12, 600, "f-on", mono=True)
    # estados
    c.text(ix + 24, iy + 372, "Estado de la instancia", 13.5, 600, "disp")
    xx = ix + 24
    xx += c.pill(xx, iy + 408, "ok", 12.5, "secc", "f-ok", h=28, weight=600) + 8
    xx += c.pill(xx, iy + 408, "waiting · fuera de la banda", 12.5, "secc", "f-warn", h=28, weight=600) + 8
    c.pill(xx, iy + 408, "error", 12.5, "secc", "f-err", h=28, weight=600)
    c.text(ix + 24, iy + 440, "error: tasa incompatible («requiere exactamente 2 Msps») o create() devuelve NULL", 11.5, 400, "f-var")
    c.arrow([(1160, 196), (1160, 200), (ix + iw / 2 + 300, 200), (ix + iw / 2 + 300, 290)], "pri", marker="ahp") if False else None
    c.arrow([(1110, 196), (1135, 196), (1135, 270), (ix + iw - 40, 270), (ix + iw - 40, 290)], "pri", marker="ahp")
    # ---- salida
    ox = 1170
    c.text(ox, 172, "Eventos", 17, 600, "disp")
    outs = [("emit(json)", "un evento por mensaje válido (CRC/FEC)", "message"),
            ("Envoltorio", '{"type":"dec","inst","proto","freq","t","msg"}', "box"),
            ("Cubo de fichas", "200 eventos/s por instancia", "gauge"),
            ("Anillo de 100", "se reenvía con replay:true", "repeat"),
            ("Salidas", "WebSocket · --print · --log-events", "send")]
    oy = 190
    for i, (t, s, ic) in enumerate(outs):
        yy = oy + i * 88
        c.rect(ox, yy, 520, 72, 20, "card", 2)
        c.chip_icon(ox + 14, yy + 14, ic, 44)
        c.text(ox + 72, yy + 33, t, 14.5, 600, "disp")
        c.text(ox + 72, yy + 53, s, 12, 400, "f-var", mono=(i == 1))
        if i < 4:
            c.arrow([(ox + 36, yy + 72), (ox + 36, yy + 88)], "", marker="ah", r=1)
    c.arrow([(ix + iw, iy + 150), (ox - 24, iy + 150), (ox - 24, oy + 36), (ox, oy + 36)], "pri", marker="ahp", label="emit", lpos=.4)
    # estado
    sy = 650
    c.rect(ox, sy, 520, 100, 24, "card2", True)
    c.chip_icon(ox + 16, sy + 16, "list", 44)
    c.text(ox + 76, sy + 38, "dec_state y dec_list", 15, 600, "disp")
    c.text(ox + 76, sy + 58, "state_json() ~1 Hz (tablas: aviones, barcos, celdas…)", 12, 400, "f-var")
    c.text(ox + 76, sy + 76, "dec_list: events, drops y load por instancia", 12, 400, "f-var")
    c.arrow([(ix + iw, iy + 330), (ox - 24, iy + 330), (ox - 24, sy + 50), (ox, sy + 50)], "", marker="ah")
    c.text(48, 790, "Ninguna instancia pide h->lock con inst->m cogido; process* se llama siempre desde el mismo hilo (sin cerrojos internos).", 12.5, 400, "f-var")
    return c
