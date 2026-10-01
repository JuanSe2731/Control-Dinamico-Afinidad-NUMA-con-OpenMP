#!/usr/bin/env bash
#
# Campaña de RÉPLICAS DE PROCESO: la variabilidad ENTRE procesos que la campaña V5
# no pudo medir.
#
# POR QUÉ
# -------
# En V5 cada punto (kernel, tamaño, hilos, configuración) es UNA ejecución, y sus
# 150 repeticiones son la muestra de Welch y del ANOVA. Esa muestra captura el ruido
# de iteración a iteración, pero no lo que cambia de un proceso a otro: en qué nodo
# cae el hilo maestro al inicializar (y con él, por first-touch, todas las páginas),
# qué CPU elige el sistema para cada hilo sin afinidad, el estado de la máquina.
# Con un proceso por punto esa variación queda confundida con el efecto de la
# configuración, y el error estándar intra-proceso la subestima.
#
# Aquí cada punto se ejecuta N veces en procesos independientes, y la UNIDAD DE
# ANÁLISIS pasa a ser el proceso: su tiempo medio sobre las 150 repeticiones.
#
# DISEÑO POR DEFECTO
# ------------------
#   tamaños   S3 S4 S5          el régimen de análisis del documento (S3-S5)
#   configs   base obs scheduler interleave bind_spread
#   hilos     8 16 32 64 128 256
#   kernels   stencil, spmv_static
#   réplicas  r01..r10          10 procesos por celda y configuración
#   = 180 ejecuciones por réplica, 1800 en total; ~25 min por réplica en exadell.
#
# bind_spread y S3 están para poder repetir por procesos EXACTAMENTE las pruebas
# del documento: su ANOVA es base / afinidad dispersa / scheduler sobre las celdas
# de S3 a S5. interleave es la mejor estática en S5.
#
# Las réplicas corren EN SECUENCIA (r01 entera, luego r02...), así que los diez
# procesos de cada celda quedan repartidos a lo largo de todo el trabajo; dentro de
# cada bloque (tamaño, hilos) el orden se baraja con una semilla distinta por
# réplica (ORDEN=aleatorio, ver run_perf_metricsV5.sh).
#
# REANUDABLE: cada réplica es un OUTDIR propio (resultados/rNN) con sus marcadores
# .done, y una réplica completa se salta entera. Si SLURM corta el trabajo, se
# reenvía el mismo sbatch hasta que el resumen final diga "PENDIENTES: 0".
#
# Uso (desde cualquier sitio; el script se sitúa en su propio directorio):
#   ./run_replicas.sh
#   N_REPLICAS=5 ./run_replicas.sh
#   REPLICAS="r11 r12" ./run_replicas.sh              # añadir réplicas más tarde
#   SIZES="S4 S5" CONFIG_LIST="base obs scheduler interleave" ./run_replicas.sh
#   # prueba de humo (~7 min; no toca resultados/). REPS completas: con menos, el
#   # scheduler no llega a migrar (en 29390 SpMV S5 t8 migro a los 13-18 s):
#   N_REPLICAS=1 SIZES="S5" THREAD_LIST="8" RESULTADOS=prueba_humo ./run_replicas.sh

set -euo pipefail
export LC_ALL=C

# Siempre desde el directorio del script: aquí están las fuentes, los binarios y la
# copia de la suite. Lanzado desde la raíz del repo, sin esto se ejecutaría la
# suite de la raíz y los resultados irían a perf_out_v5.
DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
cd "$DIR"

# ── Diseño ────────────────────────────────────────────────────────────────────
N_REPLICAS="${N_REPLICAS:-10}"
if [[ -n "${REPLICAS:-}" ]]; then
  read -r -a LISTA_REPLICAS <<< "$REPLICAS"
else
  LISTA_REPLICAS=()
  for (( i = 1; i <= N_REPLICAS; i++ )); do
    LISTA_REPLICAS+=("$(printf 'r%02d' "$i")")
  done
fi
RESULTADOS="${RESULTADOS:-resultados}"

export SIZES="${SIZES:-S3 S4 S5}"
export CONFIG_LIST="${CONFIG_LIST:-base obs scheduler interleave bind_spread}"
export THREAD_LIST="${THREAD_LIST:-8 16 32 64 128 256}"
export REPS="${REPS:-150}"
export WARMUP_ITERS="${WARMUP_ITERS:-5}"
export SPMV_SEED="${SPMV_SEED:-42}"
export SPMV_AVG_NNZ="${SPMV_AVG_NNZ:-32}"
export PERF_FAMILIA="${PERF_FAMILIA:-any}"
export ORDEN="${ORDEN:-aleatorio}"

# FORCE no se deja pasar. SLURM propaga el entorno del shell que hace sbatch, y un
# FORCE=1 olvidado de otra sesión rehacía las 1800 ejecuciones sin avisar. Para
# repetir una ejecución basta borrar su marcador .done; para repetir una réplica
# entera, su directorio.
if [[ -n "${FORCE:-}" ]]; then
  echo "AVISO: se ignora FORCE=${FORCE}. Para repetir una ejecucion borra"
  echo "       ${RESULTADOS}/<rNN>/.done/<tag>; para repetir una replica, ${RESULTADOS}/<rNN>."
  unset FORCE
fi

# El caso base es OpenMP SIN afinidad. libomp también obedece KMP_AFFINITY y
# GOMP_CPU_AFFINITY, y la suite solo gestiona OMP_PLACES/OMP_PROC_BIND: si alguna
# llegara heredada del shell (un módulo, un .bashrc), base, obs y scheduler
# correrían con hilos fijados sin que nada lo delatara.
for v in KMP_AFFINITY GOMP_CPU_AFFINITY; do
  if [[ -n "${!v:-}" ]]; then
    echo "AVISO: se elimina ${v}='${!v}' heredada del entorno (fijaria los hilos del caso base)"
    unset "$v"
  fi
done

read -r -a SIZE_ARR   <<< "$SIZES"
read -r -a THREAD_ARR <<< "$THREAD_LIST"
read -r -a CONFIG_ARR <<< "$CONFIG_LIST"
# Deben coincidir con las etiquetas de PAR_KERNELS en run_perf_metricsV5.sh.
KERNELS=(stencil spmv_static)
SUFIJO=""
[[ "$PERF_FAMILIA" == "dmnd" ]] && SUFIJO="_dmnd"

for r in "${LISTA_REPLICAS[@]}"; do
  [[ "$r" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "ERROR: nombre de replica no valido: '$r'"; exit 1; }
done

etiquetas_esperadas() {
  local s t c k
  for s in "${SIZE_ARR[@]}"; do
    for t in "${THREAD_ARR[@]}"; do
      for c in "${CONFIG_ARR[@]}"; do
        for k in "${KERNELS[@]}"; do
          echo "${k}_${s}_t${t}_${c}${SUFIJO}"
        done
      done
    done
  done
}
mapfile -t ESPERADAS < <(etiquetas_esperadas)
POR_REPLICA=${#ESPERADAS[@]}

hechas_en() {
  local dir="$1" n=0 tag
  for tag in "${ESPERADAS[@]}"; do
    [[ -f "${dir}/.done/${tag}" ]] && n=$(( n + 1 ))
  done
  echo "$n"
}

# ── Procedencia ───────────────────────────────────────────────────────────────
# Un bloque por réplica y por trabajo que la toca (una réplica cortada por SLURM
# acumula dos). El análisis compara los sha256 entre réplicas y avisa si alguna se
# hizo con otros binarios: la campaña 29390 volvió con filas de tres versiones del
# código mezcladas en un mismo CSV y nada lo delataba.
leer() {
  if [[ -r "$1" ]]; then tr '\n' ' ' < "$1" | sed 's/ *$//'; else echo NA; fi
}

escribir_procedencia() {
  local dir="$1" f
  {
    echo "# ---- $(date '+%Y-%m-%dT%H:%M:%S%z')  trabajo ${SLURM_JOB_ID:-local} ----"
    echo "fecha=$(date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "host=$(hostname)"
    echo "slurm_job_id=${SLURM_JOB_ID:-local}"
    echo "kernel_linux=$(uname -r)"
    echo "cpu=$(awk -F': *' '/^model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null)"
    echo "nodos_numa=$(numactl --hardware 2>/dev/null | awk '/^available:/{print $2}')"
    echo "compilador=$(${CXX:-clang++} --version 2>/dev/null | head -1)"
    echo "modulos=${LOADEDMODULES:-NA}"
    echo "hwloc_lib=${HWLOC_LIB:-/opt/ohpc/pub/libs/hwloc/lib (defecto)}"
    echo "git_commit=$(git -C "$DIR" rev-parse --short HEAD 2>/dev/null || echo NA)"
    # "limpio" = las fuentes son las del commit de arriba; si no, cada fichero con su
    # estado de git (M modificado, ?? sin versionar).
    estado_git=$(git -C "$DIR" status --porcelain -- sched_NUMA_optC_leaky.cpp perf_region.hpp \
                   Stencil.cpp spmv_staticSeed.cpp run_perf_metricsV5.sh run_replicas.sh 2>/dev/null \
                 | awk '{printf "%s%s %s", sep, $1, $2; sep="; "}')
    echo "git_estado_fuentes=${estado_git:-limpio}"
    echo "reps=${REPS} warmup=${WARMUP_ITERS} spmv_seed=${SPMV_SEED} avg_nnz=${SPMV_AVG_NNZ} familia=${PERF_FAMILIA} orden=${ORDEN}"
    echo "sizes=${SIZES}"
    echo "threads=${THREAD_LIST}"
    echo "configs=${CONFIG_LIST}"
    echo "nmi_watchdog=$(leer /proc/sys/kernel/nmi_watchdog)"
    echo "perf_event_paranoid=$(leer /proc/sys/kernel/perf_event_paranoid)"
    echo "numa_balancing=$(leer /proc/sys/kernel/numa_balancing)"
    echo "thp_enabled=$(leer /sys/kernel/mm/transparent_hugepage/enabled)"
    echo "thp_defrag=$(leer /sys/kernel/mm/transparent_hugepage/defrag)"
    echo "governor=$(leer /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
    echo "boost=$(leer /sys/devices/system/cpu/cpufreq/boost)"
    echo "ulimit_n=$(ulimit -n)"
    echo "entorno_openmp=$(env | grep -E '^(OMP_|KMP_|GOMP_)' | sort | tr '\n' ' ')"
    for f in sched_NUMA_optC_leaky.cpp perf_region.hpp Stencil.cpp spmv_staticSeed.cpp \
             run_perf_metricsV5.sh numa_sched_optC.so stencil spmv_static; do
      echo "sha256:${f}=$(sha256sum "$f" | cut -d' ' -f1)"
    done
  } >> "${dir}/procedencia.txt"
}

# ── Canario del tool ──────────────────────────────────────────────────────────
# Si libhwloc no está en LD_LIBRARY_PATH en el nodo de cómputo (el módulo se cargó
# en otro shell, por ejemplo), libomp no puede cargar el .so y sigue SIN tool y sin
# decir nada: obs y scheduler se ejecutarían como un base más durante horas. Y si
# perf_event_open está denegado, el tool carga pero no cuenta, y el scheduler no
# migra nunca (el fallo silencioso descrito en CLAUDE.md). Una ejecución de un
# segundo lo detecta antes de gastar el trabajo.
canario_tool() {
  local tmp resumen filas llenas
  tmp="$(mktemp -d)"
  resumen="${tmp}/resumen.csv"
  if ldd ./numa_sched_optC.so 2>/dev/null | grep -q "not found"; then
    echo "ERROR: numa_sched_optC.so tiene dependencias sin resolver:"
    ldd ./numa_sched_optC.so | grep "not found"
    echo "       Carga el modulo de hwloc (module load hwloc) ANTES de hacer sbatch."
    rm -rf "$tmp"; exit 1
  fi
  OMP_TOOL=enabled OMP_TOOL_LIBRARIES="$(realpath numa_sched_optC.so)" \
  OMPT_TAG=canario OMPT_SUMMARY_CSV="$resumen" OMPT_DISABLE_MIGRATION=1 \
  OMPT_LOG_FILE="${tmp}/tool.log" OMP_NUM_THREADS=2 \
    ./stencil 200 2 5 "${tmp}/k" >/dev/null 2>"${tmp}/stderr" || true
  filas=0
  [[ -s "$resumen" ]] && filas=$(( $(wc -l < "$resumen") - 1 ))
  if (( filas < 1 )); then
    echo "ERROR: el tool OMPT no se cargo (el canario no escribio su resumen)."
    echo "       Revisa que el binario sea de clang (libomp) y que hwloc este en LD_LIBRARY_PATH."
    sed 's/^/       | /' "${tmp}/stderr" "${tmp}/tool.log" 2>/dev/null | head -20
    rm -rf "$tmp"; exit 1
  fi
  llenas=$(awk -F, 'NR==1{for(i=1;i<=NF;i++) if($i=="perf_status") k=i; next}
                    k && $k=="full"{n++} END{print n+0}' "$resumen")
  if (( llenas == 0 )) && [[ "${PERMITIR_SIN_CONTADORES:-0}" != "1" ]]; then
    echo "ERROR: el tool carga pero ningun hilo abrio los contadores (perf_status != full)."
    echo "       Con esto el scheduler no migraria nunca. Comprueba perf_event_paranoid <= 2"
    echo "       y 'perf list | grep -i fills'. (PERMITIR_SIN_CONTADORES=1 lo salta, solo"
    echo "       para probar la tuberia en una maquina que no es exadell.)"
    rm -rf "$tmp"; exit 1
  fi
  echo "    canario: tool cargado, ${llenas}/${filas} hilos con contadores"
  rm -rf "$tmp"
}

# ── Preflight ─────────────────────────────────────────────────────────────────
for f in sched_NUMA_optC_leaky.cpp perf_region.hpp Stencil.cpp spmv_staticSeed.cpp \
         run_perf_metricsV5.sh; do
  [[ -f "$f" ]] || { echo "ERROR: falta $f en $DIR"; exit 1; }
done

# Mismo motivo que en el .sbatch: cada hilo abre varios fd de perf_event_open y con
# el límite de 1024 los puntos de 128 y 256 hilos pierden su CSV en silencio.
ulimit -n 65536 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
if (( $(ulimit -n) < 4096 )); then
  echo "AVISO: limite de descriptores $(ulimit -n) < 4096; 128 y 256 hilos pueden fallar" >&2
fi

paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 99)
if (( paranoid > 2 )); then
  echo "ERROR: perf_event_paranoid=${paranoid} (> 2). El tool no podria contar y el"
  echo "       scheduler nunca migraria. Hace falta <= 2."
  exit 1
fi
nodos=$(numactl --hardware 2>/dev/null | awk '/^available:/{print $2}')
if [[ "${nodos:-0}" != "2" ]]; then
  echo "AVISO: esta maquina tiene ${nodos:-?} nodo(s) NUMA; el experimento esta pensado para"
  echo "       los 2 de exadell. Sirve para probar la tuberia, no para medir."
fi

echo "=== Campana de replicas ==="
echo "    replicas   : ${LISTA_REPLICAS[*]}  -> ${RESULTADOS}/"
echo "    tamanos    : ${SIZES}"
echo "    hilos      : ${THREAD_LIST}"
echo "    configs    : ${CONFIG_LIST}"
echo "    por replica: ${POR_REPLICA} ejecuciones   reps=${REPS} warmup=${WARMUP_ITERS} orden=${ORDEN}"
echo "    inicio     : $(date '+%F %T')"
echo

# ── Compilar UNA vez ──────────────────────────────────────────────────────────
COMPILAR=solo ./run_perf_metricsV5.sh
canario_tool

# ── Réplicas en secuencia ─────────────────────────────────────────────────────
declare -a CON_ERROR=()
for r in "${LISTA_REPLICAS[@]}"; do
  outdir="${RESULTADOS}/${r}"
  mkdir -p "$outdir"
  hechas=$(hechas_en "$outdir")
  if (( hechas == POR_REPLICA )); then
    echo "=== replica ${r}: completa (${hechas}/${POR_REPLICA}), se salta ==="
    continue
  fi
  echo
  echo "================ replica ${r}  ($(date '+%F %T'))  ${hechas}/${POR_REPLICA} hechas ================"
  escribir_procedencia "$outdir"
  if OUTDIR="$outdir" COMPILAR=no ORDEN_SEMILLA="$r" ./run_perf_metricsV5.sh; then
    :
  else
    rc=$?
    echo "AVISO: la suite termino con rc=${rc} en la replica ${r}; se sigue con la siguiente"
    CON_ERROR+=("$r")
  fi
done

# ── Estado final ──────────────────────────────────────────────────────────────
echo
echo "=== Estado de la campana en ${RESULTADOS}/  ($(date '+%F %T')) ==="
pendientes=0
for r in "${LISTA_REPLICAS[@]}"; do
  hechas=$(hechas_en "${RESULTADOS}/${r}")
  faltan=$(( POR_REPLICA - hechas ))
  pendientes=$(( pendientes + faltan ))
  if (( faltan == 0 )); then
    printf '    %-6s %4d/%d  completa\n' "$r" "$hechas" "$POR_REPLICA"
  else
    printf '    %-6s %4d/%d  faltan %d\n' "$r" "$hechas" "$POR_REPLICA" "$faltan"
  fi
done
echo "    PENDIENTES: ${pendientes}"
if (( ${#CON_ERROR[@]} > 0 )); then
  echo "    replicas donde la suite aborto: ${CON_ERROR[*]}"
fi
if (( pendientes > 0 )); then
  echo "    -> reenvia el mismo sbatch; retoma donde se quedo. Si una ejecucion falla"
  echo "       siempre, mira ${RESULTADOS}/<rNN>/ompt_logs/<tag>.stderr"
  exit 3
fi
echo "    -> campana completa. Analisis: python3 analisis_replicas.py"
