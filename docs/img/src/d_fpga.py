"""Imagen FPGA «lab»: ruta de RX con el motor de espectro y el banco Wishbone (docs/FIRMWARE.md §9–§12)."""
from lib import *


def build():
    W, H = 1740, 1060
    c = Canvas(W, H, "Imagen FPGA «lab»: espectro dentro de la FPGA",
               "bladeRF 2.0 micro xA9 (Cyclone V 5CEBA9). Con rx_mux ≠ 3 la ruta de datos es la de serie; con rx_mux = 3 sale el espectro promediado.")
    y0, h = 150, 250
    c.node(48, y0 + 60, 176, 110, "AD9361", None, "radio", "g-bc", sublines=["ADC de 12 bits", "canal activo (×1)"])
    c.arrow([(224, y0 + 115), (262, y0 + 115)], "pri", marker="ahp")
    c.rect(262, y0 + 80, 104, 70, 20, "pric", True)
    c.text(314, y0 + 112, "rx_mux", 14, 600, "f-onpric", "middle")
    c.text(314, y0 + 132, "= 3", 14, 700, "f-onpric", "middle")
    # motor
    ex, ew = 420, 1000
    c.rect(ex, y0, ew, h, 30, "card2", True)
    c.chip_icon(ex + 20, y0 + 16, "fpga", 42, "g-spec")
    c.text(ex + 74, y0 + 42, "Motor de espectro promediado · spec_engine", 17, 600, "disp")
    c.text(ex + 74, y0 + 62, "todo el HDL es propio (sin IP de Altera): solo RAM, ROM, PLL y multiplicadores inferidos", 12, 400, "f-var")
    steps = [("Ventana", ["Blackman-Harris Q15", "ROM 7680 palabras"]),
             ("spec_fft", ["SDF radix-2², 13 etapas", "N = 1024 … 8192"]),
             ("|X|² + acumulación", ["K tramas en 64 bit", "2 bancos ping-pong"]),
             ("Lectura fftshift", ["1 palabra por ciclo", "relleno a 2048 (PAD)"]),
             ("Formato {E, M}", ["M·2^E: 26 bit de", "mantisa, 6 de exponente"])]
    sw, sg = 168, 24
    for i, (t, ls) in enumerate(steps):
        x = ex + 24 + i * (sw + sg)
        c.rect(x, y0 + 88, sw, 134, 20, "card", 2)
        c.add(f'<circle class="pri" cx="{x + 24}" cy="{y0 + 114}" r="13"/>')
        c.text(x + 24, y0 + 119, str(i + 1), 13, 700, "f-onpri", "middle")
        c.text(x + 14, y0 + 154, t, 13.5, 600, "disp")
        c.lines(x + 14, y0 + 178, ls, 11.5, 17)
        if i < 4:
            c.arrow([(x + sw + 2, y0 + 155), (x + sw + sg - 2, y0 + 155)], "pri", marker="ahp", r=1)
    c.arrow([(366, y0 + 115), (ex, y0 + 115)], "pri", marker="ahp")
    c.node(1470, y0 + 60, 222, 110, "FIFO + FX3", None, "plug", None, sublines=["flujo SC16_Q11 normal", "USB 3, sin metadatos"])
    c.arrow([(ex + ew, y0 + 115), (1470, y0 + 115)], "pri", marker="ahp")
    # ruta de serie
    c.arrow([(314, y0 + 150), (314, y0 + h + 40), (1530, y0 + h + 40), (1530, y0 + 170)], "dash", marker="ah")
    c.label(1250, y0 + h + 40, "rx_mux ≠ 3: ruta de serie (hosted), sin cambios", 12)
    # host
    c.node(1470, 480, 222, 120, "Host", None, "cpu", "g-bc", sublines=["fpga_lab_parser", "sincroniza por «LAP1»"])
    c.arrow([(1636, y0 + 170), (1636, 480)], "pri", marker="ahp", label="USB", lpos=.45)
    # banco wishbone
    wy = 480
    c.text(48, wy - 14, "Banco de registros Wishbone", 17, 600, "disp")
    chain = [("libbladeRF", ["bladerf_wishbone_", "master_read/write"], "wrench"),
             ("Nios · wishbone_master", ["1 ciclo de petición,", "1 ack"], "cpu"),
             ("lab_regs", ["16 registros · responde", "siempre (0xBAD0BAD0)"], "list"),
             ("lab_cdc_bus", ["handshake sys_clock →", "rx_clock (80 → 125 MHz)"], "repeat")]
    cw = 250
    for i, (t, ls, ic) in enumerate(chain):
        x = 48 + i * (cw + 40)
        c.node(x, wy, cw, 120, t, None, ic, "g-spec" if i == 2 else None, sublines=ls)
        if i < 3:
            c.arrow([(x + cw + 2, wy + 60), (x + cw + 38, wy + 60)], "pri", marker="ahp")
    # a motor
    cxl = 48 + 3 * (cw + 40) + cw
    c.arrow([(cxl, wy + 40), (cxl + 40, wy + 40), (cxl + 40, y0 + h + 4 - 4), (cxl + 40 - 0, y0 + h)], "pri", marker="ahp", label="N, K, PAD", lpos=.5) if False else None
    c.arrow([(48 + 3 * (cw + 40) + cw / 2, wy), (48 + 3 * (cw + 40) + cw / 2, y0 + h + 4)], "pri", marker="ahp", label="N · K · PAD", lpos=.5)
    # tarjetas inferiores
    by, bh = 660, 340
    # registros
    c.rect(48, by, 560, bh, 26, "card2", True)
    c.text(72, by + 38, "Registros principales (índice = dirección/4)", 15, 600, "disp")
    regs = [("0 ID", "0x4C414231 «LAB1»"), ("2 FEATURES", "0x0DA1 (N 1024…8192)"), ("5 SPEC_LOG2N", "10…13 (por defecto 12)"),
            ("6 SPEC_K", "tramas promediadas ≥ 1 (16)"), ("7 SPEC_CTRL", "bit 0 = PAD"),
            ("8–13", "contadores: muestras, FIFO, FFT, espectro, tardías, palabras"), ("14 STATUS", "bit 0 = motor activo"), ("15 CLEAR", "pone a 0 los contadores")]
    for i, (a, b) in enumerate(regs):
        yy = by + 68 + i * 32
        c.text(72, yy + 14, a, 12.5, 600, "f-pri", mono=True)
        c.text(222, yy + 14, b, 12.5, 400, "f-on")
    # trama
    c.rect(632, by, 520, bh, 26, "card2", True)
    c.text(656, by + 38, "Trama de salida (palabras de 32 bit)", 15, 600, "disp")
    words = [("w0", "0x4C415031 «LAP1»"), ("w1", "0xB3BEAFCE (= ~w0)"), ("w2", "N (15:0) · K (31:16)"), ("w3", "contador de trama"),
             ("w4–5", "marca de tiempo de la 1.ª muestra"), ("w6", "8 palabras · versión 1 · trama perdida"), ("w7", "palabras totales (con relleno)"),
             ("w8…", "N potencias, bin −N/2 … N/2−1")]
    for i, (a, b) in enumerate(words):
        yy = by + 68 + i * 32
        c.rect(656, yy, 52, 24, 12, "pric", 2)
        c.text(682, yy + 17, a, 11.5, 600, "f-onpric", "middle", mono=True)
        c.text(722, yy + 17, b, 12.5, 400, "f-on")
    c.text(656, by + 68 + 8 * 32 + 14, "Se sincroniza por (w0, w1); dBFS = 10·log10(4·acc/K) − 20·log10(2048·Σw)", 11, 400, "f-var")
    # recursos
    c.rect(1176, by, 516, bh, 26, "card2", True)
    c.text(1200, by + 38, "Recursos y temporización (xA9)", 15, 600, "disp")
    res = [("ALM", 9510, 113560, "9 510 · 8 %"), ("M10K", 512, 1220, "512 · 42 %"), ("DSP", 42, 342, "42 · 12 %"), ("PLL", 3, 8, "3 / 8")]
    for i, (n, v, tot, tx) in enumerate(res):
        yy = by + 70 + i * 40
        c.text(1200, yy + 14, n, 12.5, 600, "f-on")
        c.rect(1262, yy, 270, 16, 8, "card3", False)
        c.add(f'<rect class="pri" x="1262" y="{yy}" width="{max(8, 270 * v / tot):.1f}" height="16" rx="8"/>')
        c.text(1544, yy + 13, tx, 12, 500, "f-var")
    c.pill(1200, by + 262, "reloj de muestras: holgura +0,010 ns", 12, "terc", "f-on", padx=11)
    c.text(1200, by + 296, "Multiciclo de 2 ciclos en la cadena del motor: solo vale ≤ 61,44 Msps", 11.5, 400, "f-var")
    c.text(1200, by + 314, "(el modo oversample de 122,88 Msps no está soportado). Imagen 0.16.101.", 11.5, 400, "f-var")
    return c
