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

static inline double now_sec() {
    using clock = std::chrono::high_resolution_clock;
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

static void stencil2D_omp_static(const float* A, float* result, int n) {
#pragma omp parallel for collapse(2) schedule(static)
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
    double gflops{0};
    double bandwidth_gibs{0};
    double bandwidth_gbs{0};
    double mlups{0};
};

static Stats compute_stats(std::vector<double>& times) {
    std::sort(times.begin(), times.end());
    const double min_t = times.front();
    const double max_t = times.back();
    const double avg_t = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    double var = 0.0;
    for (double t : times) var += (t - avg_t) * (t - avg_t);
    const double stddev = std::sqrt(var / times.size());
    return Stats{min_t, avg_t, max_t, stddev};
}

static double compute_gflops(int n, double elapsed_s) {
    const double points = static_cast<double>(n - 2) * (n - 2);
    const double flops = 5.0 * points;
    return flops / (elapsed_s * 1e9);
}

static double compute_bandwidth_gibs(int n, double elapsed_s) {
    const double points = static_cast<double>(n - 2) * (n - 2);
    const double bytes = points * 6.0 * sizeof(float);
    return (bytes / (1024.0 * 1024.0 * 1024.0)) / elapsed_s;
}

static double compute_bandwidth_gbs(int n, double elapsed_s) {
    const double points = static_cast<double>(n - 2) * (n - 2);
    const double bytes = points * 6.0 * sizeof(float);
    return (bytes / 1e9) / elapsed_s;
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

    f << "strategy,n,threads,min_ms,avg_ms,max_ms,stddev_ms,gflops,bw_gibs,bw_gbs,mlups\n";
    for (const auto& r : results) {
        f << r.strategy << ","
          << n << ","
          << threads << ","
          << std::fixed << std::setprecision(6)
          << r.min_time_s * 1e3 << ","
          << r.avg_time_s * 1e3 << ","
          << r.max_time_s * 1e3 << ","
          << r.stddev_s * 1e3 << ","
          << r.gflops << ","
          << r.bandwidth_gibs << ","
          << r.bandwidth_gbs << ","
          << r.mlups << "\n";
    }
    std::cout << "[CSV] Resultados guardados en: " << filename << "\n";
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

    init_matrix_seq(A, n);
    std::memset(result, 0, static_cast<size_t>(n) * n * sizeof(float));

    // Warm-up
    perf_events_disable_all_threads();
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

    // checksum
    double checksum = 0.0;
    for (int j = 1; j < n - 1; ++j) {
        checksum += result[j * n + (j % (n - 2) + 1)];
    }

    const Stats st = compute_stats(times);
    const double gflops = compute_gflops(n, st.min_s);
    const double bw_gibs = compute_bandwidth_gibs(n, st.min_s);
    const double bw_gbs = compute_bandwidth_gbs(n, st.min_s);
    const double mlups = compute_mlups(n, st.min_s);

    std::cout << "[Stencil2D] Variant: OpenMP (static) + SeqInit\n";
    std::cout << "N=" << n << " warmup=" << WARMUP_ITERS << " reps=" << reps
              << " threads=" << threads << "\n";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Time: "
              << (st.min_s * 1e3) << " ms (min), "
              << (st.avg_s * 1e3) << " ms (avg), "
              << (st.max_s * 1e3) << " ms (max), stddev="
              << (st.stddev_s * 1e3) << " ms\n";
    std::cout << "GFlops  : " << gflops << "\n";
    std::cout << "MLUPS   : " << mlups << "\n";
    std::cout << "BW GiB/s: " << bw_gibs << "  |  BW GB/s: " << bw_gbs << "\n";
    std::cout << "Checksum: " << checksum << "\n";

    std::vector<BenchmarkResult> results;
    BenchmarkResult res;
    res.strategy = "OpenMP-SeqInit";
    res.min_time_s = st.min_s;
    res.avg_time_s = st.avg_s;
    res.max_time_s = st.max_s;
    res.stddev_s = st.stddev_s;
    res.gflops = gflops;
    res.bandwidth_gibs = bw_gibs;
    res.bandwidth_gbs = bw_gbs;
    res.mlups = mlups;
    results.push_back(res);

    if (!csv_prefix.empty()) {
        export_csv(csv_prefix + ".csv", n, threads, results);
    }

    std::free(A);
    std::free(result);
    return 0;
}
