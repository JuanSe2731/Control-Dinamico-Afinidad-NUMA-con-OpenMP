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

#ifdef __linux__
#include <sys/prctl.h>
#endif

// Auto-instrumentación acotada a la región medida. Sustituye al `perf stat`
// externo, que contaba también la generación de la matriz y hacía inutilizables el
// IPC y el ratio_rm de las configuraciones sin tool OMPT. Se desactiva sola si el
// tool está cargado, para no medir lo mismo dos veces.
#include "perf_region.hpp"

// Compuerta de contadores hardware, igual que en Stencil.cpp. Sin esto las
// ventanas de perf de SpMV incluían la carga de la matriz, init_vector, la
// validación SERIAL y los 2 warmups, con lo que sus series no eran comparables
// con las del stencil (que sí las excluía).
static inline void perf_events_disable_all_threads() {
#ifdef __linux__
#ifdef _OPENMP
#pragma omp parallel
    { (void)prctl(PR_TASK_PERF_EVENTS_DISABLE); }
#else
    (void)prctl(PR_TASK_PERF_EVENTS_DISABLE);
#endif
#endif
}

static inline void perf_events_enable_all_threads() {
#ifdef __linux__
#ifdef _OPENMP
#pragma omp parallel
    { (void)prctl(PR_TASK_PERF_EVENTS_ENABLE); }
#else
    (void)prctl(PR_TASK_PERF_EVENTS_ENABLE);
#endif
#endif
}

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

// Semilla del experimento. Fija por defecto: la campaña tiene que poder repetirse
// y dar la misma matriz. SPMV_SEED la sobreescribe para estudios de sensibilidad.
static constexpr unsigned SEMILLA_DEFECTO = 42;

static unsigned semilla_spmv()
{
    if (const char* e = std::getenv("SPMV_SEED")) {
        long v = std::atol(e);
        if (v > 0) return static_cast<unsigned>(v);
    }
    return SEMILLA_DEFECTO;
}

// Warm-up subido de 2 a 5, configurable por entorno para que el valor efectivo
// quede en el registro experimental.
static constexpr int WARMUP_ITERS_DEFECTO = 5;

static int warmup_iters()
{
    if (const char* e = std::getenv("WARMUP_ITERS")) {
        int v = std::atoi(e);
        if (v >= 0) return v;
    }
    return WARMUP_ITERS_DEFECTO;
}

//  Generación de matriz aleatoria (modo sintético)
//  Usa schedule(static) — no forma parte de las métricas de rendimiento
CsrMatrix generate_random_matrix(IndexType rows, IndexType cols,
                                  int avg_nnz, unsigned seed = SEMILLA_DEFECTO)
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

    // La semilla local depende SOLO del índice de fila, nunca de
    // omp_get_thread_num(). Antes sí dependía, y eso hacía que la MISMA matriz
    // saliera con índices de columna distintos según el número de hilos: el patrón
    // de acceso a memoria cambiaba entre la corrida de 8 hilos y la de 128, que es
    // exactamente la variable que este proyecto pretende medir. Ahora la matriz es
    // idéntica con cualquier recuento de hilos y con o sin OpenMP.
#pragma omp parallel for schedule(static)
    for (IndexType i = 0; i < rows; ++i) {
        std::mt19937 lgen(seed + (unsigned)i);
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

    // Semilla FIJA. Antes se derivaba del reloj XOR random_device, con lo que cada
    // ejecución usaba un vector distinto y el experimento no era reproducible; y
    // como además entraba omp_get_thread_num(), el vector cambiaba con el número de
    // hilos. Se corrigen las dos cosas: SPMV_SEED la sobreescribe si hace falta.
    static const unsigned base_seed = semilla_spmv();

#pragma omp parallel for schedule(static)
    for (IndexType i = 0; i < n; ++i) {
        std::mt19937 lgen(base_seed + (unsigned)i);
        std::uniform_real_distribution<ValueType> d(0.0, 1.0);
        v[i] = d(lgen);
    }
}

//  Kernel SpMV con schedule(static, 256) — ÚNICA variante para métricas.
//
//  El nombre y las etiquetas decían "dynamic" mientras el pragma era static:
//  todas las filas SpMV de los resultados anteriores están rotuladas como
//  "Dynamic" pero se produjeron con reparto estático por bloques de 256 filas.
//  Se corrige el ETIQUETADO (el reparto estático es intencional, por localidad:
//  con first-touch/interleave un chunk fijo mantiene a cada hilo sobre las mismas
//  filas entre repeticiones). El documento de metodología, que en su Fase 2 dice
//  que SpMV usa schedule(dynamic), debe actualizarse para reflejar esto.
void spmv_static_chunk(const CsrMatrix& A,
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

// Working set del CSR: values + col_idxs + row_ptrs + vector x + vector y. Es lo
// que sitúa cada ejecución en su peldaño de la escalera de tamaños.
double working_set_bytes(const CsrMatrix& mat)
{
    return static_cast<double>(mat.nnz)          * sizeof(ValueType)
         + static_cast<double>(mat.nnz)          * sizeof(IndexType)
         + static_cast<double>(mat.num_rows + 1) * sizeof(IndexType)
         + static_cast<double>(mat.num_cols)     * sizeof(ValueType)
         + static_cast<double>(mat.num_rows)     * sizeof(ValueType);
}

// caudal_util_gibs SE RETIRA (campana V6). Los bytes eran una constante fijada
// por la matriz, asi que la columna era 1/t reescalado: no aportaba nada sobre
// avg_ms y se confundia con el trafico real de memoria, que se mide aparte con
// los rellenos por origen (perf_region.hpp) y donde MENOS ES MEJOR.

// GFLOPS. El cuerpo del kernel es `sum += val[k] * xv[ci[k]]`: una multiplicación
// y una suma por cada no-cero, es decir 2*nnz flop por producto matriz-vector.
double compute_gflops(const CsrMatrix& mat, double elapsed_s)
{
    return (2.0 * static_cast<double>(mat.nnz)) / (elapsed_s * 1e9);
}

//  Framework de benchmark
//
// TODAS las métricas derivadas se calculan sobre el tiempo MEDIO, no sobre el
// mínimo. El mínimo es la repetición más afortunada de 150: favorece a cualquier
// configuración y favorece MÁS a las de mayor varianza, que son justamente las que
// no fijan afinidad. La convención anterior sesgaba a favor de la propuesta.
struct BenchmarkResult {
    const char* strategy{nullptr};
    double min_time_s{0};
    double avg_time_s{0};
    double max_time_s{0};
    double stddev_s{0};
    double gflops{0};            // derivado de avg_time_s
};

using SpMVFunc = void(*)(const CsrMatrix&,
                         const std::vector<ValueType>&,
                               std::vector<ValueType>&);

BenchmarkResult benchmark_spmv(const CsrMatrix& A,
                                const std::vector<ValueType>& x,
                                      std::vector<ValueType>& y,
                                const char* strategy_name,
                                SpMVFunc spmv_func,
                                std::vector<double>& times_out,
                                int reps   = 10,
                                int warmup = WARMUP_ITERS_DEFECTO)
{
    std::cout << "\n[BENCH] Estrategia: " << strategy_name << "\n";

    // Warm-up con la misma función; no se mide y se excluye de los contadores.
    perf_events_disable_all_threads();
    // Los contadores propios se abren antes del warm-up para que el coste de
    // abrirlos no caiga en la región medida. Inerte si el tool OMPT está cargado.
    perf_region::init();
    for (int i = 0; i < warmup; ++i) spmv_func(A, x, y);
    perf_events_enable_all_threads();

    times_out.clear();
    times_out.reserve(reps);
    omp_control_tool(omp_control_tool_start, 1, nullptr);
    perf_region::begin();
    for (int r = 0; r < reps; ++r) {
        // steady_clock, no high_resolution_clock: éste último es alias de
        // system_clock en libstdc++ y no es monótono.
        auto t0 = std::chrono::steady_clock::now();
        spmv_func(A, x, y);
        auto t1 = std::chrono::steady_clock::now();
        times_out.push_back(std::chrono::duration<double>(t1 - t0).count());
        // El printf por repetición estaba DENTRO de la región medida: eran 150
        // write() por corrida en la ventana que el tool OMPT está contando.
    }
    perf_region::end();
    omp_control_tool(omp_control_tool_pause, 1, nullptr);
    // Se vuelven a apagar para que la validación, los printf y el volcado de CSV
    // no entren en el conteo agregado de perf.
    perf_events_disable_all_threads();

    std::vector<double> times = times_out;   // copia: el orden original se conserva
    std::sort(times.begin(), times.end());
    double min_t = times.front();
    double max_t = times.back();
    double avg_t = std::accumulate(times.begin(), times.end(), 0.0) / reps;
    double var   = 0.0;
    for (double t : times) var += (t - avg_t) * (t - avg_t);
    double stddev = std::sqrt(var / ((reps > 1) ? (reps - 1) : 1));  // muestral (N-1)

    // Sobre la MEDIA, no sobre el mínimo.
    double gflops = compute_gflops(A, avg_t);

    printf("  Tiempo  : %.4f ms (min) | %.4f ms (avg) | %.4f ms (max) | stddev=%.4f ms\n",
           min_t*1e3, avg_t*1e3, max_t*1e3, stddev*1e3);
    printf("  GFLOPS       : %.3f  (sobre la media)\n", gflops);

    BenchmarkResult res;
    res.strategy         = strategy_name;
    res.min_time_s       = min_t;
    res.avg_time_s       = avg_t;
    res.max_time_s       = max_t;
    res.stddev_s         = stddev;
    res.gflops           = gflops;
    return res;
}

//  Exportación CSV
void export_csv(const std::string& filename,
                const CsrMatrix& A,
                int warmup,
                int reps,
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

    f << "strategy,matrix,rows,cols,nnz,ws_bytes,threads,warmup,reps,"
         "min_ms,avg_ms,max_ms,stddev_ms,gflops\n";

    for (const auto& r : results) {
        f << r.strategy       << ","
          << A.source_file    << ","
          << A.num_rows       << ","
          << A.num_cols       << ","
          << A.nnz            << ","
          << std::fixed << std::setprecision(0) << working_set_bytes(A) << ","
          << threads          << ","
          << warmup           << ","
          << reps             << ","
          << std::setprecision(6)
          << r.min_time_s*1e3 << ","
          << r.avg_time_s*1e3 << ","
          << r.max_time_s*1e3 << ","
          << r.stddev_s*1e3   << ","
          << r.gflops         << "\n";
    }
    std::cout << "[CSV] Resultados guardados en: " << filename << "\n";
}

// Vuelca las repeticiones individuales EN ORDEN DE EJECUCIÓN. Es la muestra con
// la que se hacen Welch/ANOVA y los boxplots.
void export_times_csv(const std::string& filename,
                      const std::vector<double>& times_s)
{
    std::ofstream f(filename);
    if (!f.is_open()) {
        std::cerr << "[CSV] No se pudo crear: " << filename << "\n";
        return;
    }
    f << "rep,time_ms\n";
    f << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < times_s.size(); ++i)
        f << i << "," << times_s[i]*1e3 << "\n";
}

//  Main
int main(int argc, char* argv[])
{
    // Uso: ./spmv_static <filas|archivo.mtx> [hilos] [reps] [prefijo_csv]
    //
    // El primer argumento admite las dos formas y se distingue solo: si es un
    // entero positivo se genera una matriz sintética de ese número de filas; en
    // otro caso se interpreta como ruta a un .mtx. Así el tamaño pasa a ser un
    // parámetro barrido (peldaños S0..S5) sin cambiar la forma posicional de la
    // línea de órdenes, que es lo que construye build_cmd como arreglo de bash.
    //
    // La campaña ya no usa stokes.mtx: todos los peldaños salen del generador con
    // semilla fija, de modo que el experimento es reproducible de punta a punta sin
    // depender de un fichero de 4 GB que no está en git. La carga de .mtx se
    // conserva porque sigue siendo útil para comprobaciones puntuales.
    std::string mtx_file    = "";
    IndexType   rows_arg    = 0;
    int         num_threads = 4;
    int         reps        = 10;
    std::string csv_prefix  = "";

    if (argc >= 2) {
        char* fin = nullptr;
        long v = std::strtol(argv[1], &fin, 10);
        if (fin && *fin == '\0' && v > 0) rows_arg = static_cast<IndexType>(v);
        else                              mtx_file = argv[1];
    }
    if (argc >= 3) num_threads  = std::stoi(argv[2]);
    if (argc >= 4) reps         = std::stoi(argv[3]);
    if (argc >= 5) csv_prefix   = argv[4];

    const int warmup = warmup_iters();

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
              << "  SpMV CSR Benchmark  (schedule: static, chunk=256)\n"
              << "=======================================================\n";

    // Contadores apagados desde el principio: carga de la matriz, construcción
    // del CSR, init_vector y la validación SERIAL no deben entrar en el conteo.
    // Se encienden justo antes del bucle medido, dentro de benchmark_spmv.
    perf_events_disable_all_threads();

    // ── Cargar o generar matriz
    CsrMatrix A;
    if (mtx_file.empty()) {
        // avg_nnz = 32 por defecto: da un working set de ~404 B por fila, con lo
        // que los peldaños de tamaño del SpMV quedan emparejados uno a uno con los
        // del Stencil (S0 253 KiB ... S5 4.11 GiB).
        const IndexType ROWS = (rows_arg > 0) ? rows_arg : 10923000;
        const IndexType COLS = ROWS;
        int avg_nnz = 32;
        if (const char* env = std::getenv("SPMV_AVG_NNZ")) {
            int v = std::atoi(env);
            if (v > 0) avg_nnz = v;
        }
        const unsigned run_seed = semilla_spmv();
        std::cout << "\n[INFO] Generando matriz sintetica "
                  << ROWS << " x " << COLS
                  << "  avg_nnz_por_fila=" << avg_nnz << "\n";
        std::cout << "[RNG] Semilla FIJA de ejecucion: " << run_seed
                  << "  (SPMV_SEED para cambiarla)\n";
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
    std::cout << "\n[INFO] Validacion con spmv_static_chunk...\n";
    spmv_static_chunk(A, x, y);
    validate_result(A, x, y);

    // ── Benchmark — ÚNICA estrategia medida
    std::vector<BenchmarkResult> results;
    std::vector<double> times_s;

    init_vector(y, 0.0);
    results.push_back(
        benchmark_spmv(A, x, y, "Static-256", spmv_static_chunk, times_s,
                       reps, warmup));

    // ── Tabla resumen
    std::cout << "\n"
              << "+----------------------+----------+----------+----------+\n"
              << "| Estrategia           | min (ms) | avg (ms) | GFLOPS   |\n"
              << "+----------------------+----------+----------+----------+\n";
    for (const auto& r : results) {
        printf("| %-20s | %8.3f | %8.3f | %8.3f |\n",
               r.strategy,
               r.min_time_s * 1e3,
               r.avg_time_s * 1e3,
               r.gflops);
    }
    std::cout << "+----------------------+----------+----------+----------+\n";

    // ── Primeras entradas del resultado
    std::cout << "\n=== Primeras entradas de y = A*x ===\n";
    const int print_n = std::min(10, A.num_rows);
    for (int i = 0; i < print_n; ++i)
        printf("  y[%4d] = %.6f\n", i, y[i]);

    // ── Exportar CSV
    if (!csv_prefix.empty()) {
        export_csv(csv_prefix + ".csv", A, warmup, reps, results);
        export_times_csv(csv_prefix + "_times.csv", times_s);
        // Contadores por hilo acotados a la región medida. Vacío si el tool OMPT
        // era el propietario: entonces los datos salen de ompt_summary.csv.
        perf_region::escribir_csv((csv_prefix + "_counters.csv").c_str(),
                                  csv_prefix.c_str());
    }
    perf_region::shutdown();

    return 0;
}
