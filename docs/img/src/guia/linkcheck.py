#!/usr/bin/env python3
"""Comprueba que todos los enlaces e imágenes relativos de docs/*.md existen."""
import os, re, sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
bad = 0; n = 0
files = [os.path.join(root, "README.md")]
for d, _, fs in os.walk(os.path.join(root, "docs")):
    if "/img" in d: continue
    files += [os.path.join(d, f) for f in fs if f.endswith(".md")]
for f in files:
    if not os.path.exists(f): continue
    txt = open(f, encoding="utf-8").read()
    refs = re.findall(r'\]\(([^)\s]+)\)', txt) + re.findall(r'(?:src|srcset|href)="([^"]+)"', txt)
    heads = None
    for r in refs:
        if re.match(r'^(https?:|mailto:|data:)', r): continue
        path, _, frag = r.partition('#')
        tgt = os.path.normpath(os.path.join(os.path.dirname(f), path)) if path else f
        n += 1
        if not os.path.exists(tgt):
            print(f"{os.path.relpath(f, root)}: falta {r}"); bad += 1
print(f"{n} enlaces relativos revisados, {bad} rotos")
sys.exit(1 if bad else 0)
