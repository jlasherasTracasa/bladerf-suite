#!/usr/bin/env python3
"""Inyector del piloto: suma copias de una señal limpia (--gen) a un fondo.

Subórdenes:
  gen     genera M variantes limpias (una por idx) con bladerf-lab --gen.
  inject  construye S ficheros SigMF («shards») con N copias por nivel de SNR.

DEFINICIÓN DE SNR (la misma en todo el piloto):
    SNR = P_señal / P_ruido, ambas en dB, medidas DENTRO del ancho ocupado B
    del protocolo (el que imprime --gen), centrado en la frecuencia del canal:
      * P_señal = potencia media de la ráfaga activa (la copia sin silencios)
        multiplicada por la fracción de esa potencia que cae en B (periodograma
        de Welch de la variante 0; 1 si B = tasa de muestreo).
      * P_ruido = potencia del fondo integrada en esa misma banda B (Welch,
        ventana de Hann, segmentos de 2048 muestras al 50 %). En ruido blanco
        vale sigma^2 * B/fs. En fondo real se mide en la grabación y se escala
        el fondo para que P_ruido sea IGUAL a la del caso blanco: así la
        diferencia entre curvas se debe a la estructura del fondo y no al nivel.
Cada copia lleva SNR fijo; se mezclan todos los niveles en cada fichero con el
ruido a nivel constante (se varía la amplitud de la señal).
"""
import argparse, json, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from comun import PROTOS, REAL_BG

SIGMA_RMS = 60.0      # potencia compleja total del ruido: E|n|^2 = SIGMA_RMS^2 (LSB Q11)
LEAD, TRAIL = 0.3, 0.6  # s de fondo antes de la 1.ª copia y tras la última
PAD_S = 0.002         # silencio (s) alrededor de cada copia dentro del recorte


def read_ci16(prefix):
    d = np.fromfile(prefix + ".sigmf-data", np.int16).astype(np.float32)
    return d[0::2] + 1j * d[1::2]


def cmd_gen(a):
    P = PROTOS[a.proto]
    os.makedirs(a.outdir, exist_ok=True)

    def one(i):
        out = os.path.join(a.outdir, "v%05d" % i)
        r = subprocess.run([a.bin, "--gen", a.proto, "--gen-params", json.dumps(P["params"](i)),
                            "--gen-freq", str(P["freq"]), "--gen-center", str(P["center"]),
                            "--gen-rate", str(P["rate"]), "--out", out, "--config", "none"],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("fallo gen %d: %s" % (i, r.stderr[-300:]))
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(one, range(a.n)))


def band_power(x, fs, f0, bw, nseg=2048):
    """Potencia media de x dentro de [f0-bw/2, f0+bw/2] (Welch, Hann, 50 %)."""
    nseg = min(nseg, len(x))
    w = np.hanning(nseg); k = np.sum(w ** 2)
    f = np.fft.fftfreq(nseg, 1 / fs)
    sel = np.abs(f - f0) <= bw / 2
    acc, cnt = 0.0, 0
    for s in range(0, len(x) - nseg + 1, max(1, nseg // 2)):
        X = np.fft.fft(x[s:s + nseg] * w)
        acc += np.sum(np.abs(X[sel]) ** 2) / (nseg * k); cnt += 1
    return acc / max(cnt, 1)


def fft_resample(x, fs_in, fs_out):
    n = len(x); m = int(round(n * fs_out / fs_in))
    X = np.fft.fft(x); Y = np.zeros(m, complex)
    h = min(n, m) // 2
    Y[:h] = X[:h]; Y[-h:] = X[-h:]
    return np.fft.ifft(Y) * (m / n)


def load_real_bg(name, root, fs):
    meta = REAL_BG[name]
    raw = np.fromfile(os.path.join(root, "tests/data/real", meta["file"] + ".sigmf-data"),
                      np.uint8 if meta["dtype"] == "cu8" else np.int8).astype(np.float64)
    if meta["dtype"] == "cu8":
        raw -= 127.5
    x = raw[0::2] + 1j * raw[1::2]
    meta_j = json.load(open(os.path.join(root, "tests/data/real", meta["file"] + ".sigmf-meta")))
    fs_in = meta_j["global"]["core:sample_rate"]
    if abs(fs_in - fs) > 1:
        x = fft_resample(x, fs_in, fs)
    return x - x.mean()


def load_variants(a, P):
    fs = P["rate"]; v = []
    for i in range(a.nvar):
        x = read_ci16(os.path.join(a.vardir, "v%05d" % i))
        if "trim" in P:
            lo, hi = P["trim"]
        else:
            m = np.abs(x); idx = np.nonzero(m > 0.02 * m.max())[0]; lo, hi = idx[0], idx[-1] + 1
        pad = int(PAD_S * fs)
        v.append((x[lo:hi], pad))
    return v


def main_inject(a):
    P = PROTOS[a.proto]; fs = P["rate"]; off = P["freq"] - P["center"]; B = P["bw"]
    rng = np.random.default_rng(a.seed)
    root = a.root
    if a.noise_only:        # solo fondo (falsos positivos): sin copias
        a.snr = "0:0:1"; a.n = 0; a.shards = 1
    lo, hi, st = [float(t) for t in a.snr.split(":")]
    levels = list(np.arange(lo, hi + 1e-9, st))
    ncopy = len(levels) * a.n
    a.nvar = ncopy
    var = load_variants(a, P) if ncopy else []
    # --- potencia de señal en banda -----------------------------------
    if ncopy:
        x0 = var[0][0]
        ptot0 = np.mean(np.abs(x0) ** 2)
        if B >= 0.99 * fs:
            phi = 1.0
        else:
            phi = min(1.0, band_power(x0, fs, off, B, 1024) / ptot0)
        psig = [np.mean(np.abs(v[0]) ** 2) * phi for v in var]
    else:
        phi, psig = 1.0, []
    # --- fondo --------------------------------------------------------
    pn_target = SIGMA_RMS ** 2 * B / fs
    bg = None; gscale = 1.0
    if a.bg == "real":
        bg = load_real_bg(P["real_bg"], root, fs)
        pn_raw = band_power(bg, fs, off, B)
        gscale = np.sqrt(pn_target / pn_raw)
        bg = bg * gscale
    # --- calendario ---------------------------------------------------
    jobs = [(lv, k) for lv in levels for k in range(a.n)]
    order = rng.permutation(len(jobs))
    shards = [[] for _ in range(a.shards)]
    for j, o in enumerate(order):
        shards[j % a.shards].append((o, jobs[o]))   # o = idx de variante (único)
    truth = dict(proto=a.proto, bg=a.bg, fs=fs, center=P["center"], freq=P["freq"], bw_hz=B,
                 sigma_rms=SIGMA_RMS, phi_inband=phi, noise_inband_power=pn_target,
                 bg_gain=gscale, real_bg_file=P["real_bg"] if a.bg == "real" else None,
                 levels=levels, n_per_level=a.n, seed=a.seed, cfo_max_hz=P["cfo"],
                 shards=[], copies=[])
    clip = 0
    for s, sh in enumerate(shards):
        # posiciones
        pos = int(LEAD * fs); placed = []
        for idx, (lv, k) in sh:
            x, pad = var[idx]
            gap = int((P["gap"][0] + rng.random() * P["gap"][1]) * fs)
            start = pos + gap; end = start + len(x)
            placed.append((idx, lv, start, end, pad))
            pos = end
        total = pos + int(TRAIL * fs)
        if a.noise_only:
            total = int(a.noise_only * fs)
        # fondo
        if a.bg == "white":
            y = (rng.standard_normal(total) + 1j * rng.standard_normal(total)) * (SIGMA_RMS / np.sqrt(2))
        else:
            L = len(bg); parts = []; n = 0
            while n < total:
                parts.append(np.roll(bg, int(rng.integers(L)))); n += L
            y = np.concatenate(parts)[:total].copy()
        for idx, lv, start, end, pad in placed:
            x = var[idx][0]
            amp = np.sqrt(10 ** (lv / 10) * pn_target / psig[idx])
            t = np.arange(len(x)) + start
            ph = rng.uniform(0, 2 * np.pi); cfo = rng.uniform(-1, 1) * P["cfo"]
            y[start:end] += amp * x * np.exp(1j * (ph + 2 * np.pi * cfo * t / fs))
            truth["copies"].append(dict(id=int(idx), shard=s, snr_db=float(lv), start=int(start),
                                        end=int(end), cfo_hz=float(cfo)))
        iq = np.empty(2 * total, np.int16)
        r = np.round(y.real); q = np.round(y.imag)
        clip += int(np.sum((np.abs(r) > 2047) | (np.abs(q) > 2047)))
        iq[0::2] = np.clip(r, -2048, 2047); iq[1::2] = np.clip(q, -2048, 2047)
        pre = "%s_s%d" % (a.out, s)
        iq.tofile(pre + ".sigmf-data")
        meta = {"global": {"core:datatype": "ci16_le", "core:sample_rate": fs, "core:version": "1.0.0",
                           "core:description": "piloto de inyeccion %s/%s" % (a.proto, a.bg),
                           "bladerf_lab:full_scale": 2048, "bladerf_lab:dropped_samples": 0},
                "captures": [{"core:sample_start": 0, "core:frequency": P["center"]}], "annotations": []}
        json.dump(meta, open(pre + ".sigmf-meta", "w"), indent=1)
        truth["shards"].append(dict(prefix=os.path.basename(pre), samples=int(total), seconds=total / fs))
    truth["clipped_samples"] = clip
    json.dump(truth, open(a.out + ".truth.json", "w"))
    print("%s/%s: %d copias, %d shards, %.1f s max, clip=%d, phi=%.3f, bg_gain=%.3g" % (
        a.proto, a.bg, ncopy, a.shards, max(s["seconds"] for s in truth["shards"]), clip, phi, gscale))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("gen"); g.add_argument("--proto", required=True); g.add_argument("--n", type=int, required=True)
    g.add_argument("--outdir", required=True); g.add_argument("--bin", required=True)
    j = sp.add_parser("inject"); j.add_argument("--proto", required=True); j.add_argument("--bg", choices=["white", "real"], required=True)
    j.add_argument("--vardir", required=True); j.add_argument("--snr", default="0:0:1", help="lo:hi:paso (dB)")
    j.add_argument("--n", type=int, default=100); j.add_argument("--shards", type=int, default=10)
    j.add_argument("--seed", type=int, default=1); j.add_argument("--out", required=True)
    j.add_argument("--noise-only", type=float, default=0, help="segundos de fondo sin copias")
    j.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
    a = ap.parse_args()
    if a.cmd == "gen": cmd_gen(a)
    else: main_inject(a)
