"""learn.py — entrena y evalúa los detectores del piloto P6 (3 semillas, 7 variantes).

Entrada: build/p6/data/s<seed>_{train,test,synth,cal}/*.npz (gen.py).
Tarea: por ventana de 0,25 s (47 frames de 5,33 ms x 128 bins) y protocolo, ¿hay una ráfaga de ese
protocolo? (5 salidas sigmoide). Una ventana es positiva si una ráfaga la solapa >= 50 % de
min(duración de la ráfaga, duración de la ventana); la misma regla vale para la verdad y para los
eventos verificados.
Variantes de entrenamiento (todas con la misma CNN y las mismas épocas):
  A    lo no etiquetado = fondo (verified = 1, resto = 0).
  A2   como A, pero las ventanas «unknown» (detector sin explicación) se excluyen de la pérdida.
  B3   PU con propensión: riesgo no sesgado ponderado por 1/e (Horvitz-Thompson) con corrección
       no negativa de nnPU (Kiryo et al. 2017); e(SNR) de la calibración P3 (paper/exp/calibracion/curvas.csv),
       SNR = la que estima el host (snr_db del evento).
  Bc   igual, con e(SNR) calibrada en el propio entorno (capturas «cal», inyección en el mismo fondo) y
       la SNR del host corregida por su sesgo medio medido en «cal».
  Bo   igual que Bc pero con la SNR verdadera inyectada (cota: propensión conocida sin error de SNR).
  B3f2/B3f5  como B3 con el suelo de la propensión en 0,2 / 0,5 (peso máx. 5 / 2; sensibilidad).
  E    A2 + sintético (unión).
  C    solo sintético: entrena con captura de ruido blanco + inyección (verdad completa).
  D    cota superior: verdad completa sobre las capturas reales.
"""
import os, sys, json, glob, csv
import numpy as np, torch, torch.nn as nn, torch.nn.functional as F
from sklearn.metrics import roc_auc_score, average_precision_score
from sklearn.linear_model import LogisticRegression
RAIZ = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
DATA = os.path.join(RAIZ, "build/p6/data"); OUT = os.path.join(RAIZ, "paper/exp/aprendizaje")
NP = 5; PROTOS = ["ais", "aprs", "pocsag", "acars", "flex"]
WF, FRAME = 47, 256; WS = WF * FRAME; T_WARM = int(3.5 * 48000)
SNR_BINS = [(-5, 0), (0, 5), (5, 10), (10, 15), (15, 20.01)]
DEV = "cuda" if torch.cuda.is_available() else "cpu"


def ov_rule(a, b, s, e):
    ov = np.maximum(0, np.minimum(b, e) - np.maximum(a, s))
    return ov >= 0.5 * np.minimum(e - s, b - a)


def ev_snr(Sraw, s, e, host):
    """SNR del evento: la del host (snr_db) si existe; si no (p. ej. ACARS no la emite), una estimación
    de exceso de potencia en la banda central (±6 kHz) sobre la mediana de la captura (aprox. grosera;
    su sesgo medio se corrige con host_bias, medido en las capturas de calibración)."""
    if not np.isnan(host): return host
    band = Sraw[:, 32:96].astype(np.float64); p = 10 * np.log10(np.mean(10 ** (band / 10), axis=1))
    f0, f1 = int(s // FRAME), max(int(e // FRAME), int(s // FRAME) + 1)
    ex = 10 ** ((np.mean(p[f0:f1]) - np.median(p)) / 10) - 1
    return 10 * np.log10(max(ex, 0.05))


def windows(f, hop_frames):
    d = np.load(f); S = d["S"].astype(np.float32); T = d["truth"]; O = d["obs"]
    O = O.copy()
    for r in O:
        if r[0] == 0: r[4] = ev_snr(S, r[2], r[3], r[4])
    S = (S - np.median(S, axis=0, keepdims=True)) / 6.0
    S = np.clip(S, -3, 8)
    nfr = len(S); starts = np.arange(T_WARM // FRAME, nfr - WF, hop_frames)
    X = np.stack([S[s:s + WF] for s in starts]); a = starts * FRAME; b = a + WS
    n = len(starts)
    Y = np.zeros((n, NP), np.float32); snr_t = np.full((n, NP), np.nan, np.float32)
    for p, s, e, snr in T:
        m = ov_rule(a, b, s, e); Y[m, int(p)] = 1; snr_t[m, int(p)] = snr
    L = np.zeros((n, NP), np.float32); snr_h = np.full((n, NP), np.nan, np.float32); unk = np.zeros(n, bool)
    for lv, p, s, e, sh, _, _ in O:
        if lv == 0:
            m = ov_rule(a, b, s, e); L[m, int(p)] = 1
            snr_h[m, int(p)] = np.where(np.isnan(snr_h[m, int(p)]), sh, np.maximum(snr_h[m, int(p)], sh))
        elif lv == 1:
            unk |= (np.minimum(b, e) - np.maximum(a, s)) >= 0.25 * WS
    unk &= (L.sum(1) == 0)
    return dict(X=X, Y=Y, snr_t=snr_t, L=L, snr_h=snr_h, unk=unk)


def load(seed, split, hop):
    ds = [windows(f, hop) for f in sorted(glob.glob(os.path.join(DATA, "s%d_%s" % (seed, split), "*.npz")))]
    return {k: np.concatenate([d[k] for d in ds]) for k in ds[0]}


# ------------------------------------------------------------ propensiones --
def p3_curve(proto):
    """e(SNR) de la calibración P3 (bins, suavizada monótona) para el fondo real más parecido."""
    bg = "real_rec" if proto == "flex" else "real_rf"
    rows = [r for r in csv.DictReader(open(os.path.join(RAIZ, "paper/exp/calibracion/curvas.csv")))
            if r["proto"] == proto and r["bg"] == bg and r["cond"] == "none"]
    x = np.array([float(r["snr_local_db"]) for r in rows]); k = np.array([float(r["k"]) for r in rows]); n = np.array([float(r["n"]) for r in rows])
    o = np.argsort(x); x, k, n = x[o], k[o], n[o]
    p = np.maximum.accumulate(k / n)
    return lambda s: np.interp(s, x, p)


def own_curve(seed, proto_i):
    """e(SNR) en el entorno: regresión logística de «una ráfaga inyectada fue verificada» frente a su SNR (cal)."""
    xs, ys = [], []
    for f in sorted(glob.glob(os.path.join(DATA, "s%d_cal" % seed, "*.npz"))):
        d = np.load(f); T, O = d["truth"], d["obs"]
        for p, s, e, snr in T:
            if int(p) != proto_i: continue
            hit = np.any((O[:, 0] == 0) & (O[:, 1] == p) & (O[:, 2] < e) & (O[:, 3] > s))
            xs.append(snr); ys.append(int(hit))
    xs, ys = np.array(xs), np.array(ys)
    if ys.min() == ys.max():
        c = float(ys.mean()); return (lambda s: np.full_like(np.asarray(s, float), c)), len(xs)
    m = LogisticRegression(C=100).fit(xs[:, None], ys)
    return (lambda s: m.predict_proba(np.asarray(s, float).reshape(-1, 1))[:, 1]), len(xs)


def host_bias(seed):
    """Sesgo medio snr_db(host) - SNR inyectada, medido en «cal»."""
    dl = {p: [] for p in range(NP)}
    for f in sorted(glob.glob(os.path.join(DATA, "s%d_cal" % seed, "*.npz"))):
        d = np.load(f); T, O = d["truth"], d["obs"]
        for p, s, e, snr in T:
            h = O[(O[:, 0] == 0) & (O[:, 1] == p) & (O[:, 2] < e) & (O[:, 3] > s)]
            if len(h): dl[int(p)].append(ev_snr(d["S"].astype(np.float32), h[0, 2], h[0, 3], h[0, 4]) - snr)
    return {p: (float(np.mean(v)) if v else 0.0) for p, v in dl.items()}, {p: len(v) for p, v in dl.items()}


def weights(tr, kind, seed, floor=0.05):
    """Peso 1/e por (ventana, clase); 0 donde no hay etiqueta."""
    W = np.zeros_like(tr["L"]); biasd, _ = host_bias(seed)
    for p in range(NP):
        m = tr["L"][:, p] == 1
        bias = biasd[p]; sh = tr["snr_h"][m, p]; sh = np.where(np.isnan(sh), 5.0, sh)
        st = np.where(np.isnan(tr["snr_t"][m, p]), sh - bias, tr["snr_t"][m, p])        # sin ráfaga inyectada solapada: usa la del host
        if kind == "B3": e = p3_curve(PROTOS[p])(sh)
        elif kind == "Bc": e = own_curve(seed, p)[0](sh - bias)
        else:              e = own_curve(seed, p)[0](st)
        W[m, p] = 1.0 / np.clip(e, floor, 1.0)
    return W


# ------------------------------------------------------------------- modelo --
class Net(nn.Module):
    def __init__(s):
        super().__init__()
        def blk(i, o): return nn.Sequential(nn.Conv2d(i, o, 3, padding=1), nn.BatchNorm2d(o), nn.ReLU(), nn.MaxPool2d(2))
        s.f = nn.Sequential(blk(1, 16), blk(16, 32), blk(32, 48))
        s.fc = nn.Sequential(nn.Linear(48 * 16, 128), nn.ReLU(), nn.Dropout(0.3), nn.Linear(128, NP))

    def forward(s, x):
        h = s.f(x[:, None])                           # (B,48,5,16)
        return s.fc(h.mean(2).flatten(1))


def train(X, lab, w, mask, seed, kind, epochs=12, bs=1024):
    """lab: etiqueta (0/1); w: peso 1/e (solo kind B*); mask: 1 = la ventana cuenta."""
    torch.manual_seed(seed); np.random.seed(seed)
    net = Net().to(DEV); opt = torch.optim.AdamW(net.parameters(), 2e-3, weight_decay=1e-3)
    Xt = torch.from_numpy(X).half().to(DEV); Lt = torch.from_numpy(lab).to(DEV); Wt = torch.from_numpy(w).to(DEV)
    Mt = torch.from_numpy(mask.astype(np.float32)).to(DEV)
    n = len(X); steps = epochs * (n // bs + 1); sch = torch.optim.lr_scheduler.OneCycleLR(opt, 4e-3, total_steps=steps)
    for ep in range(epochs):
        net.train(); perm = torch.randperm(n, device=DEV)
        for i in range(0, n, bs):
            ix = perm[i:i + bs]; z = net(Xt[ix].float()); l, ww, m = Lt[ix], Wt[ix], Mt[ix][:, None]
            if kind.startswith("B"):
                sp, sn = F.softplus(-z), F.softplus(z); N = len(ix)
                Rp = (l * ww * sp).sum(0) / N
                Rn = (sn.sum(0) - (l * ww * sn).sum(0)) / N
                loss = torch.where(Rn >= 0, Rp + Rn, -Rn).sum()           # nnPU (Kiryo 2017), gamma = 1, beta = 0
            else:
                loss = (F.binary_cross_entropy_with_logits(z, l, reduction="none") * m).sum() / (m.sum() * NP)
            opt.zero_grad(); loss.backward(); opt.step(); sch.step()
    return net


@torch.no_grad()
def predict(net, X):
    net.eval(); out = []
    for i in range(0, len(X), 2048):
        out.append(torch.sigmoid(net(torch.from_numpy(X[i:i + 2048]).to(DEV))).cpu().numpy())
    return np.concatenate(out)


def evaluate(P, te):
    Y, st = te["Y"], te["snr_t"]; res = {}
    for p in range(NP):
        y, s = Y[:, p], P[:, p]; neg = s[y == 0]
        thr1 = np.quantile(neg, 0.99)                      # punto de operación común: FPR = 1 %
        r = dict(auc=float(roc_auc_score(y, s)), ap=float(average_precision_score(y, s)))
        for tag, th in (("fpr1", thr1), ("t05", 0.5)):
            pred = s >= th; tp = (pred & (y == 1)).sum(); fp = (pred & (y == 0)).sum()
            r[tag + "_prec"] = float(tp / max(tp + fp, 1)); r[tag + "_recall"] = float(tp / (y == 1).sum())
            r[tag + "_recall_bins"] = [float(pred[(y == 1) & (st[:, p] >= lo) & (st[:, p] < hi)].mean())
                                      if ((y == 1) & (st[:, p] >= lo) & (st[:, p] < hi)).any() else None for lo, hi in SNR_BINS]
        res[PROTOS[p]] = r
    return res


def main():
    seeds = [int(a) for a in sys.argv[1:]] or [1, 2, 3]
    allres = {}
    for sd in seeds:
        tr = load(sd, "train", 24); te = load(sd, "test", WF); sy = load(sd, "synth", 24)
        print("seed", sd, "train", len(tr["X"]), "test", len(te["X"]), "synth", len(sy["X"]),
              "pos truth", int(tr["Y"].sum()), "pos observed", int(tr["L"].sum()), "unknown win", int(tr["unk"].sum()), flush=True)
        one = np.ones(len(tr["X"]), np.float32); z0 = np.zeros_like(tr["L"])
        runs = {
            "A":  (tr["X"], tr["L"], z0, one, "A"),
            "A2": (tr["X"], tr["L"], z0, (~tr["unk"]).astype(np.float32), "A2"),
            "B3": (tr["X"], tr["L"], weights(tr, "B3", sd), one, "B3"),
            "B3f2": (tr["X"], tr["L"], weights(tr, "B3", sd, 0.2), one, "B3"),
            "B3f5": (tr["X"], tr["L"], weights(tr, "B3", sd, 0.5), one, "B3"),
            "Bc": (tr["X"], tr["L"], weights(tr, "Bc", sd), one, "Bc"),
            "Bo": (tr["X"], tr["L"], weights(tr, "Bo", sd), one, "Bo"),
            "C":  (sy["X"], sy["Y"], z0[:len(sy["X"])], np.ones(len(sy["X"]), np.float32), "C"),
            "E":  (np.concatenate([tr["X"], sy["X"]]), np.concatenate([tr["L"], sy["Y"]]), np.concatenate([z0, z0[:len(sy["X"])]]),
                   np.concatenate([(~tr["unk"]).astype(np.float32), np.ones(len(sy["X"]), np.float32)]), "A2"),
            "D":  (tr["X"], tr["Y"], z0, one, "D"),
        }
        for name, (X, lab, w, m, kind) in runs.items():
            net = train(X, lab, w, m, sd, kind); P = predict(net, te["X"])
            allres.setdefault(name, {})[sd] = evaluate(P, te)
            np.save(os.path.join(RAIZ, "build/p6/pred_%s_s%d.npy" % (name, sd)), P)
            print(" ", name, "AP medio %.3f" % np.mean([v["ap"] for v in allres[name][sd].values()]), flush=True)
        b, nb = host_bias(sd)
        allres.setdefault("_meta", {})[sd] = dict(host_bias_db={PROTOS[k]: round(v, 2) for k, v in b.items()}, n_cal_matched=nb, n_train=len(tr["X"]), n_test=len(te["X"]),
                                                  pos_truth=int(tr["Y"].sum()), pos_obs=int(tr["L"].sum()), unk=int(tr["unk"].sum()))
        np.save(os.path.join(RAIZ, "build/p6/te_y_s%d.npy" % sd), te["Y"]); np.save(os.path.join(RAIZ, "build/p6/te_snr_s%d.npy" % sd), te["snr_t"])
    for sd in seeds:                      # un fichero por semilla (se pueden lanzar en paralelo); analizar.py los une
        json.dump({k: {str(sd): v[sd]} for k, v in allres.items()}, open(os.path.join(OUT, "res_s%d.json" % sd), "w"), indent=1)


if __name__ == "__main__":
    main()
