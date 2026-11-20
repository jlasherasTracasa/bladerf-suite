#!/usr/bin/env python3
"""Empareja eventos con la verdad de terreno, ajusta la sigmoide y dibuja.

Uso: ajustar.py DIR_TRABAJO SALIDA_DIR
  DIR_TRABAJO contiene, por cada <proto>_<bg>: .truth.json y _s<k>.out (JSONL de --print).
Modelo: p = g + (c-g)/(1+exp(-(snr-s50)/w)) (suelo g, techo c; ver fit()). Verosimilitud
binomial en una rejilla (s50 cada 0,02 dB, w de 0,05 a 6 dB); el IC95 % de s50
sale de la verosimilitud perfil (2*dLL <= 3,84). SNR al 90 %: s50 + w*ln 9.
Intervalo por punto: Wilson 95 %.
"""
import glob, json, math, os, sys
import numpy as np

def wilson(k, n, z=1.96):
    if n == 0: return (0.0, 1.0)
    p = k / n; d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d; h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, c - h), min(1.0, c + h))

def fit(snr, k, n):
    """Ajuste p = g + (c - g) * sigmoide((snr - s50)/w) con suelo g y techo c.

    Máxima verosimilitud binomial en rejilla. Las SNR al 50 % y 90 % son
    ABSOLUTAS (p = 0,5 y p = 0,9 de la curva ajustada); None si el techo ajustado
    no llega (c < 0,9) o si el suelo ya las supera. IC95 % por la región
    2*dLL <= 3,84 (proyectada). Se devuelve también el ajuste sin suelo ni techo."""
    s50 = np.arange(snr.min() - 8, snr.max() + 8, 0.2)[:, None, None, None]
    w = np.exp(np.linspace(np.log(0.1), np.log(6), 28))[None, :, None, None]
    g = np.arange(0, 0.41, 0.01)[None, None, :, None]
    c = np.arange(0.5, 1.001, 0.01)[None, None, None, :]
    ll = np.zeros((s50.shape[0], w.shape[1], g.shape[2], c.shape[3]), np.float32)
    for s, ki, ni in zip(snr, k, n):
        sg = 1 / (1 + np.exp(-np.clip((s - s50) / w, -60, 60)))
        p = np.clip(g + (c - g) * sg, 1e-6, 1 - 1e-6)
        ll += (ki * np.log(p) + (ni - ki) * np.log(1 - p)).astype(np.float32)
    best = np.unravel_index(np.argmax(ll), ll.shape)
    S = np.broadcast_to(s50, ll.shape); Wb = np.broadcast_to(w, ll.shape)
    G = np.broadcast_to(g, ll.shape); C = np.broadcast_to(c, ll.shape)
    ok = (ll.max() - ll) <= 1.92
    out = dict(floor=float(G[best]), ceil=float(C[best]), w=float(Wb[best]), s50_param=float(S[best]))
    for tag, pt in (("50", 0.5), ("90", 0.9)):
        def at(S_, W_, G_, C_):
            q = (pt - G_) / (C_ - G_)
            good = (q > 0.001) & (q < 0.999)
            return np.where(good, S_ + W_ * np.log(np.where(good, q, 0.5) / np.where(good, 1 - q, 0.5)), np.nan)
        v = at(S[best], Wb[best], G[best], C[best])
        vv = at(S[ok], Wb[ok], G[ok], C[ok])
        out["s" + tag] = None if np.isnan(v) else float(v)
        out["s%s_lo" % tag] = None if np.isnan(v) or np.all(np.isnan(vv)) else float(np.nanmin(vv))
        out["s%s_hi" % tag] = None if np.isnan(v) or np.all(np.isnan(vv)) else float(np.nanmax(vv))
        out["s%s_cobertura_ic" % tag] = float(np.mean(~np.isnan(vv)))  # fracción de la región que lo alcanza
    return out

def analiza(truth_path):
    T = json.load(open(truth_path)); base = truth_path[:-len(".truth.json")]
    from comun import PROTOS
    P = PROTOS[T["proto"]]; fs = T["fs"]
    cop = {(c["shard"], c["id"]): c for c in T["copies"]}
    first = {}; spurious = 0; nev = 0
    for s in range(len(T["shards"])):
        fn = "%s_s%d.out" % (base, s)
        if not os.path.exists(fn): continue
        for ln in open(fn, errors="replace"):
            if not ln.startswith("{"): continue
            try: e = json.loads(ln)
            except ValueError: continue
            if e.get("type") != "dec": continue
            nev += 1
            try: kid = P["key"](e["msg"])
            except (KeyError, ValueError, TypeError): spurious += 1; continue
            c = cop.get((s, kid))
            if c is None: spurious += 1; continue
            if (s, kid) not in first: first[(s, kid)] = e["t"]
    levels = T["levels"]; rows = []
    for lv in levels:
        cs = [c for c in T["copies"] if c["snr_db"] == lv]
        k = sum(1 for c in cs if (c["shard"], c["id"]) in first)
        lo, hi = wilson(k, len(cs)); rows.append(dict(snr=lv, n=len(cs), k=k, p=k / len(cs), lo=lo, hi=hi))
    r = dict(proto=T["proto"], bg=T["bg"], rows=rows, events=nev, spurious=spurious,
             real_bg_file=T.get("real_bg_file"), clipped=T["clipped_samples"], phi=T["phi_inband"],
             bw_hz=T["bw_hz"], fs=fs)
    r.update(fit(np.array([x["snr"] for x in rows]), np.array([x["k"] for x in rows]), np.array([x["n"] for x in rows])))
    # error de posición: t del evento (reloj de pared) menos el final de la copia;
    # el origen del reloj es desconocido -> se resta la mediana por shard (SNR alto)
    errs = {}
    for s in range(len(T["shards"])):
        d = [(first[(s, c["id"])] - c["end"] / fs, c["snr_db"]) for c in T["copies"]
             if c["shard"] == s and (s, c["id"]) in first]
        if len(d) < 5: continue
        s90 = r["s90"] if r["s90"] is not None else r["rows"][-1]["snr"]
        hi = [x for x, sn in d if sn >= s90] or [x for x, _ in d]
        med = float(np.median(hi))
        for x, sn in d: errs.setdefault(sn, []).append(x - med)
    allerr = np.concatenate([v for v in errs.values()]) if errs else np.array([0.0])
    s90 = r["s90"] if r["s90"] is not None else r["rows"][-1]["snr"]
    hiE = np.concatenate([v for sn, v in errs.items() if sn >= s90]) if any(sn >= s90 for sn in errs) else allerr
    r["pos_err_s"] = dict(n=int(len(hiE)), p05=float(np.percentile(hiE, 5)), p50=float(np.percentile(hiE, 50)),
                          p95=float(np.percentile(hiE, 95)), std=float(np.std(hiE)))
    return r

def main():
    work, outd = sys.argv[1], sys.argv[2]
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    res = [analiza(f) for f in sorted(glob.glob(os.path.join(work, "*.truth.json"))) if "_noise" not in f]
    fp = []
    for f in sorted(glob.glob(os.path.join(work, "*_noise.truth.json"))):
        T = json.load(open(f)); out = f[:-len(".truth.json")] + "_s0.out"
        n = sum(1 for ln in open(out, errors="replace") if ln.startswith('{"type":"dec"')) if os.path.exists(out) else None
        fp.append(dict(proto=T["proto"], bg=T["bg"], seconds=T["shards"][0]["seconds"], events=n))
    json.dump(fp, open(os.path.join(outd, "falsos_positivos.json"), "w"), indent=1)
    json.dump(res, open(os.path.join(outd, "resultados.json"), "w"), indent=1)
    with open(os.path.join(outd, "curvas.csv"), "w") as f:
        f.write("proto,bg,snr_db,n,k,p,wilson_lo,wilson_hi\n")
        for r in res:
            for x in r["rows"]:
                f.write("%s,%s,%g,%d,%d,%.4f,%.4f,%.4f\n" % (r["proto"], r["bg"], x["snr"], x["n"], x["k"], x["p"], x["lo"], x["hi"]))
    try:
        import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib no esta instalado: solo CSV"); return
    protos = sorted({r["proto"] for r in res}); fig, ax = plt.subplots(1, len(protos), figsize=(3.4 * len(protos), 3.4), sharey=True)
    ax = np.atleast_1d(ax); col = {"white": "#1f77b4", "real": "#d95f02"}
    for a, p in zip(ax, protos):
        for r in [r for r in res if r["proto"] == p]:
            s = np.array([x["snr"] for x in r["rows"]]); pr = np.array([x["p"] for x in r["rows"]])
            lo = np.array([x["lo"] for x in r["rows"]]); hi = np.array([x["hi"] for x in r["rows"]])
            c = col[r["bg"]]; a.errorbar(s, pr, [np.maximum(pr - lo, 0), np.maximum(hi - pr, 0)], fmt="o", ms=3, color=c, lw=0.8, label="fondo " + ("blanco" if r["bg"] == "white" else "real"))
            xs = np.linspace(s.min(), s.max(), 200); a.plot(xs, r["floor"] + (r["ceil"] - r["floor"]) / (1 + np.exp(-(xs - r["s50_param"]) / r["w"])), color=c, lw=1)
        a.set_title(p); a.set_xlabel("SNR en el ancho ocupado (dB)"); a.grid(alpha=.3)
    ax[0].set_ylabel("P(recuperar el mensaje)"); ax[0].legend(fontsize=7); fig.tight_layout()
    fig.savefig(os.path.join(outd, "curvas.png"), dpi=150)

if __name__ == "__main__":
    main()
