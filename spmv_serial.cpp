#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
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
#include <map>

#ifdef _OPENMP
#include <omp.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

static inline void ompt_measure_start() {
#ifdef _OPENMP
    (void)omp_control_tool(omp_control_tool_start, 1, nullptr);
#endif
}

static inline void ompt_measure_pause() {
#ifdef _OPENMP
    (void)omp_control_tool(omp_control_tool_pause, 1, nullptr);
#endif
}

static inline void perf_events_disable_this_thread() {
#ifdef __linux__
    (void)prctl(PR_TASK_PERF_EVENTS_DISABLE);
#endif
}

static inline void perf_events_enable_this_thread() {
#ifdef __linux__
    (void)prctl(PR_TASK_PERF_EVENTS_ENABLE);
#endif
}

using ValueType = double;
using IndexType = int;

//  Estructura CSR
struct CsrMatrix {
    IndexType  num_rows{0};
    IndexType  num_cols{0};
    long long  nnz{0};

    std::vector<IndexType> row_ptrs;  // tamaño: num_rows + 1
    std::vector<IndexType> col_idxs;  // tamaño: nnz
    std::vector<ValueType> values;    // tamaño: nnz

    std::string source_file{"(generada)"};
};

//  Carga de archivo Matrix Market → CSR
CsrMatrix load_matrix_market(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open())
        throw std::runtime_error("No se pudo abrir el archivo: " + filename);

    std::string header;     // ── Leer línea de cabecera
    if (!std::getline(file, header))
        throw std::runtime_error("Archivo vacío o cabecera faltante: " + filename);

    // Convertir cabecera a minúsculas para comparación
    std::string hdr_lower = header;
    std::transform(hdr_lower.begin(), hdr_lower.end(),
                   hdr_lower.begin(), ::tolower);

    // Validar que sea formato "coordinate" (disperso)
    // El formato "array" es denso y requiere un parser completamente distinto
    if (hdr_lower.find("coordinate") == std::string::npos)
        throw std::runtime_error(
            "Solo se soporta formato 'coordinate' (disperso). "
            "El archivo parece ser formato 'array' (denso).");

    // Rechazar matrices complejas: tienen 2 valores reales por entrada
    if (hdr_lower.find("complex") != std::string::npos)
        throw std::runtime_error(
            "Matrices 'complex' no están soportadas. "
            "Solo se aceptan 'real', 'integer' o 'pattern'.");

    bool is_symmetric = (hdr_lower.find("symmetric") != std::string::npos ||
                         hdr_lower.find("hermitian")  != std::string::npos);
    bool is_pattern   = (hdr_lower.find("pattern")   != std::string::npos);
    bool is_skew      = (hdr_lower.find("skew")      != std::string::npos);

    // Saltar líneas de comentario (todas las que empiezan con %)
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (line[0] != '%') break;   // primera línea de datos
    }

    // Leer dimensiones 
    IndexType   M, N;
    long long   nnz_file;   // long long para evitar overflow en matrices grandes
    {
        std::istringstream ss(line);
        if (!(ss >> M >> N >> nnz_file))
            throw std::runtime_error(
                "No se pudieron leer las dimensiones M N nnz del archivo.");
    }

    if (M <= 0 || N <= 0 || nnz_file <= 0)
        throw std::runtime_error("Dimensiones o NNZ inválidos en el archivo.");

 if (false)     std::cout << "[MTX] Dimensiones: " << M << " × " << N
              << "  NNZ declarado=" << nnz_file
              << (is_symmetric ? "  [simétrica]" : "")
              << (is_pattern   ? "  [patrón]"    : "")
              << "\n";

    // ── Leer entradas COO ────────────────────────────────────────────────
    // Usamos un map<(r,c), v> para acumular duplicados automáticamente.
    using Key = std::pair<IndexType, IndexType>;
    std::map<Key, ValueType> coo_map;

    long long entries_read = 0;
    IndexType r_in, c_in;
    ValueType v_in;

    while (file >> r_in >> c_in) {
        if (!is_pattern) {
            if (!(file >> v_in))
                throw std::runtime_error("Error leyendo valor en entrada " +
                                         std::to_string(entries_read + 1));
        } else {
            v_in = 1.0;   // matrices patrón: todos los valores son 1
        }

        // Convertir a indexación base-0
        IndexType r = r_in - 1;
        IndexType c = c_in - 1;

        if (r < 0 || r >= M || c < 0 || c >= N)
            throw std::runtime_error(
                "Índice fuera de rango en entrada " +
                std::to_string(entries_read + 1) +
                ": (" + std::to_string(r_in) + "," + std::to_string(c_in) + ")");

        // Acumular (maneja duplicados sumando sus valores)
        coo_map[{r, c}] += v_in;

        // Expandir simétrica/hermitiana (solo entradas fuera de la diagonal)
        if (is_symmetric && r != c)
            coo_map[{c, r}] += v_in;

        // Expansión skew-symmetric: A[c][r] = -A[r][c]
        if (is_skew && r != c)
            coo_map[{c, r}] -= v_in;

        ++entries_read;
        if (entries_read > nnz_file * 2 + 10)   // margen de seguridad
            throw std::runtime_error("Más entradas de las declaradas en el archivo.");
    }

 if (false)     std::cout << "[MTX] Entradas leídas del archivo : " << entries_read << "\n"
              << "[MTX] NNZ efectivos (tras dedup)  : " << coo_map.size() << "\n";

    // ── Construir CSR 
    CsrMatrix mat;
    mat.num_rows    = M;
    mat.num_cols    = N;
    mat.nnz         = static_cast<long long>(coo_map.size());
    mat.source_file = filename;

    mat.row_ptrs.assign(M + 1, 0);
    mat.col_idxs.resize(mat.nnz);
    mat.values.resize(mat.nnz);

    // Contar entradas por fila
    for (const auto& kv : coo_map)
        mat.row_ptrs[kv.first.first + 1]++;

    // prefix sum → row_ptrs queda como offsets de inicio de cada fila
    for (IndexType i = 0; i < M; ++i)
        mat.row_ptrs[i + 1] += mat.row_ptrs[i];

    // Paso 3: llenar col_idxs y values usando un cursor por fila
    std::vector<IndexType> cursor(mat.row_ptrs.begin(),
                                  mat.row_ptrs.begin() + M);

    for (const auto& kv : coo_map) {
        IndexType row = kv.first.first;
        IndexType col = kv.first.second;
        ValueType val = kv.second;

        IndexType pos        = cursor[row]++;   // posición de escritura para esta fila
        mat.col_idxs[pos]    = col;
        mat.values[pos]      = val;
    }

    // Verificación de integridad
    assert(mat.row_ptrs[M] == static_cast<IndexType>(mat.nnz));

 if (false)     std::cout << "[MTX] CSR construido correctamente.\n";
    return mat;
}

//  Generación de matriz aleatoria (modo sintético)
CsrMatrix generate_random_matrix(IndexType rows, IndexType cols,
                                  int avg_nnz, unsigned seed = 42)
{
    CsrMatrix A;
    A.num_rows    = rows;
    A.num_cols    = cols;
    A.source_file = "(aleatoria-Poisson)";

    std::mt19937 gen(seed);
    std::poisson_distribution<> poisson(avg_nnz);

 if (false)     std::cout << "[GEN] Calculando estructura de filas...\n";
    A.row_ptrs.resize(rows + 1, 0);
    for (IndexType i = 0; i < rows; ++i) {
        int nnz_row = std::max(1, (int)poisson(gen));
        A.row_ptrs[i + 1] = A.row_ptrs[i] + nnz_row;
    }
    A.nnz = A.row_ptrs[rows];

 if (false)     std::cout << "[GEN] NNZ total: " << A.nnz
              << "  (densidad: "
              << std::scientific << std::setprecision(3)
              << (double)A.nnz / ((double)rows * cols) * 100.0
              << " %)\n" << std::defaultfloat;

    A.col_idxs.resize(A.nnz);
    A.values.resize(A.nnz);

    std::uniform_int_distribution<IndexType> col_dist(0, cols - 1);
    std::uniform_real_distribution<ValueType> val_dist(0.0, 1.0);

    for (IndexType i = 0; i < rows; ++i) {
        for (IndexType k = A.row_ptrs[i]; k < A.row_ptrs[i + 1]; ++k) {
            A.col_idxs[k] = col_dist(gen);
            A.values[k]   = val_dist(gen);
        }
    }
    return A;
}

//  Inicialización de vectores
void init_vector(std::vector<ValueType>& v, ValueType fill = -1.0)
{
    const IndexType n = static_cast<IndexType>(v.size());
    if (fill >= 0.0) {
        for (IndexType i = 0; i < n; ++i) v[i] = fill;
        return;
    }
    // fill < 0 → valores aleatorios
    std::mt19937 gen(42u);
    std::uniform_real_distribution<ValueType> d(0.0, 1.0);
    for (IndexType i = 0; i < n; ++i)
        v[i] = d(gen);
}

//  Kernel SpMV (monohilo)
void spmv_serial(const CsrMatrix& A,
                 const std::vector<ValueType>& x,
                       std::vector<ValueType>& y)
{
    const IndexType* rp  = A.row_ptrs.data();
    const IndexType* ci  = A.col_idxs.data();
    const ValueType* val = A.values.data();
    const ValueType* xv  = x.data();
          ValueType* yv  = y.data();

    for (IndexType row = 0; row < A.num_rows; ++row) {
        ValueType sum = 0.0;
        for (IndexType k = rp[row]; k < rp[row + 1]; ++k)
            sum += val[k] * xv[ci[k]];
        yv[row] = sum;
    }
}

//  Validación: compara resultado contra referencia serial
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
                if (false) std::cerr << "  [VAL] Error en fila " << i
                          << ": calc=" << y[i]
                          << " ref=" << y_ref[i]
                          << " rel_err=" << rel << "\n";
        }
    }
    if (errors == 0) { if (false) std::cout << "  [VAL] PASSED\n"; return true; }
 if (false)     std::cout << "  [VAL] FAILED (" << errors << " errores de "
              << A.num_rows << " filas)\n";
    return false;
}

//  Métricas de rendimiento
double compute_bandwidth_gibs(const CsrMatrix& mat, double elapsed_s)
{
    // Bytes accedidos: values + col_idxs + row_ptrs + x (lectura) + y (escritura)
    double bytes =
          static_cast<double>(mat.nnz)          * sizeof(ValueType)   // values
        + static_cast<double>(mat.nnz)          * sizeof(IndexType)   // col_idxs
        + static_cast<double>(mat.num_rows + 1) * sizeof(IndexType)   // row_ptrs
        + static_cast<double>(mat.num_cols)     * sizeof(ValueType)   // x lectura
        + static_cast<double>(mat.num_rows)     * sizeof(ValueType);  // y escritura
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
 if (false)     std::cout << "\n[BENCH] Estrategia: " << strategy_name << "\n";

    // Warm-up: evita medir efectos de cache fría en las primeras iteraciones
    perf_events_disable_this_thread();
    for (int i = 0; i < warmup; ++i) spmv_func(A, x, y);
    perf_events_enable_this_thread();

    std::vector<double> times;
    times.reserve(reps);

    ompt_measure_start();
    for (int r = 0; r < reps; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        spmv_func(A, x, y);
        auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double>(t1 - t0).count());
        if (false) printf("  Rep %2d: %.4f ms\n", r, times[r] * 1e3);
    }
    ompt_measure_pause();

    std::sort(times.begin(), times.end());
    double min_t = times.front();
    double max_t = times.back();
    double avg_t = std::accumulate(times.begin(), times.end(), 0.0) / reps;
    double var   = 0.0;
    for (double t : times) var += (t - avg_t) * (t - avg_t);
    double stddev = std::sqrt(var / ((reps > 1) ? (reps - 1) : 1));  // muestral (N-1)

    // GFlops: SpMV hace exactamente 2*nnz operaciones (1 mul + 1 add por nnz)
    double gflops  = (2.0 * static_cast<double>(A.nnz)) / (min_t * 1e9);
    double bw_gibs = compute_bandwidth_gibs(A, min_t);
    double bw_gbs  = compute_bandwidth_gbs(A, min_t);

 if (false)     printf("  Tiempo  : %.4f ms (min) | %.4f ms (avg) | %.4f ms (max) | stddev=%.4f ms\n",
           min_t*1e3, avg_t*1e3, max_t*1e3, stddev*1e3);
 if (false)     printf("  GFlops  : %.3f\n", gflops);
 if (false)     printf("  BW GiB/s: %.3f  |  BW GB/s: %.3f\n", bw_gibs, bw_gbs);

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

    f << "strategy,matrix,rows,cols,nnz,threads,"
         "min_ms,avg_ms,max_ms,stddev_ms,gflops,bw_gibs,bw_gbs\n";

    for (const auto& r : results) {
        f << r.strategy       << ","
          << A.source_file    << ","
          << A.num_rows       << ","
          << A.num_cols       << ","
          << A.nnz            << ","
          << 1                << ","   // monohilo: siempre 1 hilo
          << std::fixed << std::setprecision(6)
          << r.min_time_s*1e3 << ","
          << r.avg_time_s*1e3 << ","
          << r.max_time_s*1e3 << ","
          << r.stddev_s*1e3   << ","
          << r.gflops         << ","
          << r.bandwidth_gibs << ","
          << r.bandwidth_gbs  << "\n";
    }
 if (false)     std::cout << "[CSV] Resultados guardados en: " << filename << "\n";
}

//  Main
int main(int argc, char* argv[])
{
    // Modos soportados:
    //   1) Sintético (recomendado):
    //        ./spmv_serial <N> [threads] [reps] [avg_nnz] [csv_prefix]
    //   2) Archivo MatrixMarket:
    //        ./spmv_serial <archivo.mtx> [threads] [reps] [csv_prefix]
    //
    // Nota: en modo serial el parámetro threads se acepta por consistencia,
    //       pero la ejecución sigue siendo monohilo.

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <N|archivo.mtx> [threads] [reps] [avg_nnz] [csv_prefix]\n";
        return 1;
    }

    std::string arg1       = argv[1];
    int         threads    = 1;
    int         reps       = 30;
    int         avg_nnz    = 32;
    std::string csv_prefix = "";

    auto is_integer = [](const std::string& s) {
        if (s.empty()) return false;
        for (unsigned char ch : s) if (ch < '0' || ch > '9') return false;
        return true;
    };

    bool synthetic_mode = is_integer(arg1);

    if (synthetic_mode) {
        // ./spmv_serial N [threads] [reps] [avg_nnz] [csv_prefix]
        const int N = std::stoi(arg1);
        if (argc >= 3) threads    = std::stoi(argv[2]);
        if (argc >= 4) reps       = std::stoi(argv[3]);
        if (argc >= 5) avg_nnz    = std::stoi(argv[4]);
        if (argc >= 6) csv_prefix = argv[5];

        if (N <= 0 || threads <= 0 || reps <= 0 || avg_nnz <= 0) return 1;

        CsrMatrix A = generate_random_matrix(N, N, avg_nnz);

        std::vector<ValueType> x(A.num_cols), y(A.num_rows);
        init_vector(x);
        init_vector(y, 0.0);

        spmv_serial(A, x, y);
        validate_result(A, x, y);

        std::vector<BenchmarkResult> results;
        init_vector(y, 0.0);
        results.push_back(benchmark_spmv(A, x, y, "Serial", spmv_serial, reps));

        if (!csv_prefix.empty())
            export_csv(csv_prefix + ".csv", A, results);

        return 0;
    }

    // ./spmv_serial <archivo.mtx> [threads] [reps] [csv_prefix]
    std::string mtx_file = arg1;
    if (argc >= 3) threads    = std::stoi(argv[2]);
    if (argc >= 4) reps       = std::stoi(argv[3]);
    if (argc >= 5) csv_prefix = argv[4];
    if (threads <= 0 || reps <= 0) return 1;

 if (false)     std::cout << "=======================================================\n"
              << "  SpMV CSR Benchmark  (monohilo)\n"
              << "=======================================================\n";

    // ── Cargar o generar matriz 
    CsrMatrix A;
    if (mtx_file.empty()) {
        constexpr IndexType ROWS    = 5000000;
        constexpr IndexType COLS    = 5000000;
        constexpr int       AVG_NNZ = 128;
        if (false) std::cout << "\n[INFO] Sin archivo .mtx → generando matriz aleatoria "
                  << ROWS << " x " << COLS
                  << "  avg_nnz_por_fila=" << AVG_NNZ << "\n";
        A = generate_random_matrix(ROWS, COLS, AVG_NNZ);
    } else {
        if (false) std::cout << "\n[INFO] Cargando archivo: " << mtx_file << "\n";
        A = load_matrix_market(mtx_file);
    }

 if (false)     std::cout << "\n[MAT] " << A.num_rows << " x " << A.num_cols
              << "  NNZ=" << A.nnz
              << "  fuente=" << A.source_file << "\n"
              << "[MAT] Densidad: "
              << std::scientific << std::setprecision(3)
              << (100.0 * static_cast<double>(A.nnz) /
                  (static_cast<double>(A.num_rows) * A.num_cols))
              << " %\n" << std::defaultfloat;

    // ── Preparar vectores 
    std::vector<ValueType> x(A.num_cols), y(A.num_rows);
    init_vector(x);         // valores aleatorios en [0,1]
    init_vector(y, 0.0);    // ceros

    // ── Validación previa 
 if (false)     std::cout << "\n[INFO] Validacion con spmv_serial...\n";
    spmv_serial(A, x, y);
    validate_result(A, x, y);

    // ── Benchmark 
    std::vector<BenchmarkResult> results;

    init_vector(y, 0.0);
    results.push_back(
        benchmark_spmv(A, x, y, "Serial", spmv_serial, reps));

    // ── Tabla resumen
 if (false)     std::cout << "\n"
              << "+----------------------+----------+----------+----------+----------+\n"
              << "| Estrategia           | min (ms) | avg (ms) | GFlops   | GiB/s    |\n"
              << "+----------------------+----------+----------+----------+----------+\n";
    for (const auto& r : results) {
        if (false) printf("| %-20s | %8.3f | %8.3f | %8.3f | %8.3f |\n",
               r.strategy,
               r.min_time_s * 1e3,
               r.avg_time_s * 1e3,
               r.gflops,
               r.bandwidth_gibs);
    }
 if (false)     std::cout << "+----------------------+----------+----------+----------+----------+\n";

    // ── Primeras entradas del resultado 
 if (false)     std::cout << "\n=== Primeras entradas de y = A*x ===\n";
    const int print_n = std::min(10, A.num_rows);
    for (int i = 0; i < print_n; ++i)
        if (false) printf("  y[%4d] = %.6f\n", i, y[i]);

    // ── Exportar CSV 
    if (!csv_prefix.empty())
        export_csv(csv_prefix + ".csv", A, results);

    return 0;
}
