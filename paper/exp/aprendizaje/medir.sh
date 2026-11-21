#!/bin/bash
# medir.sh — piloto P6 (aprendizaje con el sesgo de etiquetado corregido). Reproducible: semillas 1, 2, 3.
# Uso:  paper/exp/aprendizaje/medir.sh          todo (~25 min con 3 procesos + GPU opcional)
#       SOLO=gen|learn|analisis  ejecuta una fase.   Datos y binario en build/p6 (fuera de git).
# Requisitos: python3 con numpy, scikit-learn, matplotlib, torch (con CUDA si hay; si no, CPU). Ningún paquete nuevo.
# Reutiliza fondos remuestreados de la calibración (build/p3/bg, los crea `calib.py prep`) y paper/exp/calibracion/curvas.csv.
set -u
AQUI=$(cd "$(dirname "$0")" && pwd); RAIZ=$(cd "$AQUI/../../.." && pwd); cd "$RAIZ" || exit 2
SOLO=${SOLO:-todo}; mkdir -p build/p6/data build/p6/work
if [ "$SOLO" = todo ] || [ "$SOLO" = gen ]; then
    make BUILD=build/p6 CUDA=0 -j4 >build/p6.make.log 2>&1 || { tail -20 build/p6.make.log; exit 2; }
    [ -d build/p3/bg ] || { BIN=$RAIZ/build/p6/bladerf-lab BGDIR=$RAIZ/build/p3/bg python3 -I paper/exp/calibracion/calib.py prep || exit 2; }
    for s in 1 2 3; do   # 3 procesos (uno por semilla); gen.py reanuda: salta lo ya generado
        ( for sp in train:48 test:20 synth:48 cal:20; do
              nice -n 10 python3 -I "$AQUI/gen.py" "${sp%%:*}" $s "${sp##*:}"; done > build/p6/gen_s$s.log 2>&1 ) &
    done; wait
fi
if [ "$SOLO" = todo ] || [ "$SOLO" = learn ]; then
    for s in 1 2 3; do nice python3 -I "$AQUI/learn.py" $s > build/p6/learn_s$s.log 2>&1 & done; wait
fi
if [ "$SOLO" = todo ] || [ "$SOLO" = analisis ]; then
    python3 -I "$AQUI/analizar.py"
fi
