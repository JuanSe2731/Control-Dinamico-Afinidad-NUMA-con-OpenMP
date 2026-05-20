#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
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

static constexpr int WARMUP_ITERS = 2;
static constexpr int REPS = 30;

static inline double now_sec() {
    using clock = std::chrono::high_resolution_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

static void init_matrix_seq(float* A, int n) {
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            A[i * n + j] = static_cast<float>(i * n + j);
        }
    }
}

static void stencil2D_serial(const float* A, float* result, int n) {
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " N\n";
        return 1;
    }

    const int n = std::atoi(argv[1]);
    if (n < 3) {
        std::cerr << "ERROR: N must be >= 3\n";
        return 1;
    }

    float* A = static_cast<float*>(std::malloc(static_cast<size_t>(n) * n * sizeof(float)));
    float* result = static_cast<float*>(std::malloc(static_cast<size_t>(n) * n * sizeof(float)));
    if (!A || !result) {
        std::cerr << "ERROR: malloc failed\n";
        return 1;
    }

    init_matrix_seq(A, n);
    std::memset(result, 0, static_cast<size_t>(n) * n * sizeof(float));

    // Warm-up
    perf_events_disable_this_thread();
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        stencil2D_serial(A, result, n);
    }
    perf_events_enable_this_thread();

    // Timed repetitions
    std::vector<double> times;
    times.reserve(REPS);

    ompt_measure_start();
    for (int r = 0; r < REPS; ++r) {
        const double t0 = now_sec();
        stencil2D_serial(A, result, n);
        const double t1 = now_sec();
        times.push_back(t1 - t0);
    }
    ompt_measure_pause();

    // Simple checksum to keep results live
    double checksum = 0.0;
    for (int j = 1; j < n - 1; ++j) {
        checksum += result[j * n + (j % (n - 2) + 1)];
    }

    // [COMMENTED: stats logging disabled for clean benchmark]
    // const Stats st = compute_stats(times);
    //
    // std::cout << "[Stencil2D] Variant: Serial\n";
    // std::cout << "N=" << n << " warmup=" << WARMUP_ITERS << " reps=" << REPS << "\n";
    // std::cout << std::fixed << std::setprecision(6);
    // std::cout << "Time: "
    //           << (st.min_s * 1e3) << " ms (min), "
    //           << (st.avg_s * 1e3) << " ms (avg), "
    //           << (st.max_s * 1e3) << " ms (max), stddev="
    //           << (st.stddev_s * 1e3) << " ms\n";
    // std::cout << "Checksum: " << checksum << "\n";

    std::free(A);
    std::free(result);
    return 0;
}
