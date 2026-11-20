#!/bin/bash
# medir.sh — piloto de calibración por inyección (reproducible, semilla fija).
#
# Uso (desde la raíz del repositorio o desde cualquier sitio):
#   paper/exp/piloto/medir.sh             medida completa (rejillas finas, N=100 por punto)
#   COARSE=1 paper/exp/piloto/medir.sh    barrido grueso (pasos de 5 dB, N=20) para ubicar las rejillas
#   SOLO="ais lora" paper/exp/piloto/medir.sh
# Salidas pesadas (IQ) en build/p3a/piloto/; resultados ligeros en paper/exp/piloto/.
# Puertos 8940-8949 (10 réplicas en paralelo, una por «shard» del fichero).
set -u
AQUI=$(cd "$(dirname "$0")" && pwd)
RAIZ=$(cd "$AQUI/../../.." && pwd)
cd "$RAIZ" || exit 2
BUILD=build/p3a
BIN=$BUILD/bladerf-lab
W=$BUILD/piloto${COARSE:+_coarse}
SEED=${SEED:-20261120}
SHARDS=10
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; }
trap cleanup EXIT INT TERM

make BUILD=$BUILD CUDA=0 -j"$(nproc)" >"$BUILD.make.log" 2>&1 || { tail -20 "$BUILD.make.log"; echo "FALLO de compilación"; exit 2; }
mkdir -p "$W"

# rejilla por protocolo: "lo:hi:paso" en dB (se fija tras el barrido grueso)
declare -A GRID
if [ -n "${COARSE:-}" ]; then
    for p in ais pocsag aprs lora adsb; do GRID[$p]="-10:30:5"; done
    N=20
else
    # shellcheck disable=SC1091
    source "$AQUI/rejillas.sh"
    N=100
fi

IDS=${SOLO:-"ais pocsag aprs lora adsb"}
for p in $IDS; do
    g=${GRID[$p]}
    nlev=$(python3 -c "import sys;lo,hi,st=map(float,sys.argv[1].split(':'));print(int(round((hi-lo)/st))+1)" "$g")
    nvar=$((nlev * N))
    rm -rf "${W:?}/var_$p"
    python3 "$AQUI/inyectar.py" gen --proto "$p" --n "$nvar" --outdir "$W/var_$p" --bin "$BIN" || exit 2
    dec=$p; [ "$p" != adsb ] && dec=$(python3 -c "import sys;sys.path.insert(0,'$AQUI');from comun import dec_arg;print(dec_arg('$p'))")
    for bg in white real; do
        python3 "$AQUI/inyectar.py" inject --proto "$p" --bg "$bg" --vardir "$W/var_$p" --snr="$g" \
            --n "$N" --shards $SHARDS --seed "$SEED" --out "$W/${p}_$bg" || exit 2
        PIDS=()
        for ((s = 0; s < SHARDS; s++)); do
            d=$(python3 -c "import json;print(json.load(open('$W/${p}_$bg.truth.json'))['shards'][$s]['seconds'])")
            "$BIN" --replay "$W/${p}_${bg}_s$s.sigmf-meta" --decode "$dec" --print --duration "$d" \
                --port $((8940 + s)) --config none --no-gpu >"$W/${p}_${bg}_s$s.out" 2>"$W/${p}_${bg}_s$s.err" &
            PIDS+=($!)
        done
        for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null; done
        PIDS=()
        rm -f "${W:?}/${p}_${bg}"_s*.sigmf-data      # el IQ se regenera con la semilla
    done
    rm -rf "${W:?}/var_$p"
done
# Falsos positivos: 60 s de fondo SIN copias por protocolo y fondo (10 réplicas, puertos 8940-8949)
PIDS=(); k=0
for p in $IDS; do
    dec=$p; [ "$p" != adsb ] && dec=$(python3 -c "import sys;sys.path.insert(0,'$AQUI');from comun import dec_arg;print(dec_arg('$p'))")
    for bg in white real; do
        python3 "$AQUI/inyectar.py" inject --proto "$p" --bg "$bg" --vardir none --noise-only 60 \
            --seed "$SEED" --out "$W/${p}_${bg}_noise" >/dev/null || exit 2
        "$BIN" --replay "$W/${p}_${bg}_noise_s0.sigmf-meta" --decode "$dec" --print --duration 60 \
            --port $((8940 + k)) --config none --no-gpu >"$W/${p}_${bg}_noise_s0.out" 2>/dev/null &
        PIDS+=($!); k=$((k + 1))
    done
done
for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null; done
PIDS=()
rm -f "${W:?}"/*_noise_s0.sigmf-data
mkdir -p "$W/res"
python3 "$AQUI/ajustar.py" "$W" "$([ -n "${COARSE:-}" ] && echo "$W/res" || echo "$AQUI")" 2>&1 | tail -3
