"""gen.py — genera capturas con verdad completa y etiquetas observadas (piloto P6).

Cada captura (48 kS/s, ci16) = fondo (real remuestreado y reciclado, o blanco) + ráfagas inyectadas
de 5 protocolos con SNR local uniforme en [-5, 20] dB (definición de inyectar.local_noise).
La verdad (protocolo, inicio, fin, SNR) se conoce por construcción. Las etiquetas observadas salen de
`bladerf-lab --replay-fast --labels` con los cinco decodificadores en el centro: verified (evento con
CRC), unknown (detector OS-CFAR sin explicación), resto = empty implícito.
Guarda por captura un .npz con el espectrograma (frames de 128 muestras, promediados de dos en dos:
256 muestras = 5,33 ms, 128 bins), la verdad y las anotaciones observadas.
Uso: gen.py SPLIT SEMILLA N   (SPLIT: train|test|synth|cal)
"""
import os, sys, json, subprocess, glob, zlib, time
import numpy as np
RAIZ = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
os.environ.setdefault("BIN", os.path.join(RAIZ, "build/p6/bladerf-lab"))
os.environ["BGDIR"] = os.path.join(RAIZ, "build/p3/bg")
P6 = os.path.join(RAIZ, "build/p6")
os.environ["WORK"] = os.path.join(P6, "work")
sys.path.insert(0, os.path.join(RAIZ, "paper/exp/calibracion"))
import calib as C                       # noqa: E402
iny = C.iny
BIN = os.environ["BIN"]
FS = 48000.0; CENTER = 433.92e6
PROTOS = ["ais", "aprs", "pocsag", "acars", "flex"]
SNR_LO, SNR_HI = -5.0, 20.0
T_TOTAL, T_WARM = 48.0, 3.5
NID = 24                                  # mensajes distintos por protocolo con ID
REAL = os.path.join(RAIZ, "tests/data/real")
# Fondos reales a 48 kS/s: SOLO de protocolos que NO inyectamos (rs41 + ISM de un RTL), para que la
# verdad no tenga positivos ocultos. Particion por fichero: train / test sin compartir fuente.
BG_TRAIN = [os.path.join(P6.replace("build/p6", "build/p3"), "bg", f + "_48k.sigmf-meta") for f in
            ("ism_acurite592txr", "ism_f007th", "ism_oregon_thgr122n", "ism_oregon_thn132n", "ism_tx141bv2", "ism_wh1080")] \
           + [os.path.join(REAL, "rs41.sigmf-meta")]
BG_TEST = [os.path.join(RAIZ, "build/p3/bg", f + "_48k.sigmf-meta") for f in ("ism_nexus", "ism_tx141bv3")]
BG_TRAIN = [p.replace("build/p6/../", "") for p in BG_TRAIN]


class BgMezcla:
    """Fondo de n muestras: real reciclado (desplazado en frecuencia, conjugado/invertido, escalado) o blanco."""
    def __init__(self, rng, n, files, real):
        self.fs, self.center, self.n, self.w, self.dt, self.desc = FS, CENTER, n, 0, "mix", ""
        if not real:
            sig = float(rng.uniform(40, 110)); self.y = iny.FondoBlanco(FS, CENTER, sig, n / FS, int(rng.integers(1 << 30))).read(0, n)
            self.desc = "white%.0f" % sig; return
        parts, tot = [], 0
        while tot < n:
            b = iny.FondoSigMF(files[int(rng.integers(len(files)))]); x = b.read(0, b.n)
            if rng.random() < .5: x = x[::-1]
            if rng.random() < .5: x = np.conj(x)
            f = rng.uniform(-6000, 6000); x = x * np.exp(2j * np.pi * f * np.arange(len(x)) / FS)
            parts.append(x * rng.uniform(.7, 1.5)); tot += len(x)
        self.y = np.concatenate(parts)[:n]; self.desc = "real"

    def read(self, a, b): return self.y[max(a, 0):min(b, self.n)]


def spectrogram(y):
    """|STFT|^2 dB: ventana Hann 128, salto 128; promedio de pares -> 5,33 ms/frame; 128 bins centrados."""
    n = len(y) // 256 * 256
    f = y[:n].reshape(-1, 128) * np.hanning(128)
    P = np.abs(np.fft.fftshift(np.fft.fft(f, axis=1), axes=1)) ** 2
    P = P.reshape(-1, 2, 128).mean(1)
    return (10 * np.log10(P + 1e-3)).astype(np.float16)


def capture(split, seed, idx, wd):
    rng = np.random.default_rng([seed, zlib.crc32(split.encode()), idx])
    n = int(T_TOTAL * FS)
    real = split in ("train", "test", "cal") and rng.random() < .6
    bg = BgMezcla(rng, n, BG_TEST if split == "test" else BG_TRAIN, real)
    # lista de ráfagas: k de cada protocolo, orden aleatorio, sin solape
    kper = 5
    seq = [p for p in PROTOS for _ in range(kper)]; rng.shuffle(seq)
    copies = []
    for p in seq:
        i = int(rng.integers(NID)) if C.CFG[p]["kind"] == "id" else 0
        x, pad, bw = C.gen_copy(p, i, FS, CENTER)
        copies.append((p, x, pad, bw))
    lens = [len(c[1]) for c in copies]
    while True:
        st = C.plan_pass(rng, lens, (0.3, 0.6), FS, n, int(T_WARM * FS))
        if st is not None: break
        copies.pop(); lens.pop()
    y = bg.read(0, n).astype(np.complex128).copy()
    truth = []
    for (p, x, pad, bw), s0 in zip(copies, st):
        snr = float(rng.uniform(SNR_LO, SNR_HI)); cfo = float(rng.uniform(-1, 1) * 1000)
        pt = np.mean(np.abs(x) ** 2)
        phi = 1.0 if bw >= .99 * FS else min(1.0, iny.band_power(x, FS, 0.0, bw, 1024) / pt)
        psig = np.mean(np.abs(x[pad[0]:len(x) - pad[1]]) ** 2) * phi
        pn, _ = iny.local_noise(bg, s0, s0 + len(x), cfo, bw)
        amp = float(np.sqrt(10 ** (snr / 10) * pn / psig))
        t = np.arange(len(x))
        y[s0:s0 + len(x)] += amp * x * np.exp(1j * (rng.uniform(0, 6.28) + 2 * np.pi * cfo * t / FS))
        truth.append((PROTOS.index(p), s0 + pad[0], s0 + len(x) - pad[1], snr))
    pk = max(np.abs(y.real).max(), np.abs(y.imag).max())
    if pk > 2040: y *= 2040 / pk
    base = os.path.join(wd, "cap_%d_%s_%d" % (seed, split, idx))
    C.write_ci16(base, y, FS, CENTER, "p6")
    obs = []
    if split != "synth":
        lab = base + "_lab.sigmf-meta"
        cmd = [BIN, "--replay", base + ".sigmf-meta", "--replay-fast", "--labels", lab, "--config", "none", "--no-gpu",
               "--port", str(C.free_port(idx + seed * 1000)), "--print"]
        for p in PROTOS: cmd += ["--decode", "%s@%d" % (p, int(CENTER))]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        if r.returncode: raise RuntimeError(r.stderr[-400:])
        for a in json.load(open(lab))["annotations"]:
            lv = a.get("lab:level")
            fl, fu = a.get("core:freq_lower_edge", 0), a.get("core:freq_upper_edge", 0)
            obs.append((0 if lv == "verified" else 1 if lv == "unknown" else 2,
                        PROTOS.index(a["lab:proto"]) if lv == "verified" and a.get("lab:proto") in PROTOS else -1,
                        int(a["core:sample_start"]), int(a["core:sample_start"] + a["core:sample_count"]),
                        float(a.get("lab:snr_db", np.nan)), fl, fu))
        os.unlink(lab)
    for e in (".sigmf-data", ".sigmf-meta"): os.unlink(base + e)
    return spectrogram(y), np.array(truth, float).reshape(-1, 4), np.array(obs, float).reshape(-1, 7), bg.desc


if __name__ == "__main__":
    split, seed, N = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    out = os.path.join(P6, "data", "s%d_%s" % (seed, split)); os.makedirs(out, exist_ok=True)
    wd = os.path.join(P6, "work", "w_%s_%d" % (split, seed)); os.makedirs(wd, exist_ok=True)
    for i in range(N):
        f = os.path.join(out, "%03d.npz" % i)
        if os.path.exists(f): continue
        t0 = time.time(); S, T, O, d = capture(split, seed, i, wd)
        np.savez(f, S=S, truth=T, obs=O, bg=d)
        print(split, seed, i, d, "verdad", len(T), "verified", int((O[:, 0] == 0).sum()) if len(O) else 0,
              "unknown", int((O[:, 0] == 1).sum()) if len(O) else 0, "%.0fs" % (time.time() - t0), flush=True)
