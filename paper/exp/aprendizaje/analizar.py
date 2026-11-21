"""analizar.py — tablas (tablas.md) y figuras a partir de resultados.json (media y desv. típica entre semillas)."""
import json, os, numpy as np
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
AQ = os.path.dirname(os.path.abspath(__file__)); R = {}
for f in sorted(__import__("glob").glob(os.path.join(AQ, "res_s*.json"))):
    for k, d in json.load(open(f)).items(): R.setdefault(k, {}).update(d)
json.dump(R, open(os.path.join(AQ, "resultados.json"), "w"), indent=1)
meta = R.pop("_meta"); V = ["A", "A2", "B3", "B3f2", "B3f5", "Bc", "Bo", "C", "E", "D"]; PR = ["ais", "aprs", "pocsag", "acars", "flex"]
NOM = {"A": "A  no etiquetado = fondo", "A2": "A2 A + excluir unknown", "B3": "B3 PU-propensión (curva P3, SNR host)",
       "B3f2": "B3 suelo e=0,2", "B3f5": "B3 suelo e=0,5", "E": "E  A2 + sintético", "Bc": "Bc PU-propensión (curva propia, SNR host corr.)", "Bo": "Bo PU-propensión (SNR verdadera)",
       "C": "C  solo sintético (blanco)", "D": "D  verdad completa (cota)"}
BINS = ["-5..0", "0..5", "5..10", "10..15", "15..20"]


def agg(v, f):
    xs = []
    for sd in R[v]:
        a = [f(R[v][sd][p]) for p in PR]; a = [q for q in a if q is not None and not np.isnan(q)]
        xs.append(np.mean(a))
    return np.mean(xs), np.std(xs)


def rb(tag, i): return lambda r: r[tag + "_recall_bins"][i]


out = []
out.append("| Variante | AP | AUC | prec@FPR1% | recall@FPR1% | recall@FPR1% SNR<5 dB | prec@0,5 | recall@0,5 |\n|---|---|---|---|---|---|---|---|")
for v in V:
    lo = []
    for sd in R[v]:
        a = []
        for p in PR:
            a += [q for q in R[v][sd][p]["fpr1_recall_bins"][:2] if q is not None]
        lo.append(np.mean(a))
    c = lambda f: "%.3f ± %.3f" % agg(v, f)
    out.append("| %s | %s | %s | %s | %s | %.3f ± %.3f | %s | %s |" % (
        NOM[v], c(lambda r: r["ap"]), c(lambda r: r["auc"]), c(lambda r: r["fpr1_prec"]), c(lambda r: r["fpr1_recall"]),
        np.mean(lo), np.std(lo), c(lambda r: r["t05_prec"]), c(lambda r: r["t05_recall"])))
for titulo, tag in (("Recall a FPR = 1 % por SNR verdadera de la ráfaga (media de 5 protocolos; ± entre semillas)", "fpr1"),
                    ("Recall al umbral natural 0,5 por SNR", "t05")):
    out.append("\n" + titulo + "\n")
    out.append("| Variante | " + " | ".join(BINS) + " |\n|---|" + "---|" * 5)
    for v in V:
        out.append("| %s | " % NOM[v] + " | ".join("%.2f ± %.2f" % agg(v, rb(tag, i)) for i in range(5)) + " |")
out.append("\nAP por protocolo (media de semillas)\n")
out.append("| Variante | " + " | ".join(PR) + " |\n|---|" + "---|" * 5)
for v in V:
    out.append("| %s | " % NOM[v] + " | ".join("%.3f" % np.mean([R[v][sd][p]["ap"] for sd in R[v]]) for p in PR) + " |")
out.append("\nMeta por semilla: " + json.dumps(meta))
open(os.path.join(AQ, "tablas.md"), "w").write("\n".join(out)); print("\n".join(out))
cols = dict(B3f2="#6baed6", B3f5="#9ecae1", E="#9467bd", A="#888", A2="#b59a00", B3="#1f77b4", Bc="#2ca02c", Bo="#17becf", C="#d62728", D="#000")
fig, ax = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
for k, tag in enumerate(("fpr1", "t05")):
    for v in V:
        m = [agg(v, rb(tag, i)) for i in range(5)]
        ax[k].errorbar(range(5), [a for a, _ in m], [b for _, b in m], label=v, color=cols[v], marker="o", capsize=2,
                       ls="--" if v == "D" else "-")
    ax[k].set_xticks(range(5)); ax[k].set_xticklabels(BINS); ax[k].set_xlabel("SNR verdadera (dB)"); ax[k].grid(alpha=.3)
    ax[k].set_title("recall a FPR 1 %" if tag == "fpr1" else "recall al umbral 0,5")
ax[0].set_ylabel("recall (media 5 protocolos)"); ax[1].legend(ncol=2, fontsize=8)
plt.tight_layout(); plt.savefig(os.path.join(AQ, "recall_snr.png"), dpi=140)
fig, ax = plt.subplots(figsize=(7, 3.8)); w = .085
for j, v in enumerate(V):
    ax.bar(np.arange(5) + (j - 4.5) * w, [np.mean([R[v][sd][p]["ap"] for sd in R[v]]) for p in PR], w, label=v, color=cols[v])
ax.set_xticks(range(5)); ax.set_xticklabels(PR); ax.set_ylabel("AP (ventanas de test)"); ax.legend(ncol=10, fontsize=7); ax.grid(alpha=.3, axis="y")
plt.tight_layout(); plt.savefig(os.path.join(AQ, "ap_proto.png"), dpi=140)
