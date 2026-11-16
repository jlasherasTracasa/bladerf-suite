#!/bin/sh
# run_tests.sh — Ejecuta los binarios de prueba ya compilados y resume el resultado.
# Uso: tests/run_tests.sh <directorio_de_binarios> <test_a> [<test_b> ...]
# Sale con código distinto de 0 si algún test falla.
dir="$1"
shift
pass=0
fail=0
failed=""
for t in "$@"; do
    echo "=== $t ==="
    if "$dir/$t"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed="$failed $t"
    fi
done
echo
echo "Resumen: $pass correctos, $fail fallidos${failed:+ (}${failed# }${failed:+)}"
[ "$fail" -eq 0 ]
