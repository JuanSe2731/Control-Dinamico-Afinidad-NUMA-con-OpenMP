# Data Guide · Guía de datos

[English](#english) · [Español](#español)

---

## English

### 1. What is in `data/`

| Path | Content |
|---|---|
| `perf_out_v5/` | Main series of the campaign (SLURM job 29390) |
| `series_adicionales/perf_out_v5_29355/` | Additional series 1 (job 29355) |
| `series_adicionales/perf_out_v6/` | Additional series 2 (job 29915) |
| `caracterizacion/latencias_exadell.csv` | Memory-latency characterization of the platform (job 29354) |
| `analisis/*.csv` | Tables of Chapter 3 of the thesis, derived from the above |
| `figuras/` | Generated figures (not versioned) |

All measurements come from the `exadell` node of the GUANE cluster (SC3-UIS):
2 × AMD EPYC 9554 (Zen 4), 2 NUMA nodes (one per socket), 128 cores and 256 hardware
threads, 377 GB of memory, Clang/LLVM and libomp 21.1.8, hwloc 2.13.0 (Table 2.1).

### 2. The three series

The same battery (2 kernels × 6 sizes × 6 thread counts × 8 configurations = 576 runs,
150 measured repetitions and 5 warm-up iterations each, matrix seed 42) was run three
times on the same node, with the same kernel code in the initialization and in the
measured loop. The tool differed between series:

| | Main (`perf_out_v5`) | Additional 1 (`perf_out_v5_29355`) | Additional 2 (`perf_out_v6`) |
|---|---|---|---|
| SLURM job | 29390 | 29355 | 29915 |
| Tool | The code in `src/` | Earlier build: the fill group could never be scheduled in the PMU, so every thread ended in `ipc_only` | Variant with a different trigger criterion |
| Counters per thread | cycles + instructions, fill pair, 3 informative masks (7 events, 3 groups) | cycles + instructions only | fill pair + 5 informative masks (7 events, 2 groups) |
| Runs with a time | 576 | 432 (no runs at 128 and 256 threads for the 6 configurations without the tool) | 576 |
| `ratio_rm` | yes | no (`NA` everywhere) | yes |
| Used in the thesis | every configuration; the **only** Scheduler run per cell | times of Base, Close, Spread, Interleave and Control | times and `ratio_rm` of Base, Close, Spread, Interleave and Control; window traces of Control |

The missing runs of series 1 were lost to the file-descriptor limit (now raised by the
`.sbatch`). Neither additional series is used for the Scheduler, because their tool was
not the one evaluated (Section 2.4.4 of the thesis).

### 3. Layout of a series

```text
perf_out_v5/
├── kernel_metrics.csv          # one row per run (576)
├── metrics/
│   ├── <tag>.csv               # summary written by the kernel
│   ├── <tag>_times.csv         # the 150 repetitions, in execution order
│   └── <tag>_counters.csv      # per-thread counters (configurations without the tool)
├── ompt_window_metrics.csv     # tool: one row per thread and 100 ms window
├── ompt_summary.csv            # tool: one row per thread
├── ompt_overhead.csv           # tool: one row per run
├── ompt_logs/                  # <tag>.log (tool) and <tag>.stderr (stderr of the run)
├── .done/                      # resume markers, one empty file per finished run
└── perf_v5.29390.out           # SLURM log of the job
```

`<tag>` is `<kernel>_<size>_t<threads>_<config>`, for example
`spmv_static_S5_t8_scheduler`. Kernels are `stencil` and `spmv_static`; the configuration
ids are listed in [operational-guide.md](operational-guide.md#4-the-campaign).

### 4. `kernel_metrics.csv`

| Column | Meaning |
|---|---|
| `kernel`, `size_tag`, `n_or_rows`, `ws_bytes`, `threads`, `config` | Identification: grid side `N` (Stencil) or matrix rows (SpMV), working set in bytes |
| `binding`, `memory_policy`, `tool`, `migration` | `none/close/spread`, `first-touch/interleave`, `no/ompt`, `-/off/on` |
| `familia`, `warmup`, `reps` | Fill-event family (`any`), warm-up iterations, measured repetitions |
| `min_ms`, `avg_ms`, `max_ms`, `stdev_ms` | Statistics of the 150 repetitions. **`avg_ms` is the primary metric** |
| `migrations` | Migrations made by the tool (0 without it) |
| `caudal_util_gibs` | Bytes of the kernel's model divided by `avg_ms`: it is `1/t` rescaled, not a measured traffic. It is the "GiB/s" of the SpMV figure |
| `mlups` | Stencil only: `(N−2)² / (t·10⁶)`; `NA` for SpMV |
| `gflops` | Stencil `5·(N−2)²/(t·10⁹)`; SpMV `2·nnz/(t·10⁹)` |
| `ipc` | Instructions / cycles summed over the threads |
| `fills_l2` … `fills_far_dram` | Informative per-origin breakdown of the fills (see section 8) |
| `bw_remoto_gibs`, `gib_remotos_total` | `(fills_far_cache + fills_far_dram) × 64 B`, per second and in total |
| `ratio_rm` | Fills served by the other node / all fills, from the exact pair of counters; `NA` when not measurable |

`mlups`, `gflops` and `caudal_util_gibs` are all proportional to `1/avg_ms` for a given
size: they express the same result in different units.

### 5. Files in `metrics/`

- `<tag>.csv` — written by the kernel. Stencil:
  `strategy,n,ws_bytes,threads,warmup,reps,min_ms,avg_ms,max_ms,stddev_ms,caudal_util_gibs,mlups,gflops`;
  SpMV: `strategy,matrix,rows,cols,nnz,ws_bytes,threads,warmup,reps,min_ms,avg_ms,max_ms,stddev_ms,caudal_util_gibs,gflops`.
- `<tag>_times.csv` — `rep,time_ms`, the individual repetitions **in execution order**.
- `<tag>_counters.csv` — only without the tool:
  `tag,slot,tid,instructions,cycles,fill_l2,…,fill_far_dram,fill_all,fill_remoto,mascaras,familia`,
  one row per thread, restricted to the measured region. `fill_all`/`fill_remoto` are the
  exact pair; the `tag` column stores the output prefix used at the time of the run.

### 6. Tool files

**`ompt_window_metrics.csv`** — one row per thread and window:
`tag,win_idx,t_ms,ompt_id,tid,thread_type,cpu,numa,d_rm,d_all,d_instr,d_cyc,ipc,ratio_rm,bucket,thr_T,g_ref,g_dev,flagged`.
`d_*` are the deltas of the window, `ratio_rm` is `-1` when the window cannot be measured,
`thr_T` is the threshold `T_w`, `g_ref`/`g_dev` the global reference and dispersion, and
`flagged = 1` marks the window in which the thread was flagged. `t_ms` is the time since
the first monitor tick.

**`ompt_summary.csv`** — one row per thread:
`tag,tid,ompt_id,thread_type,last_cpu,numa_node,migrations,mig_failures,remote_fills,all_fills,instructions,cycles,ipc,ratio_rm,windows_counted,first_migration_win,perf_status,fill_l2,…,fill_far_dram,mascaras,familia`.
`perf_status` is `full`, `ipc_only` or `failed` (see
[scheduler-internals.md](scheduler-internals.md#6-hardware-counters)).

**`ompt_overhead.csv`** — one row per run: monitor ticks and their time, callback calls
and their total time (`callback_us_total`), migration time, threads tracked, and PMU
diagnostics (resyncs, rescaled reads, dropped window rows, threads with failed or partial
counters, windows with cycles and with fills).

### 7. Latency characterization

`caracterizacion/latencias_exadell.csv`:
`modo,clase,thp,bytes,nivel_esperado,cpu_mide,cpu_calienta,nodo_mem,saltos,ns_min,ns_media,ns_p95`.
Mode `A` is the latency curve against the working-set size (`clase` = `local` or
`remoto`, `thp` = 1/0 with or without transparent huge pages); mode `B` measures pairs of
cores (`--c2c`, `--matriz`); mode `C` measures reads of lines owned by another core. The
thesis uses mode A with THP enabled (Figure 3.1, Table 3.1).

### 8. Things to know before reusing the data

1. **The tool CSVs of the main series also hold earlier attempts.** They are written in
   append mode, and `perf_out_v5/` was reused: `ompt_summary.csv`,
   `ompt_window_metrics.csv` and `ompt_overhead.csv` first contain the attempt of series 1
   (every thread `ipc_only`, `ratio_rm = -1`), then, for S0–S3 only, an intermediate
   partial run, and last the main run. Within each tag the blocks appear in that order;
   the **last block** is the main series. `kernel_metrics.csv` and `metrics/` contain only
   the main series.
2. **Per-origin breakdown.** Only the three far origins are opened
   (`fills_ccd_vecino`, `fills_far_cache`, `fills_far_dram`); the three near ones are
   structurally 0. In the configurations with the tool of the main series all six are 0,
   and so is `bw_remoto_gibs`, because that version of the tool never read the breakdown
   (a guard tested a mask that is never opened). `ratio_rm` is not affected: it comes
   from the exact pair, through an independent path.
3. **`-1` and `NA`.** In the tool CSVs `ratio_rm = -1` means "not measurable" (no fill
   counters, or a window without fills). A genuine 0 means all fills were local.
4. **S0–S2 are too short for the scheduler.** Their measured region lasts at most 0.30 s,
   below the 500 ms the scheduler observes before it may migrate (Section 3.2). The
   thesis analyses the mechanism in S3–S5.
5. **Additional series 2 has other columns.** `kernel_metrics.csv` has no
   `caudal_util_gibs` or `ipc` and adds `referencia`; `ompt_window_metrics.csv` has no
   `d_instr`, `d_cyc`, `ipc` and adds `g_min`, `margen`; `ompt_summary.csv` has no
   instructions, cycles or IPC and adds `referencia`.

### 9. Derived tables (`analisis/`)

| File | Script | Thesis |
|---|---|---|
| `tabla_3_1_latencias.csv` … `tabla_3_6_mediana_cocientes.csv` | `scripts/analisis/tablas_libro.py` | Tables 3.1–3.6 |
| `procesos_por_celda.csv` | `scripts/analisis/analisis_entre_procesos.py` | Runs per cell, mean, SD and CV of each configuration (basis of Section 3.8) |
| `variabilidad_resumen.csv`, `variabilidad_resumen_todas.csv` | same | Table 3.7 |
| `contrastes_scheduler.csv` | same | Table 3.8, Figure 3.16 |
| `anova_procesos.csv` | same | Section 3.8.3 |
| `scheduler_vs_estaticas.csv` | same | Section 3.7 (comparison with the static configurations) |
| `robustez_quiebre.csv`, `robustez_condicion_inicial.csv`, `robustez_transitorio.csv` | `scripts/analisis/robustez_scheduler.py` | Tables 3.9, 3.10 and 3.11 |

---

## Español

### 1. Qué hay en `data/`

| Ruta | Contenido |
|---|---|
| `perf_out_v5/` | Serie principal de la campaña (trabajo de SLURM 29390) |
| `series_adicionales/perf_out_v5_29355/` | Serie adicional 1 (trabajo 29355) |
| `series_adicionales/perf_out_v6/` | Serie adicional 2 (trabajo 29915) |
| `caracterizacion/latencias_exadell.csv` | Caracterización de la latencia de memoria de la plataforma (trabajo 29354) |
| `analisis/*.csv` | Tablas del Capítulo 3 del libro, derivadas de lo anterior |
| `figuras/` | Figuras generadas (no se versionan) |

Todas las medidas provienen del nodo `exadell` del clúster GUANE (SC3-UIS):
2 × AMD EPYC 9554 (Zen 4), 2 nodos NUMA (uno por socket), 128 núcleos y 256 hilos
lógicos, 377 GB de memoria, Clang/LLVM y libomp 21.1.8, hwloc 2.13.0 (Tabla 2.1).

### 2. Las tres series

La misma batería (2 kernels × 6 tamaños × 6 recuentos de hilos × 8 configuraciones = 576
ejecuciones, cada una con 150 repeticiones medidas y 5 de calentamiento, semilla de la
matriz 42) se ejecutó tres veces en el mismo nodo, con el mismo código de los kernels en
la inicialización y en el bucle medido. La herramienta cambió entre series:

| | Principal (`perf_out_v5`) | Adicional 1 (`perf_out_v5_29355`) | Adicional 2 (`perf_out_v6`) |
|---|---|---|---|
| Trabajo de SLURM | 29390 | 29355 | 29915 |
| Herramienta | El código de `src/` | Versión anterior: el grupo de rellenos nunca pudo programarse en la PMU, así que todos los hilos quedaron en `ipc_only` | Variante con otro criterio de disparo |
| Contadores por hilo | ciclos + instrucciones, par de rellenos, 3 máscaras informativas (7 eventos, 3 grupos) | solo ciclos + instrucciones | par de rellenos + 5 máscaras informativas (7 eventos, 2 grupos) |
| Ejecuciones con tiempo | 576 | 432 (faltan las de 128 y 256 hilos de las 6 configuraciones sin herramienta) | 576 |
| `ratio_rm` | sí | no (`NA` en todas) | sí |
| Uso en el libro | todas las configuraciones; la **única** ejecución del Scheduler por celda | tiempos de Base, Close, Spread, Interleave y Control | tiempos y `ratio_rm` de Base, Close, Spread, Interleave y Control; trazas por ventana del Control |

Las ejecuciones que faltan en la serie 1 se perdieron por el límite de descriptores de
archivo (que ahora eleva el `.sbatch`). Ninguna serie adicional se usa para el
Scheduler, porque su herramienta no era la evaluada (Sección 2.4.4 del libro).

### 3. Estructura de una serie

```text
perf_out_v5/
├── kernel_metrics.csv          # una fila por ejecución (576)
├── metrics/
│   ├── <tag>.csv               # resumen escrito por el kernel
│   ├── <tag>_times.csv         # las 150 repeticiones, en orden de ejecución
│   └── <tag>_counters.csv      # contadores por hilo (configuraciones sin herramienta)
├── ompt_window_metrics.csv     # herramienta: una fila por hilo y ventana de 100 ms
├── ompt_summary.csv            # herramienta: una fila por hilo
├── ompt_overhead.csv           # herramienta: una fila por ejecución
├── ompt_logs/                  # <tag>.log (herramienta) y <tag>.stderr (stderr de la ejecución)
├── .done/                      # marcadores de reanudación, un archivo vacío por ejecución terminada
└── perf_v5.29390.out           # registro de SLURM del trabajo
```

`<tag>` es `<kernel>_<tamaño>_t<hilos>_<config>`, por ejemplo
`spmv_static_S5_t8_scheduler`. Los kernels son `stencil` y `spmv_static`; los
identificadores de configuración están en
[operational-guide.md](operational-guide.md#4-la-campaña).

### 4. `kernel_metrics.csv`

| Columna | Significado |
|---|---|
| `kernel`, `size_tag`, `n_or_rows`, `ws_bytes`, `threads`, `config` | Identificación: lado `N` de la malla (Stencil) o filas de la matriz (SpMV), conjunto de trabajo en bytes |
| `binding`, `memory_policy`, `tool`, `migration` | `none/close/spread`, `first-touch/interleave`, `no/ompt`, `-/off/on` |
| `familia`, `warmup`, `reps` | Familia del evento de rellenos (`any`), iteraciones de calentamiento, repeticiones medidas |
| `min_ms`, `avg_ms`, `max_ms`, `stdev_ms` | Estadísticos de las 150 repeticiones. **`avg_ms` es la métrica principal** |
| `migrations` | Migraciones realizadas por la herramienta (0 sin ella) |
| `caudal_util_gibs` | Bytes del modelo del kernel divididos por `avg_ms`: es `1/t` reescalado, no un tráfico medido. Es el «GiB/s» de la figura del SpMV |
| `mlups` | Solo Stencil: `(N−2)² / (t·10⁶)`; `NA` en SpMV |
| `gflops` | Stencil `5·(N−2)²/(t·10⁹)`; SpMV `2·nnz/(t·10⁹)` |
| `ipc` | Instrucciones / ciclos sumados sobre los hilos |
| `fills_l2` … `fills_far_dram` | Desglose informativo de los rellenos por origen (ver la sección 8) |
| `bw_remoto_gibs`, `gib_remotos_total` | `(fills_far_cache + fills_far_dram) × 64 B`, por segundo y en total |
| `ratio_rm` | Rellenos servidos por el otro nodo / rellenos totales, del par exacto de contadores; `NA` cuando no es medible |

`mlups`, `gflops` y `caudal_util_gibs` son proporcionales a `1/avg_ms` para un tamaño
dado: expresan el mismo resultado en distintas unidades.

### 5. Archivos de `metrics/`

- `<tag>.csv` — lo escribe el kernel. Stencil:
  `strategy,n,ws_bytes,threads,warmup,reps,min_ms,avg_ms,max_ms,stddev_ms,caudal_util_gibs,mlups,gflops`;
  SpMV: `strategy,matrix,rows,cols,nnz,ws_bytes,threads,warmup,reps,min_ms,avg_ms,max_ms,stddev_ms,caudal_util_gibs,gflops`.
- `<tag>_times.csv` — `rep,time_ms`, las repeticiones individuales **en orden de
  ejecución**.
- `<tag>_counters.csv` — solo sin herramienta:
  `tag,slot,tid,instructions,cycles,fill_l2,…,fill_far_dram,fill_all,fill_remoto,mascaras,familia`,
  una fila por hilo, acotada a la región medida. `fill_all`/`fill_remoto` son el par
  exacto; la columna `tag` guarda el prefijo de salida usado al ejecutar.

### 6. Archivos de la herramienta

**`ompt_window_metrics.csv`** — una fila por hilo y ventana:
`tag,win_idx,t_ms,ompt_id,tid,thread_type,cpu,numa,d_rm,d_all,d_instr,d_cyc,ipc,ratio_rm,bucket,thr_T,g_ref,g_dev,flagged`.
`d_*` son los incrementos de la ventana, `ratio_rm` vale `-1` cuando la ventana no es
medible, `thr_T` es el umbral `T_w`, `g_ref`/`g_dev` la referencia y la dispersión
globales, y `flagged = 1` señala la ventana en la que se marcó el hilo. `t_ms` es el
tiempo desde la primera activación del monitor.

**`ompt_summary.csv`** — una fila por hilo:
`tag,tid,ompt_id,thread_type,last_cpu,numa_node,migrations,mig_failures,remote_fills,all_fills,instructions,cycles,ipc,ratio_rm,windows_counted,first_migration_win,perf_status,fill_l2,…,fill_far_dram,mascaras,familia`.
`perf_status` es `full`, `ipc_only` o `failed` (ver
[scheduler-internals.md](scheduler-internals.md#6-contadores-de-hardware)).

**`ompt_overhead.csv`** — una fila por ejecución: activaciones del monitor y su tiempo,
llamadas a los *callbacks* y su tiempo total (`callback_us_total`), tiempo de migración,
hilos seguidos y diagnóstico de la PMU (resincronizaciones, lecturas reescaladas, filas
de ventana descartadas, hilos con contadores fallidos o parciales, ventanas con ciclos y
con rellenos).

### 7. Caracterización de latencias

`caracterizacion/latencias_exadell.csv`:
`modo,clase,thp,bytes,nivel_esperado,cpu_mide,cpu_calienta,nodo_mem,saltos,ns_min,ns_media,ns_p95`.
El modo `A` es la curva de latencia frente al tamaño del conjunto de trabajo
(`clase` = `local` o `remoto`, `thp` = 1/0 con o sin páginas enormes transparentes); el
modo `B` mide pares de núcleos (`--c2c`, `--matriz`); el modo `C` mide lecturas de líneas
que pertenecen a otro núcleo. El libro usa el modo A con THP habilitado (Figura 3.1,
Tabla 3.1).

### 8. Lo que conviene saber antes de reutilizar los datos

1. **Los CSV de la herramienta de la serie principal contienen también intentos
   anteriores.** Se escriben en modo de adición y `perf_out_v5/` se reutilizó:
   `ompt_summary.csv`, `ompt_window_metrics.csv` y `ompt_overhead.csv` contienen primero
   el intento de la serie 1 (todos los hilos `ipc_only`, `ratio_rm = -1`), después, solo
   para S0–S3, una corrida parcial intermedia, y por último la ejecución principal.
   Dentro de cada etiqueta los bloques aparecen en ese orden; el **último bloque** es la
   serie principal. `kernel_metrics.csv` y `metrics/` contienen solo la serie principal.
2. **Desglose por origen.** Solo se abren los tres orígenes lejanos
   (`fills_ccd_vecino`, `fills_far_cache`, `fills_far_dram`); los tres cercanos valen 0
   por construcción. En las configuraciones con herramienta de la serie principal los
   seis valen 0, igual que `bw_remoto_gibs`, porque esa versión de la herramienta nunca
   leyó el desglose (una guarda comprobaba una máscara que nunca se abre). `ratio_rm` no
   se ve afectado: sale del par exacto, por un camino independiente.
3. **`-1` y `NA`.** En los CSV de la herramienta, `ratio_rm = -1` significa «no medible»
   (sin contadores de rellenos, o ventana sin rellenos). Un 0 genuino significa que todos
   los rellenos fueron locales.
4. **S0–S2 son demasiado cortos para el scheduler.** Su región medida dura como máximo
   0,30 s, menos que los 500 ms que el scheduler observa antes de poder migrar
   (Sección 3.2). El libro analiza el mecanismo en S3–S5.
5. **La serie adicional 2 tiene otras columnas.** `kernel_metrics.csv` no tiene
   `caudal_util_gibs` ni `ipc` y añade `referencia`; `ompt_window_metrics.csv` no tiene
   `d_instr`, `d_cyc` ni `ipc` y añade `g_min` y `margen`; `ompt_summary.csv` no tiene
   instrucciones, ciclos ni IPC y añade `referencia`.

### 9. Tablas derivadas (`analisis/`)

| Archivo | Script | Libro |
|---|---|---|
| `tabla_3_1_latencias.csv` … `tabla_3_6_mediana_cocientes.csv` | `scripts/analisis/tablas_libro.py` | Tablas 3.1 a 3.6 |
| `procesos_por_celda.csv` | `scripts/analisis/analisis_entre_procesos.py` | Ejecuciones por celda, media, DE y CV de cada configuración (base de la Sección 3.8) |
| `variabilidad_resumen.csv`, `variabilidad_resumen_todas.csv` | ídem | Tabla 3.7 |
| `contrastes_scheduler.csv` | ídem | Tabla 3.8, Figura 3.16 |
| `anova_procesos.csv` | ídem | Sección 3.8.3 |
| `scheduler_vs_estaticas.csv` | ídem | Sección 3.7 (comparación con las configuraciones estáticas) |
| `robustez_quiebre.csv`, `robustez_condicion_inicial.csv`, `robustez_transitorio.csv` | `scripts/analisis/robustez_scheduler.py` | Tablas 3.9, 3.10 y 3.11 |
