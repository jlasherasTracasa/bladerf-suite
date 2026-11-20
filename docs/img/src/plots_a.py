#!/usr/bin/env python3
"""plots_a.py — gráficas «Así es la señal» (A): ais, acars, aprs, dsc, vdl2, adsb.
Datos: grabaciones de build/v1/dl (datos no fiables: solo se leen como números) y señales
generadas con `bladerf-lab --gen` (build/doc2a/gen). Los demoduladores de aquí son
Python de ilustración (scipy), NO el código C del decodificador; el pie de cada figura lo dice.
Uso: python3 -I docs/img/src/plots_a.py [id ...]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plots_common import *
from scipy.signal import resample_poly

V1 = os.path.join(ROOT, "build", "v1", "dl")
G = os.path.join(ROOT, "build", "doc2a", "gen")
FIGS = {}


def fig(name):
    def deco(f):
        FIGS[name] = f
        return f
    return deco


def burst_start(x, fs, win=0.004, rank=0):
    """Centro de la ráfaga más potente (media móvil de |x|²)."""
    n = int(win * fs)
    p = np.convolve(np.abs(x) ** 2, np.ones(n) / n, "same")
    return int(np.argmax(p))


def channel(x, fs, off, bw, fo):
    y = shift(x, off, fs)
    y = lowpass(y, bw, fs, 161)
    up, dn = int(fo), int(fs)
    from math import gcd
    g = gcd(up, dn)
    return resample_poly(y, up // g, dn // g)


def eye(ax, y, sps, c, nsym=2, osr=8, maxn=300, color=None):
    y = resample_poly(y, osr, 1)
    L = int(nsym * sps * osr)
    off = int(np.argmax([np.abs(y[o::int(sps * osr)][:200]).mean() for o in range(int(sps * osr))]))
    n = 0
    t = np.linspace(-nsym / 2, nsym / 2, L)
    for i in range(off, len(y) - L, int(sps * osr)):
        ax.plot(t, y[i:i + L], color=color or c["c"][0], alpha=0.22, lw=0.9)
        n += 1
        if n >= maxn:
            break
    ax.set_xlabel("tiempo (símbolos)")


@fig("ais-ojo")
def ais_eye(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V1, "s_ais_mp3", "ais.sigmf-meta"), 0, int(250000 * 9))
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    im, S = spec_img(axs[0], iq, fs, fc, nfft=512, hop=256, cm=cmap(t), fmt="mhz")
    axs[0].set_title("Cascada, 9 s en 162,000 MHz")
    axs[0].set_xlabel("frecuencia (MHz)"); axs[0].set_ylabel("tiempo (ms)")
    axs[0].axvline(161.975, color=c["c"][4], lw=1, ls="--")
    g, gfs, gfc = load_sigmf(os.path.join(G, "ais.sigmf-meta"))
    y = channel(g, gfs, 161975000 - gfc, 9000, 48000)
    rng = np.random.default_rng(3)
    y = y + (rng.standard_normal(len(y)) + 1j * rng.standard_normal(len(y))) * np.sqrt(np.mean(np.abs(y[np.abs(y) > 0.05]) ** 2) / 2 / 10 ** 3.0)
    s0 = burst_start(y, 48000, 0.02)
    seg = y[max(0, s0 - 600):s0 + 900]
    d = fm_disc(seg) / (2 * np.pi * 2400 / 48000)
    eye(axs[1], d, 5, c)
    axs[1].set_title("Ojo GMSK (señal generada, SNR 30 dB)")
    axs[1].set_ylabel("desviación / 2,4 kHz"); axs[1].set_ylim(-1.8, 1.8)
    save_tight(fig); title(fig, "AIS: así es la señal", c)
    footer(fig, "Izquierda: audio FM real (sigidwiki «AIS Sound», Long Beach) remodulado en FM; la cascada muestra el canal con ruido de audio y las ráfagas. Derecha: señal SINTÉTICA de bladerf-lab --gen ais con ruido añadido. Demodulación: Python de ilustración, no el código C.", c)
    return fig


def _gen_spec(name, off_hz, bw, fo, label, tmax=None):
    iq, fs, fc = load_sigmf(os.path.join(G, name + ".sigmf-meta"))
    return iq, fs, fc


@fig("acars-senal")
def acars(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V1, "z_acars", "acars.sigmf-meta"))
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    seg = iq[:int(fs * 10)]
    im, S = spec_img(axs[0], seg, fs, fc, nfft=1024, hop=512, cm=cmap(t), fmt="mhz")
    axs[0].set_title("Cascada: portadora AM con bloques"); axs[0].set_xlabel("frecuencia (MHz)"); axs[0].set_ylabel("tiempo (ms)")
    # canal: pico de portadora más fuerte
    f, p = psd(iq[:int(fs * 4)], fs, 4096)
    f0 = f[np.argmax(p)]
    env = np.abs(lowpass(shift(iq, f0, fs), 4000, fs, 129))
    a = np.where(np.convolve(env, np.ones(200) / 200, "same") > 0)[0]
    # ráfaga de datos: zona de mayor variación de la envolvente
    v = np.convolve((env - np.convolve(env, np.ones(400) / 400, "same")) ** 2, np.ones(2000) / 2000, "same")
    i0 = int(np.argmax(v))
    seg2 = env[max(0, i0 - 3000):i0 + 3000]
    axs[1].plot(np.arange(len(seg2)) / fs * 1e3, seg2, color=c["c"][2], lw=1)
    axs[1].set_title("Envolvente AM con datos MSK 2400")
    axs[1].set_xlabel("tiempo (ms)"); axs[1].set_ylabel("|x|")
    save_tight(fig); title(fig, "ACARS: así es la señal", c)
    footer(fig, "Señal: grabación IQ real (sigidwiki «ACARS IQ», VQ-BKV), 135,5 kS/s. Gráfica: Python de ilustración.", c)
    return fig



@fig("aprs-senal")
def aprs(t, c):
    iq, fs, fc = load_sigmf(os.path.join(G, "aprs.sigmf-meta"))
    y = channel(iq, fs, 144800000 - fc, 7000, 48000)
    y = y[: 48000 * 2]
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    spec_img(axs[0], y, 48000, 0, nfft=256, hop=64, cm=cmap(t))
    axs[0].set_title("Cascada del canal FM (2 s)"); axs[0].set_xlabel("frecuencia (kHz)"); axs[0].set_ylabel("tiempo (ms)")
    d = fm_disc(y[: 48000])
    f, p = psd(d.astype(np.complex64), 48000, 1024)
    m = f > 0
    axs[1].plot(f[m] / 1e3, p[m], color=c["c"][1])
    axs[1].set_xlim(0, 4); axs[1].set_xlabel("frecuencia del audio (kHz)"); axs[1].set_ylabel("dB")
    axs[1].set_title("Audio: tonos Bell 202 a 1200 y 2200 Hz")
    for fr in (1.2, 2.2):
        axs[1].axvline(fr, color=c["c"][4], ls="--", lw=1)
    save_tight(fig); title(fig, "APRS: así es la señal", c)
    footer(fig, "Señal SINTÉTICA de bladerf-lab --gen aprs (no hay grabación APRS de posición que decodifique). Demodulación: Python de ilustración, no el código C.", c)
    return fig


@fig("dsc-senal")
def dsc(t, c):
    iq, fs, fc = load_sigmf(os.path.join(G, "dsc.sigmf-meta"))
    y = channel(iq, fs, 156525000 - fc, 6000, 48000)
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    spec_img(axs[0], y[: 48000 * 4], 48000, 0, nfft=256, hop=64, cm=cmap(t))
    axs[0].set_title("Cascada del canal 70 (4,4 s)"); axs[0].set_xlabel("frecuencia (kHz)"); axs[0].set_ylabel("tiempo (ms)")
    d = fm_disc(y) * 48000 / (2 * np.pi)
    a = np.abs(y[1:]) > 0.3 * np.abs(y).max()
    seg = d[a][:2400]
    axs[1].plot(np.arange(len(seg)) / 48, seg / 1e3, color=c["c"][3], lw=1)
    axs[1].set_xlabel("tiempo (ms)"); axs[1].set_ylabel("frecuencia (kHz)")
    axs[1].set_title("Frecuencia instantánea: FSK de 1200 bd")
    save_tight(fig); title(fig, "DSC: así es la señal", c)
    footer(fig, "Señal SINTÉTICA de bladerf-lab --gen dsc (no hay grabación real de DSC). Demodulación: Python de ilustración, no el código C.", c)
    return fig


def _diffconst(y, sps, c, ax, col, maxn=2500):
    # elige la fase de muestreo con más energía, y la constelación diferencial d = y[k]·conj(y[k-1])
    best = max(range(sps), key=lambda o: np.mean(np.abs(y[o::sps]) ** 2))
    z = y[best::sps]
    d = z[1:] * np.conj(z[:-1])
    d = d / (np.abs(d) + 1e-9)
    d = d[np.abs(z[1:]) > 0.35 * np.abs(z).max()][:maxn]
    ax.scatter(d.real, d.imag, s=5, color=col, alpha=0.35, linewidths=0)
    ax.set_aspect("equal"); ax.set_xlim(-1.4, 1.4); ax.set_ylim(-1.4, 1.4)
    th = np.linspace(0, 2 * np.pi, 200); ax.plot(np.cos(th), np.sin(th), color=c["outline"], lw=0.8)


@fig("vdl2-senal")
def vdl2(t, c):
    fig, axs = fig_axes(1, 3, h=3.9, gridspec_kw=dict(width_ratios=[1.2, 1, 1]))
    iq, fs, fc = load_sigmf(os.path.join(V1, "z_vdl2", "vdl2.sigmf-meta"), 0, 105000 * 6)
    spec_img(axs[0], iq, fs, fc, nfft=256, hop=128, cm=cmap(t), fmt="khz")
    axs[0].set_title("Cascada real, 6 s"); axs[0].set_xlabel("kHz"); axs[0].set_ylabel("tiempo (ms)")
    g, gfs, gfc = load_sigmf(os.path.join(G, "vdl2.sigmf-meta"))
    y = shift(g, 136975000 - gfc, gfs)
    y = lowpass(y, 8000, gfs, 201)[:: int(gfs // 105000)]
    rng = np.random.default_rng(1)
    y = y + (rng.standard_normal(len(y)) + 1j * rng.standard_normal(len(y))) * 0.01
    _diffconst(y, 10, c, axs[1], c["c"][2])
    axs[1].set_title("D8PSK diferencial, sintética"); axs[1].set_xlabel("I"); axs[1].set_ylabel("Q")
    s0 = burst_start(iq[:: 1], fs, 0.01)
    yr = lowpass(iq[max(0, s0 - 3000):s0 + 3000], 6500, fs, 101)
    _diffconst(yr, 10, c, axs[2], c["c"][1])
    axs[2].set_title("D8PSK diferencial, real"); axs[2].set_xlabel("I")
    save_tight(fig); title(fig, "VDL Modo 2: así es la señal", c)
    footer(fig, "Izq. y der.: grabación IQ real (sigidwiki «VDL-M2 IQ», 105 kS/s); la real sale emborronada (ISI y CFO sin corregir: aquí no hay RRC ni Gardner ni CFO; es una ilustración; el decodificador sí saca 38 de 41 tramas de esa grabación). Centro: señal SINTÉTICA de --gen vdl2 con ruido.", c)
    return fig


@fig("adsb-trama")
def adsb(t, c):
    iq, fs, fc = load_sigmf(os.path.join(G, "adsb.sigmf-meta"))
    m = np.abs(iq)
    i0 = int(np.argmax(np.convolve(m, np.ones(8) / 8, "same")))
    # busca el comienzo del preámbulo: primer pulso alto antes del máximo
    thr = 0.5 * m.max()
    a = np.where(m[: i0 + 40] > thr)[0]
    st = int(a[a > i0 - 240][0]) if len(a[a > i0 - 240]) else i0
    seg = m[st - 4: st + 16 + 224 + 8]
    x = (np.arange(len(seg)) - 4) / 2.0
    fig, ax = fig_axes(1, 1, h=3.5)
    ax.step(x, seg, where="post", color=c["c"][2], lw=1.3)
    for p in (0, 1, 3.5, 4.5):
        ax.axvspan(p, p + 0.5, color=c["c"][4], alpha=0.25, lw=0)
    ax.axvspan(8, 8 + 112, color=c["c"][0], alpha=0.06, lw=0)
    ax.text(4, seg.max() * 1.05, "preámbulo 8 µs", ha="center", fontsize=8.5, color=c["c"][4])
    ax.text(40, seg.max() * 1.05, "112 bits PPM: 1 µs por bit, pulso en la 1.ª mitad = 1, en la 2.ª = 0", ha="left", fontsize=8.5, color=c["c"][0])
    ax.set_xlabel("tiempo (µs)"); ax.set_ylabel("|x|"); ax.set_xlim(-2, 122); ax.set_ylim(0, seg.max() * 1.18)
    ax.set_title("Módulo de una trama DF17 a 2 Msps (una muestra por ½ µs)")
    save_tight(fig, top=0.84, bottom=0.2); title(fig, "ADS-B: así es la señal", c)
    footer(fig, "Señal SINTÉTICA de bladerf-lab --gen adsb (la antena actual no recibe 1090 MHz; no hay grabación real). Gráfica: Python de ilustración.", c)
    return fig


def frames():
    frame_render("adsb-estructura", "ADS-B: estructura de la trama DF17 (112 bits)", [
        ("DF17, 112 bits tras el preámbulo", [("DF", 5, "17"), ("CA", 3, "capab."), ("ICAO", 24, "dirección de 24 bits"), ("ME", 56, "type code (5) + datos"), ("PI = CRC-24", 24, "0xFFF409; síndrome 0")]),
        ("ME de posición en vuelo (tc 9-18)", [("tc", 5, "type"), ("SS", 2, ""), ("NIC", 1, ""), ("ALT", 12, "25·N−1000 ft"), ("T", 1, ""), ("F", 1, "par/impar"), ("LAT-CPR", 17, ""), ("LON-CPR", 17, "")]),
    ], "Estructura según docs/protocolos/adsb.md (J. Sun, «The 1090 Megahertz Riddle»). Ilustración dibujada, no es una captura.")
    frame_render("ais-estructura", "AIS: estructura de la ráfaga (256 bits a 9600 bd)", [
        ("Ráfaga GMSK de 26,7 ms", [("rampa", 8, "8 bit"), ("entrenamiento", 24, "0101…"), ("bandera", 8, "0x7E"), ("carga", 168, "campos M.1371, MSB primero"), ("FCS", 16, "CRC-16/X-25"), ("bandera", 8, "0x7E"), ("colchón", 24, "")]),
        ("Mensaje 1/2/3 (carga de 168 bits)", [("tipo", 6, "1-3"), ("rep", 2, ""), ("MMSI", 30, ""), ("estado", 4, ""), ("giro", 8, ""), ("SOG", 10, "1/10 kn"), ("pos", 55, "lon 28 + lat 27 (1/10000 min)"), ("COG", 12, ""), ("resto", 41, "")]),
    ], "Estructura según ITU-R M.1371 y docs/protocolos/ais.md. Ilustración dibujada, no es una captura.")
    frame_render("acars-estructura", "ACARS: estructura del bloque (MSK 2400 bd)", [
        ("Bloque de datos", [("prefijo", 16, "2400 bit de 1"), ("SYN SYN", 16, "0x16 0x16"), ("SOH", 8, "0x01"), ("modo", 8, "1 car."), ("matrícula", 56, "7 car."), ("TAK", 8, ""), ("etiqueta", 16, "2 car."), ("blq", 8, ""), ("n.º mensaje + vuelo", 80, ""), ("texto", 90, "hasta 220 car."), ("ETX", 8, ""), ("BCS", 16, "CRC-16"), ("DEL", 8, "")]),
    ], "Estructura según docs/protocolos/acars.md (ARINC 618). Cada carácter: 7 bits + paridad impar, LSB primero. Ilustración dibujada.", rh=1.4)


if __name__ == "__main__":
    run(FIGS, sys.argv[1:])
    if not sys.argv[1:] or "estructura" in sys.argv[1:]:
        frames()
