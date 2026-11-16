"""plots_common.py — estilo Material 3 para las gráficas de señal (matplotlib) y utilidades.

Mismo tema que los diagramas (lib.py): fuente Inter, paleta tonal de M3 en claro y oscuro,
superficies de tarjeta, rejilla suave. Cada gráfica se dibuja dos veces (claro/oscuro) con
render(). Cada figura lleva en el pie la procedencia de la señal (grabación real o sintética)
y qué parte del procesado es nuestro código y cuál es solo ilustración en Python.
"""
import os
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager as fm
from matplotlib.colors import LinearSegmentedColormap
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(ROOT, "docs", "img", "decoders")
os.makedirs(OUT, exist_ok=True)

for f in ("Regular", "Medium", "SemiBold", "Bold"):
    p = f"/usr/share/fonts/opentype/inter/Inter-{f}.otf"
    if os.path.exists(p):
        fm.fontManager.addfont(p)

TH = {
    "light": dict(bg="#FEF7FF", card="#F3EDF7", card2="#ECE6F0", on="#1D1B20", var="#49454F", outline="#CAC4D0",
                  grid="#E0D9E3", pri="#6750A4", pric="#EADDFF",
                  c=["#6750A4", "#B0286A", "#0A6AA6", "#08695B", "#8A5700", "#26702B"],
                  cmap=["#F3EDF7", "#D9C9F3", "#8467C8", "#4F378B", "#21005D"]),
    "dark": dict(bg="#141218", card="#211F26", card2="#2B2930", on="#E6E0E9", var="#CAC4D0", outline="#49454F",
                 grid="#3A3741", pri="#D0BCFF", pric="#4F378B",
                 c=["#D0BCFF", "#F2A7C3", "#8ECDF7", "#7ED6C4", "#F7C873", "#A6E3A1"],
                 cmap=["#141218", "#3A2D55", "#8E6FD6", "#F7C873", "#FFFFFF"]),
}


def cmap(t):
    return LinearSegmentedColormap.from_list("m3", TH[t]["cmap"])


def setup(t):
    c = TH[t]
    plt.rcParams.update({
        "font.family": "Inter", "font.size": 10, "text.color": c["on"], "axes.labelcolor": c["var"],
        "axes.edgecolor": c["outline"], "xtick.color": c["var"], "ytick.color": c["var"],
        "figure.facecolor": c["bg"], "axes.facecolor": c["card"], "savefig.facecolor": c["bg"],
        "axes.grid": True, "grid.color": c["grid"], "grid.linewidth": 0.8, "axes.axisbelow": True,
        "axes.spines.top": False, "axes.spines.right": False, "axes.titlesize": 11.5,
        "axes.titleweight": "semibold", "axes.titlecolor": c["on"], "axes.titlelocation": "left",
        "legend.frameon": False, "axes.prop_cycle": plt.cycler(color=c["c"]),
        "lines.linewidth": 1.4, "figure.dpi": 100,
    })
    return c


def fig_axes(nrows, ncols, w=9.6, h=3.6, **kw):
    fig, axs = plt.subplots(nrows, ncols, figsize=(w, h), **kw)
    return fig, axs


def footer(fig, text, c):
    import textwrap
    fig.text(0.012, 0.012, "\n".join(textwrap.wrap(text, 150)), fontsize=7.8, color=c["var"], ha="left", va="bottom", linespacing=1.4)


def title(fig, text, c, sub=None):
    fig.text(0.012, 0.985, text, fontsize=14, fontweight="semibold", color=c["on"], ha="left", va="top")
    if sub:
        fig.text(0.012, 0.985 - 0.058 * (3.6 / fig.get_figheight()) * 1.0, sub, fontsize=8.8, color=c["var"], ha="left", va="top")


def render(name, draw):
    """draw(theme_name, colors) -> fig. Guarda OUT/<name>-{light,dark}.png (< 400 KB)."""
    for t in ("light", "dark"):
        c = setup(t)
        fig = draw(t, c)
        p = os.path.join(OUT, f"{name}-{t}.png")
        fig.savefig(p, dpi=130)
        plt.close(fig)
        if os.path.getsize(p) > 380 * 1024:
            im = Image.open(p).convert("RGB").quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
            im.save(p, optimize=True)
        print(f"{name}-{t}.png {os.path.getsize(p)//1024} KB")
    return name


def save_tight(fig, top=0.84, bottom=0.22):
    fig.subplots_adjust(top=top, bottom=bottom, left=0.085, right=0.985, wspace=0.28, hspace=0.45)


# ---------------------------------------------------------------- lectura de grabaciones
def load_sigmf(meta, start=0, count=None):
    """Lee un .sigmf-meta/.sigmf-data como datos (nunca se ejecuta nada). -> (iq complex64, fs, fc)."""
    import json
    m = json.load(open(meta))
    g = m["global"]
    fs = float(g["core:sample_rate"])
    fc = float(m["captures"][0].get("core:frequency", 0))
    dt = g["core:datatype"]
    data = meta.replace(".sigmf-meta", ".sigmf-data")
    return _read(data, dt, start, count) , fs, fc


def _read(path, dt, start=0, count=None):
    isz = {"ci16_le": 4, "cs16": 4, "cs8": 2, "ci8": 2, "cu8": 2, "cf32": 8}[dt]
    cnt = -1 if count is None else int(count)
    if dt in ("ci16_le", "cs16"):
        a = np.fromfile(path, "<i2", count=-1 if cnt < 0 else 2 * cnt, offset=start * 4).astype(np.float32) / 2048.0
        return (a[0::2] + 1j * a[1::2]).astype(np.complex64)
    if dt in ("cs8", "ci8"):
        a = np.fromfile(path, "i1", count=-1 if cnt < 0 else 2 * cnt, offset=start * 2).astype(np.float32) / 128.0
        return (a[0::2] + 1j * a[1::2]).astype(np.complex64)
    if dt == "cu8":
        a = (np.fromfile(path, "u1", count=-1 if cnt < 0 else 2 * cnt, offset=start * 2).astype(np.float32) - 127.5) / 127.5
        return (a[0::2] + 1j * a[1::2]).astype(np.complex64)
    raise ValueError(dt)


def load_raw(path, dt, start=0, count=None):
    return _read(path, dt, start, count)


# ---------------------------------------------------------------- DSP de ilustración
def shift(x, f, fs):
    n = np.arange(len(x))
    return (x * np.exp(-2j * np.pi * f / fs * n)).astype(np.complex64)


def lowpass(x, fc, fs, ntaps=129):
    from scipy.signal import firwin, lfilter
    h = firwin(ntaps, fc / (fs / 2))
    return lfilter(h, 1.0, x)


def fm_disc(x):
    return np.angle(x[1:] * np.conj(x[:-1]))


def spec_img(ax, x, fs, fc=0.0, nfft=512, hop=None, db=(-90, 0), t=None, cm=None, tmax=None, fmt="khz"):
    """Espectrograma (cascada) con el eje de frecuencia en kHz/MHz relativo al centro."""
    hop = hop or nfft // 2
    w = np.hanning(nfft)
    nseg = (len(x) - nfft) // hop
    S = np.empty((nseg, nfft), np.float32)
    for i in range(nseg):
        S[i] = np.abs(np.fft.fftshift(np.fft.fft(x[i * hop:i * hop + nfft] * w))) ** 2
    S = 10 * np.log10(S / nfft + 1e-12)
    ref = np.percentile(S, 99.8)
    lo = np.percentile(S, 5)
    scale = 1e3 if fmt == "khz" else 1e6
    if fmt == "khz":
        fc = 0.0
    ext = [(fc - fs / 2) / scale, (fc + fs / 2) / scale, nseg * hop / fs * 1e3, 0]
    im = ax.imshow(S, aspect="auto", extent=ext, cmap=cm, vmin=lo, vmax=ref, interpolation="nearest")
    ax.grid(False)
    return im, S


def psd(x, fs, nfft=4096):
    from scipy.signal import welch
    f, p = welch(x, fs, nperseg=nfft, return_onesided=False, window="hann")
    i = np.argsort(f)
    return f[i], 10 * np.log10(p[i] + 1e-15)


def stamp(t):
    return t


def frame_fig(rows, w=9.6, rh=1.05, note=None):
    """Estructura de una trama con campos coloreados.
    rows: [(titulo_fila, [(etiqueta, ancho_relativo, detalle), ...]), ...]. Devuelve draw(t, c)."""
    def draw(t, c):
        n = len(rows)
        fig, ax = plt.subplots(figsize=(w, 1.5 + rh * n))
        ax.set_xlim(0, 1); ax.set_ylim(0, n); ax.invert_yaxis(); ax.axis("off"); ax.grid(False)
        fig.subplots_adjust(top=0.84 - 0.02, bottom=0.22 if note else 0.08, left=0.012, right=0.988)
        pal = c["c"]
        for r, (rt, fields) in enumerate(rows):
            tot0 = sum(f[1] for f in fields)
            ws = [max(f[1], 0.055 * tot0) for f in fields]
            tot = sum(ws)
            x = 0.0
            ax.text(0.0, r + 0.1, rt, fontsize=9, color=c["var"], va="center", fontweight="semibold")
            for i, (lab, wd, det) in enumerate(fields):
                ww = ws[i] / tot
                col = pal[i % len(pal)]
                ax.add_patch(plt.Rectangle((x + 0.002, r + 0.22), ww - 0.004, 0.46, facecolor=col, alpha=0.22, edgecolor=col, lw=1.2))
                fs = 8.6 if ww > 0.09 else 7.4
                ax.text(x + ww / 2, r + 0.40, lab, ha="center", va="center", fontsize=fs, color=c["on"], fontweight="semibold")
                ax.text(x + ww / 2, r + 0.57, det, ha="center", va="center", fontsize=fs - 1.2, color=c["var"])
                x += ww
        return fig
    return draw


def frame_render(name, ttl, rows, foot, **kw):
    d = frame_fig(rows, **kw)

    def draw(t, c):
        fig = d(t, c)
        title(fig, ttl, c)
        footer(fig, foot, c)
        return fig
    render(name, draw)


def run(FIGS, only):
    for k, f in FIGS.items():
        if not only or any(k.startswith(o) for o in only):
            render(k, f)
