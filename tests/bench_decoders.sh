#!/bin/sh
# bench_decoders.sh — Compila y ejecuta el medidor de CPU por decodificador (AGENTS.md §4.1).
#
# Uso (desde cualquier directorio):
#   tests/bench_decoders.sh          mide todos los decodificadores con modulador (tarda varios minutos)
#   tests/bench_decoders.sh ais      mide solo ais
#
# Compila en build/q4/ con -O2, con las mismas banderas que el Makefile (-std=c11 -D_GNU_SOURCE,
# -march=native, libbladeRF por pkg-config) y enlaza todo src/ menos main.c. Como con CUDA=0 el
# Makefile usa los stubs de src/gpu/*_stub.c, aquí también: el medidor no usa GPU.
# Sale con 1 si algún decodificador supera el 30 % de un núcleo a su tasa.
set -eu

cd "$(dirname "$0")/.."
BUILD=build/q4
mkdir -p "$BUILD"

BL_CFLAGS=$(pkg-config --cflags libbladeRF 2>/dev/null || true)
BL_LIBS=$(pkg-config --libs libbladeRF 2>/dev/null || true)
[ -n "$BL_LIBS" ] || BL_LIBS=-lbladeRF
BL_RPATH=$(for f in $BL_LIBS; do case $f in -L*) echo "-Wl,-rpath,${f#-L}" ;; esac; done)

SRCS=$(find src -name '*.c' ! -path src/main.c | sort)

# shellcheck disable=SC2086
gcc -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -march=native -Isrc -Itests $BL_CFLAGS \
    tests/bench_decoders.c $SRCS -o "$BUILD/bench_decoders" $BL_RPATH $BL_LIBS -lm -lpthread

exec "$BUILD/bench_decoders" "$@"
