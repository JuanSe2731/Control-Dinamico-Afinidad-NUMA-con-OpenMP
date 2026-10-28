# data/

[English](#english) · [Español](#español)

---

## English

Raw data of the experimental campaign and the tables derived from it. Everything here
was measured on the `exadell` node (2 × AMD EPYC 9554, 2 NUMA nodes, 256 hardware
threads). The full dictionary of files and columns is in
[docs/data-guide.md](../docs/data-guide.md).

| Path | Content |
|---|---|
| `perf_out_v5/` | **Main series** (SLURM job 29390). 576 runs: 2 kernels × 6 sizes × 6 thread counts × 8 configurations. Produced by the code in `src/`. Contains the only run of the scheduler per cell. |
| `series_adicionales/perf_out_v5_29355/` | Additional series 1 (job 29355). The same campaign with an earlier build of the tool: the fill counters could not be scheduled, so it provides **times only**, and the runs with 128 and 256 threads of the configurations without the tool are missing. |
| `series_adicionales/perf_out_v6/` | Additional series 2 (job 29915). The same campaign with a tool that used a different trigger criterion. Its scheduler is **not** used; its other configurations are. |
| `caracterizacion/latencias_exadell.csv` | Memory-latency characterization (pointer chasing, local vs. remote node), produced by `src/latencias_numa.cpp`. |
| `analisis/*.csv` | Tables of the results chapter of the thesis, written by `scripts/analisis/`. |
| `figuras/` | Generated figures (not versioned; created by the scripts). |

Each series keeps the SLURM log of its job (`perf_v5.29390.out`, …) next to its data.

The thesis uses the three series as independent runs of the same battery: up to three
runs per cell for Base, Close, Spread, Interleave and Control, and one for the
Scheduler (Section 2.4.4 of the thesis).

---

## Español

Datos crudos de la campaña experimental y tablas derivadas de ellos. Todo se midió en el
nodo `exadell` (2 × AMD EPYC 9554, 2 nodos NUMA, 256 hilos lógicos). El diccionario
completo de archivos y columnas está en [docs/data-guide.md](../docs/data-guide.md).

| Ruta | Contenido |
|---|---|
| `perf_out_v5/` | **Serie principal** (trabajo de SLURM 29390). 576 ejecuciones: 2 kernels × 6 tamaños × 6 recuentos de hilos × 8 configuraciones. La produjo el código de `src/`. Contiene la única ejecución del scheduler por celda. |
| `series_adicionales/perf_out_v5_29355/` | Serie adicional 1 (trabajo 29355). La misma campaña con una versión anterior de la herramienta: los contadores de rellenos no pudieron programarse, así que aporta **solo tiempos**, y faltan las ejecuciones con 128 y 256 hilos de las configuraciones sin herramienta. |
| `series_adicionales/perf_out_v6/` | Serie adicional 2 (trabajo 29915). La misma campaña con una herramienta que usaba otro criterio de disparo. Su scheduler **no** se usa; sus demás configuraciones sí. |
| `caracterizacion/latencias_exadell.csv` | Caracterización de la latencia de memoria (persecución de punteros, nodo local frente a remoto), producida por `src/latencias_numa.cpp`. |
| `analisis/*.csv` | Tablas del capítulo de resultados del libro, escritas por `scripts/analisis/`. |
| `figuras/` | Figuras generadas (no se versionan; las crean los scripts). |

Cada serie conserva junto a sus datos el registro de SLURM de su trabajo
(`perf_v5.29390.out`, …).

El libro usa las tres series como ejecuciones independientes de la misma batería: hasta
tres ejecuciones por celda de Base, Close, Spread, Interleave y Control, y una del
Scheduler (Sección 2.4.4 del libro).
