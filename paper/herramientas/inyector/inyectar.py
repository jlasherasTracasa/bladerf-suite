#!/usr/bin/env python3
"""Inyector de señales sintéticas en una captura SigMF (verdad de terreno incluida).

Qué hace
  1. Lee un fondo: captura SigMF real (ci16_le, ci8, cu8, cf32_le) por ventanas
     (np.memmap: admite fondos de varios GB) o ruido blanco reproducible.
  2. Para cada copia pedida genera con `bladerf-lab --gen` una variante limpia con
     un ID único en el mensaje (protos.py), centrada en banda base a la tasa del fondo.
  3. La recorta a su ráfaga activa, la desplaza a su frecuencia (+CFO), le da fase
     aleatoria y la escala para lograr la SNR sorteada, y la suma al fondo.
  4. Escribe un SigMF ci16_le con una anotación por copia (verdad de terreno).

Definición de SNR (local, por copia)
    SNR = P_señal / P_ruido, en dB, donde
      P_señal = potencia media de la ráfaga activa (sin silencios) × fracción de
                esa potencia que cae en el ancho ocupado B del protocolo;
      P_ruido = potencia del FONDO (antes de inyectar nada) integrada en B, centrada
                en la frecuencia de ESA copia y medida SOLO en la ventana de tiempo de
                esa copia (Welch, Hann, segmentos de 2048 muestras al 50 %; si la
                ventana es más corta de 4096 muestras se amplía simétricamente hasta
                4096 para tener resolución espectral, y se anota en `inj:noise_win`).
    B es el «ancho ocupado» que imprime --gen. Así un fondo no estacionario (ráfagas
    ISM, otra señal) da la SNR que de verdad ve el decodificador en ese instante y
    esa frecuencia, no la media global.

Unidades de salida: ci16_le con 2048 = fondo de escala (convención de bladerf-lab).
  ci8/cu8 se multiplican por 16; cf32 (±1 = f.e.) por 2048. Se avisa del recorte.

Ejemplo
  inyectar.py --bg captura.sigmf-meta --proto ais:20 --proto pocsag:20 \
      --snr 5:20 --seconds 30 --cfo 1000 --out /tmp/inj --seed 1
  inyectar.py --white 250000,162000000,60 --seconds 10 --proto ais:20 --snr 20 --out /tmp/w
"""
import argparse, json, os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from protos import PROTOS

VERSION = "1.0"
CHUNK = 1 << 20        # muestras por trozo de fondo/salida
MIN_WIN = 4096         # ventana mínima para medir el ruido local
PAD_S = 0.002          # silencio (s) que se conserva alrededor de la ráfaga


# ---------------------------------------------------------------- fondos ----
class FondoSigMF:
    """Fondo en disco con acceso aleatorio por ventanas, convertido a LSB de ci16."""
    def __init__(self, meta_path):
        self.meta = json.load(open(meta_path))
        g = self.meta["global"]
        self.fs = float(g["core:sample_rate"])
        cap = (self.meta.get("captures") or [{}])[0]
        self.center = float(cap.get("core:frequency", 0.0))
        dt = g["core:datatype"]
        spec = {"ci16_le": (np.int16, 1.0, 0.0), "ci8": (np.int8, 16.0, 0.0),
                "cu8": (np.uint8, 16.0, 127.5), "cf32_le": (np.float32, 2048.0, 0.0)}
        if dt not in spec:
            raise SystemExit("datatype no soportado: %s" % dt)
        self.dtype, self.scale, self.off = spec[dt]
        self.dt = dt
        data = re.sub(r"\.sigmf-meta$", ".sigmf-data", meta_path)
        self.mm = np.memmap(data, dtype=self.dtype, mode="r")
        self.n = len(self.mm) // 2
        self.desc = os.path.basename(meta_path)

    def read(self, a, b):
        b = min(b, self.n); a = max(a, 0)
        raw = np.asarray(self.mm[2 * a:2 * b], dtype=np.float64)
        raw = (raw - self.off) * self.scale
        return raw[0::2] + 1j * raw[1::2]


class FondoBlanco:
    """Ruido gaussiano complejo reproducible con acceso aleatorio (semilla por trozo)."""
    def __init__(self, fs, center, sigma, seconds, seed):
        self.fs, self.center, self.sigma = float(fs), float(center), float(sigma)
        self.n = int(round(seconds * fs)); self.seed = seed
        self.dt = "white"; self.desc = "blanco sigma=%g" % sigma

    def _chunk(self, k):
        r = np.random.default_rng([self.seed, 77, k])
        m = min(CHUNK, self.n - k * CHUNK)
        return (r.standard_normal(m) + 1j * r.standard_normal(m)) * (self.sigma / np.sqrt(2))

    def read(self, a, b):
        b = min(b, self.n); a = max(a, 0)
        out = [self._chunk(k) for k in range(a // CHUNK, (b - 1) // CHUNK + 1)]
        x = np.concatenate(out) if out else np.zeros(0, complex)
        o = (a // CHUNK) * CHUNK
        return x[a - o:b - o]


# --------------------------------------------------------------- medidas ----
def band_power(x, fs, f0, bw, nseg=2048):
    """Potencia media (por muestra compleja) de x en [f0-bw/2, f0+bw/2] (Welch, Hann, 50 %)."""
    nseg = min(nseg, len(x))
    w = np.hanning(nseg); k = np.sum(w ** 2)
    f = np.fft.fftfreq(nseg, 1 / fs)
    sel = np.abs(f - f0) <= max(bw, 2 * fs / nseg) / 2
    acc, cnt = 0.0, 0
    for s in range(0, len(x) - nseg + 1, max(1, nseg // 2)):
        X = np.fft.fft(x[s:s + nseg] * w)
        acc += np.sum(np.abs(X[sel]) ** 2) / (nseg * k); cnt += 1
    return acc / max(cnt, 1)


def shaped_power(x, fs, f0, bw, shape, nseg=2048):
    """Segunda definición de ruido (P3d): potencia de ruido equivalente TRAS el filtro de canal.
    shape = (f, h2): respuesta en potencia |H(f)|² del canal del decodificador (medida con el DDC real,
    ddc_resp.c), centrada en f0. La PSD local se pondera con |H(f − f0)|²; la densidad media ponderada
    se multiplica por el ancho de referencia bw (el de la SNR 1, el ocupado), de modo que sobre ruido
    blanco coincide con band_power() y, con un fondo no llano (espurios, portadoras), pesa lo que el
    decodificador ve de verdad dentro de su filtro y no un rectángulo de ancho bw."""
    nseg = min(nseg, len(x))
    w = np.hanning(nseg); k = np.sum(w ** 2)
    f = np.fft.fftfreq(nseg, 1 / fs)
    wt = np.interp(f - f0, shape[0], shape[1], left=0.0, right=0.0)
    acc, cnt = 0.0, 0
    for s in range(0, len(x) - nseg + 1, max(1, nseg // 2)):
        X = np.fft.fft(x[s:s + nseg] * w)
        acc += np.sum(wt * np.abs(X) ** 2) / (nseg * k) / max(np.sum(wt), 1e-30) * (bw * nseg / fs); cnt += 1
    return acc / max(cnt, 1)


def mod_offset(x, i0, thr=0.1):
    """Primer símbolo MODULADO: índice (relativo a i0, primera muestra activa) de la primera muestra
    que se aparta más de thr·max|x| de x[i0]. Una portadora sin modular delante (POCSAG: 50 ms)
    es señal activa pero no tiene símbolos; la convención de posición (P3d) empieza en el primero."""
    d = np.nonzero(np.abs(x[i0:] - x[i0]) > thr * np.abs(x).max())[0]
    return int(d[0]) if len(d) else 0


def local_noise(bg, start, end, f_off, bw, shape=None):
    """P_ruido del fondo en la ventana [start,end) y banda f_off±bw/2 (ver cabecera).
    Con shape=(f,h2) la banda se pondera con la respuesta del canal (shaped_power)."""
    n = end - start
    if n < MIN_WIN:
        extra = MIN_WIN - n
        start -= extra // 2; end = start + MIN_WIN
        if start < 0: start, end = 0, MIN_WIN
        if end > bg.n: start, end = bg.n - MIN_WIN, bg.n
    if shape is not None:
        return shaped_power(bg.read(start, end), bg.fs, f_off, bw, shape), (start, end)
    return band_power(bg.read(start, end), bg.fs, f_off, bw), (start, end)


# ------------------------------------------------------------- generación ----
def gen_copy(binpath, proto, params, fs, center, tmp, tag):
    """Genera una copia a 0 Hz (centro) a la tasa del fondo. Devuelve (iq, ancho_ocupado_Hz)."""
    out = os.path.join(tmp, tag)
    cmd = [binpath, "--gen", proto, "--gen-freq", str(int(center)), "--gen-center", str(int(center)),
           "--gen-rate", str(int(fs)), "--out", out, "--config", "none"]
    if params is not None:
        cmd += ["--gen-params", json.dumps(params)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("falló --gen %s: %s" % (proto, (r.stderr or r.stdout)[-300:]))
    m = re.search(r"ancho ocupado (\d+) Hz", r.stdout)
    bw = float(m.group(1)) if m else fs
    d = np.fromfile(out + ".sigmf-data", np.int16).astype(np.float64)
    for ext in (".sigmf-data", ".sigmf-meta"):
        os.unlink(out + ext)
    return d[0::2] + 1j * d[1::2], bw


def trim_active(x, fs):
    m = np.abs(x); idx = np.nonzero(m > 0.02 * m.max())[0]
    pad = int(PAD_S * fs)
    lo, hi = max(idx[0] - pad, 0), min(idx[-1] + 1 + pad, len(x))
    return x[lo:hi], pad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bg", help="captura de fondo (.sigmf-meta)")
    ap.add_argument("--white", metavar="FS,CENTRO,SIGMA", help="fondo blanco: tasa,centro Hz,sigma (LSB ci16)")
    ap.add_argument("--proto", action="append", required=True, metavar="ID:N[:FREQ_HZ]",
                    help="protocolo, nº de copias y (opcional) frecuencia fija; repetible")
    ap.add_argument("--params", action="append", default=[], metavar="ID=JSON",
                    help="--gen-params propios (sin ID automático): el emparejado será por tiempo/frecuencia")
    ap.add_argument("--snr", default="10:20", help="lo:hi dB, uniforme (o un valor)")
    ap.add_argument("--seconds", type=float, default=0, help="duración de la salida (0 = todo el fondo)")
    ap.add_argument("--time-dist", choices=["uniform", "poisson"], default="uniform",
                    help="uniform: instantes uniformes sin solape; poisson: llegadas con tasa media N/duración, sin solape")
    ap.add_argument("--freq-dist", choices=["uniform", "fixed"], default="uniform",
                    help="uniform: frecuencia uniforme en la banda; fixed: la de FREQ o el centro")
    ap.add_argument("--freq-grid", type=float, default=0, help="redondea las frecuencias a esta rejilla (Hz; 0 = no)")
    ap.add_argument("--dc-guard", type=float, default=0.02, help="fracción de fs alrededor del centro que se evita")
    ap.add_argument("--edge-guard", type=float, default=0.02, help="fracción de fs de margen en los bordes")
    ap.add_argument("--cfo", type=float, default=0, help="CFO máximo (Hz), uniforme ±")
    ap.add_argument("--allow-overlap", action="store_true", help="permite copias solapadas en el tiempo")
    ap.add_argument("--lead", type=float, default=0.3, help="s sin copias al principio y al final")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--bin", default=None, help="bladerf-lab (por defecto build/bladerf-lab del repositorio)")
    ap.add_argument("--out", required=True, help="prefijo del SigMF de salida")
    a = ap.parse_args()

    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
    binpath = a.bin or os.path.join(root, "build/bladerf-lab")
    rng = np.random.default_rng(a.seed)

    # --- fondo y duración -------------------------------------------------
    if a.white:
        fs, ce, sg = [float(t) for t in a.white.split(",")]
        if a.seconds <= 0: raise SystemExit("--white exige --seconds")
        bg = FondoBlanco(fs, ce, sg, a.seconds, a.seed)
    elif a.bg:
        bg = FondoSigMF(a.bg)
    else:
        raise SystemExit("indica --bg o --white")
    fs = bg.fs
    total = bg.n
    if a.seconds > 0:
        want = int(round(a.seconds * fs))
        if want > bg.n:
            print("AVISO: la captura dura %.2f s y se piden %.2f s; no se repite en bucle, "
                  "la salida dura %.2f s" % (bg.n / fs, a.seconds, bg.n / fs), file=sys.stderr)
        total = min(want, bg.n)
    lo_f, hi_f = bg.center - fs / 2, bg.center + fs / 2

    # --- lista de copias --------------------------------------------------
    custom = {}
    for s in a.params:
        k, v = s.split("=", 1); custom[k] = json.loads(v)
    jobs = []
    for spec in a.proto:
        t = spec.split(":")
        pid, n = t[0], int(t[1]); ffix = float(t[2]) if len(t) > 2 else None
        for _ in range(n):
            jobs.append(dict(proto=pid, freq_fixed=ffix))
    for j, jb in enumerate(jobs):
        jb["id"] = j
        jb["has_id"] = jb["proto"] in PROTOS and jb["proto"] not in custom
        jb["params"] = (PROTOS[jb["proto"]]["params"](j) if jb["has_id"] else custom.get(jb["proto"]))
    snr_lo, snr_hi = [float(t) for t in (a.snr.split(":") * 2)[:2]]

    tmp = tempfile.mkdtemp(prefix="inj_")
    def gen(jb):
        x, bw = gen_copy(binpath, jb["proto"], jb["params"], fs, bg.center, tmp, "c%d" % jb["id"])
        jb["x"], jb["pad"] = trim_active(x, fs); jb["bw"] = bw
    with ThreadPoolExecutor(min(8, os.cpu_count() or 2)) as ex:
        list(ex.map(gen, jobs))

    # --- instantes (sin solape salvo --allow-overlap) ----------------------
    lead = int(a.lead * fs); span = total - 2 * lead
    lens = [len(jb["x"]) for jb in jobs]
    if span < sum(lens):
        raise SystemExit("no caben %d copias (%.2f s de señal) en %.2f s útiles" % (
            len(jobs), sum(lens) / fs, span / fs))
    order = rng.permutation(len(jobs))
    if a.allow_overlap:
        for jb, L in zip(jobs, lens):
            jb["start"] = lead + int(rng.integers(0, span - L + 1))
    else:
        free = span - sum(lens)
        if a.time_dist == "uniform":       # estadísticos de orden: huecos uniformes
            cuts = np.sort(rng.integers(0, free + 1, len(jobs)))
        else:                              # Poisson: huecos exponenciales reescalados a 'free'
            g = rng.exponential(1.0, len(jobs) + 1)
            cuts = np.cumsum(g)[:-1] / g.sum() * free
        pos = lead
        for rank, k in enumerate(order):
            gap = int(cuts[rank] - (cuts[rank - 1] if rank else 0))
            jobs[k]["start"] = pos + gap; pos = jobs[k]["start"] + lens[k]
    # --- frecuencia, CFO, SNR -------------------------------------------------
    for jb in jobs:
        bw = jb["bw"]
        lo = lo_f + bw / 2 + a.edge_guard * fs; hi = hi_f - bw / 2 - a.edge_guard * fs
        if jb["freq_fixed"] is not None:
            f = jb["freq_fixed"]
        elif a.freq_dist == "fixed":
            f = bg.center + 0.25 * fs
        else:
            if hi <= lo: raise SystemExit("la banda no admite %s (ancho %g Hz)" % (jb["proto"], bw))
            for _ in range(1000):
                f = float(rng.uniform(lo, hi))
                if a.freq_grid: f = round(f / a.freq_grid) * a.freq_grid
                if abs(f - bg.center) > bw / 2 + a.dc_guard * fs and lo <= f <= hi:
                    break
            else:
                raise SystemExit("no se halló frecuencia libre de la continua")
        jb["freq"] = f
        jb["cfo"] = float(rng.uniform(-1, 1) * a.cfo) if a.cfo else 0.0
        jb["snr"] = float(rng.uniform(snr_lo, snr_hi))
        jb["phase"] = float(rng.uniform(0, 2 * np.pi))
    # --- amplitud por SNR local -----------------------------------------------
    for jb in jobs:
        x, bw = jb["x"], jb["bw"]
        pt = np.mean(np.abs(x) ** 2)
        phi = 1.0 if bw >= 0.99 * fs else min(1.0, band_power(x, fs, 0.0, bw, 1024) / pt)
        psig = np.mean(np.abs(x[jb["pad"]:len(x) - jb["pad"]]) ** 2) * phi
        s0, s1 = jb["start"], jb["start"] + len(x)
        f_off = jb["freq"] + jb["cfo"] - bg.center
        pn, win = local_noise(bg, s0, s1, f_off, bw)
        jb["pn"], jb["noise_win"] = float(pn), list(win)
        jb["amp"] = float(np.sqrt(10 ** (jb["snr"] / 10) * pn / psig))

    # --- escritura por trozos --------------------------------------------------
    clip = 0
    with open(a.out + ".sigmf-data", "wb") as fo:
        for c0 in range(0, total, CHUNK):
            c1 = min(c0 + CHUNK, total)
            y = bg.read(c0, c1).copy()
            for jb in jobs:
                s0, s1 = jb["start"], jb["start"] + len(jb["x"])
                if s1 <= c0 or s0 >= c1: continue
                u0, u1 = max(s0, c0), min(s1, c1)
                t = np.arange(u0, u1) - bg.n * 0   # índice absoluto
                f_off = jb["freq"] + jb["cfo"] - bg.center
                ph = jb["phase"] + 2 * np.pi * f_off * (t - s0) / fs
                y[u0 - c0:u1 - c0] += jb["amp"] * jb["x"][u0 - s0:u1 - s0] * np.exp(1j * ph)
            r, q = np.round(y.real), np.round(y.imag)
            clip += int(np.sum((np.abs(r) > 2047) | (np.abs(q) > 2047)))
            iq = np.empty(2 * len(y), np.int16)
            iq[0::2] = np.clip(r, -2048, 2047); iq[1::2] = np.clip(q, -2048, 2047)
            fo.write(iq.tobytes())
    if clip: print("AVISO: %d muestras recortadas" % clip, file=sys.stderr)

    # --- meta y anotaciones -----------------------------------------------------
    ann = []
    for jb in sorted(jobs, key=lambda j: j["start"]):
        pad = jb["pad"]; fc = jb["freq"] + jb["cfo"]
        ann.append({"core:sample_start": int(jb["start"] + pad),
                    "core:sample_count": int(len(jb["x"]) - 2 * pad),
                    "core:freq_lower_edge": fc - jb["bw"] / 2, "core:freq_upper_edge": fc + jb["bw"] / 2,
                    "core:label": "%s#%d" % (jb["proto"], jb["id"]),
                    "inj:proto": jb["proto"], "inj:id": jb["id"], "inj:has_id": jb["has_id"],
                    "inj:snr_db": round(jb["snr"], 3), "inj:cfo_hz": round(jb["cfo"], 3),
                    "inj:freq_hz": jb["freq"], "inj:bw_hz": jb["bw"], "inj:seed": a.seed,
                    "inj:noise_power": jb["pn"], "inj:noise_win": jb["noise_win"]})
    meta = {"global": {"core:datatype": "ci16_le", "core:sample_rate": fs, "core:version": "1.0.0",
                       "core:description": "inyección sintética (inyectar.py %s)" % VERSION,
                       "bladerf_lab:full_scale": 2048, "bladerf_lab:dropped_samples": 0,
                       "inj:tool": "inyectar.py " + VERSION, "inj:seed": a.seed, "inj:bg": bg.desc,
                       "inj:bg_datatype": bg.dt, "inj:time_dist": a.time_dist,
                       "inj:snr_range_db": [snr_lo, snr_hi], "inj:clipped_samples": clip,
                       "inj:snr_definition": "local: P_señal en B / P_fondo en B, en la ventana y banda de cada copia"},
            "captures": [{"core:sample_start": 0, "core:frequency": bg.center}], "annotations": ann}
    json.dump(meta, open(a.out + ".sigmf-meta", "w"), indent=1)
    os.rmdir(tmp)
    print("%d copias en %.2f s -> %s.sigmf-meta (clip=%d)" % (len(jobs), total / fs, a.out, clip))


if __name__ == "__main__":
    main()
