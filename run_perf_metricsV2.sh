#!/usr/bin/env bash
set -euo pipefail

########################
# USO
########################
# OMP_THREADS=6 SPMV_AVG_NNZ=15 ./run_perf_metrics.sh 200
#                                   ^ N (tamaño matriz)  -> se usa para AMBOS: stencil y spmv_2
#
# Variables de entorno:
#   OMP_THREADS     -> hilos (default: 1)
#   SPMV_AVG_NNZ    -> avg nnz en spmv_2 (default: 20)
# Opcional:
#   OUTDIR          -> carpeta salida (default: perf_out)

########################
# INPUTS
########################
if [[ $# -ne 1 ]]; then
  echo "Uso: OMP_THREADS=<hilos> SPMV_AVG_NNZ=<avg_nnz> $0 <N>" >&2
  echo "Ejemplo: OMP_THREADS=6 SPMV_AVG_NNZ=15 $0 200" >&2
  exit 1
fi

N="$1"
if ! [[ "$N" =~ ^[0-9]+$ ]] || [[ "$N" -le 0 ]]; then
  echo "ERROR: N debe ser un entero positivo. Recibido: $N" >&2
  exit 1
fi

OMP_THREADS="${OMP_THREADS:-1}"
SPMV_AVG_NNZ="${SPMV_AVG_NNZ:-20}"
OUTDIR="${OUTDIR:-perf_out}"

####################################
# --- Modules (HPC) ---
if [[ "${SKIP_MODULES:-0}" != "1" ]]; then
  # Intentar habilitar 'module' si no está disponible
  if ! command -v module >/dev/null 2>&1; then
    # Rutas típicas (puede variar en tu cluster)
    if [[ -f /etc/profile.d/modules.sh ]]; then
      # shellcheck disable=SC1091
      source /etc/profile.d/modules.sh
    elif [[ -f /usr/share/Modules/init/bash ]]; then
      # shellcheck disable=SC1091
      source /usr/share/Modules/init/bash
    fi
  fi

  if command -v module >/dev/null 2>&1; then
    module purge
    module swap gnu14 gnu15/15.2.0
    module load likwid/5.4.1
  else
    echo "WARNING: 'module' no está disponible; omitiendo module purge/swap/load" >&2
  fi
fi

########################
# COMPILACIÓN
########################
CXX=clang++
CXXFLAGS=(-std=c++17 -fopenmp -ffast-math -O3)

SPMV_SRC="./spmv_2.cpp"
SPMV_BIN="./spmv_2"

STENCIL_SRC="./Stencil.cpp"
STENCIL_BIN="./stencil"

########################
# SALIDAS
########################
REPORTDIR="${OUTDIR}/reports"
CSV="${OUTDIR}/metrics.csv"

########################
# PERF
########################
# NOTA: se removió perf record porque era redundante; el CSV se arma solo con perf stat.

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: Falta comando requerido: $1" >&2
    exit 1
  }
}

mkdirs() {
  mkdir -p "${OUTDIR}" "${REPORTDIR}"
}

compile_all() {
  echo "[*] Compilando SpMV: ${SPMV_SRC} -> ${SPMV_BIN}"
  "${CXX}" "${CXXFLAGS[@]}" "${SPMV_SRC}" -o "${SPMV_BIN}"

  echo "[*] Compilando Stencil: ${STENCIL_SRC} -> ${STENCIL_BIN}"
  "${CXX}" "${CXXFLAGS[@]}" "${STENCIL_SRC}" -o "${STENCIL_BIN}"
}


perf_stat_one_event() {
  local event="$1"
  shift
  local cmd=( "$@" )
  # perf stat imprime a stderr; -x, para parsear
  perf stat --no-big-num -x, -e "${event}" -- "${cmd[@]}" 2>&1
}

extract_count_for_event() {
  local perf_output="$1"
  local event="$2"
  awk -F',' -v ev="$event" '
    $3==ev {
      c=$1
      gsub(/^[ \t]+|[ \t]+$/, "", c)
      if (c ~ /<not supported>|<not counted>|<not running>/) { next }
      print c
      exit
    }
  ' <<< "$perf_output"
}

run_program_suite() {
  local program="$1"  # stencil | spmv_2
  local tag="${program}_t${OMP_THREADS}_n${N}_nnz${SPMV_AVG_NNZ}"

  # Control de threads
  export OMP_NUM_THREADS="${OMP_THREADS}"
  export OMP_DYNAMIC="FALSE"

  # Construir comando real
  local cmd=()
  if [[ "$program" == "stencil" ]]; then
    # Usa el MISMO N
    cmd=( "${STENCIL_BIN}" "${N}" )
  elif [[ "$program" == "spmv_2" ]]; then
    # spmv_2: ./spmv_2 [hilos] [tamaño matriz] [avg_nnz]
    # Usa el MISMO N
    cmd=( "${SPMV_BIN}" "${OMP_THREADS}" "${N}" "${SPMV_AVG_NNZ}" )
  else
    echo "ERROR: programa desconocido: $program" >&2
    exit 1
  fi

  echo "[*] Ejecutando ${program}"
  echo "    OMP_NUM_THREADS=${OMP_NUM_THREADS}"
  echo "    CMD: ${cmd[*]}"

  # perf stat para CSV (uno por uno, sin multiplex)
  local out_ref out_miss out_mig out_ins out_cyc
  out_ref="$(perf_stat_one_event "cache-references" "${cmd[@]}")"
  out_miss="$(perf_stat_one_event "cache-misses" "${cmd[@]}")"
  out_mig="$(perf_stat_one_event "cpu-migrations" "${cmd[@]}")"
  out_ins="$(perf_stat_one_event "instructions" "${cmd[@]}")"
  out_cyc="$(perf_stat_one_event "cycles" "${cmd[@]}")"

  local cache_references cache_misses cpu_migrations instructions cycles
  cache_references="$(extract_count_for_event "$out_ref" "cache-references" || true)"
  cache_misses="$(extract_count_for_event "$out_miss" "cache-misses" || true)"
  cpu_migrations="$(extract_count_for_event "$out_mig" "cpu-migrations" || true)"
  instructions="$(extract_count_for_event "$out_ins" "instructions" || true)"
  cycles="$(extract_count_for_event "$out_cyc" "cycles" || true)"

  # vacíos -> 0
  cache_references="${cache_references:-0}"
  cache_misses="${cache_misses:-0}"
  cpu_migrations="${cpu_migrations:-0}"
  instructions="${instructions:-0}"
  cycles="${cycles:-0}"

  # miss rate e IPC
  local miss_rate ipc
  miss_rate="$(awk -v m="$cache_misses" -v r="$cache_references" 'BEGIN{ if (r==0) print "0"; else printf "%.8f", (m/r) }')"
  ipc="$(awk -v i="$instructions" -v c="$cycles" 'BEGIN{ if (c==0) print "0"; else printf "%.8f", (i/c) }')"

  local now
  now="$(date -Iseconds)"

  # CSV: timestamp,program,threads,N,spmv_avg_nnz,cache_refs,cache_misses,miss_rate,cpu_migrations,instructions,cycles,ipc
  echo "${now},${program},${OMP_THREADS},${N},${SPMV_AVG_NNZ},${cache_references},${cache_misses},${miss_rate},${cpu_migrations},${instructions},${cycles},${ipc}" >> "${CSV}"
}

########################
# MAIN
########################
need_cmd perf
need_cmd "${CXX}"

mkdirs
compile_all

if [[ ! -f "${CSV}" ]]; then
  echo "timestamp,program,threads,N,spmv_avg_nnz,cache_references,cache_misses,miss_rate,cpu-migrations,instructions,cycles,ipc" > "${CSV}"
fi

run_program_suite "stencil"
run_program_suite "spmv_2"

echo "[✓] Listo."
echo "    CSV: ${CSV}"
