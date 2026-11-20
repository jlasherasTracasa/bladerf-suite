#!/usr/bin/env python3
"""Test del inyector: 20 copias de ais y 20 de pocsag en ruido blanco a 20 dB.

Ejecutar:  python3 -I paper/herramientas/inyector/test_inyector.py [ruta/a/bladerf-lab]
Comprueba (1) que >= 95 % de las copias se recuperan por ID con el binario real,
(2) que las anotaciones caen donde se pusieron: la energía en banda dentro de la
anotación supera a la de un tramo vacío del mismo tamaño en > 10 dB, y la SNR local
medida a posteriori coincide con `inj:snr_db` (±1,5 dB), y (3) que la hora de cada
evento, tras alinear el reloj, cae en [inicio - 1 bloque, fin + 1 bloque + latencia].
Granularidad actual: el evento lleva la hora de reloj del bloque (ver emparejar.py),
un bloque = ceil(fs/50 a múltiplo de 1024) >= 16384 muestras (65,5 ms a 250 kS/s).
Se reproduce en tiempo real: ~20 s por protocolo (en paralelo).
"""
import json, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import inyectar, emparejar

ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/bladerf-lab")
FS, CENTER, SIGMA, SEC, N = 250000, 162000000, 60.0, 18.0, 20
BLOCK = max(16384, (FS // 50 + 1023) // 1024 * 1024) / FS
LATENCY = 0.6    # s: el decodificador emite al acabar el mensaje (POCSAG: tras el lote)
fails = []


def check(cond, msg):
    print(("ok   " if cond else "FALLO ") + msg)
    if not cond: fails.append(msg)


def run(proto, k, tmp):
    pre = os.path.join(tmp, proto)
    freq = 161975000 if proto == "ais" else 162050000
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "inyectar.py"), "--white",
                        "%d,%d,%g" % (FS, CENTER, SIGMA), "--seconds", str(SEC), "--proto", "%s:%d:%d" % (proto, N, freq),
                        "--snr", "20", "--cfo", "500", "--seed", str(100 + k), "--bin", BIN, "--out", pre],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    ev = subprocess.run([BIN, "--replay", pre + ".sigmf-meta", "--decode", "%s@%d" % (proto, freq), "--print",
                         "--duration", str(SEC + 0.5), "--config", "none", "--port", str(8970 + k)],
                        capture_output=True, text=True).stdout
    open(pre + ".jsonl", "w").write(ev)
    return pre


def verify(proto, pre):
    meta, fs, copies = emparejar.load_truth(pre + ".sigmf-meta")
    check(len(copies) == N, "%s: %d anotaciones" % (proto, len(copies)))
    events = emparejar.load_events(pre + ".jsonl")
    ds = [e["t"] - next(c for c in copies if c["id"] == emparejar.ev_id(e))["start"]
          for e in events if emparejar.ev_id(e) is not None and emparejar.ev_id(e) < N + 100000 and
          any(c["id"] == emparejar.ev_id(e) for c in copies)]
    t0 = float(np.percentile(ds, 10)) if ds else 0.0
    emparejar.match(copies, events, t0, 0.5)
    rec = sum(1 for c in copies if c["ev"])
    check(rec >= 0.95 * N, "%s: recuperadas %d/%d (>= 95 %%)" % (proto, rec, N))
    # anotaciones donde se pusieron: potencia en banda dentro vs. tramo vacío
    d = np.fromfile(pre + ".sigmf-data", np.int16).astype(float)
    x = d[0::2] + 1j * d[1::2]
    ann = meta["annotations"]
    pn_exp = SIGMA ** 2
    for a_ in ann:
        s, n = a_["core:sample_start"], a_["core:sample_count"]
        f_off = a_["inj:freq_hz"] + a_["inj:cfo_hz"] - CENTER
        bw = a_["inj:bw_hz"]
        p_in = inyectar.band_power(x[s:s + n], fs, f_off, bw)
        e = s - n - 3 * int(0.002 * fs)      # tramo previo de igual longitud (puede solapar otra copia: se salta)
        ok = n > 0 and a_["core:freq_upper_edge"] - a_["core:freq_lower_edge"] == bw
        if e >= 0 and not any(abs(b["core:sample_start"] - e) < 2 * n for b in ann if b is not a_):
            p_out = inyectar.band_power(x[e:e + n], fs, f_off, bw)
            ok = ok and 10 * np.log10(p_in / p_out) > 10
        snr_meas = 10 * np.log10(max(p_in - a_["inj:noise_power"], 1e-9) / a_["inj:noise_power"])
        ok = ok and abs(snr_meas - a_["inj:snr_db"]) < 1.5
        if not ok:
            check(False, "%s: anotación %s mal colocada (SNR medida %.1f dB)" % (proto, a_["core:label"], snr_meas))
    # hora de los eventos dentro de la ventana de la copia +- 1 bloque
    bad = 0
    for c in copies:
        if c["ev"]:
            dt = events[c["ev"][0]]["t"] - t0 - c["start"]
            if not (-BLOCK <= dt <= c["dur"] + BLOCK + LATENCY): bad += 1
    check(bad == 0, "%s: tiempos de evento en [inicio-1 bloque, fin+1 bloque+%.1f s] (%d fuera)" % (proto, LATENCY, bad))
    check(meta["global"].get("inj:seed") in (100, 101), "%s: semilla en el meta global" % proto)


if __name__ == "__main__":
    if not os.path.exists(BIN): raise SystemExit("no existe " + BIN)
    with tempfile.TemporaryDirectory() as tmp:
        with ThreadPoolExecutor(2) as ex:
            pres = list(ex.map(lambda t: run(t[1], t[0], tmp), enumerate(["ais", "pocsag"])))
        for p, pre in zip(["ais", "pocsag"], pres): verify(p, pre)
    print("TODO OK" if not fails else "%d fallos" % len(fails))
    sys.exit(1 if fails else 0)
