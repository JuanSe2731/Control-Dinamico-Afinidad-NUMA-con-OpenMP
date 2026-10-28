# Operational Guide · Guía operativa

[English](#english) · [Español](#español)

---

## English

This guide explains how to build the tool and the kernels, run the experimental campaign
(locally or under SLURM), run a single kernel by hand with and without the scheduler, and
regenerate the figures and tables. Commands are written from the repository root, but
every script locates the root by itself and also works when launched from another
directory.

### 1. Requirements

| Component | Notes |
|---|---|
| Linux, x86-64 | Tested on Linux with SLURM (cluster) and on a single-node laptop (plumbing only). |
| Clang/LLVM with `libomp` | **Required.** OMPT is implemented by LLVM's `libomp`; GCC's `libgomp` never loads the tool. The campaign used Clang/LLVM 21.1.8. |
| hwloc | Headers and library. The campaign used hwloc 2.13.0. |
| numactl | Needed by the `Interleave` configurations. |
| Access to `perf_event_open` | `kernel.perf_event_paranoid` must be ≤ 2. |
| AMD Zen 4 CPU | The decision variable comes from the raw event `ls_any_fills_from_sys` (`0x44`) of Zen 4. On other CPUs everything builds and runs, but the counters do not mean the same thing and the results are not comparable. |
| Python 3 + `pandas`, `scipy`, `matplotlib`, `seaborn` | Analysis and figures only (`pip install --user pandas scipy matplotlib seaborn`). |
| SLURM | Optional; the full campaign takes several hours. |

Checks before a measurement campaign:

```bash
cat /proc/sys/kernel/perf_event_paranoid   # must be <= 2
cat /proc/sys/kernel/nmi_watchdog          # 1 = it permanently takes one of the 6 PMCs
perf list | grep -i fills                  # ls_any_fills_from_sys must be listed (AMD Zen)
ulimit -n                                  # >= 4096 for 128 and 256 threads
```

### 2. Where things go

| Path | Content | Versioned |
|---|---|---|
| `build/` | Binaries (`numa_sched_optC.so`, `stencil`, `spmv_static`, `latencias_numa`) | no |
| `data/perf_out_v5/` | Default output of the campaign — it already holds the **published main series** | yes |
| `data/figuras/` | Generated figures | no |
| `data/analisis/` | Tables of the thesis (CSV) | yes |

> **Important.** `data/perf_out_v5/` contains the published series together with its
> `.done` markers, so running the campaign with the default `OUTDIR` skips all 576 runs
> (`ejecutadas: 0`). For a new campaign, always use a new directory, e.g.
> `OUTDIR=data/my_campaign`. Do not run `FORCE=1` on the published directory:
> `kernel_metrics.csv` and the tool CSVs are opened in append mode, so new rows would be
> mixed with the published ones, and the counters of a repeated tag would be aggregated
> with those of the previous attempt.

### 3. Build

`scripts/run_perf_metricsV5.sh` compiles the tool and both kernels into `build/` at the
start of every run, so a manual build is only needed to run things by hand:

```bash
mkdir -p build
# OMPT tool (shared library)
clang++ -std=c++17 -fPIC -shared -fopenmp -pthread -O2 \
    src/sched_NUMA_optC_leaky.cpp -o build/numa_sched_optC.so \
    -I"$HWLOC_INCLUDE" -L"$HWLOC_LIB" -lhwloc
# Kernels
clang++ -std=c++17 -O3 -fopenmp -ffast-math src/Stencil.cpp         -o build/stencil
clang++ -std=c++17 -O3 -fopenmp -ffast-math src/spmv_staticSeed.cpp -o build/spmv_static
# Memory-latency microbenchmark
clang++ -std=c++17 -O2 -pthread src/latencias_numa.cpp -o build/latencias_numa \
    -I"$HWLOC_INCLUDE" -L"$HWLOC_LIB" -lhwloc
```

`HWLOC_INCLUDE` and `HWLOC_LIB` default to the OpenHPC paths of the cluster
(`/opt/ohpc/pub/libs/hwloc/...`). On a regular distribution:

```bash
export HWLOC_INCLUDE=$(pkg-config --variable=includedir hwloc)
export HWLOC_LIB=$(pkg-config --variable=libdir hwloc)
```

### 4. The campaign

`scripts/run_perf_metricsV5.sh` runs every combination of size, thread count,
configuration and kernel, collects the results into `kernel_metrics.csv` and leaves a
`.done` marker per run, so it can be relaunched and resumes where it stopped.

**Parameters** (all can be overridden through the environment):

| Variable | Default | Meaning |
|---|---|---|
| `OUTDIR` | `data/perf_out_v5` | Output directory (relative paths are taken from the repository root) |
| `SIZES` | `S0 S1 S2 S3 S4 S5` | Size rungs |
| `THREAD_LIST` | `8 16 32 64 128 256` | Thread counts |
| `REPS` | `150` | Measured repetitions per run |
| `WARMUP_ITERS` | `5` | Discarded warm-up iterations |
| `SPMV_SEED` / `SPMV_AVG_NNZ` | `42` / `32` | Seed and mean non-zeros per row of the synthetic matrix |
| `PERF_FAMILIA` | `any` | `any` = demand + prefetch fills (traffic, the decision variable); `dmnd` = demand only |
| `FORCE` | `0` | `1` ignores the `.done` markers |
| `CXX`, `HWLOC_INCLUDE`, `HWLOC_LIB` | `clang++`, OpenHPC paths | Toolchain |

**Size ladder** (Table 2.5 of the thesis):

| Rung | Nominal level | Stencil `N` | SpMV rows | Working set |
|---|---|---|---|---|
| S0 | L1d of one CCD (256 KiB) | 180 | 640 | 253 KiB |
| S1 | L2 of one CCD (8 MiB) | 700 | 9 700 | 3.74 MiB |
| S2 | L3 of one CCD (32 MiB) | 1 800 | 64 000 | 24.7 MiB |
| S3 | L3 of one node (256 MiB) | 5 000 | 495 000 | 190.7 MiB |
| S4 | Above the total L3 (512 MiB) | 11 500 | 2 616 000 | 1 008 MiB |
| S5 | Main memory (DRAM) | 23 500 | 10 923 000 | 4.11 GiB |

**Configurations.** The script runs eight; the thesis analyses six (Table 2.2). The two
combined ones (`bind_close_il`, `bind_spread_il`) are in the data but are not used in
the thesis.

| `config` | Name in the thesis | `OMP_PLACES` | `OMP_PROC_BIND` | Memory | Tool |
|---|---|---|---|---|---|
| `base` | Base | — | — | first-touch | no |
| `bind_close` | Close | `cores` | `close` | first-touch | no |
| `bind_spread` | Spread | `cores` | `spread` | first-touch | no |
| `interleave` | Interleave | — | — | `numactl --interleave=all` | no |
| `bind_close_il` | — | `cores` | `close` | interleave | no |
| `bind_spread_il` | — | `cores` | `spread` | interleave | no |
| `obs` | Control | — | — | first-touch | yes, never migrates |
| `scheduler` | Scheduler | — | — | first-touch | yes, migrates |

**Local run:**

```bash
OUTDIR=data/my_campaign ./scripts/run_perf_metricsV5.sh
OUTDIR=data/my_campaign SIZES="S4 S5" THREAD_LIST="8 16" ./scripts/run_perf_metricsV5.sh
```

**SLURM run** (`exadell`, `amd` partition). The `.sbatch` loads no modules: prepare the
toolchain in the shell first, SLURM propagates it (`--export=ALL`).

```bash
module load <clang module> <hwloc module>     # module names depend on the cluster
OUTDIR=data/my_campaign sbatch scripts/run_perf_metricsV5.sbatch
# The job is resumable: re-submit until the log says "ejecutadas: 0".
# Splitting by size lets the scheduler's backfill start the jobs sooner:
OUTDIR=data/my_campaign SIZES="S0 S1 S2" sbatch scripts/run_perf_metricsV5.sbatch
OUTDIR=data/my_campaign SIZES="S5"       sbatch scripts/run_perf_metricsV5.sbatch
```

`sbatch` can be launched from the repository root or from `scripts/`. The SLURM log
(`perf_v5.<job>.out`) is written in the directory where `sbatch` was called; the
`.sbatch` raises the file-descriptor limit (`ulimit -n 65536`) and records the state of
`nmi_watchdog` and `perf_event_paranoid` in it.

### 5. Running a kernel by hand

Kernel command lines:

| Binary | Arguments |
|---|---|
| `build/stencil` | `<N> [threads] [reps] [csv_prefix]` — `N` is the side of the 2D grid |
| `build/spmv_static` | `<rows\|matrix.mtx> [threads] [reps] [csv_prefix]` — an integer generates a seeded synthetic matrix with that many rows; anything else is read as a MatrixMarket file |

Each run writes `<csv_prefix>.csv` (summary), `<csv_prefix>_times.csv` (every
repetition in execution order) and, when the tool is **not** loaded,
`<csv_prefix>_counters.csv` (per-thread counters of the measured region).

```bash
export OMP_NUM_THREADS=8 OMP_DYNAMIC=FALSE
# Base: plain OpenMP
./build/spmv_static 10923000 8 150 /tmp/spmv_base
# Close / Spread
OMP_PLACES=cores OMP_PROC_BIND=close  ./build/spmv_static 10923000 8 150 /tmp/spmv_close
OMP_PLACES=cores OMP_PROC_BIND=spread ./build/spmv_static 10923000 8 150 /tmp/spmv_spread
# Interleave
numactl --interleave=all ./build/spmv_static 10923000 8 150 /tmp/spmv_interleave

# Control and Scheduler: tool loaded, no OMP_PLACES / OMP_PROC_BIND, no numactl
export OMP_TOOL=enabled
export OMP_TOOL_LIBRARIES=$(realpath build/numa_sched_optC.so)    # absolute path
export OMPT_WINDOW_CSV=/tmp/ompt_window_metrics.csv
export OMPT_SUMMARY_CSV=/tmp/ompt_summary.csv
export OMPT_OVERHEAD_CSV=/tmp/ompt_overhead.csv
OMPT_TAG=control OMPT_LOG_FILE=/tmp/control.log OMPT_DISABLE_MIGRATION=1 \
    ./build/spmv_static 10923000 8 150 /tmp/spmv_control
OMPT_TAG=scheduler OMPT_LOG_FILE=/tmp/scheduler.log \
    ./build/spmv_static 10923000 8 150 /tmp/spmv_scheduler
```

The tool's CSVs are opened in append mode and write their header only when the file is
empty; rows are told apart by `OMPT_TAG`.

### 6. Environment variables

| Variable | Read by | Meaning |
|---|---|---|
| `OMP_TOOL=enabled` | OpenMP runtime | Enables loading OMPT tools |
| `OMP_TOOL_LIBRARIES` | OpenMP runtime | **Absolute** path of `numa_sched_optC.so` |
| `OMPT_TAG` | tool | Run identifier written to every tool CSV |
| `OMPT_WINDOW_CSV` | tool | Per-thread, per-window trace (written once, at the end) |
| `OMPT_SUMMARY_CSV` | tool | Per-thread summary |
| `OMPT_OVERHEAD_CSV` | tool | Intrinsic cost of the tool and PMU diagnostics |
| `OMPT_LOG_FILE` | tool | Diagnostic log. **Without it the tool sends the process's stderr to `/dev/null`** |
| `OMPT_VERBOSE` | tool | `1` prints a per-window and per-region trace (perturbs the measurement) |
| `OMPT_DISABLE_MIGRATION` | tool | `1` monitors everything but never migrates (the Control configuration) |
| `OMPT_MASCARAS` | tool | `0` opens only the exact pair of fill counters, without the per-origin breakdown |
| `PERF_FAMILIA` | tool, kernels | `any` (default) or `dmnd` |
| `PERF_MAX_ORIGENES` | tool, kernels | Number of per-origin masks opened besides the pair (default 3) |
| `PERF_REGION_MASCARAS` | kernels | Same as `OMPT_MASCARAS`, for the kernels' own instrumentation |
| `WARMUP_ITERS` | kernels | Warm-up iterations (default 5) |
| `SPMV_SEED`, `SPMV_AVG_NNZ` | SpMV | Synthetic matrix (defaults 42 and 32) |

When the tool is loaded, the kernels' own instrumentation (`src/perf_region.hpp`)
switches itself off: each thread has a single owner of its counters.

### 7. Memory-latency characterization

```bash
sbatch scripts/run_latencias.sbatch            # requires the whole node (--exclusive)
```

It builds `build/latencias_numa` and runs modes A (latency curve, with and without
transparent huge pages), B (core-to-core pairs and full matrix) and C (victim reads).
By default it **overwrites** `data/caracterizacion/latencias_exadell.csv`; to keep the
published one, set `OUT_CSV=data/caracterizacion/<new_name>.csv`. Never run it at the
same time as the kernels. Flags of the binary:
`./build/latencias_numa [--curva] [--c2c] [--matriz] [--victima] [--csv PATH] [--reps N] [--thp si|no|ambos]`.

### 8. Figures and analysis

All of these read the versioned data, so they work on any machine:

| Command | Output |
|---|---|
| `./scripts/run_plot_metrics.sh [OUTDIR]` | 24 campaign figures + 3 latency figures in `data/figuras/` |
| `python3 scripts/plots/plot_presentacion.py` | 19 presentation figures in `data/figuras/presentacion/` |
| `python3 scripts/analisis/analisis_entre_procesos.py` | Statistics with the run as unit (Tables 3.7 and 3.8, ANOVA) in `data/analisis/` |
| `python3 scripts/analisis/robustez_scheduler.py` | Robustness analysis (Tables 3.9–3.11); run it after the previous one |
| `python3 scripts/analisis/tablas_libro.py` | Tables 3.1–3.6 |
| `python3 scripts/plots/generar_figuras_libro.py` | Figures 3.1–3.16 of the thesis in `data/figuras/libro/` |
| `python3 scripts/analisis/estadistica.py` | Welch and ANOVA over the 150 repetitions of each run (descriptive; the thesis uses the run as unit) |

`docs/thesis-map.md` lists which table and figure each output corresponds to.

### 9. Cluster workflow

1. Clone the repository in the cluster's scratch space.
2. Load an LLVM toolchain and hwloc (`module load …`) and run the checks of section 1.
3. Submit `scripts/run_perf_metricsV5.sbatch` with a new `OUTDIR` and re-submit until
   it reports `ejecutadas: 0`.
4. Bring the results back (commit the `OUTDIR` on the cluster and pull locally, or copy
   them) and run the analysis of section 8 locally; the cluster does not need Python.

### 10. Validation and troubleshooting

A run is complete when the log reports `fallidas: 0`, every run has its
`metrics/<tag>.csv` and `metrics/<tag>_times.csv`, and `kernel_metrics.csv` has one row
per run.

| Symptom | Likely cause |
|---|---|
| `migrations = 0` in every run | `perf_event_paranoid` > 2 (the counters return 0 and nothing is reported); a CPU other than Zen 4; the tool did not load (GCC/`libgomp`, or a relative `OMP_TOOL_LIBRARIES`); `OMPT_DISABLE_MIGRATION` set |
| `migrations = 0` in S0–S3 | Expected: the measured region lasts less than the 5 windows × 100 ms the scheduler observes before it may migrate (Section 3.2) |
| Empty times at 128–256 threads | File-descriptor limit: raise `ulimit -n` (the `.sbatch` already does) |
| Comma decimals in CSVs | The scripts export `LC_ALL=C`; keep it if you write your own |
| Breakdown columns `fills_*` at 0 | Expected for the three near origins, which are not opened (only three masks are), and for every origin in the tool configurations of the main series (see `docs/data-guide.md`) |

---

## Español

Esta guía explica cómo compilar la herramienta y los kernels, ejecutar la campaña
experimental (en local o con SLURM), ejecutar a mano un kernel con y sin el scheduler, y
regenerar las figuras y tablas. Los comandos se escriben desde la raíz del repositorio,
pero todos los scripts localizan la raíz por sí mismos y funcionan también si se lanzan
desde otro directorio.

### 1. Requisitos

| Componente | Notas |
|---|---|
| Linux, x86-64 | Probado en Linux con SLURM (clúster) y en un portátil de un solo nodo (solo funcionamiento, no medidas). |
| Clang/LLVM con `libomp` | **Obligatorio.** OMPT lo implementa `libomp` de LLVM; `libgomp` de GCC nunca carga la herramienta. La campaña usó Clang/LLVM 21.1.8. |
| hwloc | Cabeceras y biblioteca. La campaña usó hwloc 2.13.0. |
| numactl | Lo necesitan las configuraciones `Interleave`. |
| Acceso a `perf_event_open` | `kernel.perf_event_paranoid` debe ser ≤ 2. |
| CPU AMD Zen 4 | La variable de decisión sale del evento crudo `ls_any_fills_from_sys` (`0x44`) de Zen 4. En otras CPU todo compila y se ejecuta, pero los contadores no significan lo mismo y los resultados no son comparables. |
| Python 3 + `pandas`, `scipy`, `matplotlib`, `seaborn` | Solo para el análisis y las figuras (`pip install --user pandas scipy matplotlib seaborn`). |
| SLURM | Opcional; la campaña completa dura varias horas. |

Comprobaciones antes de una campaña de medida:

```bash
cat /proc/sys/kernel/perf_event_paranoid   # debe ser <= 2
cat /proc/sys/kernel/nmi_watchdog          # 1 = ocupa de forma permanente uno de los 6 PMC
perf list | grep -i fills                  # debe aparecer ls_any_fills_from_sys (AMD Zen)
ulimit -n                                  # >= 4096 para 128 y 256 hilos
```

### 2. Dónde queda cada cosa

| Ruta | Contenido | Versionado |
|---|---|---|
| `build/` | Binarios (`numa_sched_optC.so`, `stencil`, `spmv_static`, `latencias_numa`) | no |
| `data/perf_out_v5/` | Salida por defecto de la campaña; ya contiene la **serie principal publicada** | sí |
| `data/figuras/` | Figuras generadas | no |
| `data/analisis/` | Tablas del libro (CSV) | sí |

> **Importante.** `data/perf_out_v5/` contiene la serie publicada con sus marcadores
> `.done`, así que ejecutar la campaña con el `OUTDIR` por defecto salta las 576
> ejecuciones (`ejecutadas: 0`). Para una campaña nueva use siempre un directorio nuevo,
> por ejemplo `OUTDIR=data/mi_campana`. No ejecute `FORCE=1` sobre el directorio
> publicado: `kernel_metrics.csv` y los CSV de la herramienta se abren en modo de
> adición, así que las filas nuevas se mezclarían con las publicadas y los contadores de
> una etiqueta repetida se agregarían con los del intento anterior.

### 3. Compilación

`scripts/run_perf_metricsV5.sh` compila la herramienta y los dos kernels en `build/` al
comienzo de cada ejecución, así que compilar a mano solo hace falta para ejecutar a mano:

```bash
mkdir -p build
# Herramienta OMPT (biblioteca compartida)
clang++ -std=c++17 -fPIC -shared -fopenmp -pthread -O2 \
    src/sched_NUMA_optC_leaky.cpp -o build/numa_sched_optC.so \
    -I"$HWLOC_INCLUDE" -L"$HWLOC_LIB" -lhwloc
# Kernels
clang++ -std=c++17 -O3 -fopenmp -ffast-math src/Stencil.cpp         -o build/stencil
clang++ -std=c++17 -O3 -fopenmp -ffast-math src/spmv_staticSeed.cpp -o build/spmv_static
# Microbenchmark de latencia de memoria
clang++ -std=c++17 -O2 -pthread src/latencias_numa.cpp -o build/latencias_numa \
    -I"$HWLOC_INCLUDE" -L"$HWLOC_LIB" -lhwloc
```

`HWLOC_INCLUDE` y `HWLOC_LIB` tienen por defecto las rutas OpenHPC del clúster
(`/opt/ohpc/pub/libs/hwloc/...`). En una distribución corriente:

```bash
export HWLOC_INCLUDE=$(pkg-config --variable=includedir hwloc)
export HWLOC_LIB=$(pkg-config --variable=libdir hwloc)
```

### 4. La campaña

`scripts/run_perf_metricsV5.sh` ejecuta todas las combinaciones de tamaño, número de
hilos, configuración y kernel, reúne los resultados en `kernel_metrics.csv` y deja un
marcador `.done` por ejecución, de modo que puede relanzarse y continúa donde se detuvo.

**Parámetros** (todos se pueden cambiar por el entorno):

| Variable | Por defecto | Significado |
|---|---|---|
| `OUTDIR` | `data/perf_out_v5` | Directorio de salida (las rutas relativas se toman desde la raíz del repositorio) |
| `SIZES` | `S0 S1 S2 S3 S4 S5` | Peldaños de tamaño |
| `THREAD_LIST` | `8 16 32 64 128 256` | Recuentos de hilos |
| `REPS` | `150` | Repeticiones medidas por ejecución |
| `WARMUP_ITERS` | `5` | Iteraciones de calentamiento descartadas |
| `SPMV_SEED` / `SPMV_AVG_NNZ` | `42` / `32` | Semilla y media de no ceros por fila de la matriz sintética |
| `PERF_FAMILIA` | `any` | `any` = rellenos por demanda y por prebúsqueda (tráfico, la variable de decisión); `dmnd` = solo demanda |
| `FORCE` | `0` | `1` ignora los marcadores `.done` |
| `CXX`, `HWLOC_INCLUDE`, `HWLOC_LIB` | `clang++`, rutas OpenHPC | Toolchain |

**Escalera de tamaños** (Tabla 2.5 del libro):

| Peldaño | Nivel nominal | Stencil `N` | Filas SpMV | Conjunto de trabajo |
|---|---|---|---|---|
| S0 | L1d de un CCD (256 KiB) | 180 | 640 | 253 KiB |
| S1 | L2 de un CCD (8 MiB) | 700 | 9 700 | 3,74 MiB |
| S2 | L3 de un CCD (32 MiB) | 1 800 | 64 000 | 24,7 MiB |
| S3 | L3 de un nodo (256 MiB) | 5 000 | 495 000 | 190,7 MiB |
| S4 | Sobre la L3 total (512 MiB) | 11 500 | 2 616 000 | 1 008 MiB |
| S5 | Memoria principal (DRAM) | 23 500 | 10 923 000 | 4,11 GiB |

**Configuraciones.** El script ejecuta ocho; el libro analiza seis (Tabla 2.2). Las dos
combinadas (`bind_close_il`, `bind_spread_il`) están en los datos, pero el libro no las
usa.

| `config` | Nombre en el libro | `OMP_PLACES` | `OMP_PROC_BIND` | Memoria | Herramienta |
|---|---|---|---|---|---|
| `base` | Base | — | — | first-touch | no |
| `bind_close` | Close | `cores` | `close` | first-touch | no |
| `bind_spread` | Spread | `cores` | `spread` | first-touch | no |
| `interleave` | Interleave | — | — | `numactl --interleave=all` | no |
| `bind_close_il` | — | `cores` | `close` | intercalada | no |
| `bind_spread_il` | — | `cores` | `spread` | intercalada | no |
| `obs` | Control | — | — | first-touch | sí, nunca migra |
| `scheduler` | Scheduler | — | — | first-touch | sí, migra |

**Ejecución local:**

```bash
OUTDIR=data/mi_campana ./scripts/run_perf_metricsV5.sh
OUTDIR=data/mi_campana SIZES="S4 S5" THREAD_LIST="8 16" ./scripts/run_perf_metricsV5.sh
```

**Ejecución con SLURM** (`exadell`, partición `amd`). El `.sbatch` no carga módulos:
prepare antes el toolchain en el shell; SLURM lo propaga (`--export=ALL`).

```bash
module load <módulo de clang> <módulo de hwloc>   # los nombres dependen del clúster
OUTDIR=data/mi_campana sbatch scripts/run_perf_metricsV5.sbatch
# El trabajo es reanudable: reenvíelo hasta que el registro diga "ejecutadas: 0".
# Trocear por tamaño permite que el backfill del planificador arranque antes los trabajos:
OUTDIR=data/mi_campana SIZES="S0 S1 S2" sbatch scripts/run_perf_metricsV5.sbatch
OUTDIR=data/mi_campana SIZES="S5"       sbatch scripts/run_perf_metricsV5.sbatch
```

`sbatch` puede lanzarse desde la raíz del repositorio o desde `scripts/`. El registro
de SLURM (`perf_v5.<trabajo>.out`) se escribe en el directorio desde el que se llamó a
`sbatch`; el `.sbatch` eleva el límite de descriptores (`ulimit -n 65536`) y deja
anotado en él el estado de `nmi_watchdog` y de `perf_event_paranoid`.

### 5. Ejecutar un kernel a mano

Línea de órdenes de los kernels:

| Binario | Argumentos |
|---|---|
| `build/stencil` | `<N> [hilos] [reps] [prefijo_csv]` — `N` es el lado de la malla 2D |
| `build/spmv_static` | `<filas\|matriz.mtx> [hilos] [reps] [prefijo_csv]` — un entero genera una matriz sintética con semilla y ese número de filas; cualquier otra cosa se lee como archivo MatrixMarket |

Cada ejecución escribe `<prefijo_csv>.csv` (resumen), `<prefijo_csv>_times.csv` (cada
repetición en orden de ejecución) y, cuando la herramienta **no** está cargada,
`<prefijo_csv>_counters.csv` (contadores por hilo de la región medida).

```bash
export OMP_NUM_THREADS=8 OMP_DYNAMIC=FALSE
# Base: OpenMP sin optimizar
./build/spmv_static 10923000 8 150 /tmp/spmv_base
# Close / Spread
OMP_PLACES=cores OMP_PROC_BIND=close  ./build/spmv_static 10923000 8 150 /tmp/spmv_close
OMP_PLACES=cores OMP_PROC_BIND=spread ./build/spmv_static 10923000 8 150 /tmp/spmv_spread
# Interleave
numactl --interleave=all ./build/spmv_static 10923000 8 150 /tmp/spmv_interleave

# Control y Scheduler: herramienta cargada, sin OMP_PLACES / OMP_PROC_BIND, sin numactl
export OMP_TOOL=enabled
export OMP_TOOL_LIBRARIES=$(realpath build/numa_sched_optC.so)    # ruta absoluta
export OMPT_WINDOW_CSV=/tmp/ompt_window_metrics.csv
export OMPT_SUMMARY_CSV=/tmp/ompt_summary.csv
export OMPT_OVERHEAD_CSV=/tmp/ompt_overhead.csv
OMPT_TAG=control OMPT_LOG_FILE=/tmp/control.log OMPT_DISABLE_MIGRATION=1 \
    ./build/spmv_static 10923000 8 150 /tmp/spmv_control
OMPT_TAG=scheduler OMPT_LOG_FILE=/tmp/scheduler.log \
    ./build/spmv_static 10923000 8 150 /tmp/spmv_scheduler
```

Los CSV de la herramienta se abren en modo de adición y escriben la cabecera solo si el
archivo está vacío; las filas se distinguen por `OMPT_TAG`.

### 6. Variables de entorno

| Variable | La lee | Significado |
|---|---|---|
| `OMP_TOOL=enabled` | runtime de OpenMP | Habilita la carga de herramientas OMPT |
| `OMP_TOOL_LIBRARIES` | runtime de OpenMP | Ruta **absoluta** de `numa_sched_optC.so` |
| `OMPT_TAG` | herramienta | Identificador de la ejecución en todos los CSV de la herramienta |
| `OMPT_WINDOW_CSV` | herramienta | Traza por hilo y por ventana (se escribe una sola vez, al final) |
| `OMPT_SUMMARY_CSV` | herramienta | Resumen por hilo |
| `OMPT_OVERHEAD_CSV` | herramienta | Costo intrínseco de la herramienta y diagnóstico de la PMU |
| `OMPT_LOG_FILE` | herramienta | Registro de diagnóstico. **Sin ella, la herramienta envía el stderr del proceso a `/dev/null`** |
| `OMPT_VERBOSE` | herramienta | `1` imprime una traza por ventana y por región (perturba la medida) |
| `OMPT_DISABLE_MIGRATION` | herramienta | `1` monitoriza todo pero nunca migra (la configuración Control) |
| `OMPT_MASCARAS` | herramienta | `0` abre solo el par exacto de contadores de rellenos, sin desglose por origen |
| `PERF_FAMILIA` | herramienta, kernels | `any` (por defecto) o `dmnd` |
| `PERF_MAX_ORIGENES` | herramienta, kernels | Máscaras por origen que se abren además del par (por defecto 3) |
| `PERF_REGION_MASCARAS` | kernels | Igual que `OMPT_MASCARAS`, para la instrumentación propia de los kernels |
| `WARMUP_ITERS` | kernels | Iteraciones de calentamiento (por defecto 5) |
| `SPMV_SEED`, `SPMV_AVG_NNZ` | SpMV | Matriz sintética (por defecto 42 y 32) |

Cuando la herramienta está cargada, la instrumentación propia de los kernels
(`src/perf_region.hpp`) se desactiva sola: cada hilo tiene un único propietario de sus
contadores.

### 7. Caracterización de la latencia de memoria

```bash
sbatch scripts/run_latencias.sbatch            # exige el nodo completo (--exclusive)
```

Compila `build/latencias_numa` y ejecuta los modos A (curva de latencia, con y sin
páginas enormes transparentes), B (pares núcleo a núcleo y matriz completa) y C
(lecturas de víctima). Por defecto **sobrescribe**
`data/caracterizacion/latencias_exadell.csv`; para conservar la publicada, use
`OUT_CSV=data/caracterizacion/<nombre_nuevo>.csv`. Nunca la ejecute a la vez que los
kernels. Opciones del binario:
`./build/latencias_numa [--curva] [--c2c] [--matriz] [--victima] [--csv RUTA] [--reps N] [--thp si|no|ambos]`.

### 8. Figuras y análisis

Todos leen los datos versionados, así que funcionan en cualquier máquina:

| Comando | Salida |
|---|---|
| `./scripts/run_plot_metrics.sh [OUTDIR]` | 24 figuras de la campaña + 3 de latencia en `data/figuras/` |
| `python3 scripts/plots/plot_presentacion.py` | 19 figuras de presentación en `data/figuras/presentacion/` |
| `python3 scripts/analisis/analisis_entre_procesos.py` | Estadística con la ejecución como unidad (Tablas 3.7 y 3.8, ANOVA) en `data/analisis/` |
| `python3 scripts/analisis/robustez_scheduler.py` | Análisis de robustez (Tablas 3.9 a 3.11); se ejecuta después del anterior |
| `python3 scripts/analisis/tablas_libro.py` | Tablas 3.1 a 3.6 |
| `python3 scripts/plots/generar_figuras_libro.py` | Figuras 3.1 a 3.16 del libro en `data/figuras/libro/` |
| `python3 scripts/analisis/estadistica.py` | Welch y ANOVA sobre las 150 repeticiones de cada ejecución (descriptivo; el libro usa la ejecución como unidad) |

`docs/thesis-map.md` indica a qué tabla y figura del libro corresponde cada salida.

### 9. Flujo de trabajo en el clúster

1. Clonar el repositorio en el espacio de scratch del clúster.
2. Cargar un toolchain de LLVM y hwloc (`module load …`) y hacer las comprobaciones de
   la sección 1.
3. Enviar `scripts/run_perf_metricsV5.sbatch` con un `OUTDIR` nuevo y reenviarlo hasta
   que informe `ejecutadas: 0`.
4. Traer los resultados (hacer commit del `OUTDIR` en el clúster y pull en local, o
   copiarlos) y ejecutar en local el análisis de la sección 8; el clúster no necesita
   Python.

### 10. Validación y solución de problemas

Una campaña está completa cuando el registro informa `fallidas: 0`, cada ejecución
tiene su `metrics/<tag>.csv` y su `metrics/<tag>_times.csv`, y `kernel_metrics.csv`
tiene una fila por ejecución.

| Síntoma | Causa probable |
|---|---|
| `migrations = 0` en todas las ejecuciones | `perf_event_paranoid` > 2 (los contadores devuelven 0 y no se informa nada); una CPU distinta de Zen 4; la herramienta no se cargó (GCC/`libgomp`, o `OMP_TOOL_LIBRARIES` relativa); `OMPT_DISABLE_MIGRATION` activa |
| `migrations = 0` en S0–S3 | Esperado: la región medida dura menos que las 5 ventanas × 100 ms que el scheduler observa antes de poder migrar (Sección 3.2) |
| Tiempos vacíos con 128–256 hilos | Límite de descriptores de archivo: elevar `ulimit -n` (el `.sbatch` ya lo hace) |
| Decimales con coma en los CSV | Los scripts exportan `LC_ALL=C`; manténgalo si escribe los suyos |
| Columnas `fills_*` del desglose en 0 | Esperado para los tres orígenes cercanos, que no se abren (solo se abren tres máscaras), y para todos los orígenes en las configuraciones con herramienta de la serie principal (ver `docs/data-guide.md`) |
