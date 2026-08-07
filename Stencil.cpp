#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include <omp.h>
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

static constexpr int WARMUP_ITERS = 2; // igual que spmv.cpp / spmv_dynamic.cpp
static constexpr int REPS = 30;
static constexpr bool kPrint = false;

// steady_clock, NO high_resolution_clock: en libstdc++ este último es un alias de
// system_clock y por tanto NO es monótono — un ajuste de NTP a mitad de una tanda
// de 150 repeticiones introduce un salto en los tiempos medidos.
static inline double now_sec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// Inicialización SECUENCIAL (baseline OpenMP sin first-touch)
static void init_matrix_seq(float* A, int n) {
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            A[i * n + j] = static_cast<float>(i * n + j);
        }
    }
}

// Reparto ESTÁTICO POR FILAS (sin collapse(2)).
//
// El collapse(2) previo linearizaba el espacio (j,k) y con ello impedía que el
// compilador vectorizara el bucle interno: clang reportaba "loop not vectorized"
// para la versión colapsada frente a "vectorized loop (width 4, interleaved 2)"
// para ésta. Medido en local, el coste era 2.37x a 1 hilo, y se diluía al saturar
// el ancho de banda (1.00x a 6 hilos) — o sea, penalizaba justo en los conteos
// bajos de hilos, que es donde el speedup salía absurdo (paralelo más lento que
// serial). Con STENCIL_N=23500 y 128 hilos quedan 183 filas por hilo, de sobra
// para que el reparto estático por filas esté bien balanceado.
static void stencil2D_omp_static(const float* A, float* result, int n) {
#pragma omp parallel for schedule(static)
    for (int j = 1; j < n - 1; ++j) {
        for (int k = 1; k < n - 1; ++k) {
            result[j * n + k] = 0.2f * (
                A[j * n + k] +
                A[(j - 1) * n + k] +
                A[(j + 1) * n + k] +
                A[j * n + (k - 1)] +
                A[j * n + (k + 1)]
            );
        }
    }
}

struct Stats {
    double min_s{0}, avg_s{0}, max_s{0}, stddev_s{0};
};

struct BenchmarkResult {
    const char* strategy{nullptr};
    double min_time_s{0};
    double avg_time_s{0};
    double max_time_s{0};
    double stddev_s{0};
    double bandwidth_gibs{0};   // derivado de min_time_s (convención HPC)
    double mlups_min{0};        // mejor caso, convención
    double mlups_avg{0};        // derivado de la media: es el que admite barra de error
};

static Stats compute_stats(std::vector<double>& times) {
    std::sort(times.begin(), times.end());
    const double min_t = times.front();
    const double max_t = times.back();
    const double avg_t = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    double var = 0.0;
    for (double t : times) var += (t - avg_t) * (t - avg_t);
    // Estimador MUESTRAL (divisor N-1, corrección de Bessel). Antes dividía por N;
    // con N=150 la diferencia es del 0.33%, pero ahora que se vuelcan los tiempos
    // individuales cualquiera puede recalcularlo y debe cuadrar.
    const double denom  = (times.size() > 1) ? (double)(times.size() - 1) : 1.0;
    const double stddev = std::sqrt(var / denom);
    return Stats{min_t, avg_t, max_t, stddev};
}

// Ancho de banda bajo el MODELO DE TRÁFICO COMPULSORIO: 5 lecturas + 1 escritura
// por punto, como si cada acceso fuera a DRAM.
//
// ATENCIÓN al interpretarlo: un stencil de 5 puntos que barre fila por fila tiene
// enorme reutilización de caché (A[j][k-1], A[j][k] y A[j][k+1] caen en la misma
// línea; A[j-1][*] y A[j+1][*] ya se cargaron o se cargarán en filas contiguas).
// El tráfico REAL a DRAM son ~2 flujos (leer A una vez, escribir result una vez)
// = 8 B/punto, no 24. Es decir, esta cifra SOBREESTIMA el ancho de banda real
// por un factor ~3, y por eso el baseline serial de un solo hilo da ~70 GiB/s,
// que es físicamente imposible. Es una convención admisible mientras se declare,
// y no afecta a las comparaciones RELATIVAS, que es lo que usa el análisis.
// La métrica primaria del stencil es MLUPS, no ésta.
static double compute_bandwidth_gibs(int n, double elapsed_s) {
    const double points = static_cast<double>(n - 2) * (n - 2);
    const double bytes = points * 6.0 * sizeof(float);
    return (bytes / (1024.0 * 1024.0 * 1024.0)) / elapsed_s;
}

// MLUPS: millones de actualizaciones de malla por segundo = puntos_interiores / (t * 1e6).
// Metrica de rendimiento propia del stencil (independiente del conteo de FLOPs).
static double compute_mlups(int n, double elapsed_s) {
    const double points = static_cast<double>(n - 2) * (n - 2);
    return points / (elapsed_s * 1e6);
}

static void export_csv(const std::string& filename,
                       int n,
                       int threads,
                       const std::vector<BenchmarkResult>& results) {
    std::ofstream f(filename);
    if (!f.is_open()) {
        std::cerr << "[CSV] No se pudo crear: " << filename << "\n";
        return;
    }

    // GFlops y bw_gbs eliminados a propósito: para este kernel MLUPS = GFlops*200
    // y bw_gibs es proporcional a ambos, así que eran la misma curva por triplicado.
    f << "strategy,n,threads,min_ms,avg_ms,max_ms,stddev_ms,bw_gibs,mlups_min,mlups_avg\n";
    for (const auto& r : results) {
        f << r.strategy << ","
          << n << ","
          << threads << ","
          << std::fixed << std::setprecision(6)
          << r.min_time_s * 1e3 << ","
          << r.avg_time_s * 1e3 << ","
          << r.max_time_s * 1e3 << ","
          << r.stddev_s * 1e3 << ","
          << r.bandwidth_gibs << ","
          << r.mlups_min << ","
          << r.mlups_avg << "\n";
    }
    std::cout << "[CSV] Resultados guardados en: " << filename << "\n";
}

// Vuelca las repeticiones individuales. Es la MUESTRA con la que se hacen las
// pruebas de Welch/ANOVA y los boxplots: el CSV agregado solo guarda min/avg/max/
// stddev, que no permiten ningún contraste estadístico. Coste de ejecución nulo.
static void export_times_csv(const std::string& filename,
                             const std::vector<double>& times_s) {
    std::ofstream f(filename);
    if (!f.is_open()) {
        std::cerr << "[CSV] No se pudo crear: " << filename << "\n";
        return;
    }
    f << "rep,time_ms\n";
    f << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < times_s.size(); ++i)
        f << i << "," << times_s[i] * 1e3 << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " N [threads] [reps] [csv_prefix]\n";
        return 1;
    }

    const int n = std::atoi(argv[1]);
    if (n < 3) {
        std::cerr << "ERROR: N must be >= 3\n";
        return 1;
    }

    int threads = 0;
#ifdef _OPENMP
    threads = omp_get_max_threads();
#endif
    int reps = REPS;
    std::string csv_prefix;
    if (argc >= 3) threads = std::atoi(argv[2]);
    if (argc >= 4) reps = std::atoi(argv[3]);
    if (argc >= 5) csv_prefix = argv[4];
    if (threads <= 0 || reps <= 0) {
        std::cerr << "ERROR: threads y reps deben ser > 0\n";
        return 1;
    }

    if (!kPrint) {
        std::freopen("/dev/null", "w", stdout);
    }
#ifdef _OPENMP
    omp_set_num_threads(threads);
#endif

    float* A = static_cast<float*>(std::malloc(static_cast<size_t>(n) * n * sizeof(float)));
    float* result = static_cast<float*>(std::malloc(static_cast<size_t>(n) * n * sizeof(float)));
    if (!A || !result) {
        std::cerr << "ERROR: malloc failed\n";
        return 1;
    }

    // Info OpenMP (una sola vez; no contaminar el benchmark)
    // [COMMENTED: info logging disabled for clean benchmark]
    // #pragma omp parallel
    // {
    // #pragma omp single
    //     {
    //         std::cout << "[OMP] Max threads available: " << omp_get_max_threads() << "\n";
    //         std::cout << "[OMP] Number of threads (actual): " << omp_get_num_threads() << "\n";
    //     }
    // }

    // Contadores apagados durante inicialización y warm-up: antes el
    // init_matrix_seq (secuencial, sobre 4.4 GB) quedaba DENTRO del conteo
    // porque el disable venía después.
    perf_events_disable_all_threads();

    init_matrix_seq(A, n);
    std::memset(result, 0, static_cast<size_t>(n) * n * sizeof(float));

    // Warm-up
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        stencil2D_omp_static(A, result, n);
    }
    perf_events_enable_all_threads();

    // Timed repetitions
    std::vector<double> times;
    times.reserve(reps);

    ompt_measure_start();
    for (int r = 0; r < reps; ++r) {
        const double t0 = now_sec();
        stencil2D_omp_static(A, result, n);
        const double t1 = now_sec();
        times.push_back(t1 - t0);
    }
    ompt_measure_pause();
    // Fuera del conteo el checksum y el volcado de CSV.
    perf_events_disable_all_threads();

    // checksum
    double checksum = 0.0;
    for (int j = 1; j < n - 1; ++j) {
        checksum += result[j * n + (j % (n - 2) + 1)];
    }

    // OJO: compute_stats ORDENA el vector in situ, así que los tiempos hay que
    // volcarlos ANTES o la columna 'rep' dejaría de corresponder al orden real
    // de ejecución (y con ello se perdería poder analizar la deriva temporal).
    if (!csv_prefix.empty()) {
        export_times_csv(csv_prefix + "_times.csv", times);
    }

    const Stats st = compute_stats(times);
    const double bw_gibs   = compute_bandwidth_gibs(n, st.min_s);
    const double mlups_min = compute_mlups(n, st.min_s);
    const double mlups_avg = compute_mlups(n, st.avg_s);

    std::cout << "[Stencil2D] Variant: OpenMP (static, por filas) + SeqInit\n";
    std::cout << "N=" << n << " warmup=" << WARMUP_ITERS << " reps=" << reps
              << " threads=" << threads << "\n";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Time: "
              << (st.min_s * 1e3) << " ms (min), "
              << (st.avg_s * 1e3) << " ms (avg), "
              << (st.max_s * 1e3) << " ms (max), stddev="
              << (st.stddev_s * 1e3) << " ms\n";
    std::cout << "MLUPS   : " << mlups_min << " (min) | " << mlups_avg << " (avg)\n";
    std::cout << "BW GiB/s: " << bw_gibs << "\n";
    std::cout << "Checksum: " << checksum << "\n";

    std::vector<BenchmarkResult> results;
    BenchmarkResult res;
    res.strategy = "OpenMP-SeqInit";
    res.min_time_s = st.min_s;
    res.avg_time_s = st.avg_s;
    res.max_time_s = st.max_s;
    res.stddev_s = st.stddev_s;
    res.bandwidth_gibs = bw_gibs;
    res.mlups_min = mlups_min;
    res.mlups_avg = mlups_avg;
    results.push_back(res);

    if (!csv_prefix.empty()) {
        export_csv(csv_prefix + ".csv", n, threads, results);
    }

    std::free(A);
    std::free(result);
    return 0;
}
