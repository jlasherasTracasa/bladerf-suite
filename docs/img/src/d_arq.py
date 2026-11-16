"""Arquitectura: hilos y flujo de datos (src/engine.c, src/decoders/host.c, docs/CUDA.md)."""
from lib import *


def build():
    W, H = 1760, 1210
    c = Canvas(W, H, "Arquitectura: hilos y flujo de datos",
               "Un solo binario. El hilo DSP no se bloquea nunca: reparte punteros y cada decodificador trabaja en su propio hilo.")
    # --- fuentes
    c.node(48, 190, 232, 104, "bladeRF 2.0 micro", "libbladeRF · sync_rx", "radio", "g-bc")
    c.node(48, 316, 232, 104, "Fichero IQ", "cs16 · cs8 · cu8 · SigMF", "file", "g-data")
    # --- lector
    c.node(330, 226, 214, 160, "Hilo lector", None, "thread", None,
           sublines=["source_read()", "anillo de bloques", "0,25–0,5 s"])
    c.pill(437, 408, "drops · overruns", 11.5, "secc", "f-on", anchor="middle")
    c.arrow([(280, 242), (305, 242), (305, 290), (330, 290)], "pri", marker="ahp")
    c.arrow([(280, 368), (305, 368), (305, 330), (330, 330)], "pri", marker="ahp")
    # --- hilo DSP
    dx, dy, dw, dh = 596, 150, 330, 760
    c.rect(dx, dy, dw, dh, 28, "card2", True)
    c.chip_icon(dx + 20, dy + 18, "cpu", 44)
    c.text(dx + 76, dy + 46, "Hilo DSP", 20, 600, "disp")
    c.text(dx + 76, dy + 66, "engine.c · dsp_main", 12.5, 400, "f-var")
    steps = [("1", "Espectro", "FFT propia en CPU o GPU"),
             ("2", "Grabación", "recorder_push → hilo escritor"),
             ("3", "Continua + float", "solo si alguien lo usa"),
             ("4", "dec_host_feed()", "encola punteros: no bloquea"),
             ("5", "Canales de audio", "uno por cliente, desde Q11"),
             ("6", "dec_host_periodic()", "dec_state y dec_list ~1 Hz")]
    sy = []
    for i, (n, t, s) in enumerate(steps):
        y = dy + 100 + i * 100
        sy.append(y)
        c.rect(dx + 20, y, dw - 40, 80, 20, "card", 2)
        c.add(f'<circle class="pri" cx="{dx + 50}" cy="{y + 40}" r="15"/>')
        c.text(dx + 50, y + 45, n, 14, 700, "f-onpri", "middle")
        c.text(dx + 78, y + 36, t, 15, 600, "disp")
        c.text(dx + 78, y + 56, s, 12, 400, "f-var")
    c.arrow([(544, 306), (570, 306), (570, 520), (596, 520)], "pri", marker="ahp", label="IQ Q11", lpos=0.3)
    c.pill(dx + dw / 2, dy + dh - 24, "un hilo · nunca espera a nadie", 12, "pric", "f-onpric", anchor="middle")
    # --- columna D
    X = 976
    cy = [s + 40 for s in sy]
    c.node(X, sy[0] - 6, 290, 92, "Espectro + detector", None, "bars", "g-spec", sublines=["CPU o CUDA · OS-CFAR cada 0,5 s"])
    c.arrow([(dx + dw, cy[0]), (X, cy[0])], "pri", marker="ahp")
    c.node(X, sy[1] - 6, 290, 70, "Grabador SigMF", None, "disk", "g-data", sublines=["recordings/*.sigmf-*"])
    c.arrow([(dx + dw, cy[1]), (X, cy[1])], "pri", marker="ahp")
    # host
    hy = 428
    c.rect(X, hy, 290, 204, 24, "card", True)
    c.chip_icon(X + 16, hy + 16, "layers", 40)
    c.text(X + 68, hy + 42, "Host de decodificadores", 15.5, 600, "disp")
    c.lines(X + 20, hy + 80, ["cola de 16 bloques refcontados", "por instancia (~0,3 s)", "narrow: cf32 sin continua", "wide: Q11 crudo"], 12.5)
    c.pill(X + 20, hy + 180, "≥ 2 narrow y fs/rate ≥ 16 → DDC en GPU", 11, "terc", "f-on", padx=9)
    c.arrow([(dx + dw, cy[3]), (X, cy[3])], "pri", marker="ahp")
    c.node(X, sy[4] - 6, 290, 92, "Canales de audio", None, "volume", "g-ham", sublines=["WFM · NFM · AM · USB · LSB · CW"])
    c.arrow([(dx + dw, cy[4]), (X, cy[4])], "pri", marker="ahp")
    # --- instancias y GPU
    IX = 1316
    c.rect(IX, hy, 190, 204, 24, "card", True)
    c.chip_icon(IX + 16, hy + 16, "thread", 40)
    c.text(IX + 66, hy + 42, "Instancias", 15.5, 600, "disp")
    c.lines(IX + 20, hy + 80, ["1 hilo dec-<id>", "DDC → decoder", "eventos JSON", "≤ 200/s (drops)", "state_json ~1 Hz"], 12.5)
    c.arrow([(X + 290, hy + 100), (IX, hy + 100)], "pri", marker="ahp", label="bloques")
    c.node(IX, 660, 190, 92, "Hilo ddc-gpu", None, "zap", "g-spec", sublines=["DDC por lotes · CUDA"])
    c.arrow([(X + 290, hy + 175), (X + 310, hy + 175), (X + 310, 706), (IX, 706)], "", marker="ah", label="Q11", lpos=.8)
    c.arrow([(IX + 95, 660), (IX + 95, hy + 204)], "", marker="ah", label="mid", lpos=.5)
    # --- servidor
    SX = 1556
    c.rect(SX, 190, 160, 620, 24, "card2", True)
    c.chip_icon(SX + 16, 206, "server", 40)
    c.text(SX + 16, 272, "Servidor", 16, 600, "disp")
    c.text(SX + 16, 292, "HTTP + WS", 13, 500, "f-var")
    c.lines(SX + 16, 322, ["hilo de red (poll)", "", "órdenes JSON →", "engine_request()", "dec_host_*()", "tx_submit()", "tv_*()"], 12.5)
    for i, t in enumerate(["tipo 1 espectro", "tipo 2/3 audio", "tipo 5/6 TV", "JSON: dec, status…"]):
        c.pill(SX + 16, 520 + i * 36, t, 11, "secc", "f-on", padx=9)
    c.arrow([(X + 290, cy[0]), (SX, cy[0])], "", marker="ah")
    c.arrow([(IX + 190, hy + 100), (SX, hy + 100)], "", marker="ah", label="dec", lpos=.5)
    c.arrow([(X + 145, sy[4] - 6 + 92), (X + 145, 790), (SX, 790)], "", marker="ah")
    # --- navegador
    c.node(SX - 20, 860, 200, 110, "Navegador", None, "monitor", "g-bc", sublines=["Receptor · Mapa · TV", "Mensajes · Emisión…"])
    c.arrow([(SX + 80, 810), (SX + 80, 860)], "pri", marker="ahp", start_marker=True, label="WS", lpos=.5)
    by = 1040
    c.node(596, by, 330, 110, "Hilo de emisión · tx.c", None, "send", "g-ham",
           sublines=["modulador → política → SigMF / bucle / aire", "único hilo que llama a source_tx_*"])
    c.node(976, by, 330, 110, "tv.c · TV2", None, "tv", "g-bc",
           sublines=["TS → PSI/PES → H.264 + MP2", "hilos pub · scan · file"])
    c.node(1346, by, 360, 110, "Decodificador «tdt»", None, "video", "g-bc",
           sublines=["DDC a 64/7 MHz → dvbt_rx (TS)", "→ tv_ts_in() → tv.c"])
    c.arrow([(SX + 80, 970), (SX + 80, 1005), (761, 1005), (761, by)], "dash", marker="ah", label="tx", lpos=.8)
    c.arrow([(1141, 1005), (1141, by)], "dash", marker="ah", label="tv", lpos=.5)
    c.arrow([(1346, by + 55), (1306, by + 55)], "", marker="ah")
    c.text(W - 48, H - 22, "Solo el hilo DSP llama a las funciones de recepción de libbladeRF; la emisión tiene su propio hilo.", 12.5, 400, "f-var", "end")
    return c
