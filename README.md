# Dynamic NUMA Affinity Control for OpenMP with OMPT
# Control dinámico de la afinidad NUMA en OpenMP con OMPT

[English](#english) · [Español](#español)

Companion repository of the undergraduate thesis · Repositorio complementario del trabajo
de grado: *Implementación y evaluación de un prototipo de scheduler para el control
dinámico de la afinidad NUMA utilizando OMPT* (Universidad Industrial de Santander, 2026).

---

## English

### Overview

This repository contains a prototype **dynamic NUMA affinity scheduler for OpenMP**,
implemented as an OMPT tool (OpenMP Tools Interface). The tool is a shared library that
the OpenMP runtime loads through `OMP_TOOL_LIBRARIES`, so applications need no source
changes. Every 100 ms a monitor thread reads per-thread hardware counters
(`perf_event_open`) and computes, for each thread, the fraction of the cache lines
entering its L1d that were served by the **other** NUMA node. A thread whose fraction
stays above a threshold derived from a global adaptive reference (EWMA plus a leaky
bucket) is moved, at its next parallel-region boundary, to the opposite node with
`hwloc` — at most once per thread.

It also includes the two memory-bound kernels used to evaluate it (2D Stencil and CSR
SpMV), the memory-latency microbenchmark, the bash/SLURM scripts of the experimental
campaign, the raw data of the three campaign series used in the thesis, and the scripts
that rebuild every table and figure of its results chapter.

### Main results

Measured on a dual-socket AMD EPYC 9554 server (Zen 4, 2 NUMA nodes, 256 hardware
threads), with the data initialized by the master thread so that first-touch places it
on one node:

- Remote memory costs nothing extra while the data fit in L1d and L2 (1.00×) and 1.86×
  in DRAM, so the problem size was swept across the memory hierarchy (S0–S5).
- With the working set in DRAM and few threads the scheduler pays off: in SpMV S5 with 8
  threads the time per repetition went from 417.6 ms (plain OpenMP) to 273.2 ms, and the
  fraction of remote fills from 23.17 % to 0.86 %.
- Over the 36 cells of S3–S5, with the run as the statistical unit, the scheduler was
  significantly faster than plain OpenMP in 7, slower in 8 and indistinguishable in 21.
- Interleaving memory across nodes was the fastest static configuration in all S5
  cases; the scheduler beat all three static configurations in 2 cells (SpMV S5, 8 and
  16 threads).
- The instrument costs 0.02–0.16 ms per thread and repetition with 8 threads and
  1.4–3.2 ms with 256, because the callbacks serialize on a global lock.

### Repository map

```text
.
├── src/                          # Source code
│   ├── sched_NUMA_optC_leaky.cpp #   the scheduler (OMPT tool)
│   ├── Stencil.cpp               #   2D 5-point Stencil kernel
│   ├── spmv_staticSeed.cpp       #   CSR SpMV kernel (seeded synthetic matrix)
│   ├── perf_region.hpp           #   counters of the kernels when the tool is not loaded
│   └── latencias_numa.cpp        #   memory-latency microbenchmark (pointer chasing)
├── scripts/                      # Execution and analysis
│   ├── run_perf_metricsV5.sh     #   the campaign (resumable)
│   ├── run_perf_metricsV5.sbatch #   SLURM wrapper of the campaign
│   ├── run_latencias.sbatch      #   memory-latency characterization (SLURM)
│   ├── run_plot_metrics.sh       #   campaign and latency figures
│   ├── plots/                    #   figure scripts (including the thesis figures)
│   └── analisis/                 #   statistics and thesis tables
├── data/                         # Raw data and derived tables (see data/README.md)
│   ├── perf_out_v5/              #   main series
│   ├── series_adicionales/       #   the two additional series
│   ├── caracterizacion/          #   memory-latency characterization
│   └── analisis/                 #   tables of the thesis
├── docs/                         # Documentation
└── build/                        # Binaries (generated, not versioned)
```

### Requirements

- Linux on x86-64. Meaningful measurements need an **AMD Zen 4** NUMA machine: the
  decision variable is a raw Zen 4 event (`ls_any_fills_from_sys`).
- **Clang/LLVM with `libomp`** (OMPT is implemented by LLVM's `libomp`; GCC's `libgomp`
  never loads the tool), `hwloc`, `numactl`, and `kernel.perf_event_paranoid` ≤ 2.
- Python 3 with `pandas`, `scipy`, `matplotlib` and `seaborn` for the analysis.
- SLURM for the full campaign (optional).

### Quick start

```bash
git clone https://github.com/JuanSe2731/Control-Dinamico-Afinidad-NUMA-con-OpenMP.git
cd Control-Dinamico-Afinidad-NUMA-con-OpenMP
```

**1. Rebuild the thesis results from the published data** (any machine):

```bash
pip install --user pandas scipy matplotlib seaborn
python3 scripts/analisis/analisis_entre_procesos.py
python3 scripts/analisis/robustez_scheduler.py
python3 scripts/analisis/tablas_libro.py
python3 scripts/plots/generar_figuras_libro.py      # -> data/figuras/libro/
```

**2. Smoke test of the whole pipeline** (any Linux with Clang, hwloc and numactl; it
checks that everything builds and runs, not the measurements):

```bash
OUTDIR=/tmp/numa_smoke SIZES="S0" THREAD_LIST="2" REPS=5 \
HWLOC_INCLUDE=$(pkg-config --variable=includedir hwloc) \
HWLOC_LIB=$(pkg-config --variable=libdir hwloc) \
./scripts/run_perf_metricsV5.sh
```

**3. Full campaign on a SLURM cluster:**

```bash
module load <clang module> <hwloc module>            # names depend on the cluster
OUTDIR=data/my_campaign sbatch scripts/run_perf_metricsV5.sbatch
```

Always use a new `OUTDIR`: `data/perf_out_v5/` holds the published series.

### Documentation

| Document | Content |
|---|---|
| [docs/operational-guide.md](docs/operational-guide.md) | Requirements, build, campaign, running a kernel by hand, environment variables, figures, analysis, troubleshooting |
| [docs/scheduler-internals.md](docs/scheduler-internals.md) | How the scheduler works: counters, windows, adaptive reference, leaky bucket, migration, parameters |
| [docs/data-guide.md](docs/data-guide.md) | The three series, every CSV and its columns, and what to know before reusing the data |
| [docs/thesis-map.md](docs/thesis-map.md) | Each section, equation, table and figure of the thesis linked to its code, data and script |
| [data/README.md](data/README.md) | Short guide to the data folder |

### About the code version

`src/` holds the exact code that produced the main series (`data/perf_out_v5`), which is
the version described in the thesis: it opens cycles and instructions, the fill pair and
three informative per-origin masks. A later revision removed the cycle and instruction
counters and fixed the reading of the informative breakdown; it produced none of the
data used in the thesis and is kept only in the git history.

### Authors

- **Juan Sebastian Otero Vega** — ORCID [0009-0004-9870-538X](https://orcid.org/0009-0004-9870-538X) — [juan2220053@correo.uis.edu.co](mailto:juan2220053@correo.uis.edu.co)
- **David Fernando Naranjo Rangel** — ORCID [0009-0004-6721-2654](https://orcid.org/0009-0004-6721-2654) — [david2220046@correo.uis.edu.co](mailto:david2220046@correo.uis.edu.co)
- **Luis Alejandro Torres Niño** (advisor) — ORCID [0000-0003-2597-9430](https://orcid.org/0000-0003-2597-9430) — [luatorni@uis.edu.co](mailto:luatorni@uis.edu.co)

Escuela de Ingeniería de Sistemas e Informática, Universidad Industrial de Santander
(UIS), Bucaramanga, Colombia.

### Acknowledgments

To the CAGE research group and SC3-UIS, and to the UIS and VIE-UIS for the computing
resources of the GUANE cluster, where every experiment was run.

### How to cite

Otero Vega, J. S., & Naranjo Rangel, D. F. (2026). *Implementación y evaluación de un
prototipo de scheduler para el control dinámico de la afinidad NUMA utilizando OMPT*
[Undergraduate thesis, Universidad Industrial de Santander].
https://github.com/JuanSe2731/Control-Dinamico-Afinidad-NUMA-con-OpenMP

---

## Español

### Descripción

Este repositorio contiene un prototipo de **scheduler dinámico de afinidad NUMA para
OpenMP**, implementado como una herramienta OMPT (OpenMP Tools Interface). La herramienta
es una biblioteca compartida que el runtime de OpenMP carga mediante
`OMP_TOOL_LIBRARIES`, así que las aplicaciones no requieren cambios en su código fuente.
Cada 100 ms un hilo monitor lee contadores de hardware por hilo (`perf_event_open`) y
calcula, para cada hilo, la fracción de las líneas de caché que entran en su L1d servidas
por el **otro** nodo NUMA. Un hilo cuya fracción se mantiene por encima de un umbral
derivado de una referencia adaptativa global (EWMA más un balde con fugas) se traslada,
en su siguiente frontera de región paralela, al nodo opuesto con `hwloc`, como máximo una
vez por hilo.

Incluye además los dos kernels limitados por memoria con los que se evaluó (Stencil 2D y
SpMV en CSR), el microbenchmark de latencia de memoria, los scripts de bash y SLURM de la
campaña experimental, los datos crudos de las tres series de la campaña que usa el libro,
y los scripts que reconstruyen todas las tablas y figuras de su capítulo de resultados.

### Resultados principales

Medidos en un servidor de doble socket AMD EPYC 9554 (Zen 4, 2 nodos NUMA, 256 hilos
lógicos), con los datos inicializados por el hilo maestro, de modo que la política
first-touch los concentra en un nodo:

- La memoria remota no cuesta nada adicional mientras los datos caben en L1d y L2
  (1,00×) y cuesta 1,86× en DRAM, por lo que el tamaño del problema se barrió a lo largo
  de la jerarquía de memoria (S0–S5).
- Con el conjunto de trabajo en DRAM y pocos hilos, el scheduler compensa: en SpMV S5
  con 8 hilos el tiempo por repetición pasó de 417,6 ms (OpenMP sin optimizar) a
  273,2 ms, y la fracción de rellenos remotos del 23,17 % al 0,86 %.
- En las 36 celdas de S3–S5, con la ejecución como unidad estadística, el scheduler fue
  significativamente más rápido que OpenMP sin optimizar en 7, más lento en 8 e
  indistinguible en 21.
- Intercalar la memoria entre los nodos fue la configuración estática más rápida en
  todos los casos de S5; el scheduler superó a las tres configuraciones estáticas en 2
  celdas (SpMV S5, 8 y 16 hilos).
- El instrumento cuesta de 0,02 a 0,16 ms por hilo y repetición con 8 hilos y de 1,4 a
  3,2 ms con 256, porque los *callbacks* se serializan en un bloqueo global.

### Mapa del repositorio

```text
.
├── src/                          # Código fuente
│   ├── sched_NUMA_optC_leaky.cpp #   el scheduler (herramienta OMPT)
│   ├── Stencil.cpp               #   kernel Stencil 2D de 5 puntos
│   ├── spmv_staticSeed.cpp       #   kernel SpMV en CSR (matriz sintética con semilla)
│   ├── perf_region.hpp           #   contadores de los kernels cuando no se carga la herramienta
│   └── latencias_numa.cpp        #   microbenchmark de latencia de memoria (persecución de punteros)
├── scripts/                      # Ejecución y análisis
│   ├── run_perf_metricsV5.sh     #   la campaña (reanudable)
│   ├── run_perf_metricsV5.sbatch #   envoltorio de SLURM de la campaña
│   ├── run_latencias.sbatch      #   caracterización de la latencia de memoria (SLURM)
│   ├── run_plot_metrics.sh       #   figuras de la campaña y de latencias
│   ├── plots/                    #   scripts de figuras (incluidas las del libro)
│   └── analisis/                 #   estadística y tablas del libro
├── data/                         # Datos crudos y tablas derivadas (ver data/README.md)
│   ├── perf_out_v5/              #   serie principal
│   ├── series_adicionales/       #   las dos series adicionales
│   ├── caracterizacion/          #   caracterización de la latencia de memoria
│   └── analisis/                 #   tablas del libro
├── docs/                         # Documentación
└── build/                        # Binarios (se generan, no se versionan)
```

### Requisitos

- Linux en x86-64. Las medidas con sentido requieren una máquina NUMA **AMD Zen 4**: la
  variable de decisión es un evento crudo de Zen 4 (`ls_any_fills_from_sys`).
- **Clang/LLVM con `libomp`** (OMPT lo implementa `libomp` de LLVM; `libgomp` de GCC
  nunca carga la herramienta), `hwloc`, `numactl` y `kernel.perf_event_paranoid` ≤ 2.
- Python 3 con `pandas`, `scipy`, `matplotlib` y `seaborn` para el análisis.
- SLURM para la campaña completa (opcional).

### Inicio rápido

```bash
git clone https://github.com/JuanSe2731/Control-Dinamico-Afinidad-NUMA-con-OpenMP.git
cd Control-Dinamico-Afinidad-NUMA-con-OpenMP
```

**1. Reconstruir los resultados del libro desde los datos publicados** (cualquier
máquina):

```bash
pip install --user pandas scipy matplotlib seaborn
python3 scripts/analisis/analisis_entre_procesos.py
python3 scripts/analisis/robustez_scheduler.py
python3 scripts/analisis/tablas_libro.py
python3 scripts/plots/generar_figuras_libro.py      # -> data/figuras/libro/
```

**2. Prueba de humo de toda la cadena** (cualquier Linux con Clang, hwloc y numactl;
comprueba que todo compila y se ejecuta, no las medidas):

```bash
OUTDIR=/tmp/numa_prueba SIZES="S0" THREAD_LIST="2" REPS=5 \
HWLOC_INCLUDE=$(pkg-config --variable=includedir hwloc) \
HWLOC_LIB=$(pkg-config --variable=libdir hwloc) \
./scripts/run_perf_metricsV5.sh
```

**3. Campaña completa en un clúster con SLURM:**

```bash
module load <módulo de clang> <módulo de hwloc>      # los nombres dependen del clúster
OUTDIR=data/mi_campana sbatch scripts/run_perf_metricsV5.sbatch
```

Use siempre un `OUTDIR` nuevo: `data/perf_out_v5/` contiene la serie publicada.

### Documentación

| Documento | Contenido |
|---|---|
| [docs/operational-guide.md](docs/operational-guide.md) | Requisitos, compilación, campaña, ejecución a mano de un kernel, variables de entorno, figuras, análisis, solución de problemas |
| [docs/scheduler-internals.md](docs/scheduler-internals.md) | Cómo funciona el scheduler: contadores, ventanas, referencia adaptativa, balde con fugas, migración, parámetros |
| [docs/data-guide.md](docs/data-guide.md) | Las tres series, cada CSV y sus columnas, y lo que conviene saber antes de reutilizar los datos |
| [docs/thesis-map.md](docs/thesis-map.md) | Cada sección, ecuación, tabla y figura del libro enlazada con su código, datos y script |
| [data/README.md](data/README.md) | Guía breve de la carpeta de datos |

### Sobre la versión del código

`src/` contiene el código exacto que produjo la serie principal (`data/perf_out_v5`), que
es la versión que describe el libro: abre ciclos e instrucciones, el par de rellenos y
tres máscaras informativas por origen. Una revisión posterior retiró los contadores de
ciclos e instrucciones y corrigió la lectura del desglose informativo; no produjo ninguno
de los datos del libro y solo se conserva en el historial de git.

### Autores

- **Juan Sebastian Otero Vega** — ORCID [0009-0004-9870-538X](https://orcid.org/0009-0004-9870-538X) — [juan2220053@correo.uis.edu.co](mailto:juan2220053@correo.uis.edu.co)
- **David Fernando Naranjo Rangel** — ORCID [0009-0004-6721-2654](https://orcid.org/0009-0004-6721-2654) — [david2220046@correo.uis.edu.co](mailto:david2220046@correo.uis.edu.co)
- **Luis Alejandro Torres Niño** (director) — ORCID [0000-0003-2597-9430](https://orcid.org/0000-0003-2597-9430) — [luatorni@uis.edu.co](mailto:luatorni@uis.edu.co)

Escuela de Ingeniería de Sistemas e Informática, Universidad Industrial de Santander
(UIS), Bucaramanga, Colombia.

### Agradecimientos

Al grupo de investigación CAGE y al SC3-UIS, y a la UIS y la VIE-UIS por los recursos
de cómputo del clúster GUANE, donde se ejecutaron todos los experimentos.

### Cómo citar

Otero Vega, J. S., & Naranjo Rangel, D. F. (2026). *Implementación y evaluación de un
prototipo de scheduler para el control dinámico de la afinidad NUMA utilizando OMPT*
[Trabajo de grado, Universidad Industrial de Santander].
https://github.com/JuanSe2731/Control-Dinamico-Afinidad-NUMA-con-OpenMP
