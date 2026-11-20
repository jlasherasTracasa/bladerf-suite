#!/bin/sh
# tsan.sh — ThreadSanitizer sobre los tests con hilos y sobre el servidor completo (Q2).
#
# Uso: tests/tsan.sh [unit|server]     (sin argumento: ambos)
# Compila en build/q2/ con -fsanitize=thread -O1 -g y CUDA=0 (TSan no convive con el
# runtime de CUDA: se usan los stubs de src/gpu/*_stub.c). Informes en build/q2/tsan_*.log.
# Las supresiones justificadas están en tests/tsan.supp. Sale con 1 si queda algún informe.
set -u
cd "$(dirname "$0")/.."
B=build/q2
mkdir -p "$B"
what=${1:-all}
BL_CFLAGS=$(pkg-config --cflags libbladeRF 2>/dev/null || true)
BL_LIBS=$(pkg-config --libs libbladeRF 2>/dev/null || true)
[ -n "$BL_LIBS" ] || BL_LIBS=-lbladeRF
BL_RPATH=$(for f in $BL_LIBS; do case $f in -L*) echo "-Wl,-rpath,${f#-L}" ;; esac; done)
CF="-std=c11 -D_GNU_SOURCE -O1 -g -fsanitize=thread -fno-omit-frame-pointer -Wall -Wextra -Isrc -Itests $BL_CFLAGS"
SRCS=$(find src -name '*.c' ! -path src/main.c | sort)
export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 suppressions=$PWD/tests/tsan.supp"
# El kernel reciente aleatoriza mmap más de lo que TSan admite ("unexpected memory mapping"):
# se desactiva ASLR con setarch -R.
NOASLR="setarch -R"
fail=0
count() { grep -c "WARNING: ThreadSanitizer" "$1" 2>/dev/null || true; }

if [ "$what" = all ] || [ "$what" = unit ]; then
    for t in test_host test_engine_perf test_tv test_tx test_tap test_auto test_source; do
        [ -f tests/$t.c ] || continue
        # shellcheck disable=SC2086
        gcc $CF tests/$t.c $SRCS -o "$B/$t" $BL_RPATH $BL_LIBS -lm -lpthread || { echo "no compila $t"; fail=1; continue; }
        $NOASLR "$B/$t" > "$B/tsan_$t.log" 2>&1
        echo "$t: exit=$? informes=$(count "$B/tsan_$t.log")"
        [ "$(count "$B/tsan_$t.log")" = 0 ] || fail=1
    done
fi

if [ "$what" = all ] || [ "$what" = server ]; then
    # shellcheck disable=SC2086
    gcc $CF src/main.c $SRCS -o "$B/bladerf-lab-tsan" $BL_RPATH $BL_LIBS -lm -lpthread || exit 1
    $NOASLR "$B/bladerf-lab-tsan" --gen rds --gen-freq 100300000 --gen-center 100000000 --gen-rate 2400000 --out "$B/fm" >/dev/null 2>&1
    $NOASLR "$B/bladerf-lab-tsan" --replay "$B/fm.sigmf-meta" --port 8990 --no-gpu --web web > "$B/tsan_server.log" 2>&1 &
    pid=$!
    sleep 3
    ( node tests/tsan_clients.mjs ws://127.0.0.1:8990/ws > "$B/tsan_clients.log" 2>&1 ) &
    cpid=$!
    node build/orq/repro_gpu.mjs ws://127.0.0.1:8990/ws 50 > "$B/tsan_repro.log" 2>&1
    wait $cpid 2>/dev/null
    kill -INT $pid 2>/dev/null; sleep 2; kill $pid 2>/dev/null; wait $pid 2>/dev/null
    echo "servidor: informes=$(count "$B/tsan_server.log")"
    [ "$(count "$B/tsan_server.log")" = 0 ] || fail=1
fi
exit $fail
