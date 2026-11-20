"""Tabla de protocolos del inyector: parámetros de --gen con ID único y su extractor.

Para cada protocolo con identificador embebible:
  params(i)  -> dict de --gen-params cuyo mensaje contiene el ID entero `i`.
  key(msg)   -> ID entero recuperado del campo "msg" de un evento --print (o
                lanza excepción si el mensaje no lo trae).
Los que no están aquí se inyectan con los parámetros por defecto del modulador
(--gen-params omitido, o los que dé el usuario) y se emparejan por tiempo/frecuencia.
Los identificadores se eligen en rangos válidos del protocolo (MMSI de 9 cifras,
RIC de 7 bits de dirección, ICAO de 24 bits, FCnt de 16 bits).
"""
PROTOS = {
    "ais": dict(params=lambda i: {"mmsi": 200000000 + i, "types": [1]},
                key=lambda m: m["mmsi"] - 200000000),
    "pocsag": dict(params=lambda i: {"baud": 2400, "messages": [
                       {"ric": 1000000 + i, "func": 3, "text": "T%05d" % i}]},
                   key=lambda m: m["ric"] - 1000000),
    "aprs": dict(params=lambda i: {"callsign": "EA1ABC", "type": "position", "comment": "N%05d" % i},
                 key=lambda m: int(m["comment"][1:6])),
    "lora": dict(params=lambda i: {"sf": 7, "bw": 125000, "repeat": 1, "lorawan": {"fcnt": i}},
                 key=lambda m: m["lorawan"]["fcnt"]),
    "adsb": dict(params=lambda i: {"icao": "%06X" % (0x100000 + i), "repeat": 1},
                 key=lambda m: int(m["icao"], 16) - 0x100000),
}
