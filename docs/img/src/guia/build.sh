#!/usr/bin/env bash
# Compila los programas de volcado de DOC2b contra los objetos de src/ (sin main.o)
# y los ejecuta, dejando los datos en build/doc2b/data/. Args: ids de moduladores para occupancy.
set -euo pipefail
cd "$(dirname "$0")/../../../.."
mkdir -p build/doc2b/data build/doc2b/bin
OBJS=$(find build/doc2b/obj -name '*.o' ! -name main.o | sort)
LIBS="$(pkg-config --libs libbladeRF 2>/dev/null || echo -lbladeRF) -lm -lpthread"
for p in dump_dsp calib ber occupancy; do
  gcc -std=c11 -D_GNU_SOURCE -O2 -march=native -Wall -Wextra -Isrc "docs/img/src/guia/$p.c" $OBJS -o "build/doc2b/bin/$p" $LIBS
done
D=build/doc2b/data
build/doc2b/bin/dump_dsp $D
build/doc2b/bin/calib > $D/calib.csv
build/doc2b/bin/ber > $D/ber.csv
for id in "$@"; do build/doc2b/bin/occupancy "$id" $D; done
