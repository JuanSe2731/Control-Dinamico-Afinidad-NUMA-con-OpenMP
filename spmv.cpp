
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <omp.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <tuple>

#ifdef _OPENMP
#include <omp.h>
#endif

// OMPT (OpenMP Tools Interface)
// Se activa con -DOMPT_ENABLED.
#ifdef OMPT_ENABLED
#include <omp-tools.h>
#endif

//libnuma
// Se activa con -DNUMA_ENABLED.
#ifdef NUMA_ENABLED
#include <numa.h>
#include <numaif.h>
#include <sched.h>
#endif

using ValueType = double;
using IndexType = int;


// Estructura CSR
struct CsrMatrix {
    IndexType num_rows{0};
    IndexType num_cols{0};
    long long nnz{0};

    std::vector<IndexType> row_ptrs;   // indica cuando inicia y termina una fila
    std::vector<IndexType> col_idxs;   // indica las columnas donde se encuentran los nnz
    std::vector<ValueType> values;     // valores nnz

    std::string source_file{"(generada)"};
};

// Topología NUMA

struct NumaInfo {
    int num_nodes{1};
    int num_cpus{1};
    bool available{false};
    std::vector<std::vector<int>> node_cpus; // CPUs por nodo NUMA
};

NumaInfo query_numa_topology()
{
    NumaInfo info;
#ifdef NUMA_ENABLED
    if (numa_available() >= 0) {
        info.available  = true;
        info.num_nodes  = numa_num_configured_nodes();
        info.num_cpus   = numa_num_configured_cpus();
        info.node_cpus.resize(info.num_nodes);

        for (int cpu = 0; cpu < info.num_cpus; ++cpu) {
            int node = numa_node_of_cpu(cpu);
            if (node >= 0 && node < info.num_nodes)
                info.node_cpus[node].push_back(cpu);
        }
    }
#endif
    return info;
}

void print_numa_info(const NumaInfo& info)
{
    if (!info.available) {
        std::cout << "[NUMA] libnuma no disponible — sin detección de topología.\n";
        return;
    }
    std::cout << "[NUMA] Nodos detectados : " << info.num_nodes << "\n"
              << "[NUMA] CPUs totales     : " << info.num_cpus  << "\n";
    for (int n = 0; n < info.num_nodes; ++n) {
        std::cout << "[NUMA]   Nodo " << n << " → CPUs: ";
        for (int c : info.node_cpus[n]) std::cout << c << " ";
        std::cout << "\n";
    }
}


// OMPT – callbacks de instrumentación

#ifdef OMPT_ENABLED

struct OmptThreadStats {
    int  thread_id{-1};
    long tasks_executed{0};
    long long total_task_ns{0};
};

// Array estático indexado por thread_num
static OmptThreadStats g_ompt_stats[256];

// ── Callback: inicio de hilo OpenMP
static void on_thread_begin(ompt_thread_t thread_type,
                            ompt_data_t* thread_data)
{
#ifdef _OPENMP
    int tid = omp_get_thread_num();
#else
    int tid = 0;
#endif
    if (tid < 256) {
        g_ompt_stats[tid].thread_id = tid;
        // Aquí iría: registrar CPU actual con sched_getcpu()
        // y asociarla al nodo NUMA para el rebinding adaptativo.
    }
    (void)thread_type;
    (void)thread_data;
}

// Callback: fin de hilo OpenMP
static void on_thread_end(ompt_data_t* thread_data)
{
    (void)thread_data;
    // Aquí iría: flush de estadísticas de localidad del hilo.
}

// ── Callback: inicio/fin de tarea implícita (región paralela)
static void on_implicit_task(ompt_scope_endpoint_t endpoint,
                             ompt_data_t*          parallel_data,
                             ompt_data_t*          task_data,
                             unsigned int          actual_parallelism,
                             unsigned int          index,
                             int                   flags)
{
#ifdef _OPENMP
    int tid = omp_get_thread_num();
#else
    int tid = 0;
#endif
    if (tid < 256) {
        if (endpoint == ompt_scope_begin) {
            // Marca de tiempo de inicio de tarea — base para medir overhead
            task_data->value = static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now()
                    .time_since_epoch().count());
            g_ompt_stats[tid].tasks_executed++;
        } else {
            // Acumular tiempo de tarea
            uint64_t now = static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now()
                    .time_since_epoch().count());
            g_ompt_stats[tid].total_task_ns +=
                static_cast<long long>(now - task_data->value);
        }
    }
    (void)parallel_data;
    (void)actual_parallelism;
    (void)index;
    (void)flags;
}

// ── Inicialización del tool OMPT ─────────────────────────────────────────
static int ompt_initialize(ompt_function_lookup_t lookup,
                           int                    initial_device_num,
                           ompt_data_t*           tool_data)
{
    auto set_callback = (ompt_set_callback_t)lookup("ompt_set_callback");
    if (!set_callback) return 0;

    set_callback(ompt_callback_thread_begin,
                 (ompt_callback_t)on_thread_begin);
    set_callback(ompt_callback_thread_end,
                 (ompt_callback_t)on_thread_end);
    set_callback(ompt_callback_implicit_task,
                 (ompt_callback_t)on_implicit_task);

    std::cout << "[OMPT] Tool inicializado. Callbacks registrados: "
              << "thread_begin, thread_end, implicit_task.\n";
    (void)initial_device_num;
    (void)tool_data;
    return 1; // éxito
}

static void ompt_finalize(ompt_data_t* tool_data)
{
    std::cout << "[OMPT] Finalizando tool. Resumen por hilo:\n";
    for (int i = 0; i < 256; ++i) {
        if (g_ompt_stats[i].thread_id >= 0) {
            std::cout << "  Hilo " << g_ompt_stats[i].thread_id
                      << " | tareas=" << g_ompt_stats[i].tasks_executed
                      << " | tiempo_acum="
                      << g_ompt_stats[i].total_task_ns / 1000000 << " ms\n";
        }
    }
    (void)tool_data;
}

// Punto de entrada requerido por la interfaz OMPT
ompt_start_tool_result_t* ompt_start_tool(unsigned int omp_version,
                                          const char*  runtime_version)
{
    static ompt_start_tool_result_t tool = {
        &ompt_initialize,
        &ompt_finalize,
        {0}
    };
    std::cout << "[OMPT] ompt_start_tool() llamado. OMP version="
              << omp_version << " runtime=" << runtime_version << "\n";
    return &tool;
}

#endif

// Lectura de Matrix Market

/**
 * Carga una matriz dispersa real desde un archivo .mtx (Matrix Market).
 * Soporta: general, symmetric, real, pattern.
 * Las entradas se ordenan por (fila, columna) antes de construir el CSR.
 */
CsrMatrix load_matrix_market(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open())
        throw std::runtime_error("No se pudo abrir: " + filename);

    std::string line;
    std::getline(file, line);
    bool is_symmetric = (line.find("symmetric") != std::string::npos);
    bool is_pattern   = (line.find("pattern")   != std::string::npos);

    while (std::getline(file, line))
        if (!line.empty() && line[0] != '%') break;

    IndexType M, N, nnz_file;
    { std::istringstream ss(line); ss >> M >> N >> nnz_file; }

    using COOEntry = std::tuple<IndexType, IndexType, ValueType>;
    std::vector<COOEntry> entries;
    entries.reserve(is_symmetric ? nnz_file * 2 : nnz_file);

    for (IndexType i = 0; i < nnz_file; ++i) {
        IndexType r, c; ValueType v = 1.0;
        if (is_pattern) file >> r >> c;
        else            file >> r >> c >> v;
        r--; c--;
        entries.emplace_back(r, c, v);
        if (is_symmetric && r != c) entries.emplace_back(c, r, v);
    }

    std::sort(entries.begin(), entries.end(),
              [](const COOEntry& a, const COOEntry& b) {
                  return std::get<0>(a) != std::get<0>(b)
                       ? std::get<0>(a) < std::get<0>(b)
                       : std::get<1>(a) < std::get<1>(b);
              });

    CsrMatrix mat;
    mat.num_rows    = M;
    mat.num_cols    = N;
    mat.nnz         = static_cast<long long>(entries.size());
    mat.source_file = filename;
    mat.row_ptrs.assign(M + 1, 0);
    mat.col_idxs.resize(mat.nnz);
    mat.values.resize(mat.nnz);

    for (const auto& [r, c, v] : entries) mat.row_ptrs[r + 1]++;
    for (IndexType r = 0; r < M; ++r)
        mat.row_ptrs[r + 1] += mat.row_ptrs[r];
    for (long long i = 0; i < mat.nnz; ++i) {
        mat.col_idxs[i] = std::get<1>(entries[i]);
        mat.values[i]   = std::get<2>(entries[i]);
    }
    return mat;
}

//  Generación de matriz aleatoria

CsrMatrix generate_random_matrix(IndexType rows, IndexType cols,
                                 int avg_nnz, unsigned seed = 42)
{
    CsrMatrix A;
    A.num_rows = rows;
    A.num_cols = cols;
    A.source_file = "(aleatoria-Poisson)";

    std::mt19937 gen(seed);
    std::poisson_distribution<> poisson(avg_nnz);

    std::cout << "[GEN] Calculando estructura de filas...\n";
    A.row_ptrs.resize(rows + 1, 0);
    for (IndexType i = 0; i < rows; ++i) {
        int nnz_row = std::max(1, (int)poisson(gen));
        A.row_ptrs[i + 1] = A.row_ptrs[i] + nnz_row;
    }
    A.nnz = A.row_ptrs[rows];

    std::cout << "[GEN] NNZ total: " << A.nnz
              << " (densidad: "
              << std::scientific << std::setprecision(3)
              << (double)A.nnz / ((double)rows * cols) * 100.0
              << "%)\n";

    A.col_idxs.resize(A.nnz);
    A.values.resize(A.nnz);

    std::cout << "[GEN] Inicializando datos (first-touch NUMA-aware)...\n";

#pragma omp parallel for schedule(static)
    for (IndexType i = 0; i < rows; ++i) {
#ifdef _OPENMP
        std::mt19937 lgen(seed + (unsigned)(omp_get_thread_num() * 99991u + i));
#else
        std::mt19937 lgen(seed + (unsigned)i);
#endif
        std::uniform_int_distribution<IndexType> col_dist(0, cols - 1);
        std::uniform_real_distribution<ValueType> val_dist(0.0, 1.0);

        for (IndexType k = A.row_ptrs[i]; k < A.row_ptrs[i + 1]; ++k) {
            A.col_idxs[k] = col_dist(lgen);
            A.values[k]   = val_dist(lgen);
        }
    }
    return A;
}

// Inicialización de vectores NUMA-aware

void init_vector_numa_aware(std::vector<ValueType>& v, ValueType fill = -1.0)
{
    const IndexType n = static_cast<IndexType>(v.size());
    if (fill >= 0.0) {
#pragma omp parallel for schedule(static)
        for (IndexType i = 0; i < n; ++i) v[i] = fill;
        return;
    }
#pragma omp parallel for schedule(static)
    for (IndexType i = 0; i < n; ++i) {
#ifdef _OPENMP
        std::mt19937 lgen(42u + (unsigned)(omp_get_thread_num() * 99991u + i));
#else
        std::mt19937 lgen(42u + (unsigned)i);
#endif
        std::uniform_real_distribution<ValueType> d(0.0, 1.0);
        v[i] = d(lgen);
    }
}

// ── 7a. Kernel "clásico" (spmv) — schedule(static) por defecto
void spmv_classical(const CsrMatrix& A,
                    const std::vector<ValueType>& x,
                    std::vector<ValueType>& y)
{
    const IndexType* rp  = A.row_ptrs.data();
    const IndexType* ci  = A.col_idxs.data();
    const ValueType* val = A.values.data();
    const ValueType* xv  = x.data();
          ValueType* yv  = y.data();

#pragma omp parallel for schedule(static)
    for (IndexType row = 0; row < A.num_rows; ++row) {
        ValueType sum = 0.0;
        for (IndexType k = rp[row]; k < rp[row + 1]; ++k)
            sum += val[k] * xv[ci[k]];
        yv[row] = sum;
    }
}

// ── 7b. Schedule dynamic
void spmv_dynamic(const CsrMatrix& A,
                  const std::vector<ValueType>& x,
                  std::vector<ValueType>& y)
{
    const IndexType* rp  = A.row_ptrs.data();
    const IndexType* ci  = A.col_idxs.data();
    const ValueType* val = A.values.data();
    const ValueType* xv  = x.data();
          ValueType* yv  = y.data();

#pragma omp parallel for schedule(dynamic, 256)
    for (IndexType row = 0; row < A.num_rows; ++row) {
        ValueType sum = 0.0;
        for (IndexType k = rp[row]; k < rp[row + 1]; ++k)
            sum += val[k] * xv[ci[k]];
        yv[row] = sum;
    }
}

// ── 7c. Schedule guided
void spmv_guided(const CsrMatrix& A,
                 const std::vector<ValueType>& x,
                 std::vector<ValueType>& y)
{
    const IndexType* rp  = A.row_ptrs.data();
    const IndexType* ci  = A.col_idxs.data();
    const ValueType* val = A.values.data();
    const ValueType* xv  = x.data();
          ValueType* yv  = y.data();

#pragma omp parallel for schedule(guided)
    for (IndexType row = 0; row < A.num_rows; ++row) {
        ValueType sum = 0.0;
        for (IndexType k = rp[row]; k < rp[row + 1]; ++k)
            sum += val[k] * xv[ci[k]];
        yv[row] = sum;
    }
}

// ── 7d. Schedule auto
void spmv_auto(const CsrMatrix& A,
               const std::vector<ValueType>& x,
               std::vector<ValueType>& y)
{
    const IndexType* rp  = A.row_ptrs.data();
    const IndexType* ci  = A.col_idxs.data();
    const ValueType* val = A.values.data();
    const ValueType* xv  = x.data();
          ValueType* yv  = y.data();

#pragma omp parallel for schedule(auto)
    for (IndexType row = 0; row < A.num_rows; ++row) {
        ValueType sum = 0.0;
        for (IndexType k = rp[row]; k < rp[row + 1]; ++k)
            sum += val[k] * xv[ci[k]];
        yv[row] = sum;
    }
}

//  Validación

bool validate_result(const CsrMatrix& A,
                     const std::vector<ValueType>& x,
                     const std::vector<ValueType>& y,
                     double tol = 1e-10)
{
    std::vector<ValueType> y_ref(A.num_rows, 0.0);
    for (IndexType i = 0; i < A.num_rows; ++i) {
        ValueType sum = 0.0;
        for (IndexType k = A.row_ptrs[i]; k < A.row_ptrs[i + 1]; ++k)
            sum += A.values[k] * x[A.col_idxs[k]];
        y_ref[i] = sum;
    }

    int errors = 0;
    for (IndexType i = 0; i < A.num_rows; ++i) {
        double rel = std::abs(y[i] - y_ref[i]) /
                     (std::abs(y_ref[i]) + 1e-14);
        if (rel > tol) {
            ++errors;
            if (errors <= 3)
                std::cerr << "  [VAL] Error en fila " << i
                          << ": calc=" << y[i]
                          << " ref=" << y_ref[i] << "\n";
        }
    }
    if (errors == 0) { std::cout << "  [VAL] ✓ PASSED\n"; return true; }
    std::cout << "  [VAL] ✗ FAILED (" << errors << " errores)\n";
    return false;
}


// Métricas

double compute_bandwidth_gibs(const CsrMatrix& mat, double elapsed_s)
{
    double bytes =
          static_cast<double>(mat.nnz)            * sizeof(ValueType)  // values
        + static_cast<double>(mat.nnz)            * sizeof(IndexType)  // col_idxs
        + static_cast<double>(mat.num_rows + 1)   * sizeof(IndexType)  // row_ptrs
        + static_cast<double>(mat.num_cols)        * sizeof(ValueType)  // x lect.
        + static_cast<double>(mat.num_rows)        * sizeof(ValueType); // y escrit.
    return (bytes / (1024.0 * 1024.0 * 1024.0)) / elapsed_s;
}

// También en GB/s (base 10)
double compute_bandwidth_gbs(const CsrMatrix& mat, double elapsed_s)
{
    double bytes =
          static_cast<double>(mat.nnz) * (sizeof(IndexType) + sizeof(ValueType))
        + static_cast<double>(mat.num_cols) * sizeof(ValueType);
    return (bytes / 1e9) / elapsed_s;
}


// Framework de benchmark

struct BenchmarkResult {
    const char* strategy{nullptr};
    double      min_time_s{0};
    double      avg_time_s{0};
    double      max_time_s{0};
    double      stddev_s{0};
    double      gflops{0};
    double      bandwidth_gibs{0};  // GiB/s (spmv_2)
    double      bandwidth_gbs{0};   // GB/s  (spmv_1)
    double      arith_intensity{0}; // flops/byte (spmv_1)
};

using SpMVFunc = void(*)(const CsrMatrix&,
                         const std::vector<ValueType>&,
                               std::vector<ValueType>&);

/**
 * Ejecuta warm-up + N repeticiones de spmv_func y devuelve métricas.
 */
BenchmarkResult benchmark_spmv(const CsrMatrix& A,
                                const std::vector<ValueType>& x,
                                std::vector<ValueType>& y,
                                const char* strategy_name,
                                SpMVFunc spmv_func,
                                int reps = 10,
                                int warmup = 2)
{
    std::cout << "\n[BENCH] Estrategia: " << strategy_name << "\n";

    // Warm-up
    for (int i = 0; i < warmup; ++i) spmv_func(A, x, y);

    std::vector<double> times;
    times.reserve(reps);

    for (int r = 0; r < reps; ++r) {
        auto t0 = std::chrono::high_resolution_clock::now();
        spmv_func(A, x, y);
        auto t1 = std::chrono::high_resolution_clock::now();
        times.push_back(std::chrono::duration<double>(t1 - t0).count());
        printf("  Rep %2d: %.4f ms\n", r, times[r] * 1e3);
    }

    std::sort(times.begin(), times.end());
    double min_t = times.front();
    double max_t = times.back();
    double avg_t = std::accumulate(times.begin(), times.end(), 0.0) / reps;
    double var   = 0.0;
    for (double t : times) var += (t - avg_t) * (t - avg_t);
    double stddev = std::sqrt(var / reps);

    double gflops = (2.0 * static_cast<double>(A.nnz)) / (min_t * 1e9);
    double bw_gibs = compute_bandwidth_gibs(A, min_t);
    double bw_gbs  = compute_bandwidth_gbs(A, min_t);

    // bytes para intensidad aritmética (spmv_1 style)
    double bytes = static_cast<double>(A.nnz) *
                   (sizeof(IndexType) + sizeof(ValueType))
                 + static_cast<double>(A.num_cols) * sizeof(ValueType);
    double flops = 2.0 * static_cast<double>(A.nnz);
    double intensity = flops / bytes;

    BenchmarkResult res;
    res.strategy        = strategy_name;
    res.min_time_s      = min_t;
    res.avg_time_s      = avg_t;
    res.max_time_s      = max_t;
    res.stddev_s        = stddev;
    res.gflops          = gflops;
    res.bandwidth_gibs  = bw_gibs;
    res.bandwidth_gbs   = bw_gbs;
    res.arith_intensity = intensity;

    printf("  Tiempo  : %.4f ms (min) | %.4f ms (avg) | %.4f ms (max) | σ=%.4f ms\n",
           min_t*1e3, avg_t*1e3, max_t*1e3, stddev*1e3);
    printf("  GFlops  : %.3f\n", gflops);
    printf("  BW GiB/s: %.3f  |  BW GB/s: %.3f\n", bw_gibs, bw_gbs);
    printf("  Intensid: %.4f flops/byte\n", intensity);

    return res;
}

// Exportación CSV
void export_csv(const std::string& filename,
                const CsrMatrix& A,
                const std::vector<BenchmarkResult>& results)
{
    std::ofstream f(filename);
    if (!f.is_open()) {
        std::cerr << "[CSV] No se pudo crear: " << filename << "\n";
        return;
    }

    int threads = 1;
#ifdef _OPENMP
    threads = omp_get_max_threads();
#endif

    f << "strategy,matrix,rows,cols,nnz,min_ms,avg_ms,max_ms,"
         "stddev_ms,gflops,bw_gibs,bw_gbs,threads\n";

    for (const auto& r : results) {
        f << r.strategy        << ","
          << A.source_file     << ","
          << A.num_rows        << ","
          << A.num_cols        << ","
          << A.nnz             << ","
          << std::fixed << std::setprecision(6)
          << r.min_time_s*1e3  << ","
          << r.avg_time_s*1e3  << ","
          << r.max_time_s*1e3  << ","
          << r.stddev_s*1e3    << ","
          << r.gflops          << ","
          << r.bandwidth_gibs  << ","
          << r.bandwidth_gbs   << ","
          << threads           << "\n";
    }
    std::cout << "[CSV] Resultados guardados en: " << filename << "\n";
}



int main(int argc, char* argv[])
{
    omp_set_num_threads(12);
    // Uso: ./spmv [archivo.mtx] [reps] [prefijo_csv]
    std::string mtx_file   = "";
    int         reps       = 10;
    std::string csv_prefix = "";

    if (argc >= 2) mtx_file   = argv[1];
    if (argc >= 3) reps       = std::stoi(argv[2]);
    if (argc >= 4) csv_prefix = argv[3];


    std::cout << "========================================================\n"
              << "  spmv_hybrid — SpMV CSR benchmark + OMPT instrumentado\n"
              << "========================================================\n";

    // ── Topología NUMA ─────────────────────────────────────────────────────
    NumaInfo numa = query_numa_topology();
    print_numa_info(numa);

    // ── Info de hilos ───────────────────────────────────────────────────────
#ifdef _OPENMP
    std::cout << "[OMP] Hilos disponibles: " << omp_get_max_threads() << "\n";
#else
    std::cout << "[OMP] OpenMP no disponible — modo secuencial\n";
#endif

#ifdef OMPT_ENABLED
    std::cout << "[OMPT] Soporte OMPT compilado.\n";
#else
    std::cout << "[OMPT] OMPT no compilado (usa -DOMPT_ENABLED para activarlo).\n";
#endif

    // ── Cargar o generar matriz ─────────────────────────────────────────────
    CsrMatrix A;
    if (mtx_file.empty()) {
        // Modo spmv_1: matriz aleatoria grande
        constexpr IndexType ROWS    = 5000000;
        constexpr IndexType COLS    = 5000000;
        constexpr int       AVG_NNZ = 32;
        std::cout << "\n[INFO] Sin archivo .mtx → matriz aleatoria "
                  << ROWS << "×" << COLS
                  << " avg_nnz=" << AVG_NNZ << "\n";
        A = generate_random_matrix(ROWS, COLS, AVG_NNZ);
    } else {
        std::cout << "\n[INFO] Cargando: " << mtx_file << "\n";
        A = load_matrix_market(mtx_file);
    }

    std::cout << "[MAT] " << A.num_rows << " × " << A.num_cols
              << "  NNZ=" << A.nnz
              << "  fuente=" << A.source_file << "\n"
              << "[MAT] Densidad: "
              << std::scientific << std::setprecision(3)
              << (100.0 * static_cast<double>(A.nnz) /
                  (static_cast<double>(A.num_rows) * A.num_cols))
              << " %\n" << std::defaultfloat;

    // ── Preparar vectores (first-touch NUMA-aware) ──────────────────────────
    std::vector<ValueType> x(A.num_cols), y(A.num_rows);
    init_vector_numa_aware(x);          // valores aleatorios
    init_vector_numa_aware(y, 0.0);     // ceros

    // ── Validación previa con classical ────────────────────────────────────
    std::cout << "\n[INFO] Validación con spmv_classical...\n";
    spmv_classical(A, x, y);
    validate_result(A, x, y);

    // ── Benchmark de las 4 estrategias ─────────────────────────────────────
    std::vector<BenchmarkResult> results;

    init_vector_numa_aware(y, 0.0);
    results.push_back(
        benchmark_spmv(A, x, y, "Static (classical)", spmv_classical, reps));

    init_vector_numa_aware(y, 0.0);
    results.push_back(
        benchmark_spmv(A, x, y, "Dynamic", spmv_dynamic, reps));

    init_vector_numa_aware(y, 0.0);
    results.push_back(
        benchmark_spmv(A, x, y, "Guided", spmv_guided, reps));

    init_vector_numa_aware(y, 0.0);
    results.push_back(
        benchmark_spmv(A, x, y, "Auto", spmv_auto, reps));

    // ── Tabla resumen ───────────────────────────────────────────────────────
    std::cout << "\n"
              << "╔══════════════════════╦══════════╦══════════╦══════════╦══════════╗\n"
              << "║ Estrategia           ║ min (ms) ║ avg (ms) ║ GFlops   ║ GiB/s    ║\n"
              << "╠══════════════════════╬══════════╬══════════╬══════════╬══════════╣\n";
    for (const auto& r : results) {
        printf("║ %-20s ║ %8.3f ║ %8.3f ║ %8.3f ║ %8.3f ║\n",
               r.strategy,
               r.min_time_s * 1e3,
               r.avg_time_s * 1e3,
               r.gflops,
               r.bandwidth_gibs);
    }
    std::cout << "╚══════════════════════╩══════════╩══════════╩══════════╩══════════╝\n";

    // ── Mejor estrategia ────────────────────────────────────────────────────
    auto best = std::max_element(results.begin(), results.end(),
        [](const BenchmarkResult& a, const BenchmarkResult& b) {
            return a.gflops < b.gflops;
        });
    std::cout << "\n✓ Mejor estrategia: " << best->strategy
              << "  (" << std::fixed << std::setprecision(3)
              << best->gflops << " GFlops | "
              << best->bandwidth_gibs << " GiB/s)\n";

    // ── Primeras entradas del resultado (de spmv_2) ─────────────────────────
    std::cout << "\n=== Primeras entradas de y = A·x ===\n";
    const int print_n = std::min(10, A.num_rows);
    for (int i = 0; i < print_n; ++i)
        printf("  y[%4d] = %.6f\n", i, y[i]);

    // ── Exportar CSV ────────────────────────────────────────────────────────
    if (!csv_prefix.empty()) {
        export_csv(csv_prefix + ".csv", A, results);
    }

    return 0;
}
