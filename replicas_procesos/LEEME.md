# Campaña de réplicas de proceso

Directorio autónomo para medir la **variabilidad entre procesos** que la campaña V5
(trabajo 29390) no pudo medir, y hacer con ella las pruebas estadísticas del
documento tomando el **proceso** como unidad de análisis.

No toca nada fuera de este directorio: ni el documento de tesis, ni `perf_out_v5/`,
ni los scripts de la raíz. Las fuentes están **copiadas** aquí; los resultados y el
análisis se generan aquí.

---

## 1. Qué resuelve

En V5 cada punto (kernel, tamaño, hilos, configuración) es **una** ejecución, y las
pruebas de Welch y el ANOVA toman como muestra sus 150 repeticiones. El documento lo
declara como limitación (§3.4.2 y §4.8; `METODOLOGIA_EXPERIMENTAL.md` §6): esa muestra
mide el ruido de iteración a iteración, pero no lo que cambia de un proceso a otro
—en qué nodo cae el hilo maestro al inicializar y con él, por *first-touch*, todas las
páginas; qué CPU elige el sistema para cada hilo sin afinidad; el estado de la
máquina—, y las repeticiones están autocorrelacionadas. El error estándar sale
optimista y casi cualquier diferencia resulta significativa.

Aquí cada punto se ejecuta **10 veces en procesos independientes**. La observación es
la media de las 150 repeticiones de cada proceso, y todas las pruebas se hacen con
n = 10 por grupo. Eso convierte la limitación declarada en un resultado: cuánto
subestimaba el análisis intra-proceso la incertidumbre real, y qué diferencias
sobreviven cuando se mide bien. De paso resuelve la duda sobre el control con 64 y
128 hilos en SpMV S5 (§4.6): si la diferencia de `ratio_rm` entre base y control es
del tool o de la lotería de colocación, se verá en la dispersión entre procesos.

## 2. Diseño

| | Valor por defecto |
|---|---|
| Tamaños | **S3, S4, S5** (el régimen de análisis del documento) |
| Configuraciones | **base, obs** (control), **scheduler, interleave, bind_spread** |
| Hilos | 8, 16, 32, 64, 128, 256 |
| Kernels | stencil, spmv_static |
| Réplicas | **r01…r10** (10 procesos por grupo) |
| Ejecuciones | 180 por réplica, **1800** en total |
| Duración | ~25 min por réplica, **~4–5 h** en total (el sbatch pide 10 h) |

Respecto a la propuesta original (S4 y S5; base, control, scheduler e intercalado) se
añaden dos cosas baratas para poder repetir **exactamente** las pruebas del documento:

- **`bind_spread`**, porque el ANOVA del documento es base / afinidad dispersa /
  scheduler. Cuesta ~5 min por réplica.
- **S3**, porque ese ANOVA cubre las 36 celdas de S3 a S5. Cuesta ~2 min por réplica.

Para volver a la propuesta original:
`SIZES="S4 S5" CONFIG_LIST="base obs scheduler interleave" sbatch run_replicas.sbatch`.

Las réplicas corren **en secuencia** (r01 entera, luego r02…), así que los 10 procesos
de cada grupo quedan repartidos a lo largo de todo el trabajo. Dentro de cada bloque
(tamaño, hilos) el orden de las 10 ejecuciones se **baraja** con una semilla distinta
por réplica: con un orden fijo, cualquier arrastre de la ejecución anterior
(temperatura, frecuencia, fragmentación de memoria) caería siempre igual sobre la
misma configuración y se volvería un sesgo que las réplicas no verían. Barajado se
vuelve ruido, que las réplicas sí miden. El orden real queda en `cronologia.csv`.

La matriz del SpMV es la **misma** en todas las réplicas (`SPMV_SEED=42`): se mide la
variación entre procesos del mismo problema, no entre problemas.

## 3. Contenido

| Fichero | Origen |
|---|---|
| `sched_NUMA_optC_leaky.cpp`, `perf_region.hpp`, `Stencil.cpp`, `spmv_staticSeed.cpp` | **Copias sin cambios** de la raíz (commit `44572af`) |
| `run_perf_metricsV5.sh` | Copia de la suite V5 con siete cambios marcados `[REPLICAS]` y explicados en su cabecera: filtro `CONFIG_LIST`, `COMPILAR=si\|no\|solo`, orden barajado, limpieza de intentos anteriores, resumen OMPT por ejecución, `cronologia.csv` y `OUTDIR` obligatorio. Ver con `diff -u -w ../run_perf_metricsV5.sh run_perf_metricsV5.sh` |
| `run_replicas.sh` | Nuevo. Compila una vez, comprueba que el tool carga y cuenta, lanza las réplicas, deja la procedencia e informa de lo pendiente |
| `run_replicas.sbatch` | Nuevo. Envoltorio SLURM para exadell, mismos recursos que el de V5 |
| `analisis_replicas.py` | Nuevo. Análisis por procesos (sección 6) |
| `.gitignore` | Deja fuera los binarios y la traza por ventana (~70 MB por réplica) |

sha256 de las fuentes copiadas:

```
f465ff21667f3b4120566ae0ebd7d652cffa82e1464d0c95031a3d319fad77ab  sched_NUMA_optC_leaky.cpp
76b24b08077e0b5c25914a7cfe042e74e40c0619acc272a4b316adaeb406532f  perf_region.hpp
9d311085a5b7b0f616c83c61d3f8c82b9eb1300e9ae45b6485b57b80930e130a  Stencil.cpp
3906ab4b75debe715c8f3d259ea738d8fbbce353b6b500d24d729f99d65d4917  spmv_staticSeed.cpp
```

## 4. Qué versión del código se mide (y en qué difiere de 29390)

Las fuentes son las **actuales** de la raíz (V5 final, commit `44572af`). La campaña
29390, de la que salen las tablas del documento, se ejecutó con el código del commit
`39de621`. Entre ambas:

- **Igual**: el bucle medido de los dos kernels (solo se retiró la columna
  `caudal_util_gibs`) y todo el disparador del scheduler (`g_ref`, `g_dev`, balde,
  umbrales, `MAX_MIGRATIONS = 1`).
- **Distinto, solo instrumentación**: el tool ya no abre ciclos ni instrucciones (el
  IPC se retiró), así que el hilo monitor lee **2 contadores por hilo y ventana en
  vez de 4**, y el desglose por origen pasó de 3 a 5 máscaras. `perf_region.hpp`
  también dejó de abrir ciclos e instrucciones, pero se lee fuera del bucle medido.

Consecuencia: base, interleave y bind_spread miden lo mismo que en 29390; obs y
scheduler llevan un monitor algo más ligero, así que `C_monitor` puede salir algo
menor, sobre todo con 128 y 256 hilos. En una versión futura del documento conviene
presentar los resultados por procesos **como un conjunto**, sin mezclarlos con los
valores de una sola ejecución de 29390. `contraste_29390.csv` existe precisamente para
decir si aquella ejecución única fue representativa.

## 5. Cómo lanzarlo en el clúster

Todo **desde este directorio**. El sbatch lo comprueba y aborta si se lanza desde la
raíz, porque ahí ejecutaría la suite de la raíz y escribiría en `perf_out_v5`.

```bash
cd /scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP/replicas_procesos
module load clang/17.0.6 hwloc               # ANTES de sbatch: el sbatch no carga módulos
cat /proc/sys/kernel/perf_event_paranoid     # tiene que ser <= 2

# Prueba de humo (~7 min): una réplica de S5 con 8 hilos y las 150 repeticiones
# completas. Con menos no da tiempo a migrar: en 29390 la primera migración de SpMV
# con 8 hilos llegó a los 13-18 s. Sirve para ver que ESTA versión del tool migra.
N_REPLICAS=1 SIZES="S5" THREAD_LIST="8" RESULTADOS=prueba_humo \
    sbatch --time=00:30:00 run_replicas.sbatch
# al terminar: kernel, hilos, config, avg_ms, migrations, ratio_rm. En 29390 el
# scheduler migró 4 de 8 hilos (SpMV) y 2 de 8 (Stencil), con ratio_rm muy por
# debajo del de obs. Si sale 0 en los dos kernels, repite antes de lanzar la campaña.
cut -d, -f1,5,6,15,18,29 prueba_humo/r01/kernel_metrics.csv
rm -rf prueba_humo replicas.<JOBID_HUMO>.*

# Campaña completa
sbatch run_replicas.sbatch
squeue -u "$USER"
tail -f replicas.<JOBID>.out

# Si SLURM lo corta, el mismo comando retoma donde iba (una réplica completa se salta)
sbatch run_replicas.sbatch                   # hasta que el final diga "PENDIENTES: 0"
```

Qué comprueba `run_replicas.sh` antes de gastar el trabajo:

- `perf_event_paranoid <= 2`; si no, el scheduler no podría contar y no migraría nunca.
- **Canario**: una ejecución de un segundo con el tool cargado. Si libomp no puede
  cargar el `.so` (hwloc fuera de `LD_LIBRARY_PATH`), sigue sin tool y **sin avisar**,
  y obs y scheduler correrían horas como un base más. Si ningún hilo abre sus
  contadores, aborta por la misma razón.
- Elimina `KMP_AFFINITY` y `GOMP_CPU_AFFINITY` si llegan heredadas del shell: libomp
  las obedece y fijarían los hilos del caso base sin que nada lo delatara.
- Ignora `FORCE`: un `FORCE=1` olvidado en el shell rehacía las 1800 ejecuciones.
  Para repetir una ejecución, borra `resultados/<rNN>/.done/<tag>`; para repetir una
  réplica, su directorio.

Perillas (todas por entorno, también a través de `sbatch`): `N_REPLICAS`, `REPLICAS`
(p. ej. `"r11 r12"` para añadir réplicas más tarde), `SIZES`, `CONFIG_LIST`,
`THREAD_LIST`, `REPS`, `RESULTADOS`, `ORDEN=aleatorio|fijo`.

## 6. Traer los resultados y analizarlos

Los resultados sin la traza por ventana pesan ~5 MB por réplica y viajan por git como
en las campañas anteriores (el `.gitignore` de este directorio deja fuera la traza y
los binarios):

```bash
# en el clúster, al terminar
cd /scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP
git add replicas_procesos/resultados replicas_procesos/replicas.*.out
git commit -m "Resultados de la campaña de réplicas (trabajo <JOBID>)"
git push origin pruebas

# en local
git pull origin pruebas
python3 replicas_procesos/analisis_replicas.py     # solo necesita numpy
```

`analisis_replicas.py` escribe en `analisis/` e imprime un resumen (guardado también
en `analisis/resumen.txt`):

| Fichero | Contenido |
|---|---|
| `procesos.csv` | Una fila por proceso: media y DE de sus repeticiones, `ratio_rm`, migraciones, posición en el bloque, marca de tiempo y, para obs y scheduler, el overhead que registra el tool. Es la tabla limpia de la que sale todo lo demás |
| `resumen_celdas.csv` | Por grupo: media entre procesos con su IC95, DE y CV entre procesos, rango, **`factor_ee`**, ICC, `ratio_rm` (media, DE, mín., máx.) y cuántos procesos migraron |
| `contrastes.csv` | t de Welch por pareja: las tres de la Tabla 9 (scheduler/base, scheduler/control, control/base) y las de las estáticas. Diferencia en ms y %, IC95 de Welch y bootstrap, p, **p corregida por Holm**, d de Cohen, g de Hedges y **diferencia mínima detectable** |
| `descomposicion.csv` | `C_monitor`, `G_migrar`, `G_neta`, `G_interleave` y `G_spread` en ms con el signo del documento (G > 0 ahorra tiempo), con IC95 de Welch y bootstrap |
| `anova.csv` | ANOVA de un factor clásica (la del documento) y de **Welch**, para base/bind_spread/scheduler y para las cuatro competidoras |
| `contraste_29390.csv` | Dónde cae la ejecución única de 29390 dentro de las 10 réplicas: z, percentil, si está dentro del rango, y su `ratio_rm` y migraciones frente a las de las réplicas |

Qué significa cada cosa:

- **`factor_ee`** = DE entre procesos / (DE intra / √150). El denominador es el error
  estándar que el análisis intra-proceso atribuía a la media de un proceso; el
  numerador, cuánto varía de verdad esa media de un proceso a otro. 1 = el análisis
  del documento acertaba; 10 = subestimaba la incertidumbre 10 veces. Es el número
  que convierte la limitación declarada en un resultado.
- **IC95 de Welch y bootstrap.** El bootstrap remuestrea **procesos**, 10 000 veces y
  con semilla fija (reproducible). Con n = 10 el intervalo percentil tiende a quedarse
  algo corto; si difiere mucho del de Welch, las medias por proceso no son normales
  (por ejemplo, bimodales por la lotería de colocación) y conviene mirar
  `procesos.csv`.
- **Holm** corrige por comparaciones múltiples dentro de cada familia (cada pareja,
  cada término, cada conjunto del ANOVA) a lo largo de todas las celdas.
  `significativo` usa la p corregida.
- **Diferencia mínima detectable** (`dmd_ms`): la diferencia que esta prueba, con la
  dispersión y el n observados, detectaría con potencia 0,80 (verificado contra la t
  no central: exacta a ±0,01 con n ≥ 5). Es lo que da sentido a un "no
  significativo": no es "no hay efecto", sino "no hay efecto mayor que esto".
- **d y g.** La d usa la definición del documento, √((s₁² + s₂²)/2), pero aquí s es
  la dispersión **entre medias de proceso**, no entre repeticiones, así que no es
  comparable con la d de la Tabla 9. g es la d corregida por muestra pequeña (×0,958
  con n = 10). La interpretación debe apoyarse en la diferencia en ms y su intervalo.
- **ANOVA de Welch**: no supone varianzas iguales, y aquí no lo son (la dispersión del
  scheduler depende de si migra; la del base, de dónde cae el maestro).
- La identidad **G_neta = G_migrar − C_monitor** se verifica por celda y se informa
  en el resumen; si no cuadra, falta algún proceso en alguna configuración.

Controles que hace el análisis sin que haya que pedírselos: solo cuenta procesos con
marcador `.done`; si una ejecución se reintentó, usa el último intento; descarta
procesos con un número de repeticiones distinto del de la campaña; avisa si la media
de `_times.csv` no coincide con el `avg_ms` del kernel; y compara los sha256 de
`procedencia.txt` entre réplicas y avisa de **MEZCLA DE VERSIONES** si alguna se hizo
con otros binarios.

## 7. Salidas por réplica (`resultados/rNN/`)

Las mismas que `perf_out_v5/` (`kernel_metrics.csv`, `metrics/`, `ompt_*.csv`,
`ompt_logs/`, `.done/`), más:

- `metrics/<tag>_ompt_summary.csv` y `metrics/<tag>_ompt_overhead.csv`: el resumen y el
  overhead del tool **de esa ejecución**. Los globales se siguen escribiendo, pero
  acumulan por tag: en `perf_out_v5/ompt_summary.csv` las 144 etiquetas con tool
  tienen filas de dos campañas (29355 y 29390).
- `cronologia.csv`: tag, bloque, posición en el bloque, trabajo SLURM, inicio, fin y
  código de salida de cada ejecución.
- `procedencia.txt`: un bloque por trabajo que toca la réplica, con host, compilador,
  módulos, commit, estado de las fuentes en git, parámetros, `nmi_watchdog`,
  `perf_event_paranoid`, `numa_balancing`, THP, gobernador, entorno OpenMP y el sha256
  de fuentes y binarios.

## 8. Lo que sigue siendo limitación

- **Sin root** no se puede fijar la frecuencia ni desactivar el turbo. La diferencia es
  que ahora esa variación entra en la dispersión entre procesos y queda medida.
- **Un solo día.** Las diez réplicas en un trabajo capturan la variación a lo largo de
  unas horas, no entre días. Para reforzarlo se pueden repartir en dos envíos en días
  distintos: `REPLICAS="r01 r02 r03 r04 r05"` y más tarde `REPLICAS="r06 r07 r08 r09 r10"`.
- **Independencia.** Welch trata los grupos como independientes. Si existiera un
  efecto de réplica (algo que afecte igual a todas las configuraciones de una misma
  réplica), ignorarlo hace la prueba **conservadora**, no optimista.
- **n = 10** detecta diferencias del orden de `dmd_ms`; por debajo, "no significativo"
  no permite afirmar igualdad.
- La media del scheduler sigue mezclando el régimen transitorio y el estacionario
  (§4.4.5 del documento).
- En S4 la mejor estática no es `interleave` sino `bind_close` (SpMV) o `bind_spread`
  (Stencil). `bind_spread` está incluida; `bind_close` se añade con `CONFIG_LIST` si
  hiciera falta (~6 min más por réplica).

## 9. Referencias útiles para la metodología

- A. Georges, D. Buytaert y L. Eeckhout, "Statistically rigorous Java performance
  evaluation", *OOPSLA*, 2007. Varias invocaciones del proceso e intervalos de
  confianza sobre sus medias: el esquema de este directorio.
- T. Kalibera y R. Jones, "Rigorous benchmarking in reasonable time", *ISMM*, 2013.
  Variabilidad por niveles (repeticiones dentro de ejecuciones); es lo que mide
  `factor_ee`.
- T. Mytkowicz, A. Diwan, M. Hauswirth y P. F. Sweeney, "Producing wrong data without
  doing anything obviously wrong!", *ASPLOS*, 2009. Sesgo de montaje; motiva barajar
  el orden.
- T. Hoefler y R. Belli, "Scientific benchmarking of parallel computing systems",
  *SC*, 2015. Recomendaciones para reportar rendimiento con su variabilidad.
- B. L. Welch, "On the comparison of several mean values: an alternative approach",
  *Biometrika*, 38, 1951. S. Holm, "A simple sequentially rejective multiple test
  procedure", *Scandinavian Journal of Statistics*, 6, 1979.
