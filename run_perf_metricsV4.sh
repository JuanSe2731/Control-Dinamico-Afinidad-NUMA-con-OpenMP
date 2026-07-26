#!/usr/bin/env bash
set -euo pipefail

# Forzar locale C: garantiza separador decimal "." en awk/printf y en los CSV,
# independiente del locale de la maquina (p.ej. es_ES usa "," y corrompe el CSV).
export LC_ALL=C

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
STENCIL_N=23500
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
OMPT_WINDOW_CSV_FILE="${OUTDIR}/ompt_window_metrics.csv"

CXX=clang++
CXXFLAGS=(-std=c++17 -O3 -fopenmp -ffast-math)

OMPT_TOOL_SRC="sched_NUMA_optC_leaky.cpp"
OMPT_TOOL_BIN="numa_sched_optC.so"
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
    $4 ~ /^(rD044|rFF44|instructions|cycles)$/ {
      t=$1; gsub(/^[ \t]+|[ \t]+$/, "", t);
      c=$2; gsub(/^[ \t]+|[ \t]+$/, "", c);
      e=$4; gsub(/^[ \t]+|[ \t]+$/, "", e);
      key=t;
      if (e=="rD044") rm[key]=c;
      else if (e=="rFF44") all[key]=c;
      else if (e=="instructions") ins[key]=c;
      else if (e=="cycles") cyc[key]=c;
      seen[key]=1;
    }
    END {
      for (k in seen) {
        rmv  = (k in rm)?  rm[k]  : "0";
        inv  = (k in ins)? ins[k] : "0";
        cyv  = (k in cyc)? cyc[k] : "0";
        allv = (k in all)? all[k] : "0";
        # Saltar ventanas sin medida real (contadores apagados:
        # "<not counted>"/"<not supported>" o 0 ciclos): no son datos del kernel.
        if (cyv !~ /^[0-9]+$/ || cyv+0 == 0) continue;
        cyv_n  = cyv+0;
        inv_n  = (inv  ~ /^[0-9]+$/)? inv+0  : 0;
        rmv_n  = (rmv  ~ /^[0-9]+$/)? rmv+0  : 0;
        allv_n = (allv ~ /^[0-9]+$/)? allv+0 : 0;
        ipc   = inv_n / cyv_n;
        ratio = (allv_n > 0)? rmv_n / allv_n : 0;
        printf "%s,%.3f,%s,%s,%s,%.6f,%.6f\n", tag, k*1000, rmv, inv, cyv, ipc, ratio;
      }
    }' "$perf_out" >> "$WINDOW_CSV" || echo "[!] WARN: append_perf_windows fallo en $tag (continuo)" >&2
}

extract_kernel_metrics() {
  local csv="$1"
  awk -F',' '
    NR==1 { for (i=1;i<=NF;i++) idx[$i]=i; next }
    NR==2 {
      m = ("mlups" in idx) ? $(idx["mlups"]) : "NA";
      printf "%s %s %s %s\n", $(idx["stddev_ms"]), $(idx["gflops"]), $(idx["bw_gibs"]), m;
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

  read -r stdev_ms gflops bw_gibs mlups <<< "$(extract_kernel_metrics "$kernel_csv")"
  echo "${config},${threads},${scheduler},${stdev_ms},${migrations},${bw_gibs},${gflops},${mlups}" >> "$KERNEL_CSV"
}

run_kernel_cmd() {
  local kernel_label="$1"
  local kernel_bin="$2"
  local kernel_type="$3"
  local threads="$4"
  local csv_prefix="$5"

  if [[ "$kernel_type" == "STENCIL" ]]; then
    echo "./${kernel_bin} ${STENCIL_N} ${threads} ${REPS} ${csv_prefix}"
  else
    echo "./${kernel_bin} ${SPMV_MTX} ${threads} ${REPS} ${csv_prefix}"
  fi
}

build_cmd() {
  # Construye el arreglo global 'cmd' de forma SEGURA para rutas con espacios
  # (p.ej. "planes de mejora"). NO usar run_kernel_cmd + read -a: el word-splitting
  # parte csv_prefix en el espacio y el kernel escribe su CSV en la ruta equivocada.
  local ktype="$1" bin="$2" threads="$3" csv_prefix="$4"
  if [[ "$ktype" == "STENCIL" ]]; then
    cmd=("./${bin}" "${STENCIL_N}" "${threads}" "${REPS}" "${csv_prefix}")
  else
    cmd=("./${bin}" "${SPMV_MTX}" "${threads}" "${REPS}" "${csv_prefix}")
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

rm -f "$WINDOW_CSV" "$KERNEL_CSV" "$OMPT_SUMMARY_CSV" "$OMPT_WINDOW_CSV_FILE"
echo "tag,window_ms,d_rm,d_instr,d_cyc,ipc,ratio_rm" > "$WINDOW_CSV"
echo "config,threads,scheduler,stdev_ms,migrations,bw_gibs,gflops,mlups" > "$KERNEL_CSV"

unset OMP_TOOL OMP_TOOL_LIBRARIES OMPT_LOG_FILE OMPT_TAG

if [[ "${RUN_SERIAL:-1}" == "1" ]]; then
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

    build_cmd "$ktype" "$bin" "$threads" "$csv_prefix"
    run_with_perf_windows "$tag" "${cmd[@]}"
    append_kernel_summary "$tag" "${label}_serial" "$threads" "none" "0" "${csv_prefix}.csv"
  done
done
else
  echo "[i] Stage 1 (serial) OMITIDA (RUN_SERIAL=0)"
fi

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

      build_cmd "$ktype" "$bin" "$threads" "$csv_prefix"
      run_with_perf_windows "$tag" numactl --interleave=all -- "${cmd[@]}"
      append_kernel_summary "$tag" "${label}_${cfg_name}" "$threads" "none" "0" "${csv_prefix}.csv"
    done
  done
done

echo ""
echo "[*] Stage 3/3: OMPT scheduler (sin OMP_PLACES/OMP_PROC_BIND; el scheduler decide la afinidad)"
# El scheduler NO usa las configuraciones OMP de la etapa 2 (deja OMP_PLACES/OMP_PROC_BIND
# sin fijar a proposito). Por eso se ejecuta UNA sola vez por (hilos,kernel) y se etiqueta
# como "<kernel>_scheduler" — NO con un nombre de config OMP, para no confundir el analisis.
for threads in "${THREAD_LIST[@]}"; do
  for kernel_spec in "${PAR_KERNELS[@]}"; do
    IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
    tag="${label}_t${threads}_scheduler"
    csv_prefix="${METRICSDIR}/${tag}"
    ompt_log="${OMPT_LOGDIR}/${tag}.log"

    export OMP_NUM_THREADS="${threads}"
    export OMP_DYNAMIC="FALSE"
    unset OMP_PLACES OMP_PROC_BIND

    export OMP_TOOL="enabled"
    export OMP_TOOL_LIBRARIES="$(realpath "$OMPT_TOOL_BIN")"
    export OMPT_WINDOW_CSV="${OMPT_WINDOW_CSV_FILE}"
    export OMPT_SUMMARY_CSV="${OMPT_SUMMARY_CSV}"
    export OMPT_LOG_FILE="${ompt_log}"
    export OMPT_TAG="${tag}"

    build_cmd "$ktype" "$bin" "$threads" "$csv_prefix"
    numactl --interleave=all -- "${cmd[@]}" >/dev/null 2>/dev/null || true

    migrations=$(sum_migrations_for_tag "$tag")
    append_kernel_summary "$tag" "${label}_scheduler" "$threads" "ompt" "$migrations" "${csv_prefix}.csv"
  done
done

echo ""
echo "[✓] Benchmark V4 completed."
echo "    Kernel CSV: ${KERNEL_CSV}"
echo "    Perf window CSV: ${WINDOW_CSV}"
echo "    OMPT window CSV: ${OMPT_WINDOW_CSV_FILE}"
echo "    OMPT summary CSV: ${OMPT_SUMMARY_CSV}"
echo "    Logs: ${OMPT_LOGDIR}/"
