#!/usr/bin/env python3
"""Calibración por inyección (P3): mide P(etiquetado | SNR local) y errores de posición,
frecuencia y SNR con la verdad de terreno del inyector. Solo numpy; no toca src/.

Subórdenes
  prep                      prepara los fondos reales remuestreados (build/p3/bg/)
  run  --proto P --bg B     una serie (protocolo, fondo, semilla, condición) -> datos/*.csv
  fp   --fam F              falsos positivos sobre ruido blanco y sobre fondos reales sin inyectar

Método (resumen; el detalle está en CALIBRACION.md)
  * Cada «pasada» es un fichero SigMF = fondo (ruido blanco gaussiano o grabación real,
    SIN bucle) + k copias de `bladerf-lab --gen` con un identificador único en el mensaje
    (ais, aprs, pocsag, lora, adsb) o sin él (acars, rs41, flex, dsc, ism: emparejado por
    tiempo con la marca `sample` del evento).
  * SNR LOCAL: SNR = P_señal(B) / P_fondo(B), con P_fondo medida con Welch en la ventana de
    tiempo y la banda de ESA copia (inyectar.local_noise). Un fondo no estacionario ya no
    desplaza la curva: lo que se mide es la SNR que el decodificador ve de verdad.
  * Se reproduce con `--replay-fast` (sin bucle, sin descartar bloques) y se empareja.
"""
import argparse, csv, glob, json, math, os, re, subprocess, sys, time, zlib
import numpy as np

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.abspath(os.path.join(AQUI, "../../.."))
sys.path.insert(0, os.path.join(RAIZ, "paper/herramientas/inyector"))
import inyectar as iny                                   # noqa: E402
from protos import PROTOS as IDP                         # noqa: E402

BIN = os.environ.get("BIN", os.path.join(RAIZ, "build/p3/bladerf-lab"))
BGDIR = os.environ.get("BGDIR", os.path.join(RAIZ, "build/p3/bg"))
WORK = os.environ.get("WORK", os.path.join(RAIZ, "build/p3/work"))
DATOS = os.path.join(AQUI, "datos")
REAL = os.path.join(RAIZ, "tests/data/real")
SIGMA = 60.0           # ruido blanco: LSB Q11 por muestra compleja (igual que el piloto)

F48_REC = ["acars", "ais", "aprs", "flex", "pocsag512", "pocsag1200", "pocsag2400", "rs41"]
ISM = ["ism_acurite592txr", "ism_f007th", "ism_nexus", "ism_oregon_thgr122n", "ism_oregon_thn132n",
       "ism_tx141bv2", "ism_tx141bv3", "ism_wh1080"]
OWN = {"ais": ["ais"], "aprs": ["aprs"], "pocsag": ["pocsag512", "pocsag1200", "pocsag2400"],
       "acars": ["acars"], "flex": ["flex"], "rs41": ["rs41"], "dsc": []}

# fs: tasa de la serie principal; occ_fs/occ_df: tasa y separación del canal adyacente en la
# serie de ocupación; sym: símbolos/s (para expresar el error de posición); gap: huecos (s)
# entre copias; step/n: paso de SNR (dB) y copias por punto y semilla; cfo: ± Hz.
CFG = {
    "ais":    dict(kind="id", fs=48000, occ_fs=250000, occ_df=25000, sym=9600.0, gap=(0.06, 0.12), step=1, n=34, cfo=1000),
    "aprs":   dict(kind="id", fs=48000, occ_fs=250000, occ_df=25000, sym=1200.0, gap=(0.08, 0.12), step=1, n=34, cfo=1000),
    "pocsag": dict(kind="id", fs=48000, occ_fs=250000, occ_df=25000, sym=2400.0, gap=(0.08, 0.12), step=1, n=34, cfo=1000),
    "lora":   dict(kind="id", fs=500000, occ_fs=1000000, occ_df=250000, sym=125000 / 128.0, gap=(0.10, 0.20), step=1, n=34,
                   cfo=5000, bw=125000, warm=6.0),
    "adsb":   dict(kind="id", fs=2000000, occ_fs=2000000, occ_df=None, sym=1e6, gap=(0.003, 0.01), step=1, n=34, cfo=0,
                   trim=240),
    "acars":  dict(kind="time", fs=48000, sym=2400.0, gap=(0.6, 1.0), step=2, n=10, cfo=1000),
    "rs41":   dict(kind="time", fs=48000, sym=4800.0, gap=(0.6, 1.0), step=2, n=10, cfo=1000),
    "flex":   dict(kind="time", fs=48000, sym=1600.0, gap=(0.6, 1.0), step=2, n=10, cfo=1000),
    "dsc":    dict(kind="time", fs=48000, sym=1200.0, gap=(0.6, 1.0), step=2, n=10, cfo=1000),
    "ism":    dict(kind="time", fs=250000, sym=10000.0, gap=(0.3, 0.6), step=2, n=10, cfo=0, bw=50000,
                   params={"model": "Nexus-TH", "id": 67, "channel": 2, "temp": 21.4, "hum": 56, "repeat": 3},
                   pfn=lambda i: {"model": "Nexus-TH", "id": 1 + i % 250, "channel": 1 + (i // 250) % 3,
                                  "temp": 5 + (i % 97) * 0.3, "hum": 30 + i % 60, "repeat": 3}),
}
SNR_LO, SNR_HI = -10, 25
WARM = 3.0            # s de calentamiento sin copias al principio de cada pasada
LEAD = 0.01            # s sin copias al principio y al final de la pasada


# ------------------------------------------------------------------ fondos --
class Pool:
    """Concatenación (sin bucle) de grabaciones de la misma tasa. read(a,b) -> cf64 en LSB Q11."""
    def __init__(self, metas):
        self.parts = [iny.FondoSigMF(m) for m in metas]
        self.fs = self.parts[0].fs
        assert all(abs(p.fs - self.fs) < 1e-6 for p in self.parts), "tasas distintas en el pool"
        self.center = self.parts[0].center
        self.offs = np.cumsum([0] + [p.n for p in self.parts]); self.n = int(self.offs[-1])
        self.dt = "pool"; self.desc = "+".join(p.desc for p in self.parts)

    def read(self, a, b):
        a = max(a, 0); b = min(b, self.n); out = []
        for k, p in enumerate(self.parts):
            lo, hi = max(a, self.offs[k]), min(b, self.offs[k + 1])
            if hi > lo: out.append(p.read(lo - self.offs[k], hi - self.offs[k]))
        return np.concatenate(out) if out else np.zeros(0, complex)

    def file_at(self, s):
        return self.parts[int(np.searchsorted(self.offs, s, side="right") - 1)].desc


class ConLead:
    """Calentamiento sin copias al principio de cada pasada (`w` muestras). El decodificador de LoRa
    (y quizá otros) tarda ~2-3 s en decodificar de forma estable tras arrancar: con 0,3 s las
    primeras copias, incluso a +25 dB, fallaban. Fondo blanco: el propio fondo es `w` más largo (se
    construye así). Grabación real (corta, sin bucle): se antepone el fondo en palíndromo
    (directo, invertido en el tiempo, directo…) hasta `w`; las copias solo van tras el
    calentamiento y la SNR local solo mira el fondo en la ventana de cada copia."""
    def __init__(self, bg, w, tile):
        self.bg, self.fs, self.center = bg, bg.fs, bg.center
        self.dt, self.desc = bg.dt, getattr(bg, "desc", "")
        if tile:
            base = bg.read(0, bg.n); parts = []; k = 0; tot = 0
            while tot < w:
                parts.append(base if k % 2 == 0 else base[::-1]); tot += len(base); k += 1
            self.lead = np.concatenate(parts)[:int(w)]
            self.w = len(self.lead); self.n = self.w + bg.n
        else:
            self.lead = None; self.w = int(w); self.n = bg.n

    def read(self, a, b):
        if self.lead is None: return self.bg.read(a, b)
        a = max(a, 0); b = min(b, self.n); out = []
        if a < self.w: out.append(self.lead[a:min(b, self.w)])
        if b > self.w: out.append(self.bg.read(max(a, self.w) - self.w, b - self.w))
        return np.concatenate(out) if out else np.zeros(0, complex)

    def file_at(self, s):
        f = getattr(self.bg, "file_at", None)
        return f(max(s - (self.w if self.lead is not None else 0), 0)) if f else "white"


def fft_resample(x, fs_in, fs_out):
    """Remuestreo espectral (recorte o relleno del espectro). Conserva la potencia por Hz."""
    n = len(x); m = int(round(n * fs_out / fs_in))
    X = np.fft.fft(x); Y = np.zeros(m, complex)
    h = min(n, m) // 2
    Y[:h] = X[:h]; Y[m - h:] = X[n - h:]
    return np.fft.ifft(Y) * (m / n)


def write_ci16(path, x, fs, center, desc):
    iq = np.empty(2 * len(x), np.int16)
    iq[0::2] = np.clip(np.round(x.real), -2048, 2047); iq[1::2] = np.clip(np.round(x.imag), -2048, 2047)
    open(path + ".sigmf-data", "wb").write(iq.tobytes())
    json.dump({"global": {"core:datatype": "ci16_le", "core:sample_rate": fs, "core:version": "1.0.0",
                          "core:description": desc},
               "captures": [{"core:sample_start": 0, "core:frequency": center}], "annotations": []},
              open(path + ".sigmf-meta", "w"))


def prep():
    """Fondos reales remuestreados: ISM (RTL, cu8 250 kS/s) -> 48 kS/s; lte/gsm/wifi -> 2 MS/s."""
    os.makedirs(BGDIR, exist_ok=True)
    for f in ISM:
        b = iny.FondoSigMF(os.path.join(REAL, f + ".sigmf-meta"))
        y = fft_resample(b.read(0, b.n), b.fs, 48000.0)
        write_ci16(os.path.join(BGDIR, f + "_48k"), y, 48000.0, b.center, "ISM 433,92 MHz remuestreado a 48 kS/s")
    for f in ("lte", "gsm", "wifi"):
        b = iny.FondoSigMF(os.path.join(REAL, f + ".sigmf-meta"))
        for tag, fo in (("2M", 2e6), ("500k", 5e5)):
            if f == "wifi" and tag == "500k": continue            # 3,8 ms: no sirve
            y = fft_resample(b.read(0, b.n), b.fs, fo)
            write_ci16(os.path.join(BGDIR, f + "_" + tag), y, fo, b.center, f + " remuestreado/recortado a %s" % tag)
    print("fondos preparados en", BGDIR)


def make_bg(proto, bg, fs_ovr=None):
    """Devuelve (kind, lista de metas) del fondo real; None si es blanco."""
    c = CFG[proto]
    if bg == "white": return None
    if proto == "adsb":     return [os.path.join(BGDIR, f + "_2M.sigmf-meta") for f in ("lte", "gsm", "wifi")]
    if proto == "lora":     return [os.path.join(BGDIR, f + "_500k.sigmf-meta") for f in ("lte", "gsm")]   # lora.sigmf (paquete real) se excluye: es la señal propia 
    if proto == "ism":      return [os.path.join(REAL, f + ".sigmf-meta") for f in ISM]
    if bg == "real_rf":     return [os.path.join(BGDIR, f + "_48k.sigmf-meta") for f in ISM]
    if bg == "real_rec":
        own = OWN.get(proto, [])
        return [os.path.join(REAL, f + ".sigmf-meta") for f in F48_REC if f not in own]
    raise SystemExit("fondo desconocido %s para %s" % (bg, proto))


# -------------------------------------------------------------- generación --
_cache = {}
def gen_copy(proto, i, fs, center):
    """Copia limpia (banda base, centrada) con ID i; en caché. Devuelve (x, pad, bw)."""
    key = (proto, fs, i if (CFG[proto]["kind"] == "id" or "pfn" in CFG[proto]) else None)
    if key in _cache: return _cache[key]
    c = CFG[proto]
    params = IDP[proto]["params"](i) if c["kind"] == "id" else (c["pfn"](i) if "pfn" in c else c.get("params"))
    tmp = os.path.join(WORK, "gen_%d" % os.getpid()); os.makedirs(tmp, exist_ok=True)
    x, bw = iny.gen_copy(BIN, proto, params, fs, center, tmp, "g")
    m = np.abs(x); idx = np.nonzero(m > 0.02 * m.max())[0]
    i0, i1 = int(idx[0]), int(idx[-1]) + 1
    if proto == "adsb": i1 = min(i1, i0 + c["trim"])     # solo la 1.ª trama: una copia = una trama
    pad = int(iny.PAD_S * fs)
    lo, hi = max(i0 - pad, 0), min(i1 + pad, len(x))
    x, pad = x[lo:hi], (i0 - lo, hi - i1)                # (silencio previo, silencio posterior)
    bw = float(c.get("bw", bw))
    _cache[key] = (x, pad, bw)
    return _cache[key]


def free_port(base):
    return 20000 + (os.getpid() * 7 + base) % 20000


def replay(meta, dec, port):
    r = subprocess.run([BIN, "--replay", meta, "--replay-fast", "--decode", dec, "--print", "--config", "none",
                        "--no-gpu", "--port", str(port)], capture_output=True, text=True, timeout=1800)
    if r.returncode:
        raise RuntimeError("replay falló (%d): %s" % (r.returncode, r.stderr[-300:]))
    ev, gaps = [], []
    for ln in r.stdout.splitlines():
        if not ln.startswith("{"): continue
        try: j = json.loads(ln)
        except ValueError: continue
        if j.get("type") == "dec": ev.append(j)
        elif j.get("type") == "gap": gaps.append((int(j["sample"]), int(j["n"])))
    return ev, gaps


# ------------------------------------------------------------------- serie --
def plan_pass(rng, lens, gapr, fs, n_bg, t0=0):
    """Instantes de inicio sin solape: huecos mínimos + holgura repartida como estadísticos de orden.
    lens: longitud (muestras) de la ranura de cada copia. Devuelve inicios de ranura o None si no cabe."""
    lead = int(LEAD * fs) + t0; span = n_bg - lead - int(LEAD * fs)
    gmin = int(gapr[0] * fs); gmax = int(gapr[1] * fs)
    k = len(lens)
    need = sum(lens) + k * gmin
    if need > span: return None
    free = span - need
    cuts = np.sort(rng.integers(0, free + 1, k))
    pos = lead; st = []
    for r in range(k):
        gap = int(cuts[r] - (cuts[r - 1] if r else 0))
        st.append(pos + gap); pos = st[-1] + lens[r] + gmin
    return st


def run_series(a):
    proto, bgk, cond, seed = a.proto, a.bg, a.cond, a.seed
    c = CFG[proto]; rng = np.random.default_rng([seed, zlib.crc32(proto.encode()), zlib.crc32(bgk.encode()), zlib.crc32(cond.encode())])
    occ = cond in ("adj", "co0", "co-6")                 # con interferente
    fs = float(c["occ_fs"] if cond != "none" else c["fs"])   # "ctl": control sin interferente a la tasa de ocupación
    step = a.step or c["step"]; n_pt = a.n or c["n"]
    levels = np.arange(SNR_LO, SNR_HI + 1e-9, step)
    todo = list(np.repeat(levels, n_pt)); rng.shuffle(todo)
    metas = make_bg(proto, bgk)
    pool = Pool(metas) if metas else None
    if pool is not None and abs(pool.fs - fs) > 1e-6:
        raise SystemExit("el fondo real %s va a %g Hz y la serie a %g" % (bgk, pool.fs, fs))
    center = pool.center if pool else {"ais": 162e6, "aprs": 144.8e6, "pocsag": 466.075e6, "lora": 868.1e6,
                                       "adsb": 1090e6}.get(proto, 433.92e6)
    dec = "%s@%d" % (proto, int(center + c.get("off", 0)))
    # condición de ocupación -> (df Hz, delta dB del interferente sobre el objetivo, solape)
    if occ:
        df, dB = {"adj": (c["occ_df"], +10.0), "co0": (0.0, 0.0), "co-6": (0.0, -6.0)}[cond]
        if df is None: raise SystemExit("sin canal adyacente en %s" % proto)
    os.makedirs(WORK, exist_ok=True); os.makedirs(DATOS, exist_ok=True)
    tag = "%s_%s_%s_s%d" % (proto, bgk, cond, seed)
    rows, extras, sigsec, npass, clipsc, gaps_t = [], 0, 0.0, 0, 0, 0
    port = free_port(seed)
    ident = c["kind"] == "id"; IOFF = 5000
    t_start = time.time()
    while todo:
        # --- tamaño de la pasada
        tmpl = gen_copy(proto, 0, fs, center)
        Lcopy = len(tmpl[0])
        mult = 2 if occ else 1
        kmax = 60 if ident else 12
        if pool is None:
            kk = min(len(todo), kmax)
            avg_gap = 0.5 * sum(c["gap"])
            seconds = 2 * LEAD + kk * (mult * Lcopy / fs + avg_gap + c["gap"][1] * 0.5) * 1.15 + 0.1
            bg = ConLead(iny.FondoBlanco(fs, center, SIGMA, seconds + c.get("warm", WARM), int(rng.integers(1 << 30))), c.get("warm", WARM) * fs, False)
        else:
            bg = ConLead(pool, c.get("warm", WARM) * fs, True)
            kk = min(len(todo), kmax)
        n_bg = bg.n
        # recorta kk hasta que quepa
        while True:
            copies = []
            for j in range(kk):
                x, pad, bw = gen_copy(proto, j if (ident or "pfn" in c) else 0, fs, center)
                copies.append(dict(j=j, x=x, pad=pad, bw=bw))
            lens = [len(cp["x"]) * (2 if occ else 1) for cp in copies]
            st = plan_pass(rng, lens, c["gap"], fs, n_bg, bg.w)
            if st is not None or kk == 0: break
            kk -= 1
        if kk == 0: raise SystemExit("ni una copia cabe en el fondo (%s)" % tag)
        # --- copias de la pasada
        jobs = []
        for j, cp in enumerate(copies):
            L = len(cp["x"])
            snr = float(todo.pop())
            s0 = st[j] + (L // 2 if occ else 0)                 # objetivo: ranura con holgura L/2 a cada lado
            cfo = float(rng.uniform(-1, 1) * c["cfo"]) if c["cfo"] else 0.0
            jobs.append(dict(role="t", id=j, x=cp["x"], pad=cp["pad"], bw=cp["bw"], start=s0, f_rel=float(c.get("off", 0)), cfo=cfo,
                             snr=snr, phase=float(rng.uniform(0, 2 * np.pi))))
            if occ:
                dl = int(rng.uniform(-0.5, 0.5) * L) if cond != "adj" else int(rng.uniform(-0.2, 0.2) * L)
                xi, padi, bwi = gen_copy(proto, IOFF + j, fs, center)
                jobs.append(dict(role="i", id=IOFF + j, x=xi, pad=padi, bw=bwi, start=s0 + dl, f_rel=c.get("off", 0) + df,
                                 cfo=float(rng.uniform(-1, 1) * c["cfo"]) if c["cfo"] else 0.0, snr=snr + dB,
                                 phase=float(rng.uniform(0, 2 * np.pi))))
        for jb in jobs:
            x, bw = jb["x"], jb["bw"]
            pt = np.mean(np.abs(x) ** 2)
            phi = 1.0 if bw >= 0.99 * fs else min(1.0, iny.band_power(x, fs, 0.0, bw, 1024) / pt)
            psig = np.mean(np.abs(x[jb["pad"][0]:len(x) - jb["pad"][1]]) ** 2) * phi
            s0, s1 = jb["start"], jb["start"] + len(x)
            pn, _ = iny.local_noise(bg, s0, s1, jb["f_rel"] + jb["cfo"], bw)
            jb["amp"] = float(np.sqrt(10 ** (jb["snr"] / 10) * pn / psig)); jb["pn"] = float(pn)
        y = bg.read(0, n_bg).astype(np.complex128).copy()
        n_bg = len(y)
        for jb in jobs:
            s0, s1 = jb["start"], jb["start"] + len(jb["x"])
            t = np.arange(len(jb["x"]))
            y[s0:s1] += jb["amp"] * jb["x"] * np.exp(1j * (jb["phase"] + 2 * np.pi * (jb["f_rel"] + jb["cfo"]) * t / fs))
        pk = max(np.abs(y.real).max(), np.abs(y.imag).max())
        if pk > 2040:
            y *= 2040 / pk; clipsc += 1                       # escala global: conserva la SNR local
        base = os.path.join(WORK, "p_%d" % os.getpid())
        write_ci16(base, y, fs, center, "pasada")
        ev, g = replay(base + ".sigmf-meta", dec, port)
        gaps_t += len(g)
        for ext in (".sigmf-data", ".sigmf-meta"): os.unlink(base + ext)
        npass += 1; sigsec += (n_bg - bg.w) / fs
        # --- emparejado
        used = set()
        evs = []
        for e in ev:
            m = e.get("msg", {})
            if proto == "ism" and m.get("model") != "Nexus-TH": continue   # solo la copia (el fondo real trae mensajes ISM ajenos)
            evs.append(dict(s=m.get("sample"), sm=m.get("sample_src"), snr=m.get("snr_db"), rssi=m.get("rssi_dbfs"),
                            cfo=m.get("cfo") if proto == "lora" else None, temp=m.get("temperature_c"),
                            id=(IDP[proto]["key"](m) if ident and _ok(proto, m) else None)))
        for jb in jobs:
            if jb["role"] != "t": continue
            s0, L = jb["start"] + jb["pad"][0], len(jb["x"]) - jb["pad"][0] - jb["pad"][1]
            if ident:
                hit = [k for k, e in enumerate(evs) if e["id"] == jb["id"]]
            else:
                hit = [k for k, e in enumerate(evs) if e["s"] is not None
                       and s0 - 0.05 * fs <= e["s"] <= s0 + L + 0.5 * fs and k not in used
                       and (proto != "ism" or (e["temp"] is not None and abs(e["temp"] - (5 + (jb["id"] % 97) * 0.3)) < 0.15))]
            used.update(hit)
            e = min((evs[k] for k in hit), key=lambda q: q["s"]) if hit else None
            ds = (e["s"] - s0) if e and e["s"] is not None else ""
            ingap = int(any(gs < s0 + L + fs and gs + gn > s0 - fs for gs, gn in g))
            rows.append([proto, bgk, cond, seed, npass, round(jb["snr"], 3), round(jb["cfo"], 2), int(bool(hit)), len(hit),
                         ds, e["sm"] if e else "", e["snr"] if e and e["snr"] is not None else "",
                         e["rssi"] if e and e["rssi"] is not None else "",
                         e["cfo"] if e and e["cfo"] is not None else "",
                         bg.file_at(s0) if pool else "white", round(jb["pn"], 2), s0, L, ingap])
        if ident:
            ids = {jb["id"] for jb in jobs}
            extras += sum(1 for e in evs if e["id"] not in ids)
        else:
            extras += sum(1 for k in range(len(evs)) if k not in used)
    with open(os.path.join(DATOS, tag + ".csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["proto", "bg", "cond", "seed", "pasada", "snr_inj", "cfo_inj", "rec", "n_ev", "dsample", "sample_src",
                    "ev_snr", "ev_rssi", "ev_cfo", "fichero_fondo", "pn_local", "start", "len", "en_hueco"])
        w.writerows(rows)
    json.dump(dict(tag=tag, proto=proto, bg=bgk, cond=cond, seed=seed, fs=fs, copies=len(rows), pasadas=npass,
                   segundos_senal=sigsec, eventos_ajenos=extras, gaps=gaps_t, pasadas_escaladas=clipsc,
                   segundos_cpu=round(time.time() - t_start, 1), nominal_n_por_punto=n_pt, paso_db=step,
                   fondo=(pool.desc if pool else "blanco sigma=%g" % SIGMA)),
              open(os.path.join(DATOS, tag + ".json"), "w"))
    print(tag, len(rows), "copias,", npass, "pasadas, ajenos", extras, "gaps", gaps_t, "%.0fs" % (time.time() - t_start))


def _ok(proto, m):
    try: IDP[proto]["key"](m); return True
    except Exception: return False


# ---------------------------------------------------------- falsos positivos --
FAM = {"f48": (48000.0, ["ais", "aprs", "pocsag", "acars", "rs41", "flex", "dsc"], 3600.0),
       "f250": (250000.0, ["ism"], 900.0), "lora": (500000.0, ["lora"], 900.0), "adsb": (2000000.0, ["adsb"], 600.0)}


def run_fp(a):
    fs, protos, total = FAM[a.fam]; total = a.seconds or total
    os.makedirs(DATOS, exist_ok=True); os.makedirs(WORK, exist_ok=True)
    out = []
    ctr = {"f48": 162e6, "f250": 433.92e6, "lora": 868.1e6, "adsb": 1090e6}[a.fam]
    decs = []
    for p in protos: decs += ["--decode", "%s@%d" % (p, int(ctr))]
    nseed = 3
    cnt = {p: 0 for p in protos}
    for s in range(nseed):                                   # ruido blanco, 3 semillas
        dur = total / nseed
        bg = iny.FondoBlanco(fs, ctr, SIGMA, dur, 1000 + s)
        base = os.path.join(WORK, "fp_%s_%d" % (a.fam, os.getpid()))
        with open(base + ".sigmf-data", "wb") as fo:
            for c0 in range(0, bg.n, iny.CHUNK):
                y = bg.read(c0, min(c0 + iny.CHUNK, bg.n))
                iq = np.empty(2 * len(y), np.int16)
                iq[0::2] = np.clip(np.round(y.real), -2048, 2047); iq[1::2] = np.clip(np.round(y.imag), -2048, 2047)
                fo.write(iq.tobytes())
        json.dump({"global": {"core:datatype": "ci16_le", "core:sample_rate": fs, "core:version": "1.0.0"},
                   "captures": [{"core:sample_start": 0, "core:frequency": ctr}], "annotations": []},
                  open(base + ".sigmf-meta", "w"))
        r = subprocess.run([BIN, "--replay", base + ".sigmf-meta", "--replay-fast", "--print", "--config", "none",
                            "--no-gpu", "--port", str(free_port(50 + s))] + decs, capture_output=True, text=True, timeout=7200)
        for ext in (".sigmf-data", ".sigmf-meta"): os.unlink(base + ext)
        if r.returncode: raise SystemExit("fp falló: " + r.stderr[-300:])
        for ln in r.stdout.splitlines():
            if ln.startswith('{"type":"dec"'):
                cnt[json.loads(ln)["proto"]] += 1
    for p in protos:
        out.append(dict(fam=a.fam, proto=p, fondo="blanco", segundos=total, eventos=cnt[p]))
    # fondos reales sin inyectar (sin los de la propia señal): un reproductor por fichero
    real = {}
    for p in protos:
        if p in ("adsb", "lora", "ism"):
            files = make_bg(p, "real_rf") if p != "ism" else make_bg(p, "real")
            kinds = {"real_rf": files}
        else:
            kinds = {"real_rf": make_bg(p, "real_rf"), "real_rec": make_bg(p, "real_rec")}
        for kn, files in kinds.items():
            n = 0; sec = 0.0
            for m in files:
                meta = json.load(open(m)); sec += os.path.getsize(m.replace(".sigmf-meta", ".sigmf-data")) / (
                    {"ci8": 2, "cu8": 2, "ci16_le": 4, "cf32_le": 8}[meta["global"]["core:datatype"]]) / meta["global"]["core:sample_rate"]
                r = subprocess.run([BIN, "--replay", m, "--replay-fast", "--print", "--config", "none", "--no-gpu",
                                    "--decode", "%s@%d" % (p, int(meta["captures"][0].get("core:frequency", ctr))),
                                    "--port", str(free_port(90))], capture_output=True, text=True, timeout=600)
                n += sum(1 for ln in r.stdout.splitlines() if ln.startswith('{"type":"dec"'))
            out.append(dict(fam=a.fam, proto=p, fondo=kn, segundos=sec, eventos=n, ficheros=[os.path.basename(f) for f in files]))
    json.dump(out, open(os.path.join(DATOS, "fp_%s.json" % a.fam), "w"), indent=1)
    print("fp", a.fam, [(o["proto"], o["fondo"], o["eventos"], round(o["segundos"], 1)) for o in out])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    sp.add_parser("prep")
    r = sp.add_parser("run")
    r.add_argument("--proto", required=True, choices=sorted(CFG)); r.add_argument("--bg", required=True)
    r.add_argument("--seed", type=int, required=True); r.add_argument("--cond", default="none")
    r.add_argument("--step", type=float, default=0); r.add_argument("--n", type=int, default=0)
    f = sp.add_parser("fp"); f.add_argument("--fam", required=True, choices=sorted(FAM)); f.add_argument("--seconds", type=float, default=0)
    a = ap.parse_args()
    if a.cmd == "prep": prep()
    elif a.cmd == "run": run_series(a)
    else: run_fp(a)


if __name__ == "__main__":
    main()
