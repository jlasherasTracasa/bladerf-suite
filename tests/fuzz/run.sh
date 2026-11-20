#!/usr/bin/env bash
# run.sh — Compila (ASan + UBSan) y ejecuta los arneses de fuzzing propio (Q3).
#
#   tests/fuzz/run.sh [N] [objetivo ...]
#
#   N          segundos por objetivo (por defecto 60; también FUZZ_SECONDS)
#   objetivo   json ws_cmd http sigmf ts decoders (por defecto, todos)
#   BUILD      directorio de compilación (por defecto build/q3)
#   SEED       semilla común (para repetir una ejecución)
#
# Devuelve ≠ 0 si algún objetivo falla (ASan/UBSan abortan el proceso). La entrada
# que mató al proceso queda en /tmp/fuzz_<objetivo>_crash.bin: cópiala a
# tests/fuzz/casos/<objetivo>/ cuando la corrijas.
set -u
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-build/q3}
SECS=${1:-${FUZZ_SECONDS:-60}}
shift || true
TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(json ws_cmd http sigmf ts decoders)

# Biblioteca del proyecto con sanitizadores (todos los objetos menos main.o). Además de
# ASan+UBSan se activa float-cast-overflow (double→entero fuera de rango, indefinido en C;
# no entra en -fsanitize=undefined) y se aborta en el primer error.
FSAN="-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer"
make --no-print-directory BUILD="$BUILD" SAN=1 SANFLAGS="$FSAN" CUDA=0 -j"$(nproc)" all >"$BUILD.fuzz-make.log" 2>&1 \
    || { cat "$BUILD.fuzz-make.log"; echo "fallo al compilar el proyecto"; exit 2; }
# El binario completo que ataca ws_cmd se compila SIN float-cast-overflow: main.c convierte
# números JSON a entero sin acotar (hallazgo H1 del informe de Q3, fuera del alcance de Q3).
WSBIN="$PWD/$BUILD/bladerf-lab"
case " ${TARGETS[*]} " in *" ws_cmd "*)
    make --no-print-directory BUILD="$BUILD-bin" SAN=1 CUDA=0 -j"$(nproc)" all >"$BUILD.fuzz-make-bin.log" 2>&1 \
        || { cat "$BUILD.fuzz-make-bin.log"; echo "fallo al compilar el binario"; exit 2; }
    WSBIN="$PWD/$BUILD-bin/bladerf-lab" ;;
esac
OBJS=$(find "$BUILD/obj" -name '*.o' ! -name main.o | sort)
LIBS="$(pkg-config --libs libbladeRF 2>/dev/null || echo -lbladeRF) -lm -lpthread"
RPATH=$(pkg-config --libs libbladeRF 2>/dev/null | tr ' ' '\n' | sed -n 's/^-L\(.*\)/-Wl,-rpath,\1/p' | tr '\n' ' ')
mkdir -p "$BUILD/fuzz"

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:allocator_may_return_null=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}

fail=0
for t in "${TARGETS[@]}"; do
    src=tests/fuzz/fuzz_$t.c
    [ -f "$src" ] || { echo "no existe $src"; fail=1; continue; }
    bin="$BUILD/fuzz/fuzz_$t"
    # shellcheck disable=SC2086
    gcc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Isrc -Itests -Itests/fuzz \
        $FSAN \
        "$src" $OBJS -o "$bin" $LIBS $RPATH || { echo "fallo al compilar $src"; fail=1; continue; }
    args=(--seconds "$SECS")
    [ -n "${SEED:-}" ] && args+=(--seed "$SEED")
    echo "=== $t ($SECS s) ==="
    if ! BIN="$WSBIN" "$bin" "${args[@]}"; then
        echo "*** FALLO en $t"
        fail=1
    fi
done
[ $fail -eq 0 ] && echo "fuzz: todo en verde" || echo "fuzz: HAY FALLOS"
exit $fail
