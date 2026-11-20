"""Definición común de los 5 protocolos del piloto (generación, emparejado).

Cada protocolo se genera con `bladerf-lab --gen` variando un identificador
único por copia (idx), que el decodificador devuelve en el evento: así cada
copia inyectada se empareja de forma exacta con su evento.
"""
import json

# rate/center/freq en Hz. bw = «ancho ocupado» que imprime --gen (Hz).
# El nº de ráfagas por copia es 1, salvo ADS-B, donde se recorta la 1.ª trama.
PROTOS = {
    "ais": dict(rate=250000, center=162000000, freq=161975000, bw=18562,
                real_bg="ism_f007th", gap=(0.08, 0.12), cfo=1000.0,
                params=lambda i: {"mmsi": 200000000 + i, "types": [1]},
                key=lambda m: m["mmsi"] - 200000000),
    "pocsag": dict(rate=250000, center=466100000, freq=466075000, bw=21703,
                   real_bg="ism_f007th", gap=(0.08, 0.12), cfo=1000.0,
                   params=lambda i: {"baud": 2400, "messages": [
                       {"ric": 1000000 + i, "func": 3, "text": "T%05d" % i}]},
                   key=lambda m: m["ric"] - 1000000),
    "aprs": dict(rate=250000, center=144825000, freq=144800000, bw=13922,
                 real_bg="ism_f007th", gap=(0.08, 0.12), cfo=1000.0,
                 params=lambda i: {"callsign": "EA1ABC", "type": "position",
                                   "comment": "N%05d" % i},
                 key=lambda m: int(m["comment"][1:6])),
    "lora": dict(rate=1000000, center=868000000, freq=868100000, bw=125000,
                 real_bg="lte", gap=(0.05, 0.10), cfo=1000.0,
                 params=lambda i: {"sf": 7, "bw": 125000, "repeat": 1,
                                   "lorawan": {"fcnt": i}},
                 key=lambda m: m["lorawan"]["fcnt"]),
    "adsb": dict(rate=2000000, center=1090000000, freq=1090000000, bw=2000000,
                 real_bg="lte", gap=(0.02, 0.03), cfo=0.0,
                 params=lambda i: {"icao": "%06X" % (0x100000 + i), "repeat": 1},
                 key=lambda m: int(m["icao"], 16) - 0x100000,
                 trim=(400, 400 + 240)),   # solo la 1.ª trama (identificación)
}

REAL_BG = {  # grabaciones reales de tests/data/real/ usadas como fondo
    "ism_f007th": dict(file="ism_f007th", dtype="cu8"),
    "lte": dict(file="lte", dtype="ci8"),
}

def dec_arg(p):
    f = PROTOS[p]["freq"]
    return p if p == "adsb" else "%s@%d" % (p, f)

def dumps(o):
    return json.dumps(o, indent=1, sort_keys=True)
