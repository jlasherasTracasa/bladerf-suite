#!/bin/bash
# realtest.sh — batería de regresión con grabaciones REALES (tarea Q5).
#
# Reproduce cada fragmento de tests/data/real/<id>.sigmf-* con
#   bladerf-lab --replay <f> --decode <id>@<Hz> --print --duration <s>
# y compara con tests/data/real/esperado.json:
#   * mensajes ÚNICOS (clave jq por protocolo) >= referencia x 0,95;
#   * cada «campo clave» (cadena literal) aparece en la salida.
# Se cuentan mensajes únicos y no líneas: --replay repite el fichero en bucle y el
# nº bruto de eventos depende de cuántas vueltas caben en --duration (y de la
# fase del hilo respecto al borde del fichero); el nº de mensajes distintos no.
#
# Uso:  tests/realtest.sh            ejecuta la batería (compila en build/q5)
#       tests/realtest.sh --medir    reescribe unicos/eventos de esperado.json con
#                                    lo que sale HOY (usar solo tras una mejora validada)
#       BUILD=build/otro tests/realtest.sh
#       SOLO="ais lte" tests/realtest.sh     solo esos casos
# Puertos 8980-8989 (hasta 10 casos en paralelo). Códigos: 0 todo verde, 1 algún FALLO.
# Un fixture ausente que se pueda descargar (descargar.sh) y no haya red -> SALTADO.
set -u
cd "$(dirname "$0")/.." || exit 2
BUILD=${BUILD:-build/q5}
DIR=tests/data/real
EXP=$DIR/esperado.json
BIN=$BUILD/bladerf-lab
MEDIR=0
[ "${1:-}" = "--medir" ] && MEDIR=1
command -v jq >/dev/null || { echo "falta jq"; exit 2; }

if [ -z "${REALTEST_NOBUILD:-}" ]; then
    make BUILD="$BUILD" -j"$(nproc)" >"$BUILD.realtest-make.log" 2>&1 || { tail -20 "$BUILD.realtest-make.log"; echo "FALLO de compilación"; exit 2; }
    rm -f "$BUILD.realtest-make.log"
fi

TMP=$(mktemp -d)
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

# Lanza un caso en segundo plano; deja la salida en $TMP/<id>.out
lanza() {
    local id=$1 port=$2
    local dec freq dur
    dec=$(jq -r ".casos[\"$id\"].decoder" "$EXP"); freq=$(jq -r ".casos[\"$id\"].freq" "$EXP")
    dur=$(jq -r ".casos[\"$id\"].duracion" "$EXP")
    "$BIN" --replay "$DIR/$(jq -r ".casos[\"$id\"].fixture" "$EXP").sigmf-meta" --decode "$dec@$freq" \
        --print --duration "$dur" --port "$port" --config none --no-gpu >"$TMP/$id.out" 2>"$TMP/$id.err" &
    PIDS+=($!)
}

IDS=$(jq -r '.casos|keys[]' "$EXP")
[ -n "${SOLO:-}" ] && IDS=$SOLO
declare -A ST UNI EVT MISS
nres=0
run_batch() {   # recibe ids
    local b=("$@") i=0 port
    PIDS=()
    for id in "${b[@]}"; do port=$((8980 + i)); lanza "$id" "$port"; i=$((i + 1)); done
    for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done
    PIDS=()
}

pend=()
for id in $IDS; do
    fx=$DIR/$(jq -r ".casos[\"$id\"].fixture" "$EXP").sigmf-meta
    if [ ! -f "$fx" ]; then
        if [ -x "$DIR/descargar.sh" ] && "$DIR/descargar.sh" "$id" >/dev/null 2>&1 && [ -f "$fx" ]; then :; else
            ST[$id]=SALTADO; MISS[$id]="fixture ausente y sin red/descarga"; continue
        fi
    fi
    pend+=("$id")
done
for ((k = 0; k < ${#pend[@]}; k += 10)); do run_batch "${pend[@]:k:10}"; done

rc=0
for id in $IDS; do
    [ "${ST[$id]:-}" = SALTADO ] && continue
    out=$TMP/$id.out
    flt=$(jq -r ".casos[\"$id\"].filtro" "$EXP"); key=$(jq -r ".casos[\"$id\"].clave" "$EXP")
    ev=$(grep -c '"type":"dec"' "$out")
    uni=$(grep '"type":"dec"' "$out" | jq -c "select(.type==\"dec\") | $flt | $key" 2>/dev/null | sort -u | grep -c .)
    UNI[$id]=$uni; EVT[$id]=$ev
    if [ $MEDIR = 1 ]; then ST[$id]=MEDIDO; continue; fi
    ref=$(jq -r ".casos[\"$id\"].unicos" "$EXP")
    why=""
    awk -v u="$uni" -v r="$ref" 'BEGIN{exit !(u >= r*0.95 && r > 0)}' || why="únicos $uni < 0,95×$ref"
    while IFS= read -r campo; do
        grep -qF -- "$campo" "$out" || why="$why; falta $campo"
    done < <(jq -r ".casos[\"$id\"].campos[]" "$EXP")
    if [ -z "$why" ]; then ST[$id]=OK; else ST[$id]=FALLO; MISS[$id]=$why; rc=1; fi
done

if [ $MEDIR = 1 ]; then
    cp "$EXP" "$TMP/e.json"
    for id in $IDS; do
        jq --arg id "$id" --argjson u "${UNI[$id]}" --argjson e "${EVT[$id]}" \
            '.casos[$id].unicos=$u | .casos[$id].eventos=$e' "$TMP/e.json" >"$TMP/e2.json" && mv "$TMP/e2.json" "$TMP/e.json"
    done
    cp "$TMP/e.json" "$EXP"
    echo "esperado.json actualizado"
fi

printf '\n%-18s %-8s %7s %7s %7s  %s\n' CASO ESTADO ÚNICOS REF EVENTOS NOTA
for id in $IDS; do
    printf '%-18s %-8s %7s %7s %7s  %s\n' "$id" "${ST[$id]}" "${UNI[$id]:--}" "$(jq -r ".casos[\"$id\"].unicos" "$EXP")" \
        "${EVT[$id]:--}" "${MISS[$id]:-}"
done
echo
echo "sin grabación real:"
jq -r '.sin_grabacion_real | to_entries[] | "  \(.key): \(.value)"' "$EXP"
nf=0; ns=0; no=0
for id in $IDS; do case ${ST[$id]} in FALLO) nf=$((nf+1));; SALTADO) ns=$((ns+1));; *) no=$((no+1));; esac; done
echo; echo "realtest: $no bien, $nf FALLO, $ns SALTADO"
exit $rc
