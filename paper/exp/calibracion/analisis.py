#!/usr/bin/env python3
"""Análisis de la calibración por inyección (P3): lee datos/*.csv y *.json (calib.py) y escribe
resultados.json, curvas.csv, resumen.csv, curvas.png, curvas_emparejados.png, ocupacion.png, errores.png.

Definiciones (detalle y limitaciones en CALIBRACION.md)
  * Punto = (protocolo, fondo, condición, SNR inyectada). Se agregan las 3 semillas.
    Las copias que caen cerca de un hueco del flujo (`en_hueco`=1, bloques descartados por el
    reproductor) no miden al decodificador y se excluyen (se cuentan aparte).
  * IC 95 %: Wilson por punto. Curva: p = g + (c-g)·sigmoide((x-s50p)/w) por máxima verosimilitud
    binomial; SNR50/SNR90 = donde la curva ajustada vale 0,5 / 0,9 en términos ABSOLUTOS;
    IC 95 % por bootstrap (200 remuestreos de copias, percentiles 2,5 y 97,5).
  * Error de posición: evento.sample − inicio de la ráfaga activa inyectada, en símbolos
    (muestras / fs · símbolos por segundo). Se informa sesgo (media), desviación y p95 del valor
    absoluto, con SNR >= SNR90 (o >= 15 dB si no hay curva).
  * Error de SNR: evento.snr_db − SNR inyectada (SNR local en la banda ocupada); regresión lineal.
  * Solo numpy + matplotlib. Ajuste por Nelder-Mead propio (sin scipy).
"""
import csv, glob, json, math, os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

AQUI = os.path.dirname(os.path.abspath(__file__))
DATOS = os.path.join(AQUI, "datos")
sys.path.insert(0, AQUI)
SYM = {"ais": 9600.0, "aprs": 1200.0, "pocsag": 2400.0, "lora": 125000 / 128.0, "adsb": 1e6,
       "acars": 2400.0, "rs41": 4800.0, "flex": 1600.0, "dsc": 1200.0, "ism": 10000.0}
PRINC = ["ais", "adsb", "aprs", "lora", "pocsag"]
EMPAR = ["acars", "rs41", "flex", "dsc", "ism"]            # emparejados por tiempo (sin identificador)
COL = {"white": "#0072B2", "real": "#D55E00", "real_rf": "#D55E00", "real_rec": "#009E73"}
CCOL = {"none": "#0072B2", "ctl": "#0072B2", "adj": "#E69F00", "co0": "#D55E00", "co-6": "#CC79A7"}
NBOOT = int(os.environ.get("NBOOT", 200))


# ------------------------------------------------------------------ carga --
def carga():
    ser = {}; meta = {}
    for f in sorted(glob.glob(os.path.join(DATOS, "*.csv"))):
        if os.path.basename(f).startswith("fp_"): continue
        j = json.load(open(f[:-4] + ".json")); meta[j["tag"]] = j
        for r in csv.DictReader(open(f)):
            k = (r["proto"], r["bg"], r["cond"])
            fl = lambda s: float(s) if s != "" else float("nan")
            ser.setdefault(k, []).append((int(r["seed"]), float(r["snr_inj"]), int(r["rec"]), fl(r["dsample"]),
                                          fl(r["ev_snr"]), fl(r["ev_cfo"]), float(r["cfo_inj"]), int(r["en_hueco"]),
                                          int(r["len"]), j["fs"], r["fichero_fondo"], int(r["n_ev"])))
    out = {}
    for k, v in ser.items():
        a = np.array([x[:9] + (x[9],) for x in v], float)
        out[k] = dict(seed=a[:, 0], snr=a[:, 1], rec=a[:, 2], ds=a[:, 3], esnr=a[:, 4], ecfo=a[:, 5], cfo=a[:, 6],
                      gap=a[:, 7], len=a[:, 8], fs=a[:, 9], fich=np.array([x[10] for x in v]), nev=np.array([x[11] for x in v]))
    return out, meta


def wilson(k, n, z=1.96):
    if n == 0: return (float("nan"),) * 2
    p = k / n; d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d; h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, c - h), min(1.0, c + h)


def bins(snr, rec):
    out = []
    for x in np.unique(snr):
        m = snr == x; n = int(m.sum()); k = int(rec[m].sum()); lo, hi = wilson(k, n)
        out.append(dict(snr=float(x), n=n, k=k, p=k / n, lo=lo, hi=hi))
    return out


# ------------------------------------------------------------------ ajuste --
def curva(x, th):
    g, c, s, w = th
    return g + (c - g) / (1 + np.exp(-(x - s) / w))


def nll(th, x, n, k):
    g, c, s, w = th
    if not (0 <= g <= 0.6 and 0.4 <= c <= 1 and 0.15 <= w <= 12 and g < c): return 1e18
    p = np.clip(curva(x, th), 1e-9, 1 - 1e-9)
    return -float(np.sum(k * np.log(p) + (n - k) * np.log(1 - p)))


def nelder(f, x0, step, it=600):
    n = len(x0); P = [np.array(x0, float)]
    for i in range(n):
        q = np.array(x0, float); q[i] += step[i]; P.append(q)
    F = [f(p) for p in P]
    for _ in range(it):
        o = np.argsort(F); P = [P[i] for i in o]; F = [F[i] for i in o]
        c = np.mean(P[:-1], axis=0); r = c + (c - P[-1]); fr = f(r)
        if fr < F[0]:
            e = c + 2 * (c - P[-1]); fe = f(e)
            P[-1], F[-1] = (e, fe) if fe < fr else (r, fr)
        elif fr < F[-2]: P[-1], F[-1] = r, fr
        else:
            t = c + 0.5 * (P[-1] - c); ft = f(t)
            if ft < F[-1]: P[-1], F[-1] = t, ft
            else:
                for i in range(1, n + 1): P[i] = P[0] + 0.5 * (P[i] - P[0]); F[i] = f(P[i])
        if abs(F[-1] - F[0]) < 1e-9: break
    i = int(np.argmin(F)); return P[i], F[i]


def ajusta(x, n, k, ini=None):
    """Devuelve (g, c, s50p, w) o None. `ini`: arranque único (bootstrap, a partir del ajuste principal)."""
    if ini is not None:
        th, v = nelder(lambda t: nll(t, x, n, k), list(ini), [0.03, 0.02, 1.0, 0.5], 300)
        return th if v < 1e17 else None
    mid = x[np.argmin(np.abs(k / n - 0.5))]
    best = None
    for s0 in (mid, mid - 4, mid + 4):
        for g0 in (0.0, 0.15):
            th, v = nelder(lambda t: nll(t, x, n, k), [g0, 0.97, s0, 2.0], [0.05, 0.03, 3.0, 1.0])
            if best is None or v < best[1]: best = (th, v)
    return best[0] if best and best[1] < 1e17 else None


def inversa(th, p):
    g, c, s, w = th
    if not (g < p < c): return float("nan")
    return s + w * math.log((p - g) / (c - p))


def umbrales(snr, rec, rng):
    xs = np.unique(snr); n = np.array([(snr == x).sum() for x in xs], float)
    k = np.array([rec[snr == x].sum() for x in xs], float)
    th = ajusta(xs, n, k)
    if th is None: return None
    res = dict(g=float(th[0]), c=float(th[1]), s50p=float(th[2]), w=float(th[3]),
               s50=inversa(th, 0.5), s90=inversa(th, 0.9))
    b50, b90 = [], []
    idx = {x: np.nonzero(snr == x)[0] for x in xs}
    for _ in range(NBOOT):
        kb = np.array([rec[rng.choice(idx[x], len(idx[x]))].sum() for x in xs], float)
        t = ajusta(xs, n, kb, th)
        if t is not None: b50.append(inversa(t, 0.5)); b90.append(inversa(t, 0.9))
    for nm, b in (("s50", b50), ("s90", b90)):
        b = [v for v in b if not math.isnan(v)]
        res[nm + "_lo"], res[nm + "_hi"] = (float(np.percentile(b, 2.5)), float(np.percentile(b, 97.5))) if len(b) > 20 else (float("nan"),) * 2
        res[nm + "_cobertura"] = len(b) / NBOOT
    return res


# ------------------------------------------------------------------ errores --
def pct(v, q): return float(np.percentile(v, q)) if len(v) else float("nan")


def errores(d, fit, proto):
    lim = (fit["s90"] if fit and not math.isnan(fit["s90"]) else 15.0)
    sym = SYM[proto]
    ok = (d["rec"] == 1) & (d["gap"] == 0) & ~np.isnan(d["ds"])
    m = ok & (d["snr"] >= lim)
    ds = d["ds"][m] / d["fs"][m] * sym
    e = dict(n_pos=int(m.sum()), snr_min=float(lim))
    if len(ds):
        b = float(np.mean(ds))
        e.update(pos_bias_sym=b, pos_med_sym=float(np.median(ds)), pos_std_sym=float(np.std(ds)),
                 pos_abs_p95_sym=pct(np.abs(ds), 95), pos_cent_p95_sym=pct(np.abs(ds - b), 95),
                 pos_bias_ms=float(np.mean(d["ds"][m] / d["fs"][m]) * 1e3))
    ms = (d["rec"] == 1) & (d["gap"] == 0) & ~np.isnan(d["esnr"]) & (d["snr"] >= -5) & (d["snr"] <= 22)
    if ms.sum() > 10:
        a = np.polyfit(d["snr"][ms], d["esnr"][ms], 1); r = d["esnr"][ms] - np.polyval(a, d["snr"][ms])
        e.update(snr_n=int(ms.sum()), snr_slope=float(a[0]), snr_offset_db=float(a[1]),
                 snr_bias_db=float(np.mean(d["esnr"][ms] - d["snr"][ms])), snr_res_std_db=float(np.std(r)),
                 snr_abs_p95_db=pct(np.abs(d["esnr"][ms] - d["snr"][ms]), 95))
    mc = (d["rec"] == 1) & (d["gap"] == 0) & ~np.isnan(d["ecfo"]) & (d["snr"] >= lim)
    if mc.sum() > 5:
        ec = d["ecfo"][mc] - d["cfo"][mc]
        e.update(cfo_n=int(mc.sum()), cfo_bias_hz=float(np.mean(ec)), cfo_std_hz=float(np.std(ec)), cfo_abs_p95_hz=pct(np.abs(ec), 95))
    return e


# -------------------------------------------------------------- falsos positivos --
def poisson_up(k):
    if k == 0: return 2.996
    return (k + 1) * (1 - 1 / (9 * (k + 1)) + 1.645 / (3 * math.sqrt(k + 1))) ** 3


def falsos_positivos(meta):
    out = []
    for f in sorted(glob.glob(os.path.join(DATOS, "fp_*.json"))):
        for o in json.load(open(f)):
            h = o["segundos"] / 3600.0
            out.append(dict(proto=o["proto"], fondo=o["fondo"], segundos=o["segundos"], eventos=o["eventos"],
                            por_hora=o["eventos"] / h if h else float("nan"), por_hora_ic95_sup=poisson_up(o["eventos"]) / h if h else float("nan")))
    aj = {}
    for t, j in meta.items():
        if j["cond"] != "none": continue
        a = aj.setdefault((j["proto"], "blanco" if j["bg"] == "white" else "real"), [0, 0.0])
        a[0] += j["eventos_ajenos"]; a[1] += j["segundos_senal"]
    ajenos = [dict(proto=k[0], fondo=k[1], eventos_ajenos=v[0], segundos_con_copias=v[1],
                   por_hora=(v[0] / (v[1] / 3600)) if v[1] else float("nan")) for k, v in sorted(aj.items())]
    return out, ajenos


# -------------------------------------------------------------------- piloto --
def piloto():
    f = os.path.join(AQUI, "../piloto/resultados.json")
    if not os.path.exists(f): return {}
    out = {}
    for r in json.load(open(f)):
        out[(r["proto"], r["bg"])] = dict(floor=r.get("floor"), ceil=r.get("ceil"), s50=r.get("s50"), s90=r.get("s90"),
                                          p_bajo=float(np.mean([x["p"] for x in r["rows"] if x["snr"] <= -2])) if r["rows"] and min(x["snr"] for x in r["rows"]) <= -2 else None)
    return out


def main():
    rng = np.random.default_rng(20261120)
    D, meta = carga()
    res = []; curv = []
    for (proto, bg, cond) in sorted(D):
        d = D[(proto, bg, cond)]
        v = d["gap"] == 0
        sn, rc = d["snr"][v], d["rec"][v]
        bi = bins(sn, rc)
        fit = umbrales(sn, rc, rng)
        pl = rc[sn >= 20]; pb = rc[sn <= -5]
        r = dict(proto=proto, bg=bg, cond=cond, emparejado="tiempo" if proto in EMPAR else "id", semillas=sorted(set(int(s) for s in d["seed"])),
                 copias=int(len(d["snr"])), excluidas_en_hueco=int((~v).sum()), fit=fit,
                 p_meseta_ge20=float(pl.mean()) if len(pl) else None, n_meseta=int(len(pl)),
                 p_meseta_ic=list(wilson(int(pl.sum()), len(pl))) if len(pl) else None,
                 p_bajo_le_m5=float(pb.mean()) if len(pb) else None, n_bajo=int(len(pb)),
                 p_bajo_ic=list(wilson(int(pb.sum()), len(pb))) if len(pb) else None,
                 fs=float(d["fs"][0]), errores=errores(d, fit, proto),
                 ajenos=sum(j["eventos_ajenos"] for t, j in meta.items() if (j["proto"], j["bg"], j["cond"]) == (proto, bg, cond)),
                 fondos=sorted(set(d["fich"])) if bg != "white" else ["blanco"], bins=bi)
        res.append(r)
        for b in bi: curv.append([proto, bg, cond, b["snr"], b["n"], b["k"], round(b["p"], 4), round(b["lo"], 4), round(b["hi"], 4)])
    fp, ajenos = falsos_positivos(meta)
    pil = piloto()
    json.dump(dict(resultados=res, falsos_positivos=fp, ajenos_con_copias=ajenos,
                   piloto={"%s/%s" % k: v for k, v in pil.items()}), open(os.path.join(AQUI, "resultados.json"), "w"), indent=1, default=float)
    with open(os.path.join(AQUI, "curvas.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(["proto", "bg", "cond", "snr_local_db", "n", "k", "p", "wilson_lo", "wilson_hi"]); w.writerows(curv)
    nan = lambda x: "" if x is None or (isinstance(x, float) and math.isnan(x)) else round(x, 3)
    with open(os.path.join(AQUI, "resumen.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["proto", "bg", "cond", "emparejado", "copias", "s50", "s50_lo", "s50_hi", "s90", "s90_lo", "s90_hi", "suelo", "techo",
                    "p_snr_ge20", "p_snr_le_m5", "pos_bias_sym", "pos_std_sym", "pos_abs_p95_sym", "snr_offset_db", "snr_slope", "snr_res_std_db",
                    "cfo_std_hz", "ajenos"])
        for r in res:
            ft = r["fit"] or {}; e = r["errores"]
            w.writerow([r["proto"], r["bg"], r["cond"], r["emparejado"], r["copias"]] + [nan(ft.get(k)) for k in ("s50", "s50_lo", "s50_hi", "s90", "s90_lo", "s90_hi", "g", "c")]
                       + [nan(r["p_meseta_ge20"]), nan(r["p_bajo_le_m5"])] + [nan(e.get(k)) for k in ("pos_bias_sym", "pos_std_sym", "pos_abs_p95_sym", "snr_offset_db", "snr_slope", "snr_res_std_db", "cfo_std_hz")]
                       + [r["ajenos"]])
    graficas(res)
    print("analizado:", len(res), "series ->", os.path.join(AQUI, "resultados.json"))


# ---------------------------------------------------------------- gráficas --
def pinta(ax, r, color, label, ls="-"):
    x = [b["snr"] for b in r["bins"]]; p = [b["p"] for b in r["bins"]]
    ax.fill_between(x, [b["lo"] for b in r["bins"]], [b["hi"] for b in r["bins"]], color=color, alpha=0.18, lw=0)
    ax.plot(x, p, ".", ms=3.5, color=color)
    if r["fit"]:
        xx = np.linspace(min(x), max(x), 200); ax.plot(xx, curva(xx, (r["fit"]["g"], r["fit"]["c"], r["fit"]["s50p"], r["fit"]["w"])), ls, color=color, lw=1.6, label=label)


def graficas(res):
    def panel(protos, fn, titulo, ncol):
        fig, axs = plt.subplots(math.ceil(len(protos) / ncol), ncol, figsize=(4.2 * ncol, 3.2 * math.ceil(len(protos) / ncol)), squeeze=False)
        for ax, p in zip(axs.flat, protos):
            for r in res:
                if r["proto"] == p and r["cond"] in ("none",):
                    pinta(ax, r, COL.get(r["bg"], "#555"), {"white": "ruido blanco", "real": "fondo real", "real_rf": "real: RF (ISM)", "real_rec": "real: grabaciones"}.get(r["bg"], r["bg"]))
            ax.axhline(0.5, color="#999", lw=0.5, ls=":"); ax.axhline(0.9, color="#999", lw=0.5, ls=":")
            ax.set_title(p + (" (por tiempo)" if p in EMPAR else ""), fontsize=10); ax.set_xlabel("SNR local en la banda ocupada (dB)"); ax.set_ylabel("P(etiquetado)")
            ax.set_ylim(-0.02, 1.03); ax.grid(alpha=0.2); ax.legend(fontsize=7, loc="lower right")
        for ax in list(axs.flat)[len(protos):]: ax.axis("off")
        fig.suptitle(titulo, fontsize=11); fig.tight_layout(); fig.savefig(os.path.join(AQUI, fn), dpi=130); plt.close(fig)
    panel(PRINC, "curvas.png", "P(etiquetado | SNR local), 3 semillas, IC Wilson 95 %", 3)
    panel(EMPAR, "curvas_emparejados.png", "Emparejados por tiempo (sin identificador): P(etiquetado | SNR local)", 3)
    # ocupación
    fig, axs = plt.subplots(2, 3, figsize=(12.6, 6.4), squeeze=False)
    for ax, p in zip(axs.flat, PRINC):
        for r in res:
            if r["proto"] == p and r["bg"] == "white" and r["cond"] in ("ctl", "adj", "co0", "co-6"):
                pinta(ax, r, CCOL[r["cond"]], {"ctl": "sin interferente", "adj": "adyacente +10 dB", "co0": "cocanal 0 dB", "co-6": "cocanal -6 dB"}[r["cond"]])
        ax.set_title(p); ax.set_xlabel("SNR del objetivo (dB)"); ax.set_ylabel("P(etiquetado)"); ax.set_ylim(-0.02, 1.03); ax.grid(alpha=0.2); ax.legend(fontsize=7, loc="lower right")
    axs.flat[-1].axis("off"); fig.suptitle("Efecto de la ocupación (copias solapadas en tiempo; el objetivo ocupa el mismo canal o el adyacente)", fontsize=11)
    fig.tight_layout(); fig.savefig(os.path.join(AQUI, "ocupacion.png"), dpi=130); plt.close(fig)
    # errores: SNR medida vs inyectada y posición
    D, _ = carga()
    fig, axs = plt.subplots(2, 5, figsize=(17, 6.4), squeeze=False)
    for j, p in enumerate(PRINC):
        for bg in ("white", "real"):
            d = D.get((p, bg, "none"))
            if d is None: continue
            ok = (d["rec"] == 1) & (d["gap"] == 0)
            m = ok & ~np.isnan(d["esnr"])
            axs[0][j].plot(d["snr"][m], d["esnr"][m], ".", ms=2, color=COL[bg], alpha=0.4, label=bg)
            m = ok & ~np.isnan(d["ds"]) & (d["snr"] >= 15)
            axs[1][j].hist(d["ds"][m] / d["fs"][m] * SYM[p], bins=40, color=COL[bg], alpha=0.55, label=bg)
        axs[0][j].plot([-10, 25], [-10, 25], "k--", lw=0.6); axs[0][j].set_title(p); axs[0][j].set_xlabel("SNR inyectada (dB)"); axs[0][j].set_ylabel("snr_db del evento")
        axs[1][j].set_xlabel("error de sample (símbolos), SNR>=15 dB"); axs[1][j].set_ylabel("copias")
        for a in (axs[0][j], axs[1][j]): a.grid(alpha=0.2); a.legend(fontsize=7)
    fig.tight_layout(); fig.savefig(os.path.join(AQUI, "errores.png"), dpi=120); plt.close(fig)


if __name__ == "__main__":
    main()
