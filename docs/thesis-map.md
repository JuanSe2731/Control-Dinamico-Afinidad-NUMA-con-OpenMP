# Thesis Map · Mapa del libro

[English](#english) · [Español](#español)

Thesis / Libro: *Implementación y evaluación de un prototipo de scheduler para el control
dinámico de la afinidad NUMA utilizando OMPT*. Juan Sebastian Otero Vega, David Fernando
Naranjo Rangel. Universidad Industrial de Santander, 2026.

---

## English

The repository does not need the thesis to be used, but whoever has it can check every
table and figure of the results chapter against the data. This map links each section,
equation, table and figure of the thesis to the code, data and script behind it.

### Rebuilding everything

```bash
pip install --user pandas scipy matplotlib seaborn
python3 scripts/analisis/analisis_entre_procesos.py   # Tables 3.7, 3.8 and the ANOVA
python3 scripts/analisis/robustez_scheduler.py        # Tables 3.9–3.11 (after the previous one)
python3 scripts/analisis/tablas_libro.py              # Tables 3.1–3.6
python3 scripts/plots/generar_figuras_libro.py        # Figures 3.1–3.16 -> data/figuras/libro/
```

The CSVs land in `data/analisis/`. The scripts print more decimals than the thesis, which
rounds them; in a few cells of Tables 3.1 and 3.4 the last printed digit of the thesis
differs by one unit because an already rounded value was rounded again.

### Chapter 2 — Methodology

| Thesis | Content | Repository |
|---|---|---|
| §2.1.1, Table 2.1, Figure 2.1 | Platform (`exadell`) | Summary in [data-guide.md](data-guide.md#1-what-is-in-data) |
| §2.1.2 | Pointer-chasing latency microbenchmark | `src/latencias_numa.cpp`, `scripts/run_latencias.sbatch` → `data/caracterizacion/latencias_exadell.csv` |
| §2.1.3 | Reference kernels | `src/Stencil.cpp`, `src/spmv_staticSeed.cpp` |
| §2.2.1, Table 2.2 | Parallel versions (`schedule(static)` over rows; `schedule(static, 256)`) and the six configurations | The kernels; `CONFIGS` in `scripts/run_perf_metricsV5.sh` |
| §2.3.1 | OMPT integration, callbacks, thread table, hwloc topology | `ompt_initialize`, `upsert_thread`, `on_ompt_callback_*`, `cpu_to_numa_node_hwloc` in `src/sched_NUMA_optC_leaky.cpp` |
| §2.3.2, Table 2.3 | Monitor thread, counters, multiplexing, degradation by group | `open_perf_for_thread`, `read_counter`, `read_fills`, first pass of `monitor_loop` |
| §2.3.3, Eq. 2.1–2.3 | Adaptive reference (`ρ_w`, `M_w`, EWMA, seeding) | `monitor_loop`: seeding and the update at the end of the window |
| §2.3.3, Eq. 2.4–2.6 | Threshold, leaky bucket, capacity | `monitor_loop`: `thrT`, `leak`, `cap` and the second pass |
| §2.3.3, Table 2.4 | Parameters | Constants at the top of `src/sched_NUMA_optC_leaky.cpp` |
| §2.3.4 | Rebinding to the opposite node, one migration per thread | `on_ompt_callback_implicit_task` |
| §2.4.1 | Isolation of the environment | Operating procedure; not part of the code |
| §2.4.2, Table 2.5 | Campaign design, size ladder, 150 repetitions, 5 warm-up, measured region | `SIZES_DEF` in `scripts/run_perf_metricsV5.sh`; `omp_control_tool` and `prctl` in the kernels; `src/perf_region.hpp` |
| §2.4.3, Eq. 2.7–2.8 | Metrics and decomposition `C_monitor`, `G_migrar`, `G_neta` | `avg_ms` of `kernel_metrics.csv`; Table 3.2 in `tablas_libro.py` |
| §2.4.4, Eq. 2.9–2.10 (and 1.12–1.13) | Runs as the unit, pooled variance, t for a single run, Welch, ANOVA, Cohen's d | `scripts/analisis/analisis_entre_procesos.py` |
| §2.4.4, Eq. 2.11 | Break-even variability of the scheduler | `quiebre()` in `scripts/analisis/robustez_scheduler.py` |

### Chapter 3 — Figures

All files are written to `data/figuras/libro/` by `scripts/plots/generar_figuras_libro.py`
with the names used in the thesis.

| Figure | File | Data | Drawn by |
|---|---|---|---|
| 3.1 | `p01_curva_latencia.png` | `caracterizacion/latencias_exadell.csv` (mode A, THP) | `f01_curva_latencia` (`plot_presentacion.py`) |
| 3.2 | `p03_por_que_no_s0s2.png` | `kernel_metrics.csv` (`ratio_rm` of Base), `ompt_window_metrics.csv` (windows) | `f03_por_que_no_s0s2` |
| 3.3 | `p04_migraciones.png` | `migrations` of `kernel_metrics.csv` | `f04_migraciones` |
| 3.4 | `p05_spmv_static_tiempo.png` | `avg_ms` | `f05_tiempo` |
| 3.5 | `p06_spmv_static_ganancia_pct.png` | `avg_ms` | `f06_ganancia_pct` |
| 3.6 | `p07_spmv_static_descomposicion.png` | `avg_ms` | `f07_descomposicion_apilada` |
| 3.7 | `p11_spmv_static_gflops.png` | `gflops` | `f10_throughput` |
| 3.8 | `p12_spmv_static_gibs.png` | `caudal_util_gibs` | `f10_throughput` |
| 3.9 | `p05_stencil_tiempo.png` | `avg_ms` | `f05_tiempo` |
| 3.10 | `p06_stencil_ganancia_pct.png` | `avg_ms` | `f06_ganancia_pct` |
| 3.11 | `p07_stencil_descomposicion.png` | `avg_ms` | `f07_descomposicion_apilada` |
| 3.12 | `p10_stencil_mlups.png` | `mlups` | `f10_throughput` |
| 3.13 | `p11_stencil_gflops.png` | `gflops` | `f10_throughput` |
| 3.14 | `p08_spmv_static_estaticas.png` | `avg_ms` | `f08_estaticas_lineas` (`generar_figuras_libro.py`) |
| 3.15 | `p08_stencil_estaticas.png` | `avg_ms` | `f08_estaticas_lineas` |
| 3.16 | `p13_ganancia_ic_procesos.png` | `analisis/contrastes_scheduler.csv` | `figura_intervalos` |

Unless stated otherwise, the data are `data/perf_out_v5/kernel_metrics.csv`.

### Chapter 3 — Tables

| Table | Section | CSV in `data/analisis/` | Script |
|---|---|---|---|
| 3.1 | §3.1 | `tabla_3_1_latencias.csv` | `tablas_libro.py` |
| 3.2 | §3.3 | `tabla_3_2_ejecuciones_migradas.csv` | `tablas_libro.py` |
| 3.3 | §3.4.5 | `tabla_3_3_transitorio.csv` | `tablas_libro.py` |
| 3.4 | §3.6 | `tabla_3_4_costo_callbacks.csv` | `tablas_libro.py` |
| 3.5 | §3.7 | `tabla_3_5_s5_estaticas.csv` | `tablas_libro.py` |
| 3.6 | §3.7 | `tabla_3_6_mediana_cocientes.csv` | `tablas_libro.py` |
| 3.7 | §3.8.1 | `variabilidad_resumen.csv` (Base, Control, Static) and `variabilidad_resumen_todas.csv` (All) | `analisis_entre_procesos.py` |
| 3.8 | §3.8.2 | `contrastes_scheduler.csv` (S4 and S5 rows) | `analisis_entre_procesos.py` |
| — | §3.8.3 | `anova_procesos.csv` | `analisis_entre_procesos.py` |
| — | §3.7 | `scheduler_vs_estaticas.csv` | `analisis_entre_procesos.py` |
| 3.9 | §3.8.4 | `robustez_quiebre.csv` | `robustez_scheduler.py` |
| 3.10 | §3.8.4 | `robustez_condicion_inicial.csv` | `robustez_scheduler.py` |
| 3.11 | §3.8.4 | `robustez_transitorio.csv` | `robustez_scheduler.py` |

### Other parts of the thesis

| Thesis | Repository |
|---|---|
| Abstract and Introduction (scope) | [README.md](../README.md) |
| Chapter 1 (framework) | Background only; Eq. 1.12–1.13 are implemented in `t_test()` of `analisis_entre_procesos.py` |
| Chapter 4 (future work) | Limits listed in [scheduler-internals.md](scheduler-internals.md#18-scope-and-limits) |
| "Repositorio y recursos complementarios" | This repository |

---

## Español

El repositorio no necesita el libro para usarse, pero quien lo tenga puede comprobar cada
tabla y figura del capítulo de resultados contra los datos. Este mapa enlaza cada
sección, ecuación, tabla y figura del libro con el código, los datos y el script que la
respaldan.

### Reconstruirlo todo

```bash
pip install --user pandas scipy matplotlib seaborn
python3 scripts/analisis/analisis_entre_procesos.py   # Tablas 3.7, 3.8 y el ANOVA
python3 scripts/analisis/robustez_scheduler.py        # Tablas 3.9 a 3.11 (después del anterior)
python3 scripts/analisis/tablas_libro.py              # Tablas 3.1 a 3.6
python3 scripts/plots/generar_figuras_libro.py        # Figuras 3.1 a 3.16 -> data/figuras/libro/
```

Los CSV quedan en `data/analisis/`. Los scripts imprimen más decimales que el libro, que
los redondea; en unas pocas celdas de las Tablas 3.1 y 3.4 el último dígito del libro
difiere en una unidad porque se redondeó un valor ya redondeado.

### Capítulo 2 — Metodología

| Libro | Contenido | Repositorio |
|---|---|---|
| §2.1.1, Tabla 2.1, Figura 2.1 | Plataforma (`exadell`) | Resumen en [data-guide.md](data-guide.md#1-qué-hay-en-data) |
| §2.1.2 | Microbenchmark de latencia por persecución de punteros | `src/latencias_numa.cpp`, `scripts/run_latencias.sbatch` → `data/caracterizacion/latencias_exadell.csv` |
| §2.1.3 | Kernels de referencia | `src/Stencil.cpp`, `src/spmv_staticSeed.cpp` |
| §2.2.1, Tabla 2.2 | Versiones paralelas (`schedule(static)` sobre filas; `schedule(static, 256)`) y las seis configuraciones | Los kernels; `CONFIGS` en `scripts/run_perf_metricsV5.sh` |
| §2.3.1 | Integración con OMPT, *callbacks*, tabla de hilos, topología con hwloc | `ompt_initialize`, `upsert_thread`, `on_ompt_callback_*`, `cpu_to_numa_node_hwloc` en `src/sched_NUMA_optC_leaky.cpp` |
| §2.3.2, Tabla 2.3 | Hilo monitor, contadores, multiplexación, degradación por grupo | `open_perf_for_thread`, `read_counter`, `read_fills`, primera pasada de `monitor_loop` |
| §2.3.3, Ec. 2.1–2.3 | Referencia adaptativa (`ρ_w`, `M_w`, EWMA, inicialización) | `monitor_loop`: inicialización y actualización al final de la ventana |
| §2.3.3, Ec. 2.4–2.6 | Umbral, balde con fugas, capacidad | `monitor_loop`: `thrT`, `leak`, `cap` y la segunda pasada |
| §2.3.3, Tabla 2.4 | Parámetros | Constantes al comienzo de `src/sched_NUMA_optC_leaky.cpp` |
| §2.3.4 | Reubicación en el nodo opuesto, una migración por hilo | `on_ompt_callback_implicit_task` |
| §2.4.1 | Aislamiento del entorno | Procedimiento de operación; no forma parte del código |
| §2.4.2, Tabla 2.5 | Diseño de la campaña, escalera de tamaños, 150 repeticiones, 5 de calentamiento, región medida | `SIZES_DEF` en `scripts/run_perf_metricsV5.sh`; `omp_control_tool` y `prctl` en los kernels; `src/perf_region.hpp` |
| §2.4.3, Ec. 2.7–2.8 | Métricas y descomposición `C_monitor`, `G_migrar`, `G_neta` | `avg_ms` de `kernel_metrics.csv`; Tabla 3.2 en `tablas_libro.py` |
| §2.4.4, Ec. 2.9–2.10 (y 1.12–1.13) | La ejecución como unidad, varianza agrupada, t para una sola ejecución, Welch, ANOVA, d de Cohen | `scripts/analisis/analisis_entre_procesos.py` |
| §2.4.4, Ec. 2.11 | Punto de quiebre de la variabilidad del scheduler | `quiebre()` en `scripts/analisis/robustez_scheduler.py` |

### Capítulo 3 — Figuras

`scripts/plots/generar_figuras_libro.py` escribe todos los archivos en
`data/figuras/libro/` con los nombres que usa el libro.

| Figura | Archivo | Datos | La dibuja |
|---|---|---|---|
| 3.1 | `p01_curva_latencia.png` | `caracterizacion/latencias_exadell.csv` (modo A, THP) | `f01_curva_latencia` (`plot_presentacion.py`) |
| 3.2 | `p03_por_que_no_s0s2.png` | `kernel_metrics.csv` (`ratio_rm` de Base), `ompt_window_metrics.csv` (ventanas) | `f03_por_que_no_s0s2` |
| 3.3 | `p04_migraciones.png` | `migrations` de `kernel_metrics.csv` | `f04_migraciones` |
| 3.4 | `p05_spmv_static_tiempo.png` | `avg_ms` | `f05_tiempo` |
| 3.5 | `p06_spmv_static_ganancia_pct.png` | `avg_ms` | `f06_ganancia_pct` |
| 3.6 | `p07_spmv_static_descomposicion.png` | `avg_ms` | `f07_descomposicion_apilada` |
| 3.7 | `p11_spmv_static_gflops.png` | `gflops` | `f10_throughput` |
| 3.8 | `p12_spmv_static_gibs.png` | `caudal_util_gibs` | `f10_throughput` |
| 3.9 | `p05_stencil_tiempo.png` | `avg_ms` | `f05_tiempo` |
| 3.10 | `p06_stencil_ganancia_pct.png` | `avg_ms` | `f06_ganancia_pct` |
| 3.11 | `p07_stencil_descomposicion.png` | `avg_ms` | `f07_descomposicion_apilada` |
| 3.12 | `p10_stencil_mlups.png` | `mlups` | `f10_throughput` |
| 3.13 | `p11_stencil_gflops.png` | `gflops` | `f10_throughput` |
| 3.14 | `p08_spmv_static_estaticas.png` | `avg_ms` | `f08_estaticas_lineas` (`generar_figuras_libro.py`) |
| 3.15 | `p08_stencil_estaticas.png` | `avg_ms` | `f08_estaticas_lineas` |
| 3.16 | `p13_ganancia_ic_procesos.png` | `analisis/contrastes_scheduler.csv` | `figura_intervalos` |

Salvo que se indique otra cosa, los datos son `data/perf_out_v5/kernel_metrics.csv`.

### Capítulo 3 — Tablas

| Tabla | Sección | CSV en `data/analisis/` | Script |
|---|---|---|---|
| 3.1 | §3.1 | `tabla_3_1_latencias.csv` | `tablas_libro.py` |
| 3.2 | §3.3 | `tabla_3_2_ejecuciones_migradas.csv` | `tablas_libro.py` |
| 3.3 | §3.4.5 | `tabla_3_3_transitorio.csv` | `tablas_libro.py` |
| 3.4 | §3.6 | `tabla_3_4_costo_callbacks.csv` | `tablas_libro.py` |
| 3.5 | §3.7 | `tabla_3_5_s5_estaticas.csv` | `tablas_libro.py` |
| 3.6 | §3.7 | `tabla_3_6_mediana_cocientes.csv` | `tablas_libro.py` |
| 3.7 | §3.8.1 | `variabilidad_resumen.csv` (Base, Control, Estáticas) y `variabilidad_resumen_todas.csv` (Todas) | `analisis_entre_procesos.py` |
| 3.8 | §3.8.2 | `contrastes_scheduler.csv` (filas de S4 y S5) | `analisis_entre_procesos.py` |
| — | §3.8.3 | `anova_procesos.csv` | `analisis_entre_procesos.py` |
| — | §3.7 | `scheduler_vs_estaticas.csv` | `analisis_entre_procesos.py` |
| 3.9 | §3.8.4 | `robustez_quiebre.csv` | `robustez_scheduler.py` |
| 3.10 | §3.8.4 | `robustez_condicion_inicial.csv` | `robustez_scheduler.py` |
| 3.11 | §3.8.4 | `robustez_transitorio.csv` | `robustez_scheduler.py` |

### Otras partes del libro

| Libro | Repositorio |
|---|---|
| Resumen e Introducción (alcance) | [README.md](../README.md) |
| Capítulo 1 (marco de referencia) | Solo antecedentes; las Ec. 1.12–1.13 están implementadas en `t_test()` de `analisis_entre_procesos.py` |
| Capítulo 4 (trabajo futuro) | Límites listados en [scheduler-internals.md](scheduler-internals.md#18-alcance-y-límites) |
| «Repositorio y recursos complementarios» | Este repositorio |
