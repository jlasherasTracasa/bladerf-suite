# Inyector de señales con verdad de terreno

Suma copias sintéticas (`bladerf-lab --gen`) a una captura SigMF real o a ruido
blanco, con SNR **local** por copia, y emite un SigMF anotado. Solo numpy.

```
D=paper/herramientas/inyector
python3 -I $D/inyectar.py --bg captura.sigmf-meta --proto ais:20 --proto pocsag:20 \
    --snr 5:20 --cfo 1000 --freq-grid 12500 --seconds 60 --seed 1 --out /tmp/inj
python3 -I $D/emparejar.py --truth /tmp/inj.sigmf-meta --decode-args   # --decode a usar
build/bladerf-lab --replay /tmp/inj.sigmf-meta --decode ais@161975000 --print \
    --duration 61 --config none > /tmp/ev.jsonl
python3 -I $D/emparejar.py --truth /tmp/inj.sigmf-meta --events /tmp/ev.jsonl --out /tmp/res.csv
python3 -I $D/test_inyector.py        # prueba de humo (~25 s, tiempo real)
```

* Fondo: `--bg` (ci16_le, ci8, cu8, cf32_le, por ventanas: sirve para ficheros
  grandes; no se repite en bucle, avisa si es más corto que `--seconds`) o
  `--white FS,CENTRO,SIGMA` con `--seconds`.
* `--proto ID:N[:FREQ]` (repetible); `--time-dist uniform|poisson` (sin solape salvo
  `--allow-overlap`); `--freq-dist uniform|fixed`, `--freq-grid`; `--snr lo:hi`.
* SNR = P_señal / P_fondo, ambas en el ancho ocupado de la copia, con el fondo
  medido en la ventana de tiempo y la banda de esa copia (definición completa en la
  cabecera de `inyectar.py`).
* Anotaciones: `core:sample_start/count`, `core:freq_lower/upper_edge` y `inj:*`
  (`proto`, `id`, `snr_db`, `cfo_hz`, `seed`, `freq_hz`, `bw_hz`, `noise_power`).
  La semilla también va en el meta global.
* Con ID (ais, pocsag, aprs, lora, adsb; ver `protos.py`) el emparejado es exacto;
  el resto, por tiempo y frecuencia (`--params ID=JSON` para parámetros propios).
* CSV de `emparejar.py`: columnas documentadas en su cabecera.
* Limitaciones: el evento solo trae hora de reloj (ms), así que el tiempo se alinea
  por desfase estimado; la reproducción va en tiempo real.
