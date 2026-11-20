#!/usr/bin/env bash
# ci.sh — Integración continua local de BladeRF Lab (tarea Q1). Punto de entrada: `make ci`.
#
#   tests/ci.sh                 ejecuta todos los pasos (salvo los de CI_SKIP)
#   tests/ci.sh fuzz tsan       ejecuta solo esos pasos
#   CI_SKIP="tsan fuzz" tests/ci.sh
#   CI_FUZZ_SECONDS=30 tests/ci.sh fuzz
#
# Pasos (cada uno compila en su propio directorio build/ci/<paso>):
#   check-cuda    make BUILD=build/ci/cuda check            (CUDA automático)
#   check-nocuda  make BUILD=build/ci/nocuda CUDA=0 check   (stubs de src/gpu/)
#   san           make BUILD=build/ci/san SAN=1 test        (ASan + UBSan)
#   web           make BUILD=build/ci/cuda webtest          (OMITIDO sin node)
#   fuzz          tests/fuzz/run.sh con CI_FUZZ_SECONDS s por objetivo (10 por defecto)
#   tsan          tests/tsan.sh                              (ThreadSanitizer, build/q2 fijo)
#   realtest      tests/realtest.sh                          (grabaciones reales, build/ci/realtest)
#   bench         tests/bench_decoders.sh                    (presupuesto 30 % de un núcleo)
#   linkcheck     enlaces relativos de los .md del repositorio (función de este script)
#
# Por qué un único punto de entrada: cada agente o persona ejecutaba una batería distinta
# y se colaban regresiones (p. ej. un check verde sin ThreadSanitizer). Aquí todo corre
# en el mismo orden, sin parar al primer fallo, y el resumen final dice qué quedó roto.
#
# Salida: tabla paso → OK / FALLO / OMITIDO con el tiempo. Registro completo de cada paso
# en build/ci/logs/<paso>.log. Código de salida 0 si ningún paso falla (OMITIDO no cuenta
# como fallo), 1 si alguno falla, 2 por uso incorrecto.
set -u
cd "$(dirname "$0")/.." || exit 2

LOGDIR=build/ci/logs
mkdir -p "$LOGDIR"

PASOS="check-cuda check-nocuda san web fuzz tsan realtest bench linkcheck"
SKIP=$(printf '%s' "${CI_SKIP:-}" | tr ',' ' ')
SEL=""
for p in "$@"; do
    case " $PASOS " in
        *" $p "*) SEL="$SEL $p" ;;
        *) echo "paso desconocido: $p (válidos: $PASOS)" >&2; exit 2 ;;
    esac
done

en_lista() { case " $2 " in *" $1 "*) return 0 ;; esac; return 1; }
# Descripción legible del paso (variable DESC_<paso con _>).
desc() { local v=DESC_${1//-/_}; printf '%s' "${!v}"; }

# --- Comprobador de enlaces -------------------------------------------------
# Por qué: los .md se referencian entre sí (docs/, README, figuras <img>); al mover un
# fichero los enlaces se rompen en silencio. Se comprueban los relativos; los http(s),
# mailto y anclas puras se ignoran. Excluidos: build/, firmware/bladeRF/ (copia de terceros),
# legacy/ (referencia antigua), node_modules/ y .git/.
linkcheck_paso() {
    local f t base rc=0 n=0 rotos=0
    while IFS= read -r -d '' f; do
        base=$(dirname "$f")
        while IFS= read -r t; do
            t=${t%%#*}; t=${t%%\?*}
            [ -z "$t" ] && continue
            case $t in http://*|https://*|mailto:*|tel:*|data:*|/*) continue ;; esac
            n=$((n + 1))
            if [ ! -e "$base/$t" ]; then
                echo "ROTO: $f -> $t"
                rotos=$((rotos + 1)); rc=1
            fi
        done < <(grep -oE '(\]\(|src="|href="|srcset=")[^")[:space:]]+' "$f" \
                 | sed -E 's/^(\]\(|src="|href="|srcset=")//')
    done < <(find . \( -path ./build -o -path ./firmware/bladeRF -o -path ./legacy \
                  -o -name node_modules -o -path ./.git \) -prune -o -name '*.md' -print0)
    echo "enlaces relativos comprobados: $n, rotos: $rotos"
    return $rc
}

# --- Pasos -----------------------------------------------------------------
# Cada paso devuelve 0 (OK), 3 (OMITIDO) o cualquier otro valor (FALLO).
paso_check_cuda()   { make BUILD=build/ci/cuda check; }
paso_check_nocuda() { make BUILD=build/ci/nocuda CUDA=0 check; }
paso_san()          { make BUILD=build/ci/san SAN=1 test; }
paso_web() {
    command -v node >/dev/null 2>&1 || { echo "falta node: paso omitido"; return 3; }
    make BUILD=build/ci/cuda webtest
}
paso_fuzz()     { BUILD=build/ci/fuzz FUZZ_SECONDS=${CI_FUZZ_SECONDS:-10} tests/fuzz/run.sh; }
paso_tsan()     { tests/tsan.sh; }
paso_realtest() { BUILD=build/ci/realtest tests/realtest.sh; }
paso_bench()    { tests/bench_decoders.sh; }
paso_linkcheck() { linkcheck_paso; }

DESC_check_cuda="check (CUDA auto)"
DESC_check_nocuda="check sin GPU"
DESC_san="sanitizadores"
DESC_web="web (webtest)"
DESC_fuzz="fuzz corto"
DESC_tsan="ThreadSanitizer"
DESC_realtest="regresión real"
DESC_bench="presupuesto CPU"
DESC_linkcheck="enlaces de docs"

INICIO=$(date +%s)
echo "CI local BladeRF Lab — $(date '+%Y-%m-%d %H:%M')"
echo

declare -A ESTADO TIEMPO
fallos=0
for p in $PASOS; do
    fn=paso_${p//-/_}
    log="$LOGDIR/$p.log"
    if en_lista "$p" "$SKIP" || { [ -n "$SEL" ] && ! en_lista "$p" "$SEL"; }; then
        ESTADO[$p]=OMITIDO; TIEMPO[$p]=-
        continue
    fi
    echo "--- $p: $(desc "$p")"
    t0=$(date +%s)
    "$fn" >"$log" 2>&1
    rc=$?
    TIEMPO[$p]="$(( $(date +%s) - t0 )) s"
    case $rc in
        0) ESTADO[$p]=OK ;;
        3) ESTADO[$p]=OMITIDO; TIEMPO[$p]=- ;;
        *) ESTADO[$p]=FALLO; fallos=$((fallos + 1))
           echo "    FALLO (código $rc). Últimas líneas de $log:"
           tail -n 5 "$log" | sed 's/^/    | /' ;;
    esac
done

echo
printf '%-14s %-22s %-8s %s\n' PASO DESCRIPCIÓN ESTADO TIEMPO
printf '%-14s %-22s %-8s %s\n' -------------- ---------------------- -------- ----------
for p in $PASOS; do
    printf '%-14s %-22s %-8s %s\n' "$p" "$(desc "$p")" "${ESTADO[$p]}" "${TIEMPO[$p]}"
done
echo
echo "total: $(( $(date +%s) - INICIO )) s; registros en $LOGDIR/"
if [ "$fallos" -gt 0 ]; then
    echo "CI: $fallos paso(s) con FALLO"
    exit 1
fi
echo "CI: todo en verde (omitidos no cuentan)"
exit 0
