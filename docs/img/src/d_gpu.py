"""GPU: DDC por lotes, canalizador PFB y espectro asíncrono (docs/arquitectura/CUDA.md)."""
from lib import *


def flow(c, x, y, steps, w=176, gap=34, h=118, g=None):
    for i, (t, ls) in enumerate(steps):
        xx = x + i * (w + gap)
        c.rect(xx, y, w, h, 20, "card", 2)
        c.add(f'<circle class="pri" cx="{xx + 22}" cy="{y + 24}" r="12"/>')
        c.text(xx + 22, y + 28.5, str(i + 1), 12, 700, "f-onpri", "middle")
        c.text(xx + 42, y + 29, t, 13.5, 600, "disp")
        for j, l in enumerate(ls):
            c.text(xx + 14, y + 56 + j * 17, l, 11.5, 400, "f-var")
        if i < len(steps) - 1:
            c.arrow([(xx + w + 2, y + h / 2), (xx + w + gap - 2, y + h / 2)], "pri", marker="ahp", r=1)


def tile(c, x, y, w, big, small, g):
    c.add(f'<g class="{g}">')
    c.rect(x, y, w, 96, 20, "gtint", 2)
    c.text(x + 18, y + 46, big, 26, 700, "disp gt")
    c.add('</g>')
    c.lines(x + 18, y + 68, small, 11.5, 15)


def build():
    W, H = 1740, 1070
    c = Canvas(W, H, "GPU: tres piezas de CUDA escritas desde cero",
               "RTX 4080 Laptop (sm_89), CUDA 13.2, CPU de 20 hilos. Sin CUDA el programa sigue por CPU (sustitutos *_stub.c). Sin cuFFT.")
    P = [(130, "DDC por lotes", "ddc_gpu · ddc_mgr · hilo «ddc-gpu»", "zap", "g-spec"),
         (430, "Canalizador polifásico (PFB)", "pfb_gpu · un solo núcleo k_pfb", "layers", "g-av"),
         (730, "Espectro asíncrono", "spectrum_gpu · dos ranuras", "bars", "g-bc")]
    for y, t, s, ic, g in P:
        c.rect(48, y, 1644, 280, 30, "card2", True)
        c.chip_icon(72, y + 20, ic, 46, g)
        c.text(132, y + 46, t, 21, 600, "disp")
        c.text(132, y + 67, s, 12.5, 400, "f-var")
    # --- 1 DDC
    y = 130
    flow(c, 72, y + 98, [("Bloque Q11", ["copia a memoria pinned", "+ 131 072 muestras de", "historial (sin estado)"]),
                        ("k_ddc", ["un warp = una salida", "FIR polifásico complejo", "≈ 13 MAC por muestra"]),
                        ("Hasta 64 canales", ["frecuencia arbitraria,", "un solo bloque en GPU", "retune sin clics"]),
                        ("Tasa fs/D", ["a cada instancia; la", "etapa fina sigue en CPU", "(≈ 1 % de núcleo)"])])
    c.text(72, y + 236, "y[k] = e^(−jθ·e_k) · Σ c[j]·x[e_k − j]   con   c[j] = h[j]·e^(jθ·j)/2048   (solo se calculan las salidas)", 12.5, 500, "f-pri", mono=True)
    tx = 920
    c.text(tx + 4, y + 80, "16 instancias a 20 Msps (CPU del proceso, núcleos)", 13, 600, "disp")
    bw = 560
    for i, (nm, v, g, lab) in enumerate([("DDC en CPU", 5.2, "g-data", "5,2"), ("DDC en GPU", 0.32, "g-spec", "0,32")]):
        yy = y + 92 + i * 34
        c.text(tx + 4, yy + 20, nm, 12.5, 500, "f-on")
        c.add(f'<g class="{g}"><rect class="gsolid" x="{tx + 112}" y="{yy + 4}" width="{max(10, bw * v / 5.2 * .72):.1f}" height="24" rx="12"/>')
        c.text(tx + 112 + max(10, bw * v / 5.2 * .72) + 10, yy + 22, lab, 14, 700, "gt")
        c.add('</g>')
    tile(c, tx + 4, y + 176, 206, "739 Msps", ["sostenidos (16 canales", "a 20 Msps): 2,7 % de carga"], "g-av")
    tile(c, tx + 224, y + 176, 206, "−80 dB", ["rechazo del filtro", "(Kaiser) · SNR 72–95 dB"], "g-spec")
    tile(c, tx + 444, y + 176, 206, "≥ 2 narrow", ["y fs/rate ≥ 16 para", "pasar a la GPU"], "g-bc")
    # --- 2 PFB
    y = 430
    flow(c, 72, y + 98, [("Pliegue", ["z[m] = Σ h[m+pM]·x[e−m−pM]", "lecturas coalescidas"]),
                        ("FFT en compartida", ["Stockham radix-2 de M ≤ 4096", "ping-pong, 1 barrera/etapa"]),
                        ("Reorden + fase", ["y_k = X[−k]·e^(−j2πk·e/M)", "fase continua entre tramas"]),
                        ("M canales", ["uniformes, sobremuestreo 2×", "tasa por canal = ov·fs/M"])], 180, 30, 118)
    c.text(72, y + 236, "Estructura de Merry (arXiv:2303.09886): prototipo Kaiser de P·M coeficientes, 80 dB, corte a ±fs/M. Aún no lo usa ninguna función.", 12, 400, "f-var")
    tile(c, tx + 4, y + 112, 206, "1 205 Msps", ["de cálculo (M=1024, ov=2, P=8);", "348 con la bajada a la CPU (PCIe)"], "g-av")
    tile(c, tx + 224, y + 112, 206, "−71,7 dB", ["rechazo a ≥ 2 canales", "(se exigía > 60)"], "g-spec")
    tile(c, tx + 444, y + 112, 206, "139 dB", ["de acuerdo con la definición", "directa en doble precisión"], "g-bc")
    # --- 3 espectro
    y = 730
    flow(c, 72, y + 98, [("host_buffer", ["ranura de entrada pinned", "o NULL si las dos están", "en vuelo"]),
                        ("submit", ["cudaMemcpyAsync + núcleos", "+ copia del resultado", "+ evento; vuelve ya"]),
                        ("poll", ["cudaEventQuery: nunca", "espera. La trama sale en", "la llamada siguiente"]),
                        ("Sin ranura", ["la trama se descarta en", "vez de bloquear el hilo", "DSP (≤ 8 Mi muestras)"])])
    c.text(72, y + 236, "Antes: cudaStreamSynchronize con espera activa en el hilo DSP (≈ 6–7 ms por trama a 20 fps).", 12, 400, "f-var")
    tile(c, tx + 4, y + 112, 206, "122,9 Msps", ["sostenidos con espectro a", "~20 fps (4096–65536)"], "g-av")
    tile(c, tx + 224, y + 112, 206, "0,62 ms", ["por bloque de 1 Mi (FFT 4096,", "122,88 Msps); antes 1,43 ms"], "g-spec")
    tile(c, tx + 444, y + 112, 206, "880–1000", ["Msps de la GPU en sí:", "12–14 % a 122,88 Msps"], "g-ham")
    c.text(W - 48, H - 24, "Medidas de docs/arquitectura/CUDA.md (DDC: make test T=gpu; carga: --replay de 12 s). Pueden variar con la máquina.", 12, 400, "f-var", "end")
    return c
