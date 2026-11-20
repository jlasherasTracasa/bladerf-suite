#!/bin/bash
# medir.sh — calibración completa por inyección (P3). Reproducible: semillas 1, 2, 3.
#
# Uso:   paper/exp/calibracion/medir.sh            medida completa (≈ unos 30-60 min con 8 procesos)
#        SOLO=series|ocup|fp|analisis  ... ejecuta solo esa fase
#        JOBS=4 (máx. 4: la máquina la usa también el usuario)   BUILD=build/p3
# Compila el binario en build/p3 (make BUILD=build/p3 CUDA=0), prepara los fondos reales
# remuestreados en build/p3/bg, lanza las series (datos/*.csv y *.json) y analiza.
# Los ficheros IQ de cada pasada son temporales (build/p3/work) y se borran al acabar la pasada.
set -u
AQUI=$(cd "$(dirname "$0")" && pwd)
RAIZ=$(cd "$AQUI/../../.." && pwd)
cd "$RAIZ" || exit 2
BUILD=${BUILD:-build/p3}
JOBS=${JOBS:-4}; [ "$JOBS" -gt 4 ] && JOBS=4
SOLO=${SOLO:-todo}
SEEDS="1 2 3"
export BIN=$RAIZ/$BUILD/bladerf-lab BGDIR=$RAIZ/$BUILD/bg WORK=$RAIZ/$BUILD/work
mkdir -p "$WORK" "$AQUI/datos"
if [ "$SOLO" = todo ] || [ "$SOLO" = series ] || [ "$SOLO" = ocup ] || [ "$SOLO" = fp ]; then
    make BUILD="$BUILD" CUDA=0 -j4 >"$BUILD.make.log" 2>&1 || { tail -20 "$BUILD.make.log"; echo "FALLO de compilación"; exit 2; }
    python3 -I "$AQUI/calib.py" prep || exit 2
fi
C="python3 -I $AQUI/calib.py"
lista=$(mktemp)
# Reanudable: una serie ya hecha (datos/<tag>.csv y .json) se salta. Para repetirla, bórrense esos dos ficheros.
add() {  # add proto bg seed cond [extra...]
    local p=$1 b=$2 s=$3 c=$4; shift 4
    [ -s "$AQUI/datos/${p}_${b}_${c}_s${s}.csv" ] && [ -s "$AQUI/datos/${p}_${b}_${c}_s${s}.json" ] && return
    echo "run --proto $p --bg $b --seed $s --cond $c${*:+ $*}" >>"$lista"
}
if [ "$SOLO" = todo ] || [ "$SOLO" = series ]; then
    for s in $SEEDS; do
        for pb in "ais white" "ais real_rf" "ais real_rec" "aprs white" "aprs real_rf" "aprs real_rec" \
                  "pocsag white" "pocsag real_rf" "pocsag real_rec" "lora white" "lora real" "adsb white" "adsb real" \
                  "acars white" "acars real_rf" "acars real_rec" "rs41 white" "rs41 real_rec" "flex white" "flex real_rec" \
                  "dsc white" "dsc real_rec" "ism white" "ism real"; do
            set -- $pb; add $1 $2 $s none
        done
    done
fi
if [ "$SOLO" = todo ] || [ "$SOLO" = ocup ]; then
    for s in $SEEDS; do
        for p in ais aprs pocsag lora adsb; do
            conds="ctl adj co0 co-6"; [ "$p" = adsb ] && conds="ctl co0 co-6"
            for cd in $conds; do add $p white $s $cd --step 2 --n 20; done
        done
    done
fi
if [ -s "$lista" ]; then
    # primero lo más pesado (las series de LoRa y las largas) para equilibrar
    sort -r "$lista" | xargs -P "$JOBS" -L 1 bash -c 'nice -n 10 python3 -I '"$AQUI"'/calib.py "$@" 2>&1 | grep -v Warn | tail -1' _
fi
rm -f "$lista"; rm -rf "$WORK"/gen_* "$WORK"/p_*   # restos de pasadas interrumpidas
if [ "$SOLO" = todo ] || [ "$SOLO" = fp ]; then
    for fam in f48 f250 lora adsb; do [ -s "$AQUI/datos/fp_$fam.json" ] || echo "fp --fam $fam"; done | xargs -P "$JOBS" -L 1 bash -c 'nice -n 10 python3 -I '"$AQUI"'/calib.py "$@" 2>&1 | tail -1' _
fi
if [ "$SOLO" = todo ] || [ "$SOLO" = analisis ]; then
    python3 -I "$AQUI/analisis.py" || exit 2
fi
