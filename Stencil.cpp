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

// Auto-instrumentación acotada a la región medida. Sustituye al `perf stat`
// externo, que contaba también la inicialización y hacía inutilizables el IPC y el
// ratio_rm de las configuraciones sin tool OMPT. Se desactiva sola si el tool está
// cargado, para no medir lo mismo dos veces.
#include "perf_region.hpp"

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

// Warm-up subido de 2 a 5: con dos iteraciones el primer punto medido todavía
// arrastraba efectos de página fría y de escalado de frecuencia. Configurable por
// entorno para que quede en el registro experimental qué valor se usó.
static constexpr int WARMUP_ITERS_DEFECTO = 5;
static constexpr int REPS = 30;
static constexpr bool kPrint = false;

static int warmup_iters() {
    if (const char* e = std::getenv("WARMUP_ITERS")) {
        int v = std::atoi(e);
        if (v >= 0) return v;
    }
    return WARMUP_ITERS_DEFECTO;
}

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

// TODAS las métricas derivadas se calculan sobre el tiempo MEDIO, no sobre el
// mínimo. El mínimo es la repetición más afortunada de 150: favorece a cualquier
// configuración, y favorece MÁS a las de mayor varianza, que son justamente las
// que no fijan afinidad (base, obs, scheduler). Es decir, la convención anterior
// sesgaba a favor de la propuesta de este trabajo. min_ms y max_ms se siguen
// publicando, pero como descriptores de dispersión, no como base de nada.
struct BenchmarkResult {
    const char* strategy{nullptr};
    double min_time_s{0};
    double avg_time_s{0};
    double max_time_s{0};
    double stddev_s{0};
    double mlups{0};             // derivado de avg_time_s
    double gflops{0};            // derivado de avg_time_s
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

// caudal_util_gibs SE RETIRA (campana V6). Era bytes_del_modelo / tiempo con los
// bytes FIJADOS por N, o sea 1/t reescalado: para un tamano dado el producto
// caudal_util_gibs * avg_ms era una constante exacta, asi que la columna no
// aportaba ni un bit sobre avg_ms y ademas se confundia sistematicamente con el
// trafico real de memoria. El trafico real —los bytes que de verdad cruzan el
// enlace inter-nodo, donde MENOS ES MEJOR— se mide con los contadores de rellenos
// por origen (perf_region.hpp) y esa es la columna que hay que mirar.

// MLUPS: millones de actualizaciones de malla por segundo = puntos_interiores / (t * 1e6).
// Metrica de rendimiento propia del stencil (independiente del conteo de FLOPs).
static double compute_mlups(int n, double elapsed_s) {
    const double points = static_cast<double>(n - 2) * (n - 2);
    return points / (elapsed_s * 1e6);
}

// GFLOPS. El cuerpo del kernel es
//     result[j][k] = 0.2f * (A[j][k] + A[j-1][k] + A[j+1][k] + A[j][k-1] + A[j][k+1])
// es decir 4 sumas + 1 multiplicación = 5 flop por punto.
//
// Se declara sin rodeos: para el stencil GFLOPS = MLUPS / 200 EXACTAMENTE, así que
// no es evidencia independiente de MLUPS. Se reinstaura porque con el barrido de
// tamaños es la única unidad que permite poner los seis peldaños y los dos kernels
// en el mismo eje y contrastarlos contra el techo de la máquina.
static constexpr double FLOP_POR_PUNTO = 5.0;

static double compute_gflops(int n, double elapsed_s) {
    const double points = static_cast<double>(n - 2) * (n - 2);
    return (points * FLOP_POR_PUNTO) / (elapsed_s * 1e9);
}

static void export_csv(const std::string& filename,
                       int n,
                       int threads,
                       int warmup,
                       int reps,
                       const std::vector<BenchmarkResult>& results) {
    std::ofstream f(filename);
    if (!f.is_open()) {
        std::cerr << "[CSV] No se pudo crear: " << filename << "\n";
        return;
    }

    // ws_bytes: los dos arreglos float N*N. Es lo que sitúa cada ejecución en el
    // peldaño de la escalera de tamaños (L1 / L2 / L3-CCD / L3-nodo / DRAM).
    const double ws_bytes = 2.0 * static_cast<double>(n) * n * sizeof(float);

    f << "strategy,n,ws_bytes,threads,warmup,reps,"
         "min_ms,avg_ms,max_ms,stddev_ms,mlups,gflops\n";
    for (const auto& r : results) {
        f << r.strategy << ","
          << n << ","
          << std::fixed << std::setprecision(0) << ws_bytes << ","
          << threads << ","
          << warmup << ","
          << reps << ","
          << std::setprecision(6)
          << r.min_time_s * 1e3 << ","
          << r.avg_time_s * 1e3 << ","
          << r.max_time_s * 1e3 << ","
          << r.stddev_s * 1e3 << ","
          << r.mlups << ","
          << r.gflops << "\n";
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

    // Los contadores propios se abren aquí, antes del warm-up, para que el coste de
    // abrirlos no caiga dentro de la región medida. Queda inerte si el tool OMPT
    // está cargado: en ese caso el propietario de los contadores es él.
    perf_region::init();

    // Warm-up
    const int warmup = warmup_iters();
    for (int i = 0; i < warmup; ++i) {
        stencil2D_omp_static(A, result, n);
    }
    perf_events_enable_all_threads();

    // Timed repetitions
    std::vector<double> times;
    times.reserve(reps);

    ompt_measure_start();
    perf_region::begin();
    for (int r = 0; r < reps; ++r) {
        const double t0 = now_sec();
        stencil2D_omp_static(A, result, n);
        const double t1 = now_sec();
        times.push_back(t1 - t0);
    }
    perf_region::end();
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
    // Todo sobre la MEDIA, no sobre el mínimo (ver el comentario de BenchmarkResult).
    const double mlups  = compute_mlups(n, st.avg_s);
    const double gflops = compute_gflops(n, st.avg_s);

    std::cout << "[Stencil2D] Variant: OpenMP (static, por filas) + SeqInit\n";
    std::cout << "N=" << n << " warmup=" << warmup << " reps=" << reps
              << " threads=" << threads << "\n";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Time: "
              << (st.min_s * 1e3) << " ms (min), "
              << (st.avg_s * 1e3) << " ms (avg), "
              << (st.max_s * 1e3) << " ms (max), stddev="
              << (st.stddev_s * 1e3) << " ms\n";
    std::cout << "MLUPS        : " << mlups << "  (sobre la media)\n";
    std::cout << "GFLOPS       : " << gflops << "  (= MLUPS/200, exacto)\n";
    std::cout << "Checksum: " << checksum << "\n";

    std::vector<BenchmarkResult> results;
    BenchmarkResult res;
    res.strategy = "OpenMP-SeqInit";
    res.min_time_s = st.min_s;
    res.avg_time_s = st.avg_s;
    res.max_time_s = st.max_s;
    res.stddev_s = st.stddev_s;
    res.mlups = mlups;
    res.gflops = gflops;
    results.push_back(res);

    if (!csv_prefix.empty()) {
        export_csv(csv_prefix + ".csv", n, threads, warmup, reps, results);
        // Contadores por hilo, acotados a la región medida. Vacío si el tool OMPT
        // era el propietario: entonces los datos salen de ompt_summary.csv.
        perf_region::escribir_csv((csv_prefix + "_counters.csv").c_str(),
                                  csv_prefix.c_str());
    }
    perf_region::shutdown();

    std::free(A);
    std::free(result);
    return 0;
}
