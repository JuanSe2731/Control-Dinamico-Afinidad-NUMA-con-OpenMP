# Planes de mejora — banco de pruebas aislado

Este directorio contiene **experimentos independientes** derivados de
[PLANES_MEJORA_RENDIMIENTO.md](../PLANES_MEJORA_RENDIMIENTO.md). Cada plan tiene
su **propia copia** de las fuentes y su propio scheduler/kernels modificados, de
modo que **el directorio principal del proyecto no se ve afectado**.

Un solo `sbatch` corre todos los planes en orden; un script de análisis compara
los resultados y dice cuál gana.

## Estructura

```
planes de mejora/
├── BASE/ B1/ C1/ B3/ C3/ B2/ C2/ A1/ A2/ D1/         # un experimento por carpeta
│     ├── sched_NUMA_optC_leaky.cpp   # scheduler (modificado segun el plan)
│     ├── Stencil.cpp, spmv_staticSeed.cpp, ...      # kernels (modif. en A1/A2)
│     ├── run_perf_metricsV4.sh       # runner del plan (hereda los fixes del principal)
│     └── stokes.mtx -> ../../stokes.mtx             # symlink (no se duplica ~GB)
├── run_all_plans.sh        # orquestador REANUDABLE (corre todos los planes)
├── run_all_plans.sbatch    # 1 solo envio a SLURM
├── analyze_plans.py        # comparacion + graficas + ranking
├── results/<PLAN>/         # kernel_metrics.csv y demas salidas por plan
└── comparison_figures/     # graficas comparativas + combined_kernel_metrics.csv + ranking.csv
```

Cada plan corre las etapas **2 (baseline OMP `spread`/`close`) y 3 (scheduler)**.
La **etapa 1 (serial) se OMITE** en estas pruebas (`RUN_SERIAL=0`) porque no es la
comparación principal y ahorra tiempo. Así cada carpeta trae su **propio baseline**
`spread` para una comparación justa.

### Dónde debe estar `stokes.mtx`
El SpMV usa `stokes.mtx` en la **raíz del repo** (junto a `run_perf_metricsV4.sh`):
`/scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP/stokes.mtx`.
Cada carpeta de plan ya tiene un symlink `stokes.mtx -> ../../stokes.mtx` que apunta
ahí, así que **solo debe existir esa única copia en la raíz** (la misma que ya usa el
proyecto principal). No hay que copiarla a cada plan.

## Qué cambia cada plan

| Plan | Qué modifica | Madurez |
|---|---|---|
| **BASE** | Nada — optC actual, como **referencia** a batir | ✅ estable |
| **B1** | Scheduler: migra solo si `ratio>T` **y** el IPC del hilo cae (gate por IPC) | ✅ estable (compila) |
| **B2** | Scheduler: dispara si el IPC del hilo está muy por debajo del IPC típico (proxy de "stall por memoria") | ✅ estable (compila) |
| **B3** | Scheduler: migración de **prueba** + evalúa IPC y hace **rollback** si no mejora (hasta 2 migr.) | ✅ estable (compila) |
| **C1** | Scheduler: **bloquea** migraciones que sobrecargarían un nodo (balance) | ✅ estable (compila) |
| **C2** | Scheduler: decisión **global** por ventana — migra 1 mejor candidato con capacidad | ✅ estable (compila) |
| **C3** | Scheduler: **bajo overhead** — presupuesto global (1 migr. total), sin `fflush` por ventana, traza tras `OMPT_VERBOSE` | ✅ estable (compila) |
| **A1** | Kernel Stencil con **first-touch paralelo** + corre **sin** `interleave` (Stencil); SpMV mantiene interleave | ✅ estable (compila/corre) |
| **A2** | A1 + **`move_pages`**: las páginas del hilo se re-alojan a su nodo actual cada 25 reps (Stencil) | ⚠️ prototipo (compila/corre; ajustar en cluster) |
| **D1** | Stencil con **mala colocación** (init secuencial en nodo 0, sin interleave) → el estático sufre; testbed para migración | ⚠️ escenario (compila/corre) |

> **D2 (AutoNUMA) fue eliminado por completo**: activar `numa_balancing` requiere
> permisos de root que no tenemos en el cluster, así que esa prueba no se puede llevar a cabo.

Notas de prototipo:
- **A2** usa `move_pages` vía syscall crudo (sin `-lnuma`); si falla, se ignora
  (no afecta correctitud). El mapeo hilo→filas es aproximado (contiguo por hilo);
  afínalo si el ratio_rm no baja tras migrar.
- **A2/D1** aplican solo a **Stencil** (SpMV con first-touch correcto necesita un
  alocador que no inicialice; es trabajo futuro — ver A1 en el doc de planes).

## Cómo correr (cluster, 1 solo envío)

```bash
cd "/scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP"
sbatch "planes de mejora/run_all_plans.sbatch"
```

Es **reanudable**: las 10 corridas (sin serial, pero con Stencil ahora a
`N=23500` ≈ 4.4 GB) pueden pasarse de 24 h. Si el job se corta por tiempo,
**vuelve a enviar el mismo sbatch** — saltará los planes ya terminados
(marca `results/<PLAN>/.done`) y seguirá con los que falten. Puede requerir 2–3
reenvíos. Si tu partición permite más tiempo, sube `--time`. Si necesitas acortar,
baja `REPS` en el `run_perf_metricsV4.sh` de cada plan.

Correr solo algunos planes, o re-correr todo:
```bash
PLANS="B1 C1 A1" sbatch "planes de mejora/run_all_plans.sbatch"   # subconjunto
FORCE=1          sbatch "planes de mejora/run_all_plans.sbatch"   # ignora .done
```

## Cómo analizar y comparar

El sbatch ya lo intenta al final. Manual:
```bash
python3 "planes de mejora/analyze_plans.py" --results "planes de mejora/results"
```
Genera en `planes de mejora/comparison_figures/`, por métrica
(`mlups` [solo Stencil], `gflops`, `bw_gibs`, `stdev_ms`):
- `cmp_<kernel>_<metrica>_abs.png/.eps` — la métrica del scheduler de cada plan vs. el baseline `spread`.
- `cmp_<kernel>_<metrica>_rel.png/.eps` — **scheduler / su propio baseline spread**;
  `>1.0` (o `<1.0` en `stdev_ms`) = **supera al estático**. Esta es la gráfica clave.
- `combined_kernel_metrics.csv`, `ranking.csv` y un resumen en consola de qué plan gana más veces.

El **ranking del Stencil usa MLUPS** (millones de actualizaciones de malla/s) como
métrica principal; el de SpMV usa `bw_gibs`.

## Prueba local rápida (humo, sin cluster)

```bash
cd "planes de mejora"
PLANS="BASE B1" ./run_all_plans.sh    # requiere perf + numactl + hwloc + stokes.mtx
```
En local sin `stokes.mtx`/`perf`/`numactl` la parte SpMV y las ventanas `perf`
fallarán; sirve para validar que compila y que la mecánica del orquestador corre.
El tamaño de corrida (hilos, REPS) se edita en el `run_perf_metricsV4.sh` de cada plan.

## Cambios hechos en el directorio PRINCIPAL (a pedido)

En [../run_perf_metricsV4.sh](../run_perf_metricsV4.sh):
1. **Se eliminó** la 2.ª repetición `cores_close_interleave_rep2` de `OMP_CONFIGS`.
2. La **etapa 3 (scheduler)** ya **no recorre** las configuraciones OMP (el scheduler
   deja `OMP_PLACES`/`OMP_PROC_BIND` sin fijar): corre **una vez** por `(hilos,kernel)`
   y se etiqueta como `<kernel>_scheduler` — no con un nombre de config OMP — para no
   confundir el análisis.
3. **`STENCIL_N` = 8000 → 23500**: el working set del Stencil pasa de ~512 MB a
   **~4.42 GB**, equivalente al de SpMV (`stokes.mtx`: values `NNZ×8` + col_idxs
   `NNZ×4` + vectores ≈ 4.42 GB). Cálculo: Stencil usa 2 arrays float → `N²×8`
   bytes; `√(4.42e9/8) ≈ 23500`. Así ambos kernels trabajan sobre un working set
   comparable (aunque no se comparen entre sí).
4. **MLUPS** en [../Stencil.cpp](../Stencil.cpp): nueva métrica de rendimiento del
   stencil = puntos_interiores `(N-2)²` / (tiempo_min × 1e6). Se añadió al CSV del
   kernel y a `kernel_metrics.csv` (columna `mlups`; `NA` para SpMV).

Estos 4 cambios están **heredados en cada carpeta de plan**. Adicionalmente, en las
pruebas de plan la **etapa 1 (serial) se omite** (`RUN_SERIAL=0`, solo en el
orquestador; el script principal sigue corriendo la serial por defecto).
