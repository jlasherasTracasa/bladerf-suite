#!/usr/bin/env python3
"""plots_b.py — gráficas «Así es la señal» (B): rds, dab, pocsag, flex, dmr, ism, rs41.
Datos: build/v1/dl, build/v2/dl, tests/data y señales de `bladerf-lab --gen` (build/doc2a/gen).
Los demoduladores son Python de ilustración (scipy), NO el código C. Uso: python3 -I plots_b.py [id ...]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plots_common import *
from plots_a import channel, eye, burst_start
from scipy.signal import resample_poly

V1 = os.path.join(ROOT, "build", "v1", "dl")
V2 = os.path.join(ROOT, "build", "v2", "dl")
G = os.path.join(ROOT, "build", "doc2a", "gen")
TD = os.path.join(ROOT, "tests", "data")
FIGS = {}


def fig(name):
    def deco(f):
        FIGS[name] = f
        return f
    return deco


@fig("rds-senal")
def rds(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V1, "ref", "gen_rds.sigmf-meta"), 0, 250000 * 8)
    off = 0.0
    d = fm_disc(iq) * fs / (2 * np.pi)          # MPX en Hz de desviación
    d = d - np.mean(d)
    f, p = psd(d.astype(np.complex64), fs, 8192)
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.2, 1]))
    m = (f >= 0) & (f < 80e3)
    axs[0].plot(f[m] / 1e3, p[m], color=c["c"][0], lw=1)
    axs[0].set_xlabel("frecuencia del MPX (kHz)"); axs[0].set_ylabel("dB")
    axs[0].set_title("Espectro MPX: piloto 19 kHz y subportadora RDS 57 kHz")
    for fr, lab in ((19, "piloto"), (57, "RDS")):
        axs[0].annotate(lab, (fr, p[m][np.argmin(np.abs(f[m] / 1e3 - fr))]), textcoords="offset points", xytext=(0, 8), ha="center", fontsize=9, color=c["c"][4])
    # BPSK: baja 57 kHz, filtra y diezma a 25 kS/s, fase por cuadrado simple, muestreo a 1187,5 bd
    n = np.arange(len(d))
    x = d * np.exp(-2j * np.pi * 57000 / fs * n)
    x = lowpass(x, 2400, fs, 201)[:: 10]                    # 25 kS/s
    fs2 = fs / 10
    from scipy.ndimage import uniform_filter1d
    sq = uniform_filter1d((x ** 2).real, 4000) + 1j * uniform_filter1d((x ** 2).imag, 4000)
    out = x * np.exp(-1j * np.angle(sq) / 2)      # elevar al cuadrado quita la modulación BPSK
    sps = fs2 / 1187.5
    z = out[int(len(out) * 0.3):]
    k = np.arange(int(len(z) / sps) - 2)
    best = max(range(int(sps)), key=lambda o: np.mean(np.abs(z[np.round(k * sps + o).astype(int)].real)))
    s = z[np.round(k * sps + best).astype(int)]
    s = s / np.mean(np.abs(s.real))
    axs[1].scatter(s.real[:3000], s.imag[:3000], s=5, color=c["c"][1], alpha=0.35, linewidths=0)
    axs[1].set_xlim(-2.5, 2.5); axs[1].set_ylim(-2.5, 2.5); axs[1].set_aspect("equal")
    axs[1].set_title("Constelación BPSK (2 puntos)"); axs[1].set_xlabel("I"); axs[1].set_ylabel("Q")
    save_tight(fig); title(fig, "RDS: así es la señal", c)
    footer(fig, "Señal SINTÉTICA de bladerf-lab --gen rds (250 kS/s). Demodulación (FM, mezcla, recuperación de fase elevando al cuadrado): Python de ilustración, no el código C. La señal generada no lleva ruido: en el aire los puntos se ensanchan.", c)
    return fig


@fig("dab-senal")
def dab(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V2, "sigid_dab", "dab.sigmf-meta"), 0, 2048000 * 0.5 // 1)
    fig, axs = fig_axes(1, 3, h=3.9, gridspec_kw=dict(width_ratios=[1.2, 0.9, 0.9]))
    n = 2048
    p = np.abs(iq) ** 2
    sm = np.convolve(p, np.ones(2304) / 2304, "same")
    tt = np.arange(len(p)) / fs * 1e3
    axs[0].plot(tt[::40], 10 * np.log10(sm[::40] + 1e-9), color=c["c"][0], lw=1)
    axs[0].set_xlabel("tiempo (ms)"); axs[0].set_ylabel("potencia (dB)"); axs[0].set_title("Un símbolo nulo cada 96 ms")
    f, pp = psd(iq[: 2048 * 200], fs, 2048)
    axs[1].plot(f / 1e3, pp, color=c["c"][2], lw=0.9)
    axs[1].set_xlabel("kHz sobre 227,36 MHz"); axs[1].set_title("Espectro: 1,536 MHz")
    # constelación D-QPSK: sincroniza con el nulo y el prefijo cíclico
    nul = int(np.argmin(sm[3000:-300000])) + 3000
    L = 2552
    st = nul + 1328 + 20
    syms = []
    for k in range(60):
        s0 = st + k * L + 504
        syms.append(np.fft.fftshift(np.fft.fft(iq[s0:s0 + 2048])))
    syms = np.array(syms)
    d = syms[2:, 1024 + 1:1024 + 769] * np.conj(syms[1:-1, 1024 + 1:1024 + 769])
    ang = np.angle(d)
    d = d[np.abs(d) > np.percentile(np.abs(d), 40)]
    dd = d / np.abs(d)
    ph = np.angle(np.mean(dd ** 4)) / 4
    dd = dd * np.exp(-1j * ph) * np.exp(-1j * np.pi / 4) * 0 + dd * np.exp(-1j * ph)
    sel = dd[:4000]
    axs[2].scatter(sel.real, sel.imag, s=3, color=c["c"][1], alpha=0.3, linewidths=0)
    axs[2].set_aspect("equal"); axs[2].set_xlim(-1.4, 1.4); axs[2].set_ylim(-1.4, 1.4)
    axs[2].set_title("D-QPSK diferencial"); axs[2].set_xlabel("I"); axs[2].set_ylabel("Q")
    save_tight(fig); title(fig, "DAB: así es la señal", c)
    footer(fig, "Señal: grabación IQ real (sigidwiki «DAB», 227,36 MHz, 2,048 MS/s). Sincronía con el símbolo nulo y el prefijo cíclico, y la FFT: Python de ilustración (sin CFO entero ni ecualizar), no el código C.", c)
    return fig


def _disc_chan(iq, fs, off, bw, fo):
    y = channel(iq, fs, off, bw, fo)
    return y


@fig("pocsag-senal")
def pocsag(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V1, "multimon_samples", "pocsag1200.sigmf-meta"))
    y = channel(iq, fs, 466075000 - fc, 9000, 48000)
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    spec_img(axs[0], y[: 48000 * 3], 48000, 0, nfft=256, hop=64, cm=cmap(t))
    axs[0].set_title("Cascada del canal de 25 kHz"); axs[0].set_xlabel("frecuencia (kHz)"); axs[0].set_ylabel("tiempo (ms)")
    d = fm_disc(y)
    a = np.convolve(np.abs(y), np.ones(400) / 400, "same")
    idx = np.where(a[1:] > 0.5 * a.max())[0]
    seg = d[idx[len(idx) // 4: len(idx) // 4 + 6000]]
    seg = np.convolve(seg, np.ones(8) / 8, "same")
    seg = seg / (np.percentile(np.abs(seg), 90) + 1e-9)
    eye(axs[1], seg, 40, c, color=c["c"][3])
    axs[1].set_title("Ojo FSK 1200 bd (±4,5 kHz)"); axs[1].set_ylabel("desviación normalizada"); axs[1].set_ylim(-1.8, 1.8)
    save_tight(fig); title(fig, "POCSAG: así es la señal", c)
    footer(fig, "Señal: audio FM real (repositorio de pruebas de multimon-ng, «POCSAG 1200 bps») remodulado en FM con desviación POCSAG. Demodulación: Python de ilustración, no el código C.", c)
    return fig


@fig("flex-senal")
def flex(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V1, "multimon_samples", "flex.sigmf-meta"), 0, 1000000 * 20)
    y = channel(iq, fs, 466075000 - fc, 10000, 48000)
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    spec_img(axs[0], y[: 48000 * 12], 48000, 0, nfft=256, hop=128, cm=cmap(t))
    axs[0].set_title("Cascada del canal (12 s)"); axs[0].set_xlabel("frecuencia (kHz)"); axs[0].set_ylabel("tiempo (ms)")
    d = fm_disc(y) * 48000 / (2 * np.pi)
    a = np.convolve(np.abs(y), np.ones(200) / 200, "same")[1:]
    d = np.convolve(d, np.ones(30) / 30, "same")[a > 0.4 * a.max()]
    axs[1].hist(d / 1e3, bins=120, color=c["c"][4], alpha=0.85)
    axs[1].set_xlabel("frecuencia instantánea (kHz), suavizada a 1 símbolo"); axs[1].set_ylabel("muestras")
    axs[1].set_title("Histograma FSK de 2 niveles (1600 bd)")
    save_tight(fig); title(fig, "FLEX: así es la señal", c)
    footer(fig, "Señal: audio FM real (multimon-ng, «FLEX 1600 P2000») remodulado en FM con desviación ±4,8 kHz. Los modos 3200/6400 de 4 niveles no se han podido contrastar con una grabación real. Gráfica: Python de ilustración.", c)
    return fig


@fig("dmr-senal")
def dmr(t, c):
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    iq, fs, fc = load_sigmf(os.path.join(V2, "dmr_sigid", "dmr.sigmf-meta"), 0, 48000 * 12)
    spec_img(axs[0], iq, fs, fc, nfft=256, hop=128, cm=cmap(t))
    axs[0].set_title("Cascada real (12 s, 48 kS/s)"); axs[0].set_xlabel("frecuencia (kHz)"); axs[0].set_ylabel("tiempo (ms)")
    g, gfs, gfc = load_sigmf(os.path.join(G, "dmr.sigmf-meta"))
    y = lowpass(g, 7000, gfs, 101)
    d = fm_disc(y) * gfs / (2 * np.pi) / 1944.0
    a = np.abs(y[1:]) > 0.5 * np.abs(y).max()
    d = np.convolve(d, np.ones(10) / 10, "same")[a]
    axs[1].hist(d[10::10], bins=100, color=c["c"][4], alpha=0.85)
    for v in (-1, -1 / 3, 1 / 3, 1):
        axs[1].axvline(v, color=c["c"][1], lw=1, ls="--")
    axs[1].set_xlabel("desviación (1 = 1944 Hz)"); axs[1].set_xlim(-1.5, 1.5); axs[1].set_ylabel("símbolos")
    axs[1].set_title("4FSK: cuatro niveles (sintética)")
    save_tight(fig); title(fig, "DMR: así es la señal", c)
    footer(fig, "Izquierda: grabación IQ real (sigidwiki «DMR»); el decodificador no la valida (1 sincronía, 0 ráfagas: no hay evidencia de que sea DMR). Derecha: señal SINTÉTICA de bladerf-lab --gen dmr. Gráfica: Python de ilustración.", c)
    return fig


@fig("ism-senal")
def ism(t, c):
    iq = load_raw(os.path.join(TD, "ism_tx141bv2_rtl433.cu8"), "cu8")
    fs = 250000.0
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    env = np.convolve(np.abs(iq) ** 2, np.ones(8) / 8, "same")
    tt = np.arange(len(env)) / fs * 1e3
    axs[0].plot(tt[::4], env[::4], color=c["c"][4], lw=0.9)
    axs[0].set_title("Envolvente de potencia del fragmento"); axs[0].set_xlabel("tiempo (ms)"); axs[0].set_ylabel("|x|² (8 muestras)")
    nf = np.median(env); pk = np.percentile(env, 99.9)
    th = nf + 0.55 * (pk - nf)
    axs[0].axhline(th, color=c["c"][1], ls="--", lw=1)
    axs[0].text(tt[-1] * 0.98, th, "umbral adaptativo", ha="right", va="bottom", fontsize=8.5, color=c["c"][1])
    a = np.where(env > th)[0]
    i0 = a[0] if len(a) else 0
    seg = env[max(0, i0 - 200): i0 + 12000]
    ts = (np.arange(len(seg)) - 200) / fs * 1e3
    axs[1].plot(ts, seg, color=c["c"][2], lw=0.9)
    axs[1].axhline(th, color=c["c"][1], ls="--", lw=1)
    axs[1].set_title("Una ráfaga ampliada: el tren de pulsos"); axs[1].set_xlabel("tiempo (ms)")
    save_tight(fig); title(fig, "ISM: así es la señal", c)
    footer(fig, "Señal: grabación IQ real de rtl_433_tests (LaCrosse TX141-Bv2, 433,92 MHz, 250 kS/s, cu8). Umbral calculado como en src/decoders/ism.c (nf + 0,55·(pk − nf)) pero con los extremos del fragmento; gráfica: Python de ilustración.", c)
    return fig


@fig("rs41-senal")
def rs41(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V2, "rs41_rs1729", "20140717_402MHz.sigmf-meta"), 0, 48000 * 30)
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    spec_img(axs[0], iq, fs, fc, nfft=256, hop=128, cm=cmap(t))
    axs[0].set_title("Cascada del canal (30 s, 48 kS/s)"); axs[0].set_xlabel("frecuencia (kHz)"); axs[0].set_ylabel("tiempo (ms)")
    d = fm_disc(iq)
    a = np.convolve(np.abs(iq), np.ones(300) / 300, "same")[1:]
    ii = np.where(a > 0.5 * a.max())[0]
    seg = d[ii[len(ii) // 3: len(ii) // 3 + 4000]]
    seg = np.convolve(seg, np.ones(5) / 5, "same")
    seg = (seg - np.median(seg)) / (np.percentile(np.abs(seg - np.median(seg)), 90) + 1e-9)
    eye(axs[1], seg, 10, c, color=c["c"][5])
    axs[1].set_title("Ojo GFSK 4800 bd"); axs[1].set_ylabel("desviación normalizada"); axs[1].set_ylim(-2, 2)
    save_tight(fig); title(fig, "RS41: así es la señal", c)
    footer(fig, "Señal: grabación real de rs1729/RS (audio FM de una sonda RS41, 14-07-2014) remodulada. Demodulación: Python de ilustración, no el código C.", c)
    return fig


def frames():
    frame_render("rds-estructura", "RDS: estructura de un grupo (4 bloques × 26 bits)", [
        ("Grupo 0A (nombre de emisora, PS)", [("PI", 16, "A + 10 CRC"), ("grupo/B0/TP/PTY", 16, "tipo 0A, TP, PTY"), ("TA MS DI seg.", 16, "2 car. de PS"), ("AF", 16, "frecuencias"), ("PS", 16, "2 caracteres")]),
        ("Grupo 2A (RadioText)", [("PI", 16, "bloque A"), ("2A + A/B", 16, "bloque B"), ("RT", 16, "2 caracteres"), ("RT", 16, "2 caracteres")]),
    ], "Estructura según EN 50067 y docs/protocolos/rds.md. Cada bloque son 16 bits de información + 10 de síndrome con offset. Ilustración dibujada.", rh=1.1)
    frame_render("pocsag-estructura", "POCSAG: estructura de un lote (17 palabras de 32 bits)", [
        ("Transmisión", [("preámbulo", 18, "576 bit 1010…"), ("sincronía", 2, "0x7CD215D8"), ("lote 0", 16, "8 tramas × 2 palabras"), ("sincronía", 2, ""), ("lote 1", 16, "")]),
        ("Palabra de 32 bits", [("0/1", 1, "dir./msj"), ("20 bits", 20, "dirección (18) + función (2) o texto"), ("BCH(31,21)", 10, "10 bits"), ("par.", 1, "paridad")]),
    ], "Estructura según ITU-R M.584 y docs/protocolos/pocsag.md. Ilustración dibujada.", rh=1.2)
    frame_render("ism-estructura", "ISM: un sensor meteorológico OOK (Oregon / LaCrosse)", [
        ("Ráfaga, repetida varias veces", [("preámbulo", 12, "sincronía"), ("id/canal", 14, "tipo de sensor"), ("temp.", 16, "BCD"), ("humedad", 10, ""), ("batería", 6, ""), ("CRC / suma", 14, "valida el protocolo")]),
    ], "Esquema genérico; los campos exactos de cada modelo están en src/decoders/ism_protocols.c. Ilustración dibujada.", rh=1.5)


if __name__ == "__main__":
    run(FIGS, sys.argv[1:])
    if not sys.argv[1:] or "estructura" in sys.argv[1:]:
        frames()
