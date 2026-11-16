#!/usr/bin/env bash
# Explora semillas del fitter para la revisión lab sin repetir la síntesis.
# Uso: firmware/fit_seeds.sh 2 3 4 5     (tras un build_lab.sh; copia el directorio de trabajo)
# Cada semilla se ajusta en work/seed<N>/ y su resultado se resume en firmware/out/logs/seed_<N>.txt.
# La imagen final se genera siempre con: firmware/build_lab.sh -S <semilla elegida>
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/env.sh"
Q="$HERE/bladeRF/hdl/quartus/work"
SRC="$Q/bladerf-micro-A9-lab"
for s in "$@"; do
    (
        d="$Q/seed$s"
        rm -rf "$d"; cp -r "$SRC" "$d"; cd "$d"
        rm -rf output_files/lab.fit.* output_files/lab.sta.* output_files/lab.rbf
        quartus_fit bladerf --rev=lab --seed=$s > "$HERE/out/logs/seed_${s}_fit.log" 2>&1
        quartus_asm bladerf --rev=lab > "$HERE/out/logs/seed_${s}_asm.log" 2>&1
        quartus_sta bladerf --rev=lab > "$HERE/out/logs/seed_${s}_sta.log" 2>&1
        python3 - "$d/output_files/lab.sta.summary" > "$HERE/out/logs/seed_$s.txt" <<'PY'
import re,sys
t=open(sys.argv[1]).read().split('\n\n')
rows=[]
for b in t:
    m=re.search(r"Type\s*:\s*(.*)\nSlack\s*:\s*(\S+)",b)
    if m: rows.append((float(m.group(2)),m.group(1)))
print("min setup", min(r[0] for r in rows if ' Setup ' in r[1]), "min hold", min(r[0] for r in rows if ' Hold ' in r[1]))
PY
    ) &
done
wait
for s in "$@"; do echo "semilla $s: $(cat "$HERE/out/logs/seed_$s.txt")"; done
