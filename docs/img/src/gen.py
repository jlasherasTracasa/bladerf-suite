#!/usr/bin/env python3
"""gen.py — genera todas las fuentes HTML (docs/img/src/*.html). Luego: docs/img/render.sh"""
import importlib, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MODS = [("d_banner", "banner", "BladeRF Lab"),
        ("d_arq", "arquitectura", "Arquitectura"),
        ("d_canal", "canal-audio", "Cadena de un canal de audio"),
        ("d_bandas", "mapa-bandas", "Mapa de bandas"),
        ("d_marco", "marco-decodificadores", "Marco de decodificadores"),
        ("d_tdt", "cadena-tdt", "Cadena TDT"),
        ("d_tx", "politica-tx", "Política de emisión"),
        ("d_ws", "protocolo-ws", "Protocolo WebSocket"),
        ("d_valid", "matriz-validacion", "Matriz de validación"),
        ("d_fpga", "fpga-lab", "Imagen FPGA lab"),
        ("d_gpu", "gpu", "GPU"),
        ("d_extra", None, None)]

only = sys.argv[1:]
for mod, name, title in MODS:
    if only and mod not in only and name not in only:
        continue
    try:
        m = importlib.import_module(mod)
    except ModuleNotFoundError as e:
        if e.name == mod:
            continue
        raise
    if name is None:
        m.build_all()
    else:
        print(m.build().save(name, title))
