#!/usr/bin/env python3
"""plots_tecnicos.py — gráficas técnicas de la documentación (DOC2b).

Los datos salen de programas C que enlazan con NUESTRO src/dsp (build/doc2b/*.c,
ver build/doc2b/build.sh): coeficientes reales de los filtros, el espectro
calibrado de spectrum.c, el BER de fec.c y la medida de ancho ocupado de tx.c.
Las cifras de rendimiento son las que ya figuran en RENDIMIENTO.md y CUDA.md.

Uso:  python3 docs/img/src/plots_tecnicos.py [nombre ...]   (sin argumentos: todas)
Salida: docs/img/<carpeta>/<nombre>-light.png y -dark.png (estilo Material 3 de gen.py).
"""
import os
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager as fm
from matplotlib.patches import Rectangle

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DATA = os.path.join(ROOT, "build", "doc2b", "data")
IMG = os.path.join(ROOT, "docs", "img")

for w in ("Regular", "Medium", "SemiBold"):
    p = f"/usr/share/fonts/opentype/inter/Inter-{w}.otf"
    if os.path.exists(p):
        fm.fontManager.addfont(p)

THEMES = {
    "light": dict(bg="#FEF7FF", card="#F3EDF7", on="#1D1B20", var="#49454F", outline="#CAC4D0", grid="#E0D9E3",
                  pri="#6750A4", c=["#6750A4", "#0A6AA6", "#08695B", "#8A5700", "#B0286A", "#26702B"], ok="#26702B",
                  warn="#8A5700", err="#B3261E", shade="#EADDFF"),
    "dark": dict(bg="#141218", card="#211F26", on="#E6E0E9", var="#CAC4D0", outline="#49454F", grid="#3A3741",
                 pri="#D0BCFF", c=["#D0BCFF", "#8ECDF7", "#7ED6C4", "#F7C873", "#F2A7C3", "#A6E3A1"], ok="#A6E3A1",
                 warn="#F7C873", err="#F2B8B5", shade="#4F378B"),
}
T = THEMES["light"]


def style(theme):
    global T
    T = THEMES[theme]
    plt.rcParams.update({
        "font.family": "Inter", "font.size": 10.5, "text.color": T["on"], "axes.labelcolor": T["var"],
        "axes.edgecolor": T["outline"], "axes.facecolor": T["card"], "figure.facecolor": T["bg"],
        "savefig.facecolor": T["bg"], "xtick.color": T["var"], "ytick.color": T["var"],
        "grid.color": T["grid"], "grid.linewidth": 0.8, "axes.grid": True, "axes.axisbelow": True,
        "axes.spines.top": False, "axes.spines.right": False, "legend.frameon": False,
        "axes.titlesize": 11.5, "axes.titleweight": "semibold", "axes.titlelocation": "left",
        "lines.linewidth": 1.8,
    })


def head(fig, title, sub=None):
    fig.text(0.035, 0.965, title, fontsize=17, fontweight="semibold", va="top", color=T["on"])
    if sub:
        fig.text(0.035, 0.915, sub, fontsize=10.5, va="top", color=T["var"])


def save(fig, folder, name, theme):
    d = os.path.join(IMG, folder)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, f"{name}-{theme}.png")
    fig.savefig(p, dpi=140)
    plt.close(fig)
    if os.path.getsize(p) > 380 * 1024:  # límite 400 KB: cuantiza a paleta
        os.system(f"convert '{p}' +dither -colors 128 -define png:compression-level=9 'PNG8:{p}'")
    return p


def load_taps(name):
    path = os.path.join(DATA, name)
    with open(path) as f:
        hdr = dict(kv.split("=") for kv in f.readline()[1:].split())
    arr = np.loadtxt(path, comments="#")
    if hdr.get("kind") == "complex":
        arr = arr[:, 0] + 1j * arr[:, 1]
    return hdr, arr


def db(x, floor=-140):
    return np.maximum(20 * np.log10(np.abs(x) + 1e-12), floor)


# ----------------------------------------------------------------------- DSP
def fig_firdec():
    """Respuesta de los FIR de Kaiser del diezmador (firdec_t) en dos etapas reales del DDC."""
    fig, axs = plt.subplots(1, 2, figsize=(11, 4.6))
    fig.subplots_adjust(top=0.76, bottom=0.14, left=0.07, right=0.98, wspace=0.22)
    head(fig, "Diezmador FIR de Kaiser (firdec_t)",
         "Etapa ÷8 de un DDC con canal de 200 kHz. Coeficientes reales de src/dsp/filter.c; 1023 taps como máximo.")
    cases = [("firdec_a.csv", 1e3, "kHz", "2,4 Msps → 300 ksps"), ("firdec_b.csv", 1e6, "MHz", "61,44 Msps → 7,68 Msps")]
    for ax, (fn, u, un, ttl) in zip(axs, cases):
        h, t = load_taps(fn)
        fs, D, pas = float(h["fs"]), int(h["decim"]), float(h["pass"])
        n = 1 << 18
        H = np.fft.rfft(t, n)
        f = np.arange(len(H)) * fs / n
        ax.plot(f / u, db(H, -130), color=T["c"][0])
        fo = fs / D
        ax.axvspan(0, pas / u, color=T["c"][2], alpha=0.16, lw=0)
        ax.axvspan((fo - pas) / u, (fo + pas) / u, color=T["c"][4], alpha=0.16, lw=0)
        ax.axvline(fo / u, color=T["var"], lw=0.9, ls=":")
        ax.set_ylim(-130, 8)
        ax.set_xlim(0, fs / 2 / u)
        ax.set_xlabel(f"Frecuencia ({un})")
        ax.set_ylabel("Ganancia (dB)")
        ax.set_title(f"{ttl}  ·  {h['ntaps']} taps")
        ax.text(pas / u * 0.5, -22, "banda\nútil", color=T["c"][2], fontsize=9, va="top", ha="center")
        ax.text((fo + pas) / u, -122, " banda que aliasea\n sobre la útil", color=T["c"][4], fontsize=8.5, va="bottom", ha="left")
    return fig


def fig_cfir():
    fig, axs = plt.subplots(2, 2, figsize=(11, 6.6), sharey=True)
    fig.subplots_adjust(top=0.84, bottom=0.09, left=0.07, right=0.98, wspace=0.08, hspace=0.38)
    head(fig, "Filtro de canal complejo (cfir_t)",
         "Paso banda asimétrico: USB y LSB dejan pasar un solo lado. Tasa de canal 48 kHz; coeficientes de channel.c.")
    for ax, (fn, ttl, i) in zip(axs.flat, [("cfir_usb.csv", "USB  200…2700 Hz", 0), ("cfir_lsb.csv", "LSB  −2700…−200 Hz", 1),
                                           ("cfir_am.csv", "AM  ±5 kHz", 2), ("cfir_nfm.csv", "NFM  ±6,25 kHz", 3)]):
        h, t = load_taps(fn)
        fs = float(h["fs"])
        n = 1 << 17
        # cfir_process guarda el coeficiente i como el que multiplica a x[n-(N-1-i)]:
        # la respuesta al impulso es el vector invertido
        H = np.fft.fftshift(np.fft.fft(t[::-1], n))
        f = (np.arange(n) - n // 2) * fs / n
        ax.plot(f / 1e3, db(H, -120), color=T["c"][i])
        ax.axvspan(float(h["lo"]) / 1e3, float(h["hi"]) / 1e3, color=T["c"][i], alpha=0.12, lw=0)
        ax.set_xlim(-12, 12)
        ax.set_ylim(-120, 8)
        ax.set_title(f"{ttl}  ·  {h['ntaps']} taps")
        ax.set_xlabel("Frecuencia (kHz)")
        if i % 2 == 0:
            ax.set_ylabel("Ganancia (dB)")
    return fig


def fig_resamp():
    fig, axs = plt.subplots(1, 2, figsize=(11, 4.6))
    fig.subplots_adjust(top=0.76, bottom=0.14, left=0.07, right=0.98, wspace=0.22)
    head(fig, "Remuestreador polifásico (resamp_t, 256 fases)",
         "Prototipo reconstruido con los coeficientes del banco: respuesta del filtro anti-alias integrado, normalizada a 0 dB en continua.")
    for ax, (fn, i) in zip(axs, [("resamp_wfm.csv", 0), ("resamp_nfm.csv", 1)]):
        h, t = load_taps(fn)
        fi, fo, fc = float(h["fs_in"]), float(h["fs_out"]), float(h["cutoff"])
        P = int(h["P"])
        n = 1 << 20
        H = np.fft.rfft(t / P, n)
        f = np.arange(len(H)) * (fi * P) / n
        m = f <= fi / 2
        ax.plot(f[m] / 1e3, db(H[m], -130), color=T["c"][i])
        ax.axvline(fc / 1e3, color=T["c"][2], lw=1, ls="--")
        ax.axvline(fo / 2e3, color=T["var"], lw=1, ls=":")
        ax.axvline((fo - fc) / 1e3, color=T["c"][4], lw=1, ls="--")
        ax.axvspan((fo - fc) / 1e3, fi / 2e3, color=T["c"][4], alpha=0.10, lw=0)
        ax.set_ylim(-130, 8)
        ax.set_xlim(0, fi / 2e3)
        ax.set_xlabel("Frecuencia (kHz)")
        ax.set_ylabel("Ganancia (dB)")
        ax.set_title(f"{fi/1e3:,.3f} kHz → {fo/1e3:.0f} kHz  ·  L = {h['L']}".replace(",", " ").replace(".", ",") if fi % 1000 else f"{fi/1e3:.0f} kHz → {fo/1e3:.0f} kHz  ·  L = {h['L']}")
        ax.text(fc / 1e3, -8, f" corte {fc/1e3:.0f} kHz", color=T["c"][2], fontsize=8.5, va="top")
        if (fo - fc) / 1e3 < fi / 2e3 * 0.9:
            ax.text((fo - fc) / 1e3, -45, " desde aquí aliasea\n dentro del corte", color=T["c"][4], fontsize=8.5, va="top", ha="left")
    return fig


def fig_ventana():
    w = np.loadtxt(os.path.join(DATA, "window_bh.csv"), comments="#")
    N = len(w)
    fig, axs = plt.subplots(1, 2, figsize=(11, 4.6), gridspec_kw=dict(width_ratios=[1, 1.5]))
    fig.subplots_adjust(top=0.76, bottom=0.14, left=0.07, right=0.98, wspace=0.2)
    head(fig, "Ventana Blackman-Harris de 4 términos",
         "window_blackman_harris() de src/dsp/fft.c, N = 1024. Comparada con la ventana Hann y la rectangular.")
    ax = axs[0]
    ax.plot(np.arange(N), w, color=T["c"][0])
    ax.fill_between(np.arange(N), w, color=T["c"][0], alpha=0.12)
    ax.set_xlabel("Muestra")
    ax.set_ylabel("Peso")
    ax.set_title("Dominio del tiempo")
    ax = axs[1]
    M = 1 << 16
    def spec(x):
        S = np.fft.fftshift(np.fft.fft(x, M))
        return db(S / np.max(np.abs(S)), -140)
    bins = (np.arange(M) - M // 2) * N / M
    hann = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N) / (N - 1))
    for x, lab, col, lw in [(np.ones(N), "rectangular", T["c"][3], 1.2), (hann, "Hann", T["c"][1], 1.2), (w, "Blackman-Harris", T["c"][0], 2)]:
        ax.plot(bins, spec(x), color=col, lw=lw, label=lab)
    S = spec(w)
    far = np.abs(bins) > 5
    enbw = N * np.sum(w ** 2) / np.sum(w) ** 2
    ax.set_xlim(-12, 12)
    ax.set_ylim(-140, 5)
    ax.set_xlabel("Desplazamiento (bins)")
    ax.set_ylabel("Respuesta (dB)")
    ax.set_title(f"Espectro  ·  lóbulo lateral {S[far].max():.0f} dB  ·  ENBW {enbw:.3f} bins (N = 1024)".replace(".", ","))
    ax.legend(loc="upper right", fontsize=9)
    return fig


def fig_calibracion():
    import csv
    rows = list(csv.DictReader(open(os.path.join(DATA, "calib.csv"))))
    fig, axs = plt.subplots(1, 2, figsize=(11, 4.8), gridspec_kw=dict(width_ratios=[1, 1.15]))
    fig.subplots_adjust(top=0.76, bottom=0.14, left=0.07, right=0.98, wspace=0.22)
    head(fig, "Calibración del espectro (spectrum.c, CPU)",
         "Tono complejo de amplitud conocida frente al pico medido, con 0,3 cuentas de ruido. 0 dBFS = tono de amplitud 1,0.")
    series = sorted({(int(r["fft"]), float(r["offset_bins"])) for r in rows})
    cols = {1024: T["c"][0], 4096: T["c"][1], 65536: T["c"][2]}
    for (n, off) in series:
        rr = [r for r in rows if int(r["fft"]) == n and float(r["offset_bins"]) == off]
        x = np.array([float(r["real"]) for r in rr])
        y = np.array([float(r["medido"]) for r in rr])
        lab = f"FFT {n}" + (" · tono entre bins" if off else "")
        axs[0].plot(x, y, color=cols[n], ls="--" if off else "-", marker="o" if not off else None, ms=3, label=lab)
        axs[1].plot(x, y - x, color=cols[n], ls="--" if off else "-", marker="o" if not off else None, ms=3)
    axs[0].plot([-90, 0], [-90, 0], color=T["var"], lw=0.8, ls=":")
    axs[0].set_xlabel("Nivel real (dBFS)")
    axs[0].set_ylabel("Pico medido (dBFS)")
    axs[0].set_title("Medido frente a real")
    axs[0].legend(fontsize=8, loc="upper left")
    axs[1].axhline(0, color=T["var"], lw=0.8, ls=":")
    axs[1].set_xlabel("Nivel real (dBFS)")
    axs[1].set_ylabel("Error (dB)")
    axs[1].set_title("Error  ·  −0,8 dB entre bins (festoneo); bajo −70 dBFS manda la cuantización")
    return fig


def fig_ber():
    import csv
    from math import erfc, sqrt
    rows = list(csv.DictReader(open(os.path.join(DATA, "ber.csv"))))
    fig, ax = plt.subplots(figsize=(9, 5.4))
    fig.subplots_adjust(top=0.82, bottom=0.12, left=0.09, right=0.97)
    head(fig, "BER con y sin Viterbi (fec.c)",
         "Código K = 7 (0171, 0133), BPSK en canal AWGN, bloques de 2000 bits con cola. Puntos con < 10 errores omitidos.")
    names = ["sin codificar", "Viterbi duro 1/2", "Viterbi blando int8 1/2", "Viterbi blando float 1/2", "Viterbi blando 3/4 (DVB)"]
    for i, nm in enumerate(names):
        pts = [(float(r["ebn0_db"]), int(r["errores"]) / int(r["bits"]), int(r["errores"])) for r in rows if r["serie"] == nm]
        pts = [(x, y) for x, y, e in pts if e >= 10]
        if pts:
            ax.semilogy(*zip(*pts), marker="o", ms=4, color=T["c"][[3, 4, 1, 0, 2][i]], label=nm,
                        ls="--" if i == 0 else "-")
    x = np.linspace(0, 7, 100)
    ax.semilogy(x, [0.5 * erfc(sqrt(10 ** (v / 10))) for v in x], color=T["var"], lw=0.9, ls=":", label="teoría BPSK sin codificar")
    ax.set_ylim(1e-6, 0.6)
    ax.set_xlim(0, 7)
    ax.set_xlabel("Eb/N0 (dB)")
    ax.set_ylabel("BER")
    ax.grid(True, which="both", alpha=0.6)
    ax.legend(fontsize=9, loc="lower left")
    return fig


# ----------------------------------------------------------- RENDIMIENTO / CUDA
def barras(ax, cats, a, b, la, lb, unit, fmt="{:.1f}", ca=None, cb=None):
    x = np.arange(len(cats))
    ca = ca or T["c"][3]
    cb = cb or T["c"][0]
    for off, v, lab, col in ((-0.2, a, la, ca), (0.2, b, lb, cb)):
        vv = [0 if q is None else q for q in v]
        bars = ax.bar(x + off, vv, 0.38, color=col, label=lab)
        for r, q in zip(bars, v):
            if q is None:
                ax.text(r.get_x() + r.get_width() / 2, 0, "n/d", ha="center", va="bottom", fontsize=8, color=T["var"])
            else:
                ax.text(r.get_x() + r.get_width() / 2, r.get_height(), fmt.format(q), ha="center", va="bottom", fontsize=8, color=T["on"])
    ax.set_xticks(x)
    ax.set_xticklabels(cats, fontsize=9)
    ax.set_ylabel(unit)
    ax.grid(axis="x", visible=False)


def fig_rend_ficheros():
    cats = ["61,44 Msps\n0 clientes", "61,44 Msps\n1 escucha", "61,44 Msps\n4 (1 escucha)",
            "122,88 Msps\n0 clientes", "122,88 Msps\n1 escucha", "122,88 Msps\n4 (1 escucha)"]
    tasa_a = [61.4, 51.7, 22.9, 123.2, 52.7, 16.1]
    tasa_d = [61.6, 61.4, 61.5, 122.9, 122.9, 122.7]
    nuc_a = [0.42, 0.98, None, 0.73, 1.01, 0.98]
    nuc_d = [0.15, 0.39, 0.49, 0.26, 0.83, 0.74]
    fig, axs = plt.subplots(2, 1, figsize=(11, 7.4))
    fig.subplots_adjust(top=0.83, bottom=0.08, left=0.07, right=0.98, hspace=0.45)
    head(fig, "Motor DSP: antes y después (fuentes de fichero)",
         "Cifras de RENDIMIENTO.md §3: GPU, FFT 4096, 20 fps, un cliente NFM escuchando. «n/d»: no sostenía la tasa.")
    barras(axs[0], cats, tasa_a, tasa_d, "antes", "después", "Tasa sostenida (Msps)")
    axs[0].set_title("Tasa sostenida (más es mejor)")
    axs[0].legend(loc="upper left", ncol=2, fontsize=9)
    barras(axs[1], cats, nuc_a, nuc_d, "antes", "después", "Núcleos de CPU", "{:.2f}")
    axs[1].set_title("CPU de todo el proceso (menos es mejor)")
    return fig


def fig_rend_real():
    fig, axs = plt.subplots(1, 2, figsize=(11, 4.8))
    fig.subplots_adjust(top=0.76, bottom=0.14, left=0.07, right=0.98, wspace=0.28)
    head(fig, "Con el bladeRF real (RENDIMIENTO.md §4)",
         "Solo recepción, un cliente. A 122,88 Msps el límite es el USB y libbladeRF, no el motor (medido con un bucle vacío de lectura).")
    barras(axs[0], ["61,44 Msps", "122,88 Msps"], [60.3, 52.2], [61.5, 123.1], "antes", "después", "Msps entregados")
    axs[0].set_title("Tasa entregada")
    axs[0].legend(fontsize=9, loc="upper left")
    ax = axs[1]
    barras(ax, ["61,44 Msps\n(20 s)", "122,88 Msps\n(20 s)"], [57, 13914], [0, 5001], "antes", "después", "Overruns", "{:.0f}")
    ax.set_yscale("symlog", linthresh=10)
    ax.set_ylim(0, 8e4)
    ax.set_title("Overruns de hardware (carga 56 en el equipo)")
    return fig


def fig_cuda_ddc():
    fig, axs = plt.subplots(1, 2, figsize=(11, 4.8), gridspec_kw=dict(width_ratios=[1.1, 1]))
    fig.subplots_adjust(top=0.76, bottom=0.17, left=0.07, right=0.98, wspace=0.28)
    head(fig, "DDC por lotes en GPU (CUDA.md §1)",
         "16 instancias ais/acars/pocsag a 20 Msps con --replay 12 s: la GPU absorbe el mezclado y el diezmado.")
    cats = ["sin decod.", "1 instancia\n(CPU)", "16 inst.\nDDC en CPU", "16 inst.\nDDC en GPU"]
    v = [0.10, 0.38, 5.2, 0.32]
    cols = [T["var"], T["c"][3], T["c"][4], T["c"][0]]
    bars = axs[0].bar(cats, v, color=cols, width=0.6)
    for r, q in zip(bars, v):
        axs[0].text(r.get_x() + r.get_width() / 2, q, f"{q:.2f}", ha="center", va="bottom", fontsize=9)
    axs[0].set_ylabel("Núcleos de CPU")
    axs[0].set_title("CPU del proceso (menos es mejor)")
    axs[0].grid(axis="x", visible=False)
    ax = axs[1]
    cfg = ["20 Msps\n16 canales", "61,44 Msps\n16 canales", "61,44 Msps\n40 canales"]
    thr = [739, 705, 427]
    load = [2.7, 8.7, 14.4]
    bars = ax.bar(cfg, thr, color=T["c"][1], width=0.55)
    for r, q, l in zip(bars, thr, load):
        ax.text(r.get_x() + r.get_width() / 2, q, f"{q} Msps\n{l} % de tiempo real".replace(".", ","), ha="center", va="bottom", fontsize=8.5)
    ax.set_ylim(0, 1000)
    ax.set_ylabel("Msps sostenidos")
    ax.set_title("Rendimiento del banco (más es mejor)")
    ax.grid(axis="x", visible=False)
    return fig


def fig_cuda_otros():
    fig, axs = plt.subplots(1, 3, figsize=(12, 4.8))
    fig.subplots_adjust(top=0.76, bottom=0.2, left=0.06, right=0.99, wspace=0.38)
    head(fig, "Espectro asíncrono, correlación NR y PFB en GPU (CUDA.md §2, §6, §7)",
         "Cifras ya documentadas. nr_corr_run en CPU tardaba 15–24 ms: se dibuja el punto medio (19,5 ms).")
    barras(axs[0], ["61,44\nFFT 4k", "61,44\nFFT 64k", "122,88\nFFT 4k", "122,88\nFFT 64k"],
           [1.01, 1.06, 1.43, 1.78], [0.67, 0.80, 0.62, 1.50], "síncrono", "asíncrono", "ms por bloque (media)", "{:.2f}")
    axs[0].set_title("Espectro (Msps · FFT): tiempo medio")
    axs[0].legend(fontsize=8.5, loc="upper left")
    barras(axs[1], ["nr_corr_run\n(ms)", "decod. nr 30,72\n(s de CPU)", "decod. nr 61,44\n(s de CPU)"],
           [19.5, 1.05, 2.9], [1.1, 0.60, 2.3], "CPU", "GPU", "tiempo de CPU", "{:.2f}")
    axs[1].set_yscale("log")
    axs[1].set_title("NR: CPU frente a GPU")
    axs[1].legend(fontsize=8.5, loc="upper right")
    b = axs[2].bar(["cálculo", "con bajada\na la CPU"], [1205, 348], color=[T["c"][0], T["c"][1]], width=0.55)
    for r, q in zip(b, [1205, 348]):
        axs[2].text(r.get_x() + r.get_width() / 2, q, f"{q} Msps", ha="center", va="bottom", fontsize=9)
    axs[2].set_ylabel("Msps")
    axs[2].set_title("PFB M=1024, 2×, P=8")
    axs[2].grid(axis="x", visible=False)
    return fig


# ---------------------------------------------------------------- VALIDACION
ROWS_A = [
    ("ais", "ok", None, "tipo 1 con MMSI 367003560 y 366982310 (sigidwiki); +2,3 dB a SNR baja; 0 falsos en 15 h"),
    ("acars", "ok", (11, 11), "11 de 11 bloques = acarsdec"),
    ("flex", "partial", (44, 44), "1600/2 (20) y 6400/4 (24) = multimon-ng; 3200/4 sin referencia; 3200/2 sin grabación"),
    ("pocsag", "ok", (3, 3), "512, 1200 y 2400 bps = multimon-ng"),
    ("aprs", "ok", None, "tramas AX.25 idénticas a multimon-ng y Dire Wolf"),
    ("rds", "ok", None, "PI, grupos, PTY, AF y DI idénticos a redsea (Radio Pamplona real)"),
    ("adsb", "partial", None, "4 tramas DF17 = dump1090-fa; CPR local y pista; sin tráfico real"),
    ("vdl2", "ok", (38, 41), "38 de 41 tramas = dumpvdl2 (grabación real de sigidwiki)"),
    ("dsc", "none", None, "sin grabación real; solo tests con ITU-R M.493-15"),
]
ROWS_B = [
    ("ism", "ok", (8, 8), "8 modelos reales = rtl_433"),
    ("rs41", "ok", (38, 38), "38 de 38 tramas por grabación, posiciones coherentes"),
    ("lora", "ok", None, "SF7-12, CR 1-4, BW 125/250 en ambos sentidos con loratx/lorarx"),
    ("meshtastic", "partial", None, "cabecera, hash y clave confirmados; descifrado sin validar con aire"),
    ("dab", "ok", (36, 36), "36 de 36 FIB; ensemble 5001"),
    ("lte", "ok", (2, 2), "PCI 65 y PCI 44 con MIB (6 y 50 RB)"),
    ("gsm", "partial", (211, 211), "211 de 211 SCH, BSIC 23; BCCH/SI3 reales sin decodificar"),
    ("wifi", "partial", None, "8 BSSID reales (DSSS 1 Mb/s); OFDM sin validar"),
    ("nr", "none", None, "no apareció celda (centro desconocido)"),
    ("dmr", "none", None, "1 sincronía, 0 ráfagas; sin referencia"),
]


def fig_validacion(rows, title, sub):
    n = len(rows)
    fig, ax = plt.subplots(figsize=(11, 1.5 + 0.52 * n))
    fig.subplots_adjust(top=1 - 1.15 / (1.5 + 0.52 * n), bottom=0.07, left=0.03, right=0.98)
    head(fig, title, sub)
    ax.axis("off")
    ax.set_xlim(0, 100)
    ax.set_ylim(-n + 0.3, 0.7)
    col = {"ok": T["ok"], "partial": T["warn"], "none": T["err"]}
    lab = {"ok": "contrastado", "partial": "parcial", "none": "sin validar"}
    for i, (nm, st, ratio, txt) in enumerate(rows):
        y = -i
        ax.add_patch(Rectangle((0, y - 0.42), 100, 0.84, color=T["card"], lw=0, zorder=0))
        ax.text(1.5, y, nm, fontsize=11, fontweight="semibold", va="center")
        ax.add_patch(Rectangle((12, y - 0.12), 13, 0.24, color=T["grid"], lw=0))
        if ratio:
            ax.add_patch(Rectangle((12, y - 0.12), 13 * ratio[0] / ratio[1], 0.24, color=col[st], lw=0))
            ax.text(26, y, f"{ratio[0]}/{ratio[1]}", fontsize=9, va="center", color=T["var"])
        else:
            ax.text(18.5, y, "sin recuento", fontsize=8, va="center", ha="center", color=T["var"])
        ax.add_patch(Rectangle((32, y - 0.17), 9.5, 0.34, color=col[st], alpha=0.2, lw=0))
        ax.text(36.75, y, lab[st], fontsize=8.5, va="center", ha="center", color=col[st], fontweight="semibold")
        ax.text(43, y, txt, fontsize=9.3, va="center", color=T["on"])
    return fig


def fig_val_a():
    return fig_validacion(ROWS_A, "Evidencia del grupo A (VALIDACION.md)",
                          "Grabaciones públicas y decodificadores de referencia. La barra muestra decodificados / referencia cuando hay recuento.")


def fig_val_b():
    return fig_validacion(ROWS_B, "Evidencia del grupo B (VALIDACION.md)",
                          "Grabaciones públicas y herramientas de referencia. La barra muestra decodificados / referencia cuando hay recuento.")


# ------------------------------------------------------------------------ TX
def fig_ocupacion():
    fig, axs = plt.subplots(1, 3, figsize=(12.5, 4.6))
    fig.subplots_adjust(top=0.74, bottom=0.15, left=0.06, right=0.99, wspace=0.28)
    head(fig, "Medida del ancho ocupado (tx.c: 99,98 % de la potencia)",
         "Periodograma de Welch (FFT 1024, Blackman-Harris) de la señal modulada entera, antes de colocarla en el canal. Datos generados con nuestros moduladores.")
    for k, ax in enumerate(axs):
        ids = ["aprs", "ism", "lora"]
        path = os.path.join(DATA, f"occ_{ids[k]}.csv")
        if not os.path.exists(path):
            ax.axis("off")
            continue
        with open(path) as f:
            hdr = dict(kv.split("=") for kv in f.readline()[1:].split())
        d = np.loadtxt(path, delimiter=",", comments="#")
        f_, p = d[:, 1], d[:, 2]
        lo, hi = float(hdr["lo"]), float(hdr["hi"])
        pdb = 10 * np.log10(p / p.max() + 1e-12)
        ax.plot(f_ / 1e3, pdb, color=T["c"][k], lw=1.4)
        ax.axvspan(lo / 1e3, hi / 1e3, color=T["c"][k], alpha=0.14, lw=0)
        ax.axvline(lo / 1e3, color=T["c"][k], lw=1)
        ax.axvline(hi / 1e3, color=T["c"][k], lw=1)
        inb = p[(f_ >= lo) & (f_ < hi)].sum() / p.sum()
        ax.set_ylim(-90, 6)
        ax.set_xlabel("Frecuencia respecto al centro (kHz)")
        if k == 0:
            ax.set_ylabel("Densidad relativa (dB)")
        ax.set_title(f"{ids[k]}  ·  {float(hdr['occ_bw']) / 1e3:.2f} kHz ({inb * 100:.2f} %)".replace(".", ","))
        ax.text(0.5, 0.06, f"[{lo/1e3:+.2f}, {hi/1e3:+.2f}] kHz  ·  {float(hdr['rate'])/1e3:.0f} ksps", transform=ax.transAxes,
                ha="center", fontsize=8.5, color=T["var"])
    return fig


FIGS = {
    # nombre: (carpeta, función)
    "firdec-respuesta": ("dsp", fig_firdec),
    "cfir-canal": ("dsp", fig_cfir),
    "resamp-respuesta": ("dsp", fig_resamp),
    "ventana-blackman-harris": ("dsp", fig_ventana),
    "calibracion-espectro": ("dsp", fig_calibracion),
    "ber-viterbi": ("dsp", fig_ber),
    "rendimiento-ficheros": ("rendimiento", fig_rend_ficheros),
    "rendimiento-bladerf": ("rendimiento", fig_rend_real),
    "cuda-ddc": ("cuda", fig_cuda_ddc),
    "cuda-espectro-nr-pfb": ("cuda", fig_cuda_otros),
    "validacion-grupo-a": ("validacion", fig_val_a),
    "validacion-grupo-b": ("validacion", fig_val_b),
    "ancho-ocupado": ("tx", fig_ocupacion),
}

if __name__ == "__main__":
    only = sys.argv[1:]
    for theme in ("light", "dark"):
        style(theme)
        for name, (folder, fn) in FIGS.items():
            if only and name not in only:
                continue
            p = save(fn(), folder, name, theme)
            print(f"{os.path.relpath(p, ROOT):70s} {os.path.getsize(p) // 1024:4d} KB")
