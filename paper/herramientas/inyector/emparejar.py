#!/usr/bin/env python3
"""Empareja los eventos de `bladerf-lab --print` con la verdad de terreno del inyector.

Uso
  emparejar.py --truth salida.sigmf-meta --events eventos.jsonl [--out resultado.csv]
               [--tol-s 0.5] [--t0 UNIX_S] [--proto ais]
  emparejar.py --truth salida.sigmf-meta --decode-args     # imprime los --decode necesarios

Emparejado
  * Copias con ID (inj:has_id): por ID, extraído del mensaje con protos.PROTOS[...].key.
    Un evento con el ID de la copia basta, esté donde esté en el tiempo (los duplicados
    se cuentan en `n_events`).
  * Copias sin ID (o con --by-time): por tiempo y frecuencia. Los eventos llevan la hora
    de reloj ("t", ms) del bloque en que se decodificó, no el índice de muestra, así que
    hace falta el instante t0 de reloj en que arrancó la reproducción: se da con --t0 o se
    estima (por defecto) buscando el desfase que maximiza los emparejamientos con
    |t - t0 - inicio| <= tol-s. Además |f_evento - f_copia| <= max(bw/2, 5 kHz).
  * Si TODOS los eventos traen "sample" (índice de muestra de la fuente, `bladerf-lab --replay`),
    el tiempo del evento es sample/fs y no hace falta t0 (se fuerza 0): la latencia de bloque
    desaparece y dt_s es el error de posición. Es el caso normal desde que el binario lo escribe.
  * Eventos sin copia: filas con kind=extra (falsos positivos o duplicados de otra copia).

CSV (una fila por copia y una por evento sobrante), columnas:
  kind        copy | extra
  proto, inj_id, snr_db, cfo_hz, freq_hz, bw_hz   verdad de terreno (vacío en extra)
  start_s, dur_s   inicio y duración de la ráfaga (s desde el principio del fichero)
  recovered   1 si hay evento emparejado, 0 si no
  match_by    id | time_freq | none
  n_events    nº de eventos que casan con esa copia (>1 = duplicados)
  ev_t_s      hora del evento menos t0 (s, relativa al fichero)
  dt_s        ev_t_s - start_s (latencia + granularidad de bloque)
  ev_freq_hz  frecuencia del decodificador que emitió el evento
"""
import argparse, csv, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from protos import PROTOS

COLS = ["kind", "proto", "inj_id", "snr_db", "cfo_hz", "freq_hz", "bw_hz", "start_s", "dur_s",
        "recovered", "match_by", "n_events", "ev_t_s", "dt_s", "ev_freq_hz"]


def load_truth(path):
    m = json.load(open(path))
    fs = m["global"]["core:sample_rate"]
    cs = []
    for a in m["annotations"]:
        if "inj:proto" not in a: continue
        cs.append(dict(proto=a["inj:proto"], id=a["inj:id"], has_id=a["inj:has_id"], snr=a["inj:snr_db"],
                       cfo=a["inj:cfo_hz"], freq=a["inj:freq_hz"], bw=a["inj:bw_hz"],
                       start=a["core:sample_start"] / fs, dur=a["core:sample_count"] / fs))
    return m, fs, cs


def load_events(path):
    ev = []
    for ln in open(path):
        ln = ln.strip()
        if not ln.startswith("{"): continue
        try: j = json.loads(ln)
        except ValueError: continue
        if j.get("type") == "dec" and not j.get("replay"):
            ev.append(j)
    return ev


def ev_id(e):
    p = PROTOS.get(e["proto"])
    if not p: return None
    try: return int(p["key"](e["msg"]))
    except (KeyError, ValueError, TypeError, IndexError): return None


def estimate_t0(copies, events, tol):
    """Desfase de reloj que maximiza los emparejamientos por tiempo (ver cabecera)."""
    if not events or not copies: return 0.0
    st = np.array([c["start"] for c in copies]); et = np.array([e["t"] for e in events])
    cand = np.unique(np.round((et[:, None] - st[None, :]).ravel(), 2))
    sc = []
    for o in cand:
        d = et[:, None] - o - st[None, :]
        sc.append(np.sum(np.any((d >= -0.05) & (d <= tol), axis=0)))
    sc = np.array(sc)
    # empates (todo desfase dentro de la tolerancia puntúa igual): mediana de los óptimos
    return float(np.median(cand[sc == sc.max()]))


def match(copies, events, t0, tol_s, by_time=False):
    used = set(); res = []
    # 1) por ID
    by_id = {}
    for k, e in enumerate(events):
        i = ev_id(e)
        if i is not None: by_id.setdefault((e["proto"], i), []).append(k)
    for c in copies:
        c["ev"] = []; c["by"] = "none"
        if c["has_id"] and not by_time:
            ks = by_id.get((c["proto"], c["id"]), [])
            if ks: c["ev"] = ks; c["by"] = "id"; used.update(ks)
    # 2) por tiempo y frecuencia (copias sin ID o con --by-time)
    for c in sorted(copies, key=lambda c: c["start"]):
        if c["ev"]: continue
        best, bd = None, 1e9
        for k, e in enumerate(events):
            if k in used or e["proto"] != c["proto"]: continue
            d = e["t"] - t0 - c["start"]
            if -0.05 <= d <= c["dur"] + tol_s and abs(e["freq"] - c["freq"]) <= max(c["bw"] / 2, 5000):
                if abs(d) < bd: best, bd = k, abs(d)
        if best is not None: c["ev"] = [best]; c["by"] = "time_freq"; used.add(best)
    return used


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--truth", required=True); ap.add_argument("--events")
    ap.add_argument("--out", help="CSV (por defecto stdout)")
    ap.add_argument("--tol-s", type=float, default=0.5)
    ap.add_argument("--t0", type=float, help="hora Unix de arranque de la reproducción")
    ap.add_argument("--proto", help="solo este protocolo")
    ap.add_argument("--by-time", action="store_true", help="ignora los ID y empareja por tiempo/frecuencia")
    ap.add_argument("--decode-args", action="store_true", help="imprime los --decode necesarios y sale")
    a = ap.parse_args()
    meta, fs, copies = load_truth(a.truth)
    if a.proto: copies = [c for c in copies if c["proto"] == a.proto]
    if a.decode_args:
        print(" ".join("--decode %s@%d" % (p, f) for p, f in sorted({(c["proto"], round(c["freq"])) for c in copies})))
        return
    if not a.events: raise SystemExit("falta --events")
    events = load_events(a.events)
    if a.proto: events = [e for e in events if e["proto"] == a.proto]
    t0 = a.t0
    if events and all("sample" in e.get("msg", {}) for e in events):
        for e in events: e["t"] = e["msg"]["sample"] / fs   # tiempo de señal, no de reloj
        t0 = 0.0
    if t0 is None:
        # con ID: mediana de (t - inicio) de los emparejados; si no, búsqueda por tiempo
        ds = []
        idx = {(c["proto"], c["id"]): c for c in copies if c["has_id"]}
        for e in events:
            i = ev_id(e); c = idx.get((e["proto"], i))
            if c and not a.by_time: ds.append(e["t"] - c["start"])
        t0 = float(np.median(ds)) - 0.0 if ds else estimate_t0(copies, events, a.tol_s)
        if ds:  # la mediana incluye la latencia; usa el mínimo robusto (percentil 10) como origen
            t0 = float(np.percentile(ds, 10))
    used = match(copies, events, t0, a.tol_s, a.by_time)
    fo = open(a.out, "w", newline="") if a.out else sys.stdout
    w = csv.writer(fo); w.writerow(COLS)
    for c in sorted(copies, key=lambda c: c["start"]):
        e = events[c["ev"][0]] if c["ev"] else None
        w.writerow(["copy", c["proto"], c["id"], c["snr"], c["cfo"], round(c["freq"]), round(c["bw"]),
                    "%.4f" % c["start"], "%.4f" % c["dur"], int(bool(c["ev"])), c["by"], len(c["ev"]),
                    "%.3f" % (e["t"] - t0) if e else "", "%.3f" % (e["t"] - t0 - c["start"]) if e else "",
                    e["freq"] if e else ""])
    for k, e in enumerate(events):
        if k not in used:
            w.writerow(["extra", e["proto"], "", "", "", "", "", "", "", 0, "none", 1,
                        "%.3f" % (e["t"] - t0), "", e["freq"]])
    if a.out: fo.close()
    rec = sum(1 for c in copies if c["ev"])
    print("recuperadas %d/%d (%.1f %%), eventos sobrantes %d, t0=%.3f" % (
        rec, len(copies), 100.0 * rec / max(len(copies), 1), len(events) - len(used), t0), file=sys.stderr)


if __name__ == "__main__":
    main()
