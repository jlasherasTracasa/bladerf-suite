# Generadores de figuras y capturas (guía y documentación técnica)

Todo se ejecuta desde la raíz del repositorio; los temporales y datos van a `build/doc2b/` (no versionado).

1. `make BUILD=build/doc2b CUDA=0` — compila el binario y los objetos de `src/`.
2. `docs/img/src/guia/build.sh aprs ism lora` — compila `dump_dsp.c`, `calib.c`, `ber.c` y `occupancy.c` contra `src/dsp` y vuelca los datos en `build/doc2b/data/`.
3. `python3 docs/img/src/plots_tecnicos.py` — gráficas técnicas (`docs/img/{dsp,rendimiento,cuda,validacion,tx}/`).
4. `python3 docs/img/src/gen_guia.py --render` — diagramas (`docs/img/{guia,protocolo,arquitectura,fpga}/`).
5. `node docs/img/src/guia/shots.mjs` y `python3 docs/img/src/guia/trim.py` — capturas anotadas de la web (`docs/img/guia/captura-*.png`). Usa los puertos 8931–8935 y la batería `tests/web_e2e.mjs`; `TV_TS` apunta al TS de prueba.
6. `python3 docs/img/src/guia/linkcheck.py` — comprueba los enlaces relativos de los `.md`.
