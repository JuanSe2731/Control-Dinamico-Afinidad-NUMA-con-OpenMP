#!/usr/bin/env bash
set -euo pipefail

########################
# run_perf_metricsV4.sh
#
# Ejecuta benchmarks en 3 fases:
#  1) Seriales
#  2) OMP + numactl con 3 configuraciones
#  3) OMP + numactl + OMPT (mismas 3 configuraciones)
#
# Todas las fases usan hilos: 8,16,32,64,128
# Métricas por ventana (100ms): d_rm, d_instr, d_cyc, IPC, ratio_rm
# Métricas kernel: stddev_ms, GiB/s, GFlops
########################

N=11000000
SPMV_MTX="stokes.mtx"
SPMV_AVG_NNZ=360
REPS=150
THREAD_LIST=(8 16 32 64 128)

OUTDIR="${OUTDIR:-perf_out_v4}"
METRICSDIR="${OUTDIR}/metrics"
OMPT_LOGDIR="${OUTDIR}/ompt_logs"

WINDOW_CSV="${OUTDIR}/window_metrics.csv"
KERNEL_CSV="${OUTDIR}/kernel_metrics.csv"
OMPT_SUMMARY_CSV="${OUTDIR}/ompt_summary.csv"

CXX=clang++
CXXFLAGS=(-std=c++17 -O3 -fopenmp -ffast-math)

OMPT_TOOL_SRC="final_sched_NUMAratio.cpp"
OMPT_TOOL_BIN="numa_sched_finalratiov4.so"
OMPT_TOOL_FLAGS=(-std=c++17 -fPIC -shared -fopenmp -pthread -O2)
HWLOC_INCLUDE="${HWLOC_INCLUDE:-/opt/ohpc/pub/libs/hwloc/include}"
HWLOC_LIB="${HWLOC_LIB:-/opt/ohpc/pub/libs/hwloc/lib}"

REMOTE_EVENT="rD044"
ALL_FILLS_EVENT="rFF44"

declare -a SERIAL_KERNELS=(
  "stencil_serial:Stencil_serial.cpp:stencil_serial:STENCIL"
  "spmv_serial:spmv_serial.cpp:spmv_serial:SPMV"
)

declare -a PAR_KERNELS=(
  "stencil:Stencil.cpp:stencil:STENCIL"
  "spmv_static:spmv_staticSeed.cpp:spmv_static:SPMV"
)

declare -a OMP_CONFIGS=(
  "cores:spread:interleave:cores_spread_interleave"
  "cores:close:interleave:cores_close_interleave"
  "cores:close:interleave:cores_close_interleave_rep2"
)

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: Falta comando requerido: $1" >&2
    exit 1
  }
}

mkdirs() {
  mkdir -p "${OUTDIR}" "${METRICSDIR}" "${OMPT_LOGDIR}"
}

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

compile_ompt_tool() {
  if [[ ! -f "$OMPT_TOOL_SRC" ]]; then
    echo "[!] ERROR: OMPT tool source not found: $OMPT_TOOL_SRC" >&2
    exit 1
  fi

  echo "[*] Compiling OMPT tool: $OMPT_TOOL_SRC -> $OMPT_TOOL_BIN"
  "${CXX}" "${OMPT_TOOL_FLAGS[@]}" "$OMPT_TOOL_SRC" \
    -o "$OMPT_TOOL_BIN" \
    -I"${HWLOC_INCLUDE}" -L"${HWLOC_LIB}" -lhwloc

  if [[ ! -f "$OMPT_TOOL_BIN" ]]; then
    echo "[!] ERROR: OMPT tool compilation failed: $OMPT_TOOL_BIN" >&2
    exit 1
  fi
}

compile_all() {
  compile_ompt_tool
  for kernel_spec in "${SERIAL_KERNELS[@]}" "${PAR_KERNELS[@]}"; do
    IFS=':' read -r label src bin _ktype <<< "$kernel_spec"
    compile_kernel "$label" "$src" "$bin" || {
      echo "[!] FATAL: Failed to compile $label" >&2
      exit 1
    }
  done
}

append_perf_windows() {
  local tag="$1"
  local perf_out="$2"
  awk -F',' -v tag="$tag" '
    $3 ~ /^(rD044|rFF44|instructions|cycles)$/ {
      t=$1; gsub(/^[ \t]+|[ \t]+$/, "", t);
      c=$2; gsub(/^[ \t]+|[ \t]+$/, "", c);
      e=$3; gsub(/^[ \t]+|[ \t]+$/, "", e);
      key=t;
      if (e=="rD044") rm[key]=c;
      else if (e=="rFF44") all[key]=c;
      else if (e=="instructions") ins[key]=c;
      else if (e=="cycles") cyc[key]=c;
      seen[key]=1;
    }
    END {
      for (k in seen) {
        rmv = (k in rm)?rm[k]:0;
        inv = (k in ins)?ins[k]:0;
        cyv = (k in cyc)?cyc[k]:0;
        ipc = (cyv>0)? inv/cyv:0;
        allv = (k in all)?all[k]:0;
        ratio = (allv>0)? rmv/allv:0;
        printf "%s,%s,%s,%s,%s,%.6f,%.6f\n", tag, k, rmv, inv, cyv, ipc, ratio;
      }
    }' "$perf_out" >> "$WINDOW_CSV"
}

extract_kernel_metrics() {
  local csv="$1"
  awk -F',' '
    NR==1 { for (i=1;i<=NF;i++) idx[$i]=i; next }
    NR==2 {
      printf "%s %s %s\n", $(idx["stddev_ms"]), $(idx["gflops"]), $(idx["bw_gibs"]);
      exit
    }' "$csv"
}

sum_migrations_for_tag() {
  local tag="$1"
  if [[ ! -f "$OMPT_SUMMARY_CSV" ]]; then
    echo "0"
    return
  fi
  awk -F',' -v tag="$tag" '
    NR==1 { for (i=1;i<=NF;i++) idx[$i]=i; next }
    $(idx["tag"])==tag { sum += $(idx["migrations"]) }
    END { print (sum+0) }
  ' "$OMPT_SUMMARY_CSV"
}

append_kernel_summary() {
  local tag="$1"
  local config="$2"
  local threads="$3"
  local scheduler="$4"
  local migrations="$5"
  local kernel_csv="$6"

  read -r stdev_ms gflops bw_gibs <<< "$(extract_kernel_metrics "$kernel_csv")"
  echo "${config},${threads},${scheduler},${stdev_ms},${migrations},${bw_gibs},${gflops}" >> "$KERNEL_CSV"
}

run_kernel_cmd() {
  local kernel_label="$1"
  local kernel_bin="$2"
  local kernel_type="$3"
  local threads="$4"
  local csv_prefix="$5"

  if [[ "$kernel_type" == "STENCIL" ]]; then
    echo "./${kernel_bin} ${N} ${threads} ${REPS} ${csv_prefix}"
  else
    echo "./${kernel_bin} ${SPMV_MTX} ${threads} ${REPS} ${csv_prefix}"
  fi
}

run_with_perf_windows() {
  local tag="$1"; shift
  local perf_out="${METRICSDIR}/${tag}_perf_windows.txt"
  perf stat -I 100 -x, -e "${REMOTE_EVENT},${ALL_FILLS_EVENT},instructions,cycles" -- "$@" \
    2> "$perf_out" >/dev/null || true
  append_perf_windows "$tag" "$perf_out"
}

need_cmd perf
need_cmd "${CXX}"
need_cmd numactl

mkdirs
compile_all

export SPMV_AVG_NNZ="${SPMV_AVG_NNZ}"

rm -f "$WINDOW_CSV" "$KERNEL_CSV" "$OMPT_SUMMARY_CSV"
echo "tag,window_ms,d_rm,d_instr,d_cyc,ipc,ratio_rm" > "$WINDOW_CSV"
echo "config,threads,scheduler,stdev_ms,migrations,bw_gibs,gflops" > "$KERNEL_CSV"

unset OMP_TOOL OMP_TOOL_LIBRARIES OMPT_WINDOW_CSV OMPT_SUMMARY_CSV OMPT_LOG_FILE OMPT_TAG

echo "[*] Stage 1/3: Seriales"
for threads in "${THREAD_LIST[@]}"; do
  for kernel_spec in "${SERIAL_KERNELS[@]}"; do
    IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
    tag="${label}_t${threads}_serial"
    csv_prefix="${METRICSDIR}/${tag}"

    export OMP_NUM_THREADS="${threads}"
    export OMP_DYNAMIC="FALSE"
    unset OMP_PLACES
    unset OMP_PROC_BIND

    cmd_str=$(run_kernel_cmd "$label" "$bin" "$ktype" "$threads" "$csv_prefix")
    read -r -a cmd <<< "$cmd_str"
    run_with_perf_windows "$tag" "${cmd[@]}"
    append_kernel_summary "$tag" "${label}_serial" "$threads" "none" "0" "${csv_prefix}.csv"
  done
done

echo ""
echo "[*] Stage 2/3: OMP + numactl (sin OMPT)"
for threads in "${THREAD_LIST[@]}"; do
  for cfg in "${OMP_CONFIGS[@]}"; do
    IFS=':' read -r omp_places omp_bind numa_mode cfg_name <<< "$cfg"
    for kernel_spec in "${PAR_KERNELS[@]}"; do
      IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
      tag="${label}_t${threads}_${cfg_name}"
      csv_prefix="${METRICSDIR}/${tag}"

      export OMP_NUM_THREADS="${threads}"
      export OMP_DYNAMIC="FALSE"
      export OMP_PLACES="${omp_places}"
      export OMP_PROC_BIND="${omp_bind}"

      cmd_str=$(run_kernel_cmd "$label" "$bin" "$ktype" "$threads" "$csv_prefix")
      read -r -a cmd <<< "$cmd_str"
      run_with_perf_windows "$tag" numactl --interleave=all -- "${cmd[@]}"
      append_kernel_summary "$tag" "${label}_${cfg_name}" "$threads" "none" "0" "${csv_prefix}.csv"
    done
  done
done

echo ""
echo "[*] Stage 3/3: OMP + numactl + OMPT"
for threads in "${THREAD_LIST[@]}"; do
  for cfg in "${OMP_CONFIGS[@]}"; do
    IFS=':' read -r omp_places omp_bind numa_mode cfg_name <<< "$cfg"
    for kernel_spec in "${PAR_KERNELS[@]}"; do
      IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
      tag="${label}_t${threads}_${cfg_name}_ompt"
      csv_prefix="${METRICSDIR}/${tag}"
      ompt_log="${OMPT_LOGDIR}/${tag}.log"

      export OMP_NUM_THREADS="${threads}"
      export OMP_DYNAMIC="FALSE"
      export OMP_PLACES="${omp_places}"
      export OMP_PROC_BIND="${omp_bind}"

      export OMP_TOOL="enabled"
      export OMP_TOOL_LIBRARIES="$(realpath "$OMPT_TOOL_BIN")"
      export OMPT_WINDOW_CSV="${WINDOW_CSV}"
      export OMPT_SUMMARY_CSV="${OMPT_SUMMARY_CSV}"
      export OMPT_LOG_FILE="${ompt_log}"
      export OMPT_TAG="${tag}"

      cmd_str=$(run_kernel_cmd "$label" "$bin" "$ktype" "$threads" "$csv_prefix")
      read -r -a cmd <<< "$cmd_str"
      numactl --interleave=all -- "${cmd[@]}" >/dev/null 2>/dev/null || true

      migrations=$(sum_migrations_for_tag "$tag")
      append_kernel_summary "$tag" "${label}_${cfg_name}" "$threads" "ompt" "$migrations" "${csv_prefix}.csv"
    done
  done
done

echo ""
echo "[✓] Benchmark V4 completed."
echo "    Kernel CSV: ${KERNEL_CSV}"
echo "    Window CSV: ${WINDOW_CSV}"
echo "    OMPT summary CSV: ${OMPT_SUMMARY_CSV}"
echo "    Logs: ${OMPT_LOGDIR}/"
