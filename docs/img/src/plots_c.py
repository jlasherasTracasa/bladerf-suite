#!/usr/bin/env python3
"""plots_c.py — gráficas «Así es la señal» (C): lora, meshtastic, lte, nr, gsm, wifi, dvbt, tdt.
Datos: build/v2/dl, build/tv1/rec/c32_frag.cs8, tests/data y señales de `bladerf-lab --gen`.
Los demoduladores son Python de ilustración (scipy/numpy), NO el código C. Uso: python3 -I plots_c.py [id ...]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plots_common import *
from scipy.signal import resample_poly, fftconvolve

V2 = os.path.join(ROOT, "build", "v2", "dl")
G = os.path.join(ROOT, "build", "doc2a", "gen")
TV = os.path.join(ROOT, "build", "tv1", "rec")
FIGS = {}


def fig(name):
    def deco(f):
        FIGS[name] = f
        return f
    return deco


def _lora(meta, sf, bw, name, t, c, real_note):
    iq, fs, fc = load_sigmf(meta)
    x = resample_poly(lowpass(iq, bw * 0.7, fs, 161), int(2 * bw), int(fs))
    fs2 = 2 * bw
    N = 2 ** sf
    nw = N * 2
    p = np.convolve(np.abs(x) ** 2, np.ones(nw // 4) / (nw // 4), "same")
    a = np.where(p > 0.25 * p.max())[0]
    i0 = a[0]
    seg = x[i0: i0 + nw * 30]
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1]))
    spec_img(axs[0], seg, fs2, 0, nfft=128, hop=16, cm=cmap(t))
    axs[0].set_title("Cascada: chirps del paquete"); axs[0].set_xlabel("frecuencia (kHz)"); axs[0].set_ylabel("tiempo (ms)")
    n = np.arange(nw)
    k = np.arange(nw)
    # chirp base descendente a 2·BW: fase −π·n²/(2N)·... (dechirp con el conjugado del upchirp)
    up = np.exp(1j * np.pi * (k ** 2 / (2 * N * 2) - k / 2) * 1.0)
    pk = []
    for w in range(30):
        wnd = seg[w * nw:(w + 1) * nw]
        if len(wnd) < nw:
            break
        S = np.abs(np.fft.fft(wnd * np.conj(up))) ** 2
        S = S[:N] + S[N:2 * N] if False else S[:N] + S[N: 2 * N]
        pk.append(S)
    pk = np.array(pk)
    im = axs[1].imshow(10 * np.log10(pk.T + 1e-12), aspect="auto", origin="lower", cmap=cmap(t), vmin=np.percentile(10 * np.log10(pk + 1e-12), 40))
    axs[1].set_title("Dechirp + FFT: un pico por símbolo"); axs[1].set_xlabel("ventana (símbolo)"); axs[1].set_ylabel("bin de la FFT (valor del símbolo)")
    axs[1].grid(False)
    save_tight(fig); title(fig, name + ": así es la señal", c)
    footer(fig, real_note + " Dechirp y FFT: Python de ilustración, no el código C.", c)
    return fig


@fig("lora-senal")
def lora(t, c):
    return _lora(os.path.join(V2, "lora_ref", "ref_sf9_12.sigmf-meta"), 9, 125000, "LoRa", t, c,
                 "Señal: referencia generada con la herramienta externa loratx (SF9, BW 125 kHz, CR 4/5; no es una captura en el aire).")


@fig("meshtastic-senal")
def mesh(t, c):
    return _lora(os.path.join(V2, "mesh_gen", "m.sigmf-meta"), 11, 250000, "Meshtastic", t, c,
                 "Señal SINTÉTICA de bladerf-lab --gen meshtastic (LongFast: SF11, BW 250 kHz, sincronía 0x2B).")


def _zc_pss(u):
    n = np.arange(62)
    d = np.where(n < 31, np.exp(-1j * np.pi * u * n * (n + 1) / 63), np.exp(-1j * np.pi * u * (n + 1) * (n + 2) / 63))
    X = np.zeros(128, complex)
    for i in range(31):
        X[(-31 + i) % 128] = d[i]
        X[1 + i] = d[31 + i]
    x = np.fft.ifft(X) * np.sqrt(128)
    return x


@fig("lte-pss")
def lte(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V2, "sigid_lte", "vf14.sigmf-meta"), 0, int(1.92e6 * 0.06))
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1, 1.35]))
    f, p = psd(iq, fs, 2048)
    axs[0].plot(f / 1e3, p, color=c["c"][5])
    axs[0].set_xlabel("kHz sobre el centro"); axs[0].set_ylabel("dB"); axs[0].set_title("Espectro: LTE de 1,4 MHz (6 RB)")
    e = np.convolve(np.abs(iq) ** 2, np.ones(128), "same")
    cols = [c["c"][0], c["c"][1], c["c"][2]]
    best = None
    for r, u in enumerate((25, 29, 34)):
        tpl = _zc_pss(u)
        cc = np.abs(fftconvolve(iq, np.conj(tpl[::-1]), "same")) ** 2 / (e + 1e-9)
        axs[1].plot(np.arange(len(cc)) / fs * 1e3, cc, color=cols[r], lw=0.9, label=f"raíz {u} (N_ID2 = {r})")
        if best is None or cc.max() > best[0]:
            best = (cc.max(), r)
    axs[1].set_xlabel("tiempo (ms)"); axs[1].set_ylabel("correlación normalizada"); axs[1].legend(fontsize=8, loc="upper center", ncol=3); axs[1].set_ylim(0, 78)
    axs[1].set_title("Correlación con la PSS: un pico cada 5 ms")
    save_tight(fig); title(fig, "LTE: así es la señal", c)
    footer(fig, "Señal: grabación IQ real (sigidwiki «LTE 1,4 MHz», operador vf14, 1,92 MS/s). Correlación con las 3 plantillas ZC de 128 muestras: Python de ilustración (la C usa 48 ms plegados); el decodificador da PCI 65 (N_ID2 = 2).", c)
    return fig


def _nr_pss(nid2, fs=7.68e6):
    x = np.zeros(127 + 7, int)
    x[:7] = [0, 1, 1, 0, 1, 1, 1]
    for i in range(120):
        x[i + 7] = (x[i + 4] + x[i]) % 2
    n = np.arange(127)
    d = 1 - 2 * x[(n + 43 * nid2) % 127]
    N = 256
    X = np.zeros(N, complex)
    for i in range(127):
        X[(i - 63) % N] = d[i]
    return np.fft.ifft(X) * np.sqrt(N)


@fig("nr-pss")
def nr(t, c):
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1, 1.2]))
    iq, fs, fc = load_sigmf(os.path.join(V2, "sigid_nr", "or10.sigmf-meta"), 0, int(15.36e6 * 0.1))
    f, p = psd(iq, fs, 4096)
    axs[0].plot(f / 1e6, p, color=c["c"][5], lw=0.9)
    axs[0].set_xlabel("MHz sobre el centro"); axs[0].set_ylabel("dB"); axs[0].set_title("Grabación real: espectro de NR")
    g, gfs, gfc = load_sigmf(os.path.join(G, "nr.sigmf-meta"))
    y = resample_poly(lowpass(g, 3.2e6, gfs, 161), 1, 2)
    e = np.convolve(np.abs(y) ** 2, np.ones(256), "same")
    for r in range(3):
        cc = np.abs(fftconvolve(y, np.conj(_nr_pss(r)[::-1]), "same")) ** 2 / (e + 1e-9)
        axs[1].plot(np.arange(len(cc)) / 7.68e3, cc, color=c["c"][r], lw=1.2, alpha=0.7, label=f"N_ID2 = {r}")
    axs[1].set_xlabel("tiempo (ms)"); axs[1].set_ylabel("correlación normalizada"); axs[1].legend(fontsize=8)
    axs[1].set_title("Correlación con la PSS en una señal generada")
    save_tight(fig); title(fig, "5G NR: así es la señal", c)
    footer(fig, "Izquierda: grabación IQ real (sigidwiki «NR 10 MHz», 15,36 MS/s); el decodificador no encuentra celda (centro desconocido): sin validar. Derecha: señal SINTÉTICA de bladerf-lab --gen nr. Correlación: Python de ilustración.", c)
    return fig


@fig("gsm-fcch")
def gsm(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V2, "sigid_gsm", "bcch.sigmf-meta"), 0, int(1083333 * 0.4))
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1.2]))
    y = lowpass(iq, 170e3, fs, 33)
    spec_img(axs[0], y[: int(fs * 0.12)], fs, 0, nfft=256, hop=64, cm=cmap(t))
    axs[0].set_title("Cascada del canal GSM (120 ms)"); axs[0].set_xlabel("frecuencia (kHz)"); axs[0].set_ylabel("tiempo (ms)")
    z = y[4:] * np.conj(y[:-4])
    W = 96 * 4
    s1 = np.convolve(z, np.ones(W), "same")
    m = np.abs(s1) / (np.convolve(np.abs(z), np.ones(W), "same") + 1e-9)
    tt = np.arange(len(m)) / fs * 1e3
    axs[1].plot(tt, m, color=c["c"][3], lw=1)
    axs[1].axhline(0.8, color=c["c"][1], ls="--", lw=1)
    axs[1].set_xlabel("tiempo (ms)"); axs[1].set_ylabel("M = |Σz| / Σ|z|"); axs[1].set_title("Detector de FCCH: tono puro ⇒ M ≈ 1")
    save_tight(fig); title(fig, "GSM: así es la señal", c)
    footer(fig, "Señal: grabación IQ real (sigidwiki «GSM downlink BCCH», 1,083 MS/s). El FCCH es un tono a +67,7 kHz; M se calcula como en src/decoders/gsm.c (lag 4, ventana de 96 símbolos) pero en Python de ilustración.", c)
    return fig


@fig("wifi-senal")
def wifi(t, c):
    iq, fs, fc = load_sigmf(os.path.join(V2, "sigid_wifi", "wifi.sigmf-meta"), 0, int(20e6 * 0.06))
    fig, axs = fig_axes(1, 2, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1.1]))
    spec_img(axs[0], iq, fs, fc, nfft=1024, hop=1024 * 4, cm=cmap(t), fmt="mhz")
    axs[0].xaxis.set_major_locator(plt.MaxNLocator(4)); axs[0].set_title("Cascada, 60 ms de 2437 MHz (canal 6)"); axs[0].set_xlabel("frecuencia (MHz)"); axs[0].set_ylabel("tiempo (ms)")
    e = np.convolve(np.abs(iq) ** 2, np.ones(20) / 20, "same")
    tt = np.arange(0, len(e), 20) / fs * 1e3
    axs[1].plot(tt, 10 * np.log10(e[::20] + 1e-9), color=c["c"][5], lw=0.8)
    axs[1].set_xlabel("tiempo (ms)"); axs[1].set_ylabel("potencia (dB, bloques de 1 µs)"); axs[1].set_title("Ráfagas sobre el suelo de ruido")
    save_tight(fig); title(fig, "WiFi: así es la señal", c)
    footer(fig, "Señal: grabación IQ real (sigidwiki «Wi-Fi», 20 MS/s, 2437 MHz): balizas 802.11b DSSS a 1 Mb/s. El detector de energía por bloques de 1 µs es el de wifi.c; el dibujo, Python de ilustración. OFDM sin validar con aire.", c)
    return fig


@fig("dvbt-senal")
def dvbt(t, c):
    iq = load_raw(os.path.join(TV, "c32_frag.cs8"), "cs8")
    fs = 64e6 / 7
    fig, axs = fig_axes(1, 3, h=3.9, gridspec_kw=dict(width_ratios=[1.1, 1.1, 1]))
    f, p = psd(iq[: 1 << 20], fs, 4096)
    axs[0].plot(f / 1e6, p, color=c["c"][2], lw=0.9)
    axs[0].set_xlabel("MHz sobre 562 MHz"); axs[0].set_ylabel("dB"); axs[0].set_title("Espectro del C32")
    N = 8192
    L = N // 32
    xs = iq[: 400000]
    pr = xs[:-N] * np.conj(xs[N:])
    cs = np.convolve(pr, np.ones(N // 8), "valid")
    en = np.convolve(np.abs(xs[:-N]) ** 2, np.ones(N // 8), "valid")
    rho = np.abs(cs) / (en + 1e-9)
    Ts = N + N // 4
    k = len(rho) // Ts
    fold = rho[: k * Ts].reshape(k, Ts).mean(0)
    axs[1].plot(np.arange(Ts), fold, color=c["c"][0], lw=1)
    axs[1].set_xlabel("posición dentro del símbolo (muestras)"); axs[1].set_ylabel("ρ(i) plegada")
    axs[1].set_title("Prefijo cíclico (guarda 1/4)")
    axs[1].text(Ts * 0.5, fold.max() * 0.93, f"pico/media = {fold.max() / fold.mean():.1f}", ha="center", fontsize=9, color=c["c"][4])
    rng = np.random.default_rng(5)
    q = rng.integers(0, 8, 3000) * 2 - 7, rng.integers(0, 8, 3000) * 2 - 7
    s = (q[0] + 1j * q[1]) / np.sqrt(42)
    for snr, col, lab in ((16, c["c"][3], "16 dB"), (-9, c["c"][1], "−9 dB")):
        sig = 10 ** (-snr / 20)
        r = s + sig * (rng.standard_normal(3000) + 1j * rng.standard_normal(3000)) / np.sqrt(2)
        if snr > 0:
            axs[2].scatter(r.real, r.imag, s=3, color=col, alpha=0.5, linewidths=0, label=f"64-QAM a {lab}")
        else:
            axs[2].scatter(r.real, r.imag, s=3, color=col, alpha=0.25, linewidths=0, label=f"64-QAM a {lab}", zorder=0)
    axs[2].set_xlim(-6, 6); axs[2].set_ylim(-6, 6); axs[2].set_aspect("equal"); axs[2].legend(fontsize=7.5, loc="upper right", markerscale=3)
    axs[2].set_title("64-QAM simulada"); axs[2].set_xlabel("I"); axs[2].set_ylabel("Q")
    save_tight(fig, bottom=0.3); title(fig, "DVB-T: la señal real del C32 de Pamplona", c)
    footer(fig, "Izq. y centro: grabación IQ real (bladeRF, C32 562 MHz, ganancia 50, 64/7 MS/s; fragmento de 0,25 s). Derecha: constelación 64-QAM SIMULADA (no es la señal): con la SNR de ≈ −9 dB medida en banda (ρ_CP = 0,09) los puntos se funden en una nube; 64-QAM pide ≈ 16 dB. Por eso se decodifican TPS (BPSK diferencial muy robusta) pero no el TS. Python de ilustración.", c)
    return fig


def frames():
    frame_render("lora-estructura", "LoRa: estructura del paquete (modulación CSS)", [
        ("Paquete en el aire", [("preámbulo", 16, "8 upchirps"), ("sync", 4, "2 símbolos"), ("SFD", 5, "2,25 downchirps"), ("cabecera", 8, "explícita, CR 4/8"), ("carga", 40, "SF símbolos por bloque, CR 4/5…4/8"), ("CRC", 6, "16 bits")]),
        ("Cabecera explícita (5 nibbles)", [("longitud", 8, "8 bits"), ("CR", 3, ""), ("CRC activo", 1, ""), ("checksum", 5, "5 bits")]),
    ], "Estructura según docs/protocolos/lora.md. Ilustración dibujada.", rh=1.2)
    frame_render("meshtastic-estructura", "Meshtastic: paquete sobre LoRa (cabecera 16 B en claro + protobuf cifrado)", [
        ("Cabecera y carga", [("to", 4, "4 B LE"), ("from", 4, "4 B LE"), ("id", 4, "4 B LE"), ("flags", 1, "hop, ack"), ("canal", 1, "hash"), ("next_hop", 1, ""), ("relay", 1, ""), ("Data (protobuf)", 20, "AES-128-CTR; portnum, payload")]),
        ("Nonce de AES-CTR (16 B)", [("id (u64 LE)", 8, "8 B"), ("from (u32 LE)", 4, "4 B"), ("ceros", 4, "contador BE")]),
    ], "Estructura según docs/protocolos/meshtastic.md. Solo se descifra con la clave pública por defecto. Ilustración dibujada.", rh=1.2)
    frame_render("dvbt-estructura", "DVB-T: símbolo OFDM 8k y trama TPS", [
        ("Símbolo OFDM 8k (guarda 1/4)", [("prefijo cíclico", 2, "2048 muestras"), ("parte útil", 8, "8192 muestras = FFT; 6817 portadoras activas")]),
        ("Bits TPS (68 por trama, 1 por símbolo, DBPSK)", [("sync", 16, "16 bits"), ("long.", 6, "23/31"), ("trama", 2, ""), ("QAM", 2, "4/16/64"), ("jer.", 3, ""), ("HP", 3, "tasa"), ("LP", 3, "tasa"), ("guarda", 2, ""), ("modo", 2, "2k/8k"), ("cell id", 8, ""), ("reservado", 14, ""), ("BCH", 14, "")]),
    ], "Estructura según ETSI EN 300 744 y docs/protocolos/dvbt.md. Ilustración dibujada.", rh=1.3)
    frame_render("tdt-estructura", "TDT: paquete de transporte MPEG-TS (188 bytes)", [
        ("Paquete TS", [("sync", 2, "0x47"), ("flags + PID", 5, "TEI, PUSI, 13 bits"), ("cont. + AF", 5, "contador 4 bits"), ("carga útil", 40, "sección PSI o PES (184 B máx.); campo de adaptación con PCR opcional")]),
        ("Pila de tablas y flujos", [("PAT", 6, "PID 0"), ("PMT", 8, "pistas del servicio"), ("SDT", 8, "nombres"), ("NIT", 8, "LCN"), ("PES vídeo", 24, "H.264, PTS/DTS"), ("PES audio", 14, "MP2")]),
    ], "Estructura según ISO/IEC 13818-1 y docs/protocolos/tdt.md. Ilustración dibujada.", rh=1.3)


if __name__ == "__main__":
    run(FIGS, sys.argv[1:])
    if not sys.argv[1:] or "estructura" in sys.argv[1:]:
        frames()
