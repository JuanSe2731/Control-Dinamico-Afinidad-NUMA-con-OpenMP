#!/usr/bin/env bash
set -euo pipefail

########################
# run_perf_metricsV4.sh
#
# Ejecuta TODO en una sola corrida:
#  1) Seriales + baseline/first-touch con perf stat (y OMP_PLACES/PROC_BIND)
#  2) Baseline/first-touch con OMPT (sin perf stat, sin OMP_PLACES/PROC_BIND)
#
# Nota: los kernels ahora deshabilitan perf events durante warm-up, por lo que
#       perf stat y likwid no cuentan warm-ups.
#
# Uso recomendado (con LIKWID):
#   srun --partition=amd --nodes=0,1 --ntasks=1 --cpus-per-task=256 --mem=100G \
#     --export=ALL,OMP_THREADS=64,SPMV_AVG_NNZ=20 likwid-perfctr -g NUMA -f -- \
#     ./run_perf_metricsV4.sh 5000
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
OUTDIR="${OUTDIR:-perf_out_v4}"
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
CSV="${OUTDIR}/metrics_v4.csv"
LIKWID_CSV="${OUTDIR}/likwid_metrics_v4.csv"

########################
# Kernel Sources & Binaries
########################
declare -a ALL_KERNELS=(
  "stencil_serial:Stencil_serial.cpp:stencil_serial:STENCIL"
  "spmv_serial:spmv_serial.cpp:spmv_serial:SPMV"
  "stencil:Stencil.cpp:stencil:STENCIL"
  "spmv_dynamic:spmv_dynamic.cpp:spmv_dynamic:SPMV"
  "stencil_first_touch:Stencil_first_touch.cpp:stencil_first_touch:STENCIL"
  "spmv_dynamic_first_touch:spmv_dynamic_first_touch.cpp:spmv_dynamic_first_touch:SPMV"
)

# Seriales
declare -a SERIAL_KERNELS=(
  "stencil_serial:Stencil_serial.cpp:stencil_serial:STENCIL"
  "spmv_serial:spmv_serial.cpp:spmv_serial:SPMV"
)

# Baseline + first-touch con perf stat + OMP_PLACES/PROC_BIND
declare -a PERF_KERNELS=(
  "stencil:Stencil.cpp:stencil:STENCIL"
  "spmv_dynamic:spmv_dynamic.cpp:spmv_dynamic:SPMV"
  "stencil_first_touch:Stencil_first_touch.cpp:stencil_first_touch:STENCIL"
  "spmv_dynamic_first_touch:spmv_dynamic_first_touch.cpp:spmv_dynamic_first_touch:SPMV"
)

# OMPT (sin perf stat, sin OMP_PLACES/PROC_BIND)
declare -a OMPT_KERNELS=(
  "stencil:Stencil.cpp:stencil:STENCIL"
  "stencil_first_touch:Stencil_first_touch.cpp:stencil_first_touch:STENCIL"
  "spmv_dynamic:spmv_dynamic.cpp:spmv_dynamic:SPMV"
  "spmv_dynamic_first_touch:spmv_dynamic_first_touch.cpp:spmv_dynamic_first_touch:SPMV"
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
  for kernel_spec in "${ALL_KERNELS[@]}"; do
    IFS=':' read -r label src bin _ktype <<< "$kernel_spec"
    compile_kernel "$label" "$src" "$bin" || {
      echo "[!] FATAL: Failed to compile $label" >&2
      exit 1
    }
  done
}

# Run perf stat for a single kernel (one event at a time, no multiplex)
run_kernel_with_perf() {
  local kernel_label="$1"
  local kernel_bin="$2"
  local kernel_type="$3"
  local threads="$4"
  local omp_places="${5:-}"
  local omp_proc_bind="${6:-}"

  local tag="${kernel_label}_n${N}_t${threads}"
  if [[ -n "$omp_places" && -n "$omp_proc_bind" ]]; then
    tag="${tag}_p${omp_places}_b${omp_proc_bind}"
  else
    tag="${tag}_pna_bna"
  fi
  local csv_line="${tag}"

  export OMP_NUM_THREADS="${threads}"
  export OMP_DYNAMIC="FALSE"
  if [[ -n "$omp_places" && -n "$omp_proc_bind" ]]; then
    export OMP_PLACES="${omp_places}"
    export OMP_PROC_BIND="${omp_proc_bind}"
  else
    unset OMP_PLACES
    unset OMP_PROC_BIND
  fi

  local cmd=()
  if [[ "$kernel_type" == "STENCIL" ]]; then
    cmd=( "./${kernel_bin}" "${N}" )
  elif [[ "$kernel_type" == "SPMV" ]] && [[ "${kernel_label}" == "spmv_serial" ]]; then
    cmd=( "./${kernel_bin}" "${N}" "${SPMV_AVG_NNZ}" "${SPMV_REPS}" )
  elif [[ "$kernel_type" == "SPMV" ]]; then
    cmd=( "./${kernel_bin}" "" "${threads}" "${SPMV_REPS}" "" )
  fi

  echo "  [RUN] ${kernel_label} places=${omp_places:-na} bind=${omp_proc_bind:-na}"

  local events=(
    "cycles"
    "instructions"
    "cache-references"
    "cache-misses"
    "cpu-migrations"
  )

  for event in "${events[@]}"; do
    local out_ref="${METRICSDIR}/${tag}_${event}.txt"
    perf stat --no-big-num -x, -e "${event}" -- "${cmd[@]}" 2>&1 | tee "$out_ref" >/dev/null || true

    local count
    count=$(awk -F',' '$3 ~ /^'"${event}"'$/ {
      c=$1
      gsub(/^[ \t]+|[ \t]+$/, "", c)
      if (c !~ /<not/ && c != "") print c
      exit
    }' "$out_ref" || echo "0")

    csv_line="${csv_line},${count}"
  done

  echo "${csv_line}" >> "${CSV}"
}

# Run kernel with OMPT tool (no perf stat)
run_kernel_with_ompt() {
  local kernel_label="$1"
  local kernel_bin="$2"
  local kernel_type="$3"

  local tag="${kernel_label}_n${N}_t${OMP_THREADS}_ompt"
  local ompt_logfile="${OMPT_LOGDIR}/${tag}.log"

  export OMP_NUM_THREADS="${OMP_THREADS}"
  export OMP_DYNAMIC="FALSE"
  unset OMP_PLACES
  unset OMP_PROC_BIND

  export OMP_TOOL="enabled"
  export OMP_TOOL_LIBRARIES="$(realpath "$OMPT_TOOL_BIN")"
  export OMPT_VERBOSE="0"
  export OMPT_MONITOR="0"
  export OMPT_CSV="${CSV}"
  export OMPT_TAG="${tag}"

  local cmd=()
  if [[ "$kernel_type" == "STENCIL" ]]; then
    cmd=( "./${kernel_bin}" "${N}" )
  elif [[ "$kernel_type" == "SPMV" ]]; then
    cmd=( "./${kernel_bin}" "" "${OMP_THREADS}" "${SPMV_REPS}" "" )
  fi

  echo "  [RUN] ${kernel_label} (OMPT, sin OMP_PLACES/PROC_BIND)"
  "${cmd[@]}" >/dev/null 2> "${ompt_logfile}" || true
}

# Run kernel with likwid-perfctr to collect NUMA metrics
run_kernel_with_likwid() {
  local kernel_label="$1"
  local kernel_bin="$2"
  local kernel_type="$3"
  local threads="$4"
  local tag_suffix="${5:-}"

  local tag="${kernel_label}_n${N}_t${threads}${tag_suffix}"

  export OMP_NUM_THREADS="${threads}"
  export OMP_DYNAMIC="FALSE"
  unset OMP_PLACES
  unset OMP_PROC_BIND

  local cmd=()
  if [[ "$kernel_type" == "STENCIL" ]]; then
    cmd=( "./${kernel_bin}" "${N}" )
  elif [[ "$kernel_type" == "SPMV" ]] && [[ "${kernel_label}" == "spmv_serial" ]]; then
    cmd=( "./${kernel_bin}" "${N}" "${SPMV_AVG_NNZ}" "${SPMV_REPS}" )
  elif [[ "$kernel_type" == "SPMV" ]]; then
    cmd=( "./${kernel_bin}" "" "${threads}" "${SPMV_REPS}" "" )
  fi

  if ! command -v likwid-perfctr &> /dev/null; then
    echo "  [SKIP] likwid-perfctr not found for ${kernel_label}"
    return
  fi

  echo "  [RUN LIKWID] ${kernel_label} (${threads} threads)"

  # Run with NUMA group and extract metrics
  local likwid_out
  likwid_out=$(likwid-perfctr -g NUMA -- "${cmd[@]}" 2>&1 || true)

  # Parse LIKWID output to extract metrics
  # Expected output format:
  # | Metric | Socket 0 | Socket 1 |  Sum  |
  # where metrics include: Local BW [MByte/s], Remote BW, Local Data Volume, Remote Data Volume, etc.

  local local_bw=0 remote_bw=0 local_vol=0 remote_vol=0

  # Extract values (simplified parsing)
  local_bw=$(echo "$likwid_out" | grep "Local BW" | awk '{print $(NF-1)}' | head -1 || echo "0")
  remote_bw=$(echo "$likwid_out" | grep "Remote BW" | awk '{print $(NF-1)}' | head -1 || echo "0")
  local_vol=$(echo "$likwid_out" | grep "Local Data Volume" | awk '{print $(NF-1)}' | head -1 || echo "0")
  remote_vol=$(echo "$likwid_out" | grep "Remote Data Volume" | awk '{print $(NF-1)}' | head -1 || echo "0")

  # Calculate totals and percentages
  local total_bw=$(echo "${local_bw} + ${remote_bw}" | bc 2>/dev/null || echo "0")
  local total_vol=$(echo "${local_vol} + ${remote_vol}" | bc 2>/dev/null || echo "0")
  local remote_bw_pct=$(if (( $(echo "${total_bw} > 0" | bc -l) )); then echo "scale=2; ${remote_bw} * 100 / ${total_bw}" | bc; else echo "0"; fi)

  echo "${tag},${local_bw},${remote_bw},${local_vol},${remote_vol},${total_bw},${remote_bw_pct}" >> "${LIKWID_CSV}"
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

echo "tag,cycles,instructions,cache_references,cache_misses,cpu_migrations,tlb_misses,ipc,mpki" > "${CSV}"
echo "tag,local_bw_mbs,remote_bw_mbs,local_data_vol_mb,remote_data_vol_mb,total_bw_mbs,remote_bw_pct" > "${LIKWID_CSV}"

echo "[*] Stage 1/3: Seriales (perf stat)"
for kernel_spec in "${SERIAL_KERNELS[@]}"; do
  IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
  run_kernel_with_perf "$label" "$bin" "$ktype" "1"
done

echo ""
echo "[*] Stage 2/3: Baseline + first-touch con OMP_PLACES/PROC_BIND (perf stat)"
for kernel_spec in "${PERF_KERNELS[@]}"; do
  IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
  echo ""
  echo "[*] === Kernel: $label (type=$ktype) ==="
  run_kernel_with_perf "$label" "$bin" "$ktype" "${OMP_THREADS}" "cores" "close"
  run_kernel_with_perf "$label" "$bin" "$ktype" "${OMP_THREADS}" "cores" "spread"
  run_kernel_with_perf "$label" "$bin" "$ktype" "${OMP_THREADS}" "threads" "spread"
done

echo ""
echo "[*] Stage 3/3: Baseline + first-touch con OMPT (sin perf stat)"
for kernel_spec in "${OMPT_KERNELS[@]}"; do
  IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
  run_kernel_with_ompt "$label" "$bin" "$ktype"
done

echo ""
echo "[✓] Benchmark V4 completed."
echo "    CSV: ${CSV}"
echo "    LIKWID CSV: ${LIKWID_CSV}"
echo "    Metrics directory: ${METRICSDIR}/"
echo "    Reports directory: ${REPORTDIR}/"
echo "    OMPT logs directory: ${OMPT_LOGDIR}/"
echo ""
echo "[*] Optional: Run 'collect_likwid_metrics' function to gather LIKWID NUMA metrics"
echo "    (requires likwid-perfctr)"

# Optional function to collect LIKWID metrics separately
collect_likwid_metrics() {
  if ! command -v likwid-perfctr &> /dev/null; then
    echo "[!] likwid-perfctr not found. Install LIKWID to collect NUMA metrics." >&2
    return 1
  fi

  echo "[*] Collecting LIKWID NUMA metrics..."
  
  # Run one representative set from each kernel type
  for kernel_spec in "${PERF_KERNELS[@]}"; do
    IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
    run_kernel_with_likwid "$label" "$bin" "$ktype" "${OMP_THREADS}" "_likwid"
  done

  for kernel_spec in "${OMPT_KERNELS[@]}"; do
    IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
    run_kernel_with_likwid "$label" "$bin" "$ktype" "${OMP_THREADS}" "_likwid_ompt"
  done

  echo "[✓] LIKWID metrics collected: ${LIKWID_CSV}"
}
