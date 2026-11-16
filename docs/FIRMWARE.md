# FPGA de la bladeRF 2.0 micro: cadena de herramientas, análisis y plan

Documento del agente F1. Fase 1 (cadena de herramientas y análisis). **Todavía no
hay bloques DSP nuevos**; solo se ha compilado y simulado el HDL oficial sin
cambios. Nada de lo descrito aquí se ha cargado en la placa.

Fecha de las mediciones: 2026-11-15. Árbol HDL: commit `35174c3` del repositorio
oficial (`libbladeRF 2.6.1`), copia de trabajo en `firmware/bladeRF`, rama `lab`.

## 1. Resumen

| Cuestión | Resultado |
|---|---|
| Compilación `hosted` xA9 de serie (Quartus 20.1.1) | Correcta a la primera, **6 min 15 s** (375 s), 0 *critical warnings* |
| Recursos | 6 768 / 113 560 ALM (6 %), 14 599 registros, 274 / 1 220 M10K (22 %), 8 / 342 DSP (2 %), 3 / 8 PLL |
| Temporización | Cumplida en los 4 modelos (slow 85/0 °C, fast 85/0 °C). Peor *setup*: +0,512 ns; peor *hold*: +0,065 ns |
| Simulador | **Questa FSE 25.1 (vsim 2025.2) funciona**: ya hay una licencia válida en `~/.altera.quartus/questa_lic.dat` |
| Prueba de humo | `firmware/sim/run_rx_timestamp.sh`: testbench oficial `rx_timestamp_tb` de la ruta de RX, OK |
| Punto de enganche recomendado | Bus Wishbone del Nios (`bladerf_wishbone_master_read/write`, API pública de libbladeRF 2.6.1) para registros, y un modo nuevo del `rx_mux` para el flujo de datos. **Cero parches a libbladeRF** |

## 2. Estructura de ficheros (todo bajo `firmware/`)

```
firmware/
  env.sh                    entorno Quartus 20.1 + Nios II (source)
  build_stock.sh            compila la imagen de serie y guarda el log
  bladeRF/                  copia de trabajo del HDL (git, rama lab); NO es el original
  sim/run_rx_timestamp.sh   prueba de humo en Questa
  out/
    hosted-xA9-stock-0.16.0.rbf         imagen de serie recompilada
    hosted-xA9-stock-0.16.0.rbf.sha256
    logs/build_stock_hosted_xA9.log     log completo (3 770 líneas)
    reports/                            *.fit.rpt, *.sta.rpt, *.map.rpt, *.summary, git_info.txt
```

El original `~/bladerf-firmware/bladeRF` no se ha tocado (solo lectura).

## 3. Cadena de herramientas y compilación reproducible

### 3.1 Requisitos

- Quartus Prime **Lite 20.1.1** en `~/altera_lite/20.1` (con soporte Cyclone V y Nios II EDS).
  El README del HDL menciona 23.1.1 pero `platform.conf` solo exige >= 16.0 y
  `build_bladerf.sh` aplica los ajustes para ≤ 20.1 (`GLIBC_TUNABLES=glibc.rtld.execstack=2`).
  Con 20.1.1 todo funciona.
- `java`, `perl`, `make`, `git` del sistema. Nada de `sudo`.
- Red solo la primera vez: `build_bladerf.sh` ejecuta `git submodule update` y
  descargó `bladeRF-fsk` y `no-OS` dentro de `firmware/bladeRF` (no se usan para
  compilar el FPGA; son para host/ADI-no-OS). Si no hay red, el script sigue
  adelante (esa parte no tiene `set -e` todavía).

### 3.2 Entorno

```bash
source firmware/env.sh
```

`env.sh` hace lo que haría `nios2_command_shell.sh` pero sin abrir un subshell:
exporta `QUARTUS_ROOTDIR` y `SOPC_KIT_NIOS2`, antepone al `PATH` `quartus/bin`,
`quartus/sopc_builder/bin` (qsys-script, qsys-generate), `nios2eds/bin`,
`nios2eds/sdk2/bin` (nios2-bsp-*) y el compilador cruzado `nios2-elf-gcc`;
fija `GLIBC_TUNABLES` y `LC_ALL=C`. **No** añade Questa al `PATH` para no mezclar
bibliotecas de 25.1 con las de 20.1 (`QUESTA_BIN` queda definido para los scripts de simulación).

### 3.3 Compilar la imagen de serie

```bash
firmware/build_stock.sh      # ≈ 6 min; salida en firmware/out/logs/
```

Equivale a `cd firmware/bladeRF/hdl/quartus && ./build_bladerf.sh -b bladeRF-micro -r hosted -s A9 -D`
(`-D` quita la fecha del directorio de salida: `hostedxA9/`). Etapas y tiempos
medidos (Quartus, 20 núcleos):

| Etapa | Tiempo |
|---|---|
| Qsys (generar `nios_system`) + BSP + software Nios (`bladeRF_nios`) | ≈ 1 min |
| Análisis y síntesis (`quartus_map`) | 41 s |
| Fitter (`quartus_fit`) | 4 min 06 s |
| Ensamblador (`.sof`/`.rbf`) | 25 s |
| Temporización (`quartus_sta`) | 13 s |
| **Total** | **6 min 15 s** |

Salidas del script: `hostedxA9/hostedxA9.rbf` (12 858 972 bytes), informes y
`git_info.txt`. Para iterar en un diseño propio, `-k` conserva el directorio de
trabajo (compilación incremental) y `-l synth` solo sintetiza (≈ 1 min), útil
para comprobar sintaxis y recursos.

Quartus escribe en `firmware/bladeRF/hdl/quartus/work/` y `.../hostedxA9/`; ambos
están en `.git/info/exclude` de la copia. No se tocó ningún fichero versionado.

### 3.4 Resultado de la imagen de serie (hosted, xA9)

SHA-256 del `.rbf`: `42433119b10530f04beec980ceca43c1bd2b65f815c0adfa46c5a30de83bd054`.
Versión de FPGA integrada: 0.16.0 (`fpga_version.h`), la misma que hay en la flash.
No se ha podido (ni se debe) verificar byte a byte contra la flash.
Reproducibilidad: ver §3.6.

**Recursos** (5CEBA9F23C8):

| Recurso | Usado | Disponible | % |
|---|---|---|---|
| ALM | 6 768 | 113 560 | 6 |
| Registros | 14 599 | — | — |
| Bloques M10K | 274 | 1 220 | 22 |
| Bits de memoria | 2 209 280 | 12 492 800 | 18 |
| Bloques DSP | 8 | 342 | 2 |
| PLL | 3 | 8 | 38 |
| Pines | 173 | 224 | 77 |

Reparto por entidad (ALM): `nios_system` 4 715 (Nios II/e, ADI axi_ad9361 con 4 correctores IQ,
SPI, I2C, time_tamer…), `rx` 779, `fx3_gpif` 282, `ps_sync` 150, resto menor.
Los 8 DSP son los 8 correctores IQ de la IP de ADI (4 canales RX + 4 TX, "sum of two 18x18").
La memoria se reparte entre la RAM del Nios (1 062 400 bits) y las FIFOs de `rx` (819 200 bits).

**Relojes y holgura** (peor caso por dominio, modelo lento 1100 mV 85 °C, *setup*):

| Reloj | Frecuencia | Holgura setup |
|---|---|---|
| `pll_fclk` del receptor LVDS (ADI) | 500 MHz | +0,512 ns (peor global) |
| `pll_sclk` del receptor LVDS (reloj de muestras `ad9361.clock`) | 125 MHz (restricción) | +0,540 ns |
| `fx3_virtual` (E/S GPIF del FX3) | 100 MHz | +0,652 ns |
| PLL FX3 (`fx3_pclk_pll`) | 100 MHz | +2,129 ns |
| `adi_sclk_pin` | 40 MHz | +2,228 ns |
| PLL del sistema (`sys_clock`, Nios) | 80 MHz | +2,267 ns |
| `fx3_pclk` | 100 MHz | +4,091 ns |
| `c5_clock2` | 38,4 MHz | +8,981 ns |

*Hold* peor: +0,065 ns (modelo rápido 0 °C, `pll_sclk`); sin violaciones (TNS = 0 en todo).
La holgura del dominio de muestras es pequeña pero viene de la IP de ADI y de las
restricciones de E/S, no de la lógica de `rx.vhd`; hay que vigilarla al añadir lógica
en `rx_clock` (125 MHz = 8 ns de periodo). Los informes completos están en `firmware/out/reports/`.

### 3.5 Problemas encontrados

Ninguno en la compilación. Observaciones:
- El `.do` oficial `rx_timestamp_tb.do` compila `rx.vhd` antes que los paquetes de los que depende
  (no funciona tal cual en Questa); `firmware/sim/run_rx_timestamp.sh` lo reordena.
- `-D` es necesario para un nombre de directorio estable.

### 3.6 Reproducibilidad

Una segunda compilación completa e independiente (directorio de trabajo borrado, 414 s) produjo el
**mismo SHA-256**: la compilación es determinista con la misma versión de Quartus y la semilla 1 por
defecto (`-S`). Así, cualquier diferencia futura del `.rbf` proviene de cambios en el HDL.

## 4. Simulación

### 4.1 Questa FSE (operativo)

`~/altera_lite/25.1std/questa_fse/bin/vsim` → *Questa Altera Starter FPGA Edition-64 vsim 2025.2*.
La licencia **ya está instalada**: `~/.altera.quartus/questa_lic.dat` contiene una
`INCREMENT intelqsimstarter` de nodo fijo (HOSTID `c87f5464cb77`, caduca el **23-ago-2027**,
emitida 23-ago-2026). El HOSTID es la MAC de la primera interfaz de red (`c8:7f:54:64:cb:77`).
Hay que apuntar a ella con `SALT_LICENSE_SERVER` (los scripts lo hacen):

```bash
export PATH=~/altera_lite/25.1std/questa_fse/bin:$PATH
export SALT_LICENSE_SERVER=$HOME/.altera.quartus/questa_lic.dat
vsim -version
```

Si algún día falla (cambio de tarjeta de red, caducidad), la licencia gratuita se renueva así:
1. Entrar en el *Self-Service Licensing Center* de Altera/Intel con la cuenta gratuita
   (https://licensing.intel.com) y pedir "Questa-Intel FPGA Starter Edition" (gratuita).
2. Como identificador de host, dar la MAC de la NIC (`ip link`, formato sin dos puntos, 12 hex).
3. Descargar el `.dat` y guardarlo como `~/.altera.quartus/questa_lic.dat`.
Plan B si no hay licencia: entorno conda de usuario con `ghdl`/`iverilog` (no hizo falta; no se creó).

Las bibliotecas `altera_mf`, `altera`, `altera_lnsim` ya vienen precompiladas en
`questa_fse/intel/vhdl/` y están referenciadas en su `modelsim.ini`.

### 4.2 Prueba de humo: `rx_timestamp_tb`

```bash
firmware/sim/run_rx_timestamp.sh     # ≈ 50 s; log en firmware/sim/rx_timestamp.log
```

Compila la biblioteca `nuand` (todo `ip/nuand`, `common_dcfifo`, wrappers de FIFOs), `bladerf_p`,
`rx.vhd`, `tx.vhd`, `fx3_gpif.vhd`, el modelo del FX3 y el testbench, y simula 12 ms.
Ejercita la ruta de RX completa del micro: generador de paquetes → `rx` (mux, `fifo_writer`,
FIFO de muestras y de metadatos con `dcfifo`) → `fx3_gpif` → bus GPIF modelado.
El testbench lee las cabeceras de metadatos y comprueba que la diferencia de marcas de tiempo
entre mensajes consecutivos es la esperada (**4088 muestras** en modo SC8 con un canal:
2·(2048−4); 2044 en 16 bits). Resultado: `RESULTADO: OK`, 0 errores (67 avisos de `to_integer` de
64 bits, inocuos), tres iteraciones incluida la del reinicio de timestamp.

Para añadir bloques propios: crear testbench VHDL-2008 en `firmware/sim/`, compilar tras
`compile_nuand` (definido en `hdl/fpga/ip/nuand/nuand.do`) y comparar contra referencias en C del
árbol (`src/dsp/`) mediante ficheros de vectores (`textio`).

## 5. Análisis de la arquitectura (HDL `bladerf-micro`, revisión `hosted`)

### 5.1 Visión general

```
 AD9361 ──LVDS 6 bit DDR──► axi_ad9361 (IP de ADI, dentro de nios_system)
                              │ adc_i/q[0..1], valid, enable   (dominio ad9361.clock = rx_clock)
                              ▼
                       bladerf-hosted.vhd: adc_streams(i) (data_i/q 16 bit con 12 útiles, data_v)
                              ▼
                       rx.vhd:  rx_mux ──► fifo_writer ──► FIFO muestras (dcfifo, 8192×64→32 bit)
                                                    └────► FIFO metadatos (2048×128→32 bit)
                              ▼  (cruce de reloj rx_clock → fx3_pclk_pll)
                       fx3_gpif.vhd ──► GPIF II 32 bit @ 100 MHz ──► FX3 ──► USB 3 ──► libbladeRF
```

La ruta de TX es simétrica (`tx.vhd`, FIFOs de TX, y un FIFO de *loopback* digital que
alimenta la entrada del mux de RX).

### 5.2 Interfaz con el AD9361 y relojes

- La interfaz LVDS (receptor y transmisor, 4 muestras de 12 bit en dos ciclos) la hace la IP de
  ADI `axi_ad9361` con `altlvds_rx`. `adi_rx_clock` (DATA_CLK) está restringido a **250 MHz**
  (`.sdc`); el reloj de datos interno es la mitad, `ad9361.clock` = `rx_clock` = `tx_clock`
  (restricción 125 MHz; 122,88 MHz a 61,44 Msps).
- En `axi_ad9361_dev_if_alt.v` `adc_valid` alterna cada ciclo de `l_clk`, de modo que se obtiene
  **una muestra I/Q por canal cada 2 ciclos de `rx_clock`** (`fs = rx_clock / 2`).
  `bladerf-hosted.vhd` convierte `valid` en un pulso de un ciclo (`data_v = valid and not last_v`).
  La IP entrega 16 bit pero solo los 12 de mayor peso son datos (en TX se rellena con `"0000"`).
- `time_tamer` (`rx_tamer`, `tx_tamer`) incrementa el contador de 64 bit **una vez cada 2
  ciclos de `rx_clock`**, es decir, **un tick por periodo de muestra del AD9361**.
- Otros relojes: 38,4 MHz (`c5_clock2`, entrada) → PLL a **80 MHz** (`sys_clock`: Nios, SPI, I2C,
  registros); `fx3_pclk` 100 MHz → PLL con desfase de −3,42 ns (`fx3_pclk_pll`) para cerrar la
  temporización del GPIF.
- **Modo oversample 122,88 Msps (SC8)**: es una función de libbladeRF (`BLADERF_FEATURE_OVERSAMPLE`,
  `bladerf2.c`/`bladerf.c`), no un cambio de la FPGA. libbladeRF **divide la tasa pedida por 2**
  antes de programar el AD9361 (máx. 61,44 Msps de reloj del AD9361), y tras cada
  `set_sample_rate` reescribe registros del AD9361 (`bladerf_set_oversample_register_config`, entre
  ellos `0x003 = 0xD4`) y limita el formato a 8 bit (`SC8_Q7`; con SC16 devuelve
  `BLADERF_ERR_UNSUPPORTED`). El límite de rendimiento total pasa de 80 a 160 Msps
  (`check_total_sample_rate`). La FPGA ve el mismo flujo que en modo normal (misma
  interfaz, mismo `rx_clock` ≤ 125 MHz); el detalle de cómo se intercalan las muestras
  en el AD9361 en este modo **no se ha podido confirmar leyendo solo el HDL** y debe medirse (p. ej. con
  `RX_MUX_32BIT_COUNTER` y el modo oversample, o con SignalTap) antes de diseñar un DDC para él.
- Ancho de banda USB: GPIF 32 bit × 100 MHz = 400 MB/s brutos. 61,44 Msps × 1 canal × SC16
  (4 B) = 245,8 MB/s; 122,88 Msps SC8 (2 B) = 245,8 MB/s. Con 2 canales SC16 a 61,44 Msps harían falta 491 MB/s: imposible (por eso el límite de 80 Msps totales).
  El formato `SC16_Q11_PACKED` (12 bit/componente, `highly_packed_en`) baja a 3 B/muestra.

### 5.3 Formatos de datos

Controlados por bits de `config_gpio` (ver §5.5):

| Formato libbladeRF | Bits GPIO | Ruta |
|---|---|---|
| `SC16_Q11` | ninguno | `fifo_writer` empaqueta I(16)/Q(16) por canal en 64 bit |
| `SC8_Q7` (`eightbit_en`, bit 20) | 20 | 4 componentes de 8 bit por palabra; los 8 bit son `data >> 4` |
| `SC16_Q11_PACKED` (`highly_packed_en`, bit 21) | 21 | 12 bit por componente, 8 pares IQ cada 3 palabras de 64 bit |
| `*_META` | 16 (`meta_sync`) | añade cabecera de metadatos cada mensaje |
| `PACKET_META` | 19 (`packet_en`) + 16 | modo paquete (`rx_packet_generator`, usado por wlan) |

Un canal activo: `fifo_writer` empaqueta dos muestras seguidas del mismo canal en una palabra de 64 bit.

### 5.4 Metadatos y marcas de tiempo

La FPGA parte el flujo en mensajes de **2048 palabras de 32 bit (8192 bytes) con USB 3 SuperSpeed y
1024 palabras (4096 bytes) con USB 2** (`GPIF_BUF_SIZE_SS/HS`; el comentario de `metadata.h` habla
de 2048 bytes para USB 2, está desfasado). Con `meta_en = 1` cada mensaje empieza con una cabecera
de **16 bytes** (4 palabras) y el resto son muestras: en SC16 con un canal, 2044 muestras por mensaje
(4088 en SC8, comprobado en simulación, §4.2).

| Offset | Tamaño | Contenido |
|---|---|---|
| 0x00 | 2 B | longitud de paquete (solo modo paquete; en modo normal, marca `0x12344321` que el host ignora) |
| 0x02 | 1 B | flags de paquete |
| 0x03 | 1 B | core ID del paquete |
| 0x04 | 8 B | **timestamp** (uint64 little-endian): valor del contador de `rx_tamer` al aceptar la primera muestra del mensaje; un tick por periodo de muestra del AD9361 |
| 0x0c | 4 B | flags: bits 31:16 = `0xFFF` & `"11"` & `mini_exp[1:0]` (estado de los pines `mini_exp`); los 16 LSB los sobrescribe `fx3_gpif` con el estado de *underrun* |

(`fifo_writer.vhd` construye el vector de 128 bit; `fx3_gpif.vhd`, estado `RX_META`, sobrescribe los
16 LSB de la última palabra.) libbladeRF retira las cabeceras en `bladerf_sync_rx` con formato `*_META` y
entrega el timestamp en `bladerf_metadata`. Con `meta_en = 0` el flujo es continuo, sin cabeceras.
El contador se pone a cero mientras `meta_en = 0` (`timestamp_reset`).

### 5.5 Cómo controla el host la FPGA

Cadena: libbladeRF → USB (FX3, vendor/bulk) → **UART FX3↔FPGA** (`command_uart`) →
**Nios II/e** (software `bladeRF_nios`) → periféricos Avalon/PIO. Paquetes de 16 bytes
(`nios_pkt_*.h`): `8x8`, `8x16`, `8x32`, `8x64`, `16x64`, `32x32`, `retune`, `retune2`.
Destinos relevantes en el micro:

| Paquete / destino | Función |
|---|---|
| `8x32` `VERSION` (0x00) | versión de FPGA (0x7777 / 0.16.0) |
| `8x32` `CONTROL` (0x01) | **`config_gpio`** (32 bit, PIO `gpio_out`): controla bloques de la FPGA (abajo) |
| `8x32` `RFFE_CSR` (0x03) | GPIO del RFFE (enable, txnrx, sw RF, bias-T, AGC) |
| `8x64` `TIMESTAMP` | lectura de timestamp |
| `16x64` `AD9361`/`RFIC` | acceso a registros del AD9361 y órdenes de RFIC (SPI vía Nios) |
| `32x32` `EXP`/`EXP_DIR` | GPIO de la expansión |
| `32x32` `ADI_AXI` | registros AXI de la IP de ADI (IQ corr, DC, etc.) |
| `32x32` `WB_MSTR` (0x03) | **maestro Wishbone** → `IORD/IOWR_32DIRECT(WISHBONE_MASTER_0_BASE, addr)` |
| `8x8/8x16/8x32/...` `USR1..USR128` (0x80–0xff) | reservados para personalizaciones en el software Nios |
| `retune`, `retune2` | *quick tune* con perfiles guardados |

Mapa del `config_gpio` (`bladerf_p.vhd`, `nios_gpo_t`):

| Bits | Campo | Notas |
|---|---|---|
| 31:30 | `xb_mode` | expansión |
| 29:22 | **sin usar** | libres para registros propios |
| 21 | `highly_packed_en` | SC16_Q11_PACKED |
| 20 | `eightbit_en` | SC8 |
| 19 | `packet_en` | modo paquete |
| 18 | `si_clock_sel` | reloj Si5338/entrada |
| 17 | `ufl_clock_oe` | reloj a conector U.FL |
| 16 | `meta_sync` | metadatos/timestamps activos |
| 15 | `led_mode` | |
| 14:12 | `leds` | |
| 11 | `adf_chip_enable` | ADF4002 vs DAC |
| 10:8 | **`rx_mux_sel`** | 0 normal, 1 contador 12 bit, 2 contador 32 bit, **3 "entropía" (no implementado, libre)**, 4 loopback digital, 5–7 libres |
| 7 | `usb_speed` | |
| 0 | `pwr_status` | solo lectura |

El acceso es siempre lectura-modificación-escritura (`bladerf_config_gpio_read/write`, API pública),
con lo que los bits 29:22 sobreviven a libbladeRF.

`bladerf_set_rx_mux()` solo acepta 0, 1, 2 y 4 (valida en `bladerf2.c`), pero **`bladerf_config_gpio_write()`
escribe el valor en bruto**, por lo que se puede seleccionar el modo 3 (o 5–7) sin parchear.

### 5.6 Mejor punto de enganche (sin parchear libbladeRF)

1. **Registros de control y lectura de resultados: bus Wishbone.** libbladeRF 2.6.1 exporta
   `bladerf_wishbone_master_read/write(dev, addr, data)` (`libbladeRF.h`, descrito como "reservado para
   módem"). El Nios ya traduce el paquete `32x32 WB_MSTR` a accesos de 32 bit a una dirección de 32 bit.
   La IP `wishbone_master` (con *dcfifo* de cruce de reloj) exporta el bus (`wbm_wb_*`) fuera de
   `nios_system`; **en `hosted` está sin conectar** y se queda esperando `ack`: *no se debe leer/escribir
   por Wishbone con la imagen de serie* (cuelga el Nios hasta reiniciar). En nuestra revisión se
   conecta a un banco de registros propio. El reloj `wb_clk_i` lo aporta la lógica (usar `sys_clock`, 80 MHz).
   `bladeRF-cli` no ofrece una orden Wishbone; nuestro código llamará a la API directamente.
2. **Activación del flujo de datos propio: `rx_mux_sel = 3`** (reservado como "reservado para uso futuro" en
   `libbladeRF.h`; en `rx.vhd` es el caso `RX_MUX_ENTROPY`, "no implementado"). Se activa con
   `bladerf_config_gpio_write` o, mejor, con un registro Wishbone de "habilitación" para no depender del
   bit 8–10. Alternativa: bits 29:22 de `config_gpio` (hay que ampliar `nios_gpo_t`, `pack`/`unpack`).
3. **Salida a host**: por la ruta normal. El flujo sale por `fifo_writer` → FIFO → GPIF → `bladerf_sync_rx`,
   así que libbladeRF, `bladeRF-cli rx` y nuestro `source.c` lo ven como un flujo de muestras ordinario
   (SC16). Para resultados no continuos (espectro promediado) conviene enviar tramas con una cabecera
   reconocible dentro del flujo (p. ej. palabra mágica + contador) o leerlas por Wishbone.

**Parche mínimo si hiciera falta** (no se prevé): añadir `BLADERF_RX_MUX_*` nuevos a los `switch` de
`bladerf2_set_rx_mux/get_rx_mux`; y/o `nios_access` con destinos `USR` (cambia el software Nios).
Cambios en el software Nios (`bladeRF_nios`) requieren regenerar el BSP; el build lo hace solo.

### 5.7 Dónde insertar un bloque DSP en RX

En `rx.vhd`, proceso `rx_mux`: la rama `RX_MUX_NORMAL` hace `mux_streams <= adc_streams` y
`mux_streams` entra en `fifo_writer` (`in_samples`). Un bloque propio se inserta ahí:

```
adc_streams ──► [bloque DSP: NCO+CIC+FIR / FFT+promedio / detector]
                         │  (misma interfaz sample_streams_t: data_i/data_q 16 bit + data_v)
                         ▼
          rx_mux  (nuevo caso "3": mux_streams <= dsp_streams)  ──► fifo_writer
```

Condiciones de diseño:
- La entrada es **una muestra por canal cada 2 ciclos de `rx_clock`** (pulso `data_v`). A 61,44 Msps
  hay 2 ciclos de 122,88 MHz por muestra: un único multiplicador puede dar servicio a 2 operaciones por
  muestra y por canal (compartir DSP de 18x18 con factor 2).
- `fifo_writer` es dirigido por `data_v`; no exige cadencia regular. Un diezmador solo genera `data_v`
  una de cada N muestras. La salida alimenta el mismo empaquetador SC16/SC8/packed.
- **Los timestamps cuentan ticks de muestra del AD9361, no de la salida diezmada**: tras un diezmado por N
  el timestamp avanza N ticks por muestra de salida. La cabecera sigue siendo válida, pero el host debe
  tenerlo en cuenta (documentarlo en `docs/HARDWARE.md`/`src/sdr`). El *flow-control* es el mismo
  (si la FIFO se llena, `fifo_writer` descarta y activa el LED de overrun).
- `fifo_writer` cierra cada mensaje cuando ha escrito 2048 (o 1024) palabras: si el bloque produce datos
  esporádicos (espectro, detecciones) el mensaje se queda sin completar y el host no recibe nada hasta
  que se llene. Un bloque así debe emitir un **flujo continuo** (tramas con cabecera reconocible y relleno con
  ceros) o usar la ruta Wishbone.
- Ruta alternativa para resultados escasos (histogramas, contadores, detecciones): registros Wishbone.

### 5.8 Presupuesto de recursos libres (tras la imagen de serie)

| Recurso | Libre aprox. | Comentario |
|---|---|---|
| ALM | ~106 800 (94 %) | sobra de todo |
| M10K (10 kbit) | ~946 (≈ 9,6 Mbit) | FFT de 4096 puntos complejos de 16 bit ≈ 130 kbit; cientos de kbit para promedios |
| DSP (18x18 ×2 / bloque) | 334 bloques (≈ 668 multiplicadores 18x18) | FIR/NCO/FFT/CIC caben holgadamente |
| PLL | 5 | |
| Pines | 51 | la placa tiene la E/S fija; no se necesitan más |

Limitación real: la temporización del dominio de muestras (holgura +0,5 ns en las rutas de ADI)
y el empaquetado hacia el FX3 (400 MB/s). Los límites de lógica no serán el problema.
Nota: Quartus Lite no incluye licencia completa de la IP FFT de Intel (evaluación con límite); conviene
escribir la FFT/CIC/NCO propios en VHDL. Las IP `cordic.vhd`, `nco.vhd`, `fir_filter.vhd`,
`iq_correction.vhd` del árbol (`ip/nuand/synthesis`) son reutilizables como punto de partida.

### 5.9 Revisiones `adsb`, `foxhunt` y `wlan`

`platform.conf` del micro lista `hosted adsb foxhunt wlan`. Estado en este árbol (sin compilarlas todas):

| Revisión | Qué hace | ¿Se puede compilar con este árbol? |
|---|---|---|
| `hosted` | Imagen genérica: stream de RX/TX a host, la de serie. | **Sí (compilada y medida)** |
| `adsb` | `rx-adsb.vhd`: decodificador ADS-B en FPGA (8 decodificadores en paralelo `adsb_fe`, `adsb_decoder_p`); entrega mensajes de 128 bit por una FIFO (`adsbfifo`). No incluye SC8 ni packed (es un derivado antiguo de `hosted`). | **No**: las entidades `adsb_fe`/`adsb_decoder_p` no están en el árbol (IP no publicada) |
| `foxhunt` | Baliza de "caza del zorro": `tone_generator` + software Nios propio (`software/foxhunt`): transmite un mensaje en Morse a 768 ksps, con bias-T. Es TX puro. | Sí en principio (todo en el árbol); no compilada aquí. **No se debe usar: emite** |
| `wlan` | Modem 802.11 (`wlan.wlan_top`) con AGC propio (`bladerf_agc_adi_drv`), modo paquete. | **No**: biblioteca `wlan` propietaria ausente |

Todas están "soportadas en el micro" en el sentido de que la plataforma las declara, pero solo
`hosted` (y en teoría `foxhunt`) son reproducibles con el código público. Ninguna es útil para nuestro plan salvo como
referencia de estilo (p. ej. `adsb` muestra cómo cambiar el tipo de FIFO y el formato de `rx`).

## 6. Mejoras posibles (valor / esfuerzo)

Ordenadas de mayor a menor relación valor/esfuerzo:

1. **Registros Wishbone + banco de diagnóstico** (esfuerzo bajo, valor de habilitación alto). Nueva revisión
   `lab` copiada de `hosted`, con un banco de registros en `sys_clock`: identificador/versión propios, registro
   *scratch*, contadores de muestras, de mensajes y de *overruns*, bits de habilitación. Sin él no se
   puede verificar nada en placa, y nada exige parchear libbladeRF.
2. **Diezmador/DDC en FPGA** (NCO + CIC + FIR de compensación, 2 etapas de media banda), con registros de frecuencia y
   factor. Reduce el flujo USB: con el AD9361 a 61,44 Msps (o a 122,88 Msps en oversample) se obtiene un canal de
   unos MHz en SC16 con >12 bit efectivos (la ganancia de proceso ya está en la FPGA y no se pierde en el SC8).
   Es el bloque con más valor para los decodificadores *narrow* de `src/`. Esfuerzo medio.
3. **Espectro promediado en FPGA** (ventana + FFT de flujo de 1024–4096 puntos + |X|² + promedio de N tramas): sale
   a pocos MB/s en lugar de 245 MB/s, y el host solo dibuja. Esfuerzo medio-alto (FFT propia).
4. **Detector de energía / compuerta de ráfagas** (umbral con histéresis en `|x|²` y captura con pre-disparo): solo se
   envían las ráfagas (ADS-B, ISM, LoRa), reduciendo el USB y la carga de CPU. Esfuerzo bajo-medio. Complementa a 2.
5. **Empaquetado de 12 bit a 3 B/muestra ya existe** (`SC16_Q11_PACKED`, bit 21): no es trabajo de FPGA, solo de
   host (H1): ahorra un 25 % de ancho de banda USB frente a SC16.
6. **Marcas de tiempo diezmadas coherentes** (contador en unidades de la muestra de salida) y reloj PPS/GPS para
   fijar el instante de captura: esfuerzo medio, útil para TDOA/MLAT.
7. **Corrección de DC/IQ y AGC rápido en FPGA** (la IP de ADI ya trae corrector IQ con 8 DSP; el AGC de
   hardware del AD9361 ya se controla por GPIO): bajo valor extra.

## 7. Procedimiento seguro de prueba en placa

Restricciones del proyecto (AGENTS.md §0.11): los agentes **nunca** cargan ni flashean nada. Solo el
orquestador, con permiso explícito y expreso del usuario, puede cargar una imagen **en la RAM de la FPGA**
(volátil).

1. Compilar la imagen candidata (solo xA9; una imagen para otro tamaño no sirve) y verificar antes
   `quartus_sta` sin violaciones y la simulación. Guardar el `.rbf`, su hash y el commit en `firmware/out/`.
2. Pedir permiso explícito al usuario. Cerrar los procesos que tengan la placa abierta (p. ej. `openwebrx`);
   los agentes no paran servicios.
3. Cargar **solo en RAM**: `bladeRF-cli -l firmware/out/<imagen>.rbf`. **Nunca** `-L` (flash SPI), ni `-f`
   (firmware del FX3), ni `bladerf_flash_*`/`autoload`.
4. Probar primero con lo menos arriesgado: leer la versión de FPGA, el contador *scratch*, `rx_mux` en modo
   normal comparando con la imagen de serie; después el modo nuevo. No activar TX. No tocar el Wishbone
   con la imagen de serie (está sin conectar y cuelga el Nios).
5. Volver atrás: **desconectar la alimentación/USB** (la RAM de la FPGA se pierde y vuelve a cargarse la imagen
   de la flash, 0.16.0 de serie), o recargar el `.rbf` de serie (`firmware/out/hosted-xA9-stock-0.16.0.rbf`,
   equivalente a la de la flash) con `-l`. Si el Nios queda colgado, basta reenchufar.
