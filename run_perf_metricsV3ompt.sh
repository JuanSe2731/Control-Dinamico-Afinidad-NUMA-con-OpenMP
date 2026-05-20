#!/usr/bin/env bash
set -euo pipefail

########################
# run_perf_metricsV3ompt.sh
#
# Performance measurement with OMPT tool integration.
# Identical to run_perf_metricsV3.sh but loads ompt_tool3.so for hardware counter monitoring.
#
# OMPT Tool will:
#  - Register callbacks for thread begin/end, parallel regions, implicit tasks
#  - Open per-thread perf_event counters (cycles, instructions, cache-misses)
#  - Run a background monitoring thread every 100ms reporting IPC/MPKI deltas
#  - Write final snapshot at program exit
#
# Usage:
#   srun --partition=amd --nodes=0,1 --ntasks=1 --cpus-per-task=256 --mem=100G \
#     --export=ALL,OMP_THREADS=64,SPMV_AVG_NNZ=20 likwid-perfctr -g NUMA -f -- \
#     ./run_perf_metricsV3ompt.sh 5000
########################

########################
# INPUTS & DEFAULTS
########################
if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <N>" >&2
  echo "Recomendado: N=5000 para Stencil2D, N=10000-50000 para SpMV" >&2
  exit 1
fi

N="$1"
if ! [[ "$N" =~ ^[0-9]+$ ]] || [[ "$N" -le 0 ]]; then
  echo "ERROR: N debe ser un entero positivo. Recibido: $N" >&2
  exit 1
fi

# Environment defaults (allow override via srun --export)
# Para 2 nodos NUMA con 64 cores c/u (128 cores total):
#   - Un hilo por core (sin SMT): OMP_THREADS=128
#   - Con SMT (2 hilos/core):     OMP_THREADS=256
OMP_THREADS="${OMP_THREADS:-128}"
SPMV_AVG_NNZ="${SPMV_AVG_NNZ:-20}"
SPMV_REPS="${SPMV_REPS:-30}"
OUTDIR="${OUTDIR:-perf_out_v3ompt}"
SKIP_MODULES="${SKIP_MODULES:-0}"

########################
# COMPILATION CONFIG
########################
CXX=clang++
CXXFLAGS=(-std=c++17 -O3 -fopenmp -ffast-math)

########################
# OMPT TOOL SETUP
########################
OMPT_TOOL_SRC="ompt_tool3.cpp"
OMPT_TOOL_BIN="libnuma_sched_ompt3.so"
OMPT_TOOL_FLAGS=(-std=c++17 -fPIC -shared -fopenmp -pthread -O2)

########################
# OUTPUT DIRECTORIES
########################
METRICSDIR="${OUTDIR}/metrics"
REPORTDIR="${OUTDIR}/reports"
OMPT_LOGDIR="${OUTDIR}/ompt_logs"
CSV="${OUTDIR}/metrics_v3ompt.csv"

########################
# Kernel Sources & Binaries
########################
declare -a KERNELS=(
  # Serials first
  "stencil_serial:Stencil_serial.cpp:stencil_serial:STENCIL"
  "spmv_serial:spmv_serial.cpp:spmv_serial:SPMV"
  # Baselines
  "stencil:Stencil.cpp:stencil:STENCIL"
  "spmv_dynamic:spmv_dynamic.cpp:spmv_dynamic:SPMV"
  # First-touch variants
  "spmv_dynamic_first_touch:spmv_dynamic_first_touch.cpp:spmv_dynamic_first_touch:SPMV"
  "stencil_first_touch:Stencil_first_touch.cpp:stencil_first_touch:STENCIL"
)

########################
# FUNCTIONS
########################
need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: Falta comando requerido: $1" >&2
    exit 1
  }
}

mkdirs() {
  mkdir -p "${OUTDIR}" "${METRICSDIR}" "${REPORTDIR}" "${OMPT_LOGDIR}"
}

# Load HPC modules if available
load_modules() {
  if [[ "$SKIP_MODULES" == "1" ]]; then
    echo "[*] Skipping module loading (SKIP_MODULES=1)"
    return
  fi
  
  if ! command -v module >/dev/null 2>&1; then
    if [[ -f /etc/profile.d/modules.sh ]]; then
      # shellcheck disable=SC1091
      source /etc/profile.d/modules.sh
    elif [[ -f /usr/share/Modules/init/bash ]]; then
      # shellcheck disable=SC1091
      source /usr/share/Modules/init/bash
    fi
  fi

  if command -v module >/dev/null 2>&1; then
    echo "[*] Loading modules..."
    module purge 2>/dev/null || true
    if module avail 2>&1 | grep -q "likwid"; then
      module load likwid >/dev/null 2>&1 || true
    fi
    if module avail 2>&1 | grep -q "gnu15"; then
      module load gnu15/15.2.0 >/dev/null 2>&1 || true
    fi
    echo "[*] Modules loaded"
  else
    echo "[*] WARNING: 'module' command not available; continuing without module setup"
  fi
}

# Compile OMPT tool
compile_ompt_tool() {
  if [[ ! -f "$OMPT_TOOL_SRC" ]]; then
    echo "[!] ERROR: OMPT tool source $OMPT_TOOL_SRC not found" >&2
    return 1
  fi
  
  echo "[*] Compiling OMPT tool: $OMPT_TOOL_SRC -> $OMPT_TOOL_BIN"
  "${CXX}" "${OMPT_TOOL_FLAGS[@]}" "$OMPT_TOOL_SRC" -o "$OMPT_TOOL_BIN"
  
  if [[ ! -f "$OMPT_TOOL_BIN" ]]; then
    echo "[!] ERROR: OMPT tool compilation failed" >&2
    return 1
  fi
  
  echo "[✓] OMPT tool compiled: $(realpath "$OMPT_TOOL_BIN")"
}

# Compile a single kernel
compile_kernel() {
  local label="$1"
  local src="$2"
  local bin="$3"
  
  if [[ ! -f "$src" ]]; then
    echo "[!] ERROR: source $src not found for $label" >&2
    return 1
  fi
  
  echo "[*] Compiling $label: $src -> $bin"
  "${CXX}" "${CXXFLAGS[@]}" "$src" -o "$bin"
  
  if [[ ! -x "$bin" ]]; then
    echo "[!] ERROR: compilation failed or binary not executable: $bin" >&2
    return 1
  fi
}

compile_all() {
  compile_ompt_tool
  for kernel_spec in "${KERNELS[@]}"; do
    IFS=':' read -r label src bin _ktype <<< "$kernel_spec"
    compile_kernel "$label" "$src" "$bin" || {
      echo "[!] FATAL: Failed to compile $label" >&2
      exit 1
    }
  done
}

# Run kernel with OMPT tool + perf stat (one event at a time)
run_kernel_with_ompt_and_perf() {
  local kernel_label="$1"
  local kernel_bin="$2"
  local kernel_type="$3"
  local omp_places="$4"
  local omp_proc_bind="$5"
  
  local tag="${kernel_label}_n${N}_t${OMP_THREADS}_p${omp_places}_b${omp_proc_bind}"
  local csv_line="${tag}"
  local ompt_logfile="${OMPT_LOGDIR}/${tag}_ompt.log"
  
  # Prepare environment with OMPT tool
  export OMP_NUM_THREADS="${OMP_THREADS}"
  export OMP_DYNAMIC="FALSE"
  export OMP_PLACES="${omp_places}"
  export OMP_PROC_BIND="${omp_proc_bind}"
  
  # OMPT tool environment
  export OMP_TOOL="enabled"
  export OMP_TOOL_LIBRARIES="$(realpath "$OMPT_TOOL_BIN")"
  
  # Build command based on kernel type
  local cmd=()
  if [[ "$kernel_type" == "STENCIL" ]]; then
    cmd=( "./${kernel_bin}" "${N}" )
  elif [[ "$kernel_type" == "SPMV" ]] && [[ "${kernel_label}" == "spmv_serial" ]]; then
    cmd=( "./${kernel_bin}" "${N}" "${SPMV_AVG_NNZ}" "${SPMV_REPS}" )
  elif [[ "$kernel_type" == "SPMV" ]]; then
    cmd=( "./${kernel_bin}" "" "${OMP_THREADS}" "${SPMV_REPS}" "" )
  fi
  
  echo "  [RUN] ${kernel_label} places=${omp_places} bind=${omp_proc_bind} (with OMPT)"
  
  # Events to measure one-by-one (no multiplex)
  local events=(
    "cycles"
    "instructions"
    "cache-references"
    "cache-misses"
    "cpu-migrations"
  )
  
  for event in "${events[@]}"; do
    local out_ref="${METRICSDIR}/${tag}_${event}.txt"
    
    # Run with perf stat, capture both stdout (for OMPT logs) and stderr (for perf)
    perf stat --no-big-num -x, -e "${event}" -- "${cmd[@]}" 2>&1 | tee "$out_ref" >> "$ompt_logfile" || true
    
    # Parse the count
    local count=$(awk -F',' '$3 ~ /^'"${event}"'$/ {
      c=$1
      gsub(/^[ \t]+|[ \t]+$/, "", c)
      if (c !~ /<not/ && c != "") print c
      exit
    }' "$out_ref" || echo "0")
    
    csv_line="${csv_line},${count}"
  done
  
  echo "${csv_line}" >> "${CSV}"
}

########################
# MAIN
########################
need_cmd perf
need_cmd "${CXX}"

mkdirs
load_modules

echo "[*] Compiling all kernels and OMPT tool..."
compile_all

# Initialize CSV header
echo "tag,cycles,instructions,cache_references,cache_misses,cpu_migrations" > "${CSV}"

# Run each kernel with different OMP_PLACES and OMP_PROC_BIND configurations
echo "[*] Running benchmarks with OMPT monitoring..."

for kernel_spec in "${KERNELS[@]}"; do
  IFS=':' read -r label src bin ktype <<< "$kernel_spec"
  
  if [[ ! -x "$bin" ]]; then
    echo "[!] WARNING: $bin not executable, skipping" >&2
    continue
  fi
  
  echo ""
  echo "[*] === Kernel: $label (type=$ktype) with OMPT ==="
  
  # Test with different OpenMP placement policies
  run_kernel_with_ompt_and_perf "$label" "$bin" "$ktype" "cores" "close"
  run_kernel_with_ompt_and_perf "$label" "$bin" "$ktype" "cores" "spread"
  run_kernel_with_ompt_and_perf "$label" "$bin" "$ktype" "threads" "spread"
done

echo ""
echo "[✓] Benchmark with OMPT completed."
echo "    CSV: ${CSV}"
echo "    Metrics directory: ${METRICSDIR}/"
echo "    Reports directory: ${REPORTDIR}/"
echo "    OMPT logs directory: ${OMPT_LOGDIR}/"
echo ""
echo "[*] CSV summary:"
head -5 "${CSV}"
echo "    ... ($(wc -l < "${CSV}") rows total)"
echo ""
echo "[*] OMPT logs sample (first kernel):"
if ls "${OMPT_LOGDIR}"/*.log >/dev/null 2>&1; then
  head -20 "${OMPT_LOGDIR}"/*.log | tail -15
fi
