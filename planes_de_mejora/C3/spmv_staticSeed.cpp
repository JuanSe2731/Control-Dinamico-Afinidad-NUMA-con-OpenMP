#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <tuple>
#include <cctype>
#include <cstdint>  

#ifdef _OPENMP
#include <omp.h>
#endif

// Ajusta estos typedefs a tu proyecto:
using IndexType = int;      // o int64_t
using ValueType = double;   // o float

static constexpr bool kPrint = false;

struct CsrMatrix {
    IndexType  num_rows{0};
    IndexType  num_cols{0};
    long long  nnz{0};

    std::vector<IndexType> row_ptrs;  // tamaño: num_rows + 1
    std::vector<IndexType> col_idxs;  // tamaño: nnz
    std::vector<ValueType> values;    // tamaño: nnz

    std::string source_file{"(generada)"};
};

struct Triplet {
    IndexType r;
    IndexType c;
    ValueType v;
};

static inline std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return (char)std::tolower(ch); });
    return s;
}

// Lee Matrix Market y construye CSR con llenado paralelo por filas.
CsrMatrix load_matrix_market(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("No se pudo abrir el archivo: " + filename);
    }

    // 1) Cabecera
    std::string header;
    if (!std::getline(file, header)) {
        throw std::runtime_error("Archivo vacío o cabecera faltante: " + filename);
    }

    std::string hdr = to_lower_copy(header);

    if (hdr.find("matrixmarket") == std::string::npos) {
        throw std::runtime_error("Cabecera MatrixMarket inválida.");
    }
    if (hdr.find("coordinate") == std::string::npos) {
        throw std::runtime_error(
            "Solo se soporta formato 'coordinate' (disperso).");
    }
    if (hdr.find("complex") != std::string::npos) {
        throw std::runtime_error(
            "Matrices 'complex' no están soportadas.");
    }

    const bool is_pattern   = (hdr.find("pattern")   != std::string::npos);
    const bool is_symmetric = (hdr.find("symmetric") != std::string::npos) ||
                              (hdr.find("hermitian") != std::string::npos);
    const bool is_skew      = (hdr.find("skew")      != std::string::npos);

    // 2) Leer línea de dimensiones (saltando comentarios)
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (line[0] == '%') continue;
        break;
    }
    if (line.empty()) {
        throw std::runtime_error("No se encontró línea de dimensiones M N nnz.");
    }

    IndexType M = 0, N = 0;
    long long nnz_file = 0;
    {
        std::istringstream ss(line);
        if (!(ss >> M >> N >> nnz_file)) {
            throw std::runtime_error("No se pudieron leer M N nnz.");
        }
    }
    if (M <= 0 || N <= 0 || nnz_file < 0) {
        throw std::runtime_error("Dimensiones o nnz inválidos.");
    }

    std::cout << "[MTX] Dimensiones: " << M << " x " << N
              << "  nnz declarado=" << nnz_file
              << (is_symmetric ? " [symmetric/hermitian]" : "")
              << (is_skew ? " [skew]" : "")
              << (is_pattern ? " [pattern]" : "")
              << "\n";

    // 3) Leer entradas COO (secuencial) -> vector de triplets
    // Reservamos con margen para symmetric/skew.
    std::vector<Triplet> entries;
    entries.reserve((size_t)std::max<long long>(1, nnz_file * (is_symmetric || is_skew ? 2 : 1)));

    long long entries_read = 0;
    IndexType r_in = 0, c_in = 0;
    ValueType v_in = 0;

    while (file >> r_in >> c_in) {
        if (!is_pattern) {
            if (!(file >> v_in)) {
                throw std::runtime_error(
                    "Error leyendo valor en entrada " + std::to_string(entries_read + 1));
            }
        } else {
            v_in = (ValueType)1;
        }

        // 1-based -> 0-based
        IndexType r = r_in - 1;
        IndexType c = c_in - 1;

        if (r < 0 || r >= M || c < 0 || c >= N) {
            throw std::runtime_error("Índice fuera de rango en entrada " +
                                     std::to_string(entries_read + 1));
        }

        // entrada original
        entries.push_back({r, c, v_in});

        // expansiones por simetría/skew
        if (is_symmetric && r != c) {
            entries.push_back({c, r, v_in});
        }
        if (is_skew && r != c) {
            entries.push_back({c, r, (ValueType)(-v_in)});
        }

        ++entries_read;
    }

    std::cout << "[MTX] Entradas leídas del archivo: " << entries_read << "\n";
    std::cout << "[MTX] Entradas COO (con expansión): " << entries.size() << "\n";

    // 4) Ordenar por (row, col) y deduplicar sumando valores
    std::sort(entries.begin(), entries.end(),
              [](const Triplet& a, const Triplet& b) {
                  if (a.r != b.r) return a.r < b.r;
                  return a.c < b.c;
              });

    std::vector<Triplet> coo;
    coo.reserve(entries.size());

    for (const auto& t : entries) {
        if (!coo.empty() && coo.back().r == t.r && coo.back().c == t.c) {
            coo.back().v += t.v; // dedup
        } else {
            coo.push_back(t);
        }
    }

    // (Opcional) eliminar ceros numéricos tras suma
    // Si no quieres esto, comenta este bloque.
    {
        size_t w = 0;
        for (size_t i = 0; i < coo.size(); ++i) {
            if (coo[i].v != (ValueType)0) {
                coo[w++] = coo[i];
            }
        }
        coo.resize(w);
    }

    std::cout << "[MTX] NNZ efectivos (tras dedup): " << coo.size() << "\n";

    // 5) Construcción CSR
    CsrMatrix mat;
    mat.num_rows = M;
    mat.num_cols = N;
    mat.nnz      = (long long)coo.size();
    mat.source_file = filename;

    mat.row_ptrs.assign((size_t)M + 1, 0);
    mat.col_idxs.resize((size_t)mat.nnz);
    mat.values.resize((size_t)mat.nnz);

    // 5.1) Contar nnz por fila (secuencial, muy barato)
    for (const auto& t : coo) {
        mat.row_ptrs[(size_t)t.r + 1]++;
    }

    // 5.2) Prefix-sum row_ptrs
    for (IndexType i = 0; i < M; ++i) {
        mat.row_ptrs[(size_t)i + 1] += mat.row_ptrs[(size_t)i];
    }

    assert(mat.row_ptrs[(size_t)M] == (IndexType)mat.nnz);

    // 5.3) First-touch de row_ptrs (opcional, pero útil NUMA)
    #pragma omp parallel for schedule(static)
    for (IndexType i = 0; i <= M; ++i) {
        volatile IndexType x = mat.row_ptrs[(size_t)i];
        (void)x;
    }

    // 5.4) Obtener límites por fila en coo (porque coo está ordenado por fila)
    std::vector<IndexType> row_begin((size_t)M + 1, 0);
    {
        size_t k = 0;
        for (IndexType r = 0; r < M; ++r) {
            row_begin[(size_t)r] = (IndexType)k;
            while (k < coo.size() && coo[k].r == r) ++k;
        }
        row_begin[(size_t)M] = (IndexType)coo.size();
    }

    // 5.5) Llenado paralelo de col_idxs y values por filas
    // Cada fila r escribe solo en [row_ptrs[r], row_ptrs[r+1]) => sin races.
    #pragma omp parallel for schedule(static)
    for (IndexType r = 0; r < M; ++r) {
        IndexType out = mat.row_ptrs[(size_t)r];
        for (IndexType k = row_begin[(size_t)r]; k < row_begin[(size_t)r + 1]; ++k) {
            mat.col_idxs[(size_t)out] = coo[(size_t)k].c;
            mat.values[(size_t)out]   = coo[(size_t)k].v;
            ++out;
        }
    }

    std::cout << "[MTX] CSR construido correctamente (llenado paralelo por filas).\n";
    return mat;
}

//  Generación de matriz aleatoria (modo sintético)
//  Usa schedule(static) — no forma parte de las métricas de rendimiento
CsrMatrix generate_random_matrix(IndexType rows, IndexType cols,
                                  int avg_nnz, unsigned seed = 42)
{
    CsrMatrix A;
    A.num_rows    = rows;
    A.num_cols    = cols;
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
              << "  (densidad: "
              << std::scientific << std::setprecision(3)
              << (double)A.nnz / ((double)rows * cols) * 100.0
              << " %)\n" << std::defaultfloat;

    A.col_idxs.resize(A.nnz);
    A.values.resize(A.nnz);

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

void init_vector(std::vector<ValueType>& v, ValueType fill = -1.0)
{
    const IndexType n = static_cast<IndexType>(v.size());
    if (fill >= 0.0) {
#pragma omp parallel for schedule(static)
        for (IndexType i = 0; i < n; ++i) v[i] = fill;
        return;
    }

    // Seed base distinta en cada ejecución
    static const unsigned base_seed = static_cast<unsigned>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
        (unsigned)std::random_device{}()
    );

#pragma omp parallel for schedule(static)
    for (IndexType i = 0; i < n; ++i) {
#ifdef _OPENMP
        std::mt19937 lgen(base_seed + (unsigned)(omp_get_thread_num() * 99991u + i));
#else
        std::mt19937 lgen(base_seed + (unsigned)i);
#endif
        std::uniform_real_distribution<ValueType> d(0.0, 1.0);
        v[i] = d(lgen);
    }
}

//  Kernel SpMV con schedule(dynamic) — ÚNICA variante para métricas
void spmv_dynamic(const CsrMatrix& A,
                  const std::vector<ValueType>& x,
                        std::vector<ValueType>& y)
{
    const IndexType* rp  = A.row_ptrs.data();
    const IndexType* ci  = A.col_idxs.data();
    const ValueType* val = A.values.data();
    const ValueType* xv  = x.data();
          ValueType* yv  = y.data();

#pragma omp parallel for schedule(static, 256)
    for (IndexType row = 0; row < A.num_rows; ++row) {
        ValueType sum = 0.0;
        for (IndexType k = rp[row]; k < rp[row + 1]; ++k)
            sum += val[k] * xv[ci[k]];
        yv[row] = sum;
    }
}

//  Validación: compara resultado paralelo contra referencia serial
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
        double ref_abs = std::abs(y_ref[i]);
        double rel     = std::abs(y[i] - y_ref[i]) / (ref_abs + 1e-14);
        if (rel > tol) {
            ++errors;
            if (errors <= 3)
                std::cerr << "  [VAL] Error en fila " << i
                          << ": calc=" << y[i]
                          << " ref=" << y_ref[i]
                          << " rel_err=" << rel << "\n";
        }
    }
    if (errors == 0) { std::cout << "  [VAL] PASSED\n"; return true; }
    std::cout << "  [VAL] FAILED (" << errors << " errores de "
              << A.num_rows << " filas)\n";
    return false;
}

//  Métricas de rendimiento
double compute_bandwidth_gibs(const CsrMatrix& mat, double elapsed_s)
{
    double bytes =
          static_cast<double>(mat.nnz)          * sizeof(ValueType)
        + static_cast<double>(mat.nnz)          * sizeof(IndexType)
        + static_cast<double>(mat.num_rows + 1) * sizeof(IndexType)
        + static_cast<double>(mat.num_cols)     * sizeof(ValueType)
        + static_cast<double>(mat.num_rows)     * sizeof(ValueType);
    return (bytes / (1024.0 * 1024.0 * 1024.0)) / elapsed_s;
}

double compute_bandwidth_gbs(const CsrMatrix& mat, double elapsed_s)
{
    double bytes =
          static_cast<double>(mat.nnz) * (sizeof(IndexType) + sizeof(ValueType))
        + static_cast<double>(mat.num_cols) * sizeof(ValueType);
    return (bytes / 1e9) / elapsed_s;
}

//  Framework de benchmark
struct BenchmarkResult {
    const char* strategy{nullptr};
    double min_time_s{0};
    double avg_time_s{0};
    double max_time_s{0};
    double stddev_s{0};
    double gflops{0};
    double bandwidth_gibs{0};
    double bandwidth_gbs{0};
};

using SpMVFunc = void(*)(const CsrMatrix&,
                         const std::vector<ValueType>&,
                               std::vector<ValueType>&);

BenchmarkResult benchmark_spmv(const CsrMatrix& A,
                                const std::vector<ValueType>& x,
                                      std::vector<ValueType>& y,
                                const char* strategy_name,
                                SpMVFunc spmv_func,
                                int reps   = 10,
                                int warmup = 2)
{
    std::cout << "\n[BENCH] Estrategia: " << strategy_name << "\n";

    // Warm-up con la misma función (spmv_dynamic); no se mide
    for (int i = 0; i < warmup; ++i) spmv_func(A, x, y);

    std::vector<double> times;
    times.reserve(reps);
    omp_control_tool(omp_control_tool_start, 1, nullptr);
    for (int r = 0; r < reps; ++r) {
        auto t0 = std::chrono::high_resolution_clock::now();
        spmv_func(A, x, y);
        auto t1 = std::chrono::high_resolution_clock::now();
        times.push_back(std::chrono::duration<double>(t1 - t0).count());
        printf("  Rep %2d: %.4f ms\n", r, times[r] * 1e3);
    }
    omp_control_tool(omp_control_tool_pause, 1, nullptr);
    std::sort(times.begin(), times.end());
    double min_t = times.front();
    double max_t = times.back();
    double avg_t = std::accumulate(times.begin(), times.end(), 0.0) / reps;
    double var   = 0.0;
    for (double t : times) var += (t - avg_t) * (t - avg_t);
    double stddev = std::sqrt(var / reps);

    double gflops  = (2.0 * static_cast<double>(A.nnz)) / (min_t * 1e9);
    double bw_gibs = compute_bandwidth_gibs(A, min_t);
    double bw_gbs  = compute_bandwidth_gbs(A, min_t);

    printf("  Tiempo  : %.4f ms (min) | %.4f ms (avg) | %.4f ms (max) | stddev=%.4f ms\n",
           min_t*1e3, avg_t*1e3, max_t*1e3, stddev*1e3);
    printf("  GFlops  : %.3f\n", gflops);
    printf("  BW GiB/s: %.3f  |  BW GB/s: %.3f\n", bw_gibs, bw_gbs);

    BenchmarkResult res;
    res.strategy       = strategy_name;
    res.min_time_s     = min_t;
    res.avg_time_s     = avg_t;
    res.max_time_s     = max_t;
    res.stddev_s       = stddev;
    res.gflops         = gflops;
    res.bandwidth_gibs = bw_gibs;
    res.bandwidth_gbs  = bw_gbs;
    return res;
}

//  Exportación CSV
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

    f << "strategy,matrix,rows,cols,nnz,threads,"
         "min_ms,avg_ms,max_ms,stddev_ms,gflops,bw_gibs,bw_gbs\n";

    for (const auto& r : results) {
        f << r.strategy       << ","
          << A.source_file    << ","
          << A.num_rows       << ","
          << A.num_cols       << ","
          << A.nnz            << ","
          << threads          << ","
          << std::fixed << std::setprecision(6)
          << r.min_time_s*1e3 << ","
          << r.avg_time_s*1e3 << ","
          << r.max_time_s*1e3 << ","
          << r.stddev_s*1e3   << ","
          << r.gflops         << ","
          << r.bandwidth_gibs << ","
          << r.bandwidth_gbs  << "\n";
    }
    std::cout << "[CSV] Resultados guardados en: " << filename << "\n";
}

//  Main
int main(int argc, char* argv[])
{
    // Uso: ./spmv [archivo.mtx] [hilos] [reps] [prefijo_csv]
    std::string mtx_file   = "";
    int         num_threads = 4;
    int         reps        = 10;
    std::string csv_prefix  = "";

    if (argc >= 2) mtx_file    = argv[1];
    if (argc >= 3) num_threads  = std::stoi(argv[2]);
    if (argc >= 4) reps         = std::stoi(argv[3]);
    if (argc >= 5) csv_prefix   = argv[4];

    if (!kPrint) {
        std::freopen("/dev/null", "w", stdout);
    }

#ifdef _OPENMP
    omp_set_num_threads(num_threads);
    std::cout << "[OMP] Hilos configurados: " << omp_get_max_threads() << "\n";
#else
    std::cout << "[OMP] OpenMP no disponible — modo secuencial.\n";
#endif

    std::cout << "=======================================================\n"
              << "  SpMV CSR Benchmark  (schedule: dynamic)\n"
              << "=======================================================\n";

    // ── Cargar o generar matriz
    CsrMatrix A;
    if (mtx_file.empty()) {
        constexpr IndexType ROWS    = 5000000;
        constexpr IndexType COLS    = 5000000;
        int avg_nnz = 360;
        if (const char* env = std::getenv("SPMV_AVG_NNZ")) {
            int v = std::atoi(env);
            if (v > 0) avg_nnz = v;
        }
        std::cout << "\n[INFO] Sin archivo .mtx → generando matriz aleatoria "
                  << ROWS << " x " << COLS
                  << "  avg_nnz_por_fila=" << avg_nnz << "\n";
	unsigned run_seed = static_cast<unsigned>(
    	std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
    	(unsigned)std::random_device{}()
	);
	std::cout << "[RNG] Seed de ejecucion: " << run_seed << "\n";
        A = generate_random_matrix(ROWS, COLS, avg_nnz, run_seed);
    } else {
        std::cout << "\n[INFO] Cargando archivo: " << mtx_file << "\n";
        A = load_matrix_market(mtx_file);
    }

std::cout << "\n[MAT] " << A.num_rows << " x " << A.num_cols
              << "  NNZ=" << A.nnz
              << "  fuente=" << A.source_file << "\n"
              << "[MAT] Densidad: "
              << std::scientific << std::setprecision(3)
              << (100.0 * static_cast<double>(A.nnz) /
                  (static_cast<double>(A.num_rows) * A.num_cols))
              << " %\n" << std::defaultfloat;

    // ── Preparar vectores
    std::vector<ValueType> x(A.num_cols), y(A.num_rows);
    init_vector(x);         // schedule(static) — solo inicialización
    init_vector(y, 0.0);

    // ── Validación previa
    std::cout << "\n[INFO] Validacion con spmv_dynamic...\n";
    spmv_dynamic(A, x, y);
    validate_result(A, x, y);

    // ── Benchmark — ÚNICA estrategia medida: Dynamic
    std::vector<BenchmarkResult> results;

    init_vector(y, 0.0);
    results.push_back(
        benchmark_spmv(A, x, y, "Dynamic", spmv_dynamic, reps));

    // ── Tabla resumen
    std::cout << "\n"
              << "+----------------------+----------+----------+----------+----------+\n"
              << "| Estrategia           | min (ms) | avg (ms) | GFlops   | GiB/s    |\n"
              << "+----------------------+----------+----------+----------+----------+\n";
    for (const auto& r : results) {
        printf("| %-20s | %8.3f | %8.3f | %8.3f | %8.3f |\n",
               r.strategy,
               r.min_time_s * 1e3,
               r.avg_time_s * 1e3,
               r.gflops,
               r.bandwidth_gibs);
    }
    std::cout << "+----------------------+----------+----------+----------+----------+\n";

    // ── Primeras entradas del resultado
    std::cout << "\n=== Primeras entradas de y = A*x ===\n";
    const int print_n = std::min(10, A.num_rows);
    for (int i = 0; i < print_n; ++i)
        printf("  y[%4d] = %.6f\n", i, y[i]);

    // ── Exportar CSV
    if (!csv_prefix.empty())
        export_csv(csv_prefix + ".csv", A, results);

    return 0;
}
