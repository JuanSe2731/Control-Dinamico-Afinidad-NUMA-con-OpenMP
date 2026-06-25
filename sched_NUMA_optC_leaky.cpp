// =============================================================================
// sched_NUMA_optC_leaky.cpp   (OPCIÓN C)
//
// Scheduler NUMA adaptativo via OMPT + perf_event_open + hwloc
//
//   MECANISMO DE DISPARO: integrador con fugas + histéresis sobre una
//                         REFERENCIA GLOBAL adaptativa
//   --------------------------------------------------------------------------
//   - Se evalúa CADA ventana de 100 ms.
//   - g_ref = EWMA del ratio_rm de TODOS los hilos => referencia "típica" de la
//     corrida (autoajustable; sin la constante mágica 0.11).
//   - g_dev = EWMA de la desviación absoluta media (MAD) => dispersión robusta
//     que escala el umbral automáticamente.
//   - Umbral alto adaptativo:  T = g_ref + LEAK_MARGIN_SIG * dev.
//   - "Balde con fugas" por hilo:
//        si ratio > T  -> bucket += (ratio - T)      (se llena por exceso)
//        si ratio <= T -> bucket  -= LEAK_RATE*dev   (se vacía lentamente)
//     La histéresis (llenar rápido / vaciar lento + capacidad) evita disparos
//     por picos aislados y exige elevación SOSTENIDA.
//   - Cuando el balde supera su capacidad CAP => se marca para migrar UNA vez.
//
//   Migración: hwloc_set_thread_cpubind() — NUMA-aware, overhead mínimo.
//   Requisito: cada hilo migra COMO MÁXIMO 1 vez (MAX_MIGRATIONS = 1).
//   Compatible con numactl --interleave=all (no se cambia la política de páginas).
//
// Compilar:
//   clang++ -std=c++17 -fPIC -shared -fopenmp -pthread -O2 \
//           sched_NUMA_optC_leaky.cpp -o numa_sched_optC.so \
//           -I<path/to/hwloc/include> -L<path/to/hwloc/lib> -lhwloc
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <omp-tools.h>
#include <omp.h>

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <hwloc.h>

// =============================================================================
// Configuración del scheduler
// =============================================================================
static constexpr int      MAX_THREADS         = 1024;
static constexpr int      MONITOR_MS          = 100;
static constexpr int      MAX_NUMA_NODES      = 2;

// ── Mecanismo C: integrador con fugas + histéresis (ref. global) ─────────────
static constexpr uint64_t MIN_FILLS         = 10000; // gating anti-ruido (d_all)
static constexpr int      WARMUP_WINDOWS    = 5;      // ventanas antes de poder migrar
static constexpr double   REF_BETA          = 0.05;   // suavizado de la referencia global (lento)
static constexpr double   DEV_FLOOR         = 0.005;  // piso de dispersión (evita disparos por ruido)
static constexpr double   LEAK_MARGIN_SIG   = 1.0;    // umbral T = g_ref + LEAK_MARGIN_SIG*dev
static constexpr double   LEAK_RATE_SIG     = 0.5;    // fuga por ventana (en MAD) cuando ratio<=T
static constexpr double   LEAK_CAP_SIG      = 8.0;    // capacidad del balde (en MAD) -> dispara

// Una sola migración por hilo (requisito).
static constexpr uint64_t MAX_MIGRATIONS    = 1;

// Cooldown (heredado; con migración única es básicamente informativo).
static constexpr int      COOLDOWN_WINDOWS  = 10;

// =============================================================================
// Salida a archivos
// =============================================================================
static FILE*       g_window_csv  = nullptr;
static FILE*       g_summary_csv = nullptr;
// char[] en lugar de std::string: los atexit del runtime OpenMP pueden llamar a
// ompt_finalize DESPUÉS de que los destructores de estáticos de C++ hayan corrido,
// lo que deja std::string en estado inválido y corrompe el campo tag en el CSV.
static char        g_tag[1024]   = {};

// =============================================================================
// hwloc — topología global
// =============================================================================
static hwloc_topology_t g_topology;
static bool             g_topology_valid = false;
static int              g_num_numa_nodes = 0;
static hwloc_cpuset_t   g_node_cpusets[MAX_NUMA_NODES];

// =============================================================================
// Referencia global adaptativa (compartida por todos los hilos)
//   - Protegida por g_lock (se actualiza dentro del monitor_loop, ya con lock).
// =============================================================================
static double g_ref      = 0.0;   // EWMA del ratio_rm entre hilos
static double g_dev      = 0.0;   // EWMA de |ratio - g_ref| (MAD)
static bool   g_ref_init = false;

// =============================================================================
// Tabla de nombres de tipo de hilo (patrón LLVM callback.h)
// =============================================================================
static const char* thread_type_name[] = {
    "ompt_thread_unknown",
    "ompt_thread_initial",
    "ompt_thread_worker",
    "ompt_thread_other"
};
static const char* thread_type_str(ompt_thread_t t) {
    if (t >= 1 && t <= 3) return thread_type_name[t];
    return thread_type_name[0];
}

// =============================================================================
// Macro de registro de callbacks (patrón LLVM)
// =============================================================================
static ompt_set_callback_t g_ompt_set_callback = nullptr;

#define register_callback_t(name, type)                                    \
  do {                                                                      \
    type f_##name = &on_##name;                                             \
    ompt_set_result_t r =                                                   \
        g_ompt_set_callback(name, (ompt_callback_t)f_##name);              \
    if (r == ompt_set_never)                                                \
      fprintf(stderr, "[OMPT] WARNING: '%s' no soportado\n", #name);       \
    else                                                                    \
      fprintf(stderr, "[OMPT] callback '%s' registrado (result=%d)\n",     \
              #name, (int)r);                                               \
  } while (0)

#define register_callback(name) register_callback_t(name, name##_t)

// =============================================================================
// Function pointers del runtime OMPT
// =============================================================================
static ompt_get_thread_data_t   g_ompt_get_thread_data   = nullptr;
static ompt_get_unique_id_t     g_ompt_get_unique_id      = nullptr;
static ompt_get_proc_id_t       g_ompt_get_proc_id        = nullptr;
static ompt_get_num_procs_t     g_ompt_get_num_procs      = nullptr;
static ompt_get_parallel_info_t g_ompt_get_parallel_info  = nullptr;

// =============================================================================
// Estructuras de datos por hilo
// =============================================================================
struct PerfFDs {
    int rm_misses    {-1};   // ANY_DATA_CACHE_FILLS_REMOTE_ALL (raw 0xD044)
    int instr_fd     {-1};   // PERF_COUNT_HW_INSTRUCTIONS
    int cycles_fd    {-1};   // PERF_COUNT_HW_CPU_CYCLES
    int all_fills_fd {-1};   // ALL DATA CACHE FILLS (raw 0xFF44)
    int opened       {0};
};

struct ThreadInfo {
    // ── Identificación ──────────────────────────────────────────────────────
    pid_t     tid_linux       {0};
    uint64_t  ompt_id         {0};
    int       ompt_type       {0};
    int       last_cpu        {-1};
    int       numa_node       {-1};
    int       seen            {0};
    int       measuring       {0};
    int       needs_migration {0};

    // ── Estado del detector (Mecanismo C: integrador con fugas) ──────────
    int       win_count       {0};     // ventanas válidas vistas (warmup)
    double    bucket          {0.0};    // nivel del balde con fugas

    // ── Handle pthread — necesario para hwloc_set_thread_cpubind ─────────
    pthread_t pthread_handle {0};

    // ── Contadores hardware ──────────────────────────────────────────────
    PerfFDs   perf           {};

    // ── Últimas lecturas (para deltas) ────────────────────────────────────
    uint64_t  last_rm_misses {0};
    uint64_t  last_instr     {0};
    uint64_t  last_cycles    {0};
    uint64_t  last_all_fills {0};
    int       has_last       {0};

    // ── Estado del scheduler ─────────────────────────────────────────────
    int       cooldown       {0};   // ventanas restantes (informativo)
    uint64_t  migrations     {0};   // total de migraciones realizadas
};

static ThreadInfo g_threads[MAX_THREADS];
static int        g_count = 0;

// Spinlock liviano — evita malloc en callbacks
static std::atomic<bool> g_measuring {false};
static std::atomic_flag  g_lock = ATOMIC_FLAG_INIT;
static inline void lock()   { while (g_lock.test_and_set(std::memory_order_acquire)); }
static inline void unlock() { g_lock.clear(std::memory_order_release); }

// =============================================================================
// Helpers Linux + hwloc
// =============================================================================
static inline pid_t linux_tid() {
    return (pid_t)syscall(SYS_gettid);
}

static long perf_event_open_syscall(struct perf_event_attr* attr,
                                     pid_t pid, int cpu,
                                     int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int cpu_to_numa_node_hwloc(int cpu) {
    if (!g_topology_valid || cpu < 0) return -1;
    hwloc_obj_t pu = hwloc_get_pu_obj_by_os_index(g_topology, (unsigned)cpu);
    if (!pu || !pu->cpuset) return -1;
    for (int n = 0; n < g_num_numa_nodes; ++n) {
        if (g_node_cpusets[n] &&
            hwloc_bitmap_isincluded(pu->cpuset, g_node_cpusets[n]))
            return n;
    }
    return -1;
}

// =============================================================================
// Migración de hilo al nodo NUMA contrario via hwloc
// =============================================================================
static void migrate_to_opposite_node(ThreadInfo& t) {
    if (!g_topology_valid)              return;
    if (t.pthread_handle == 0)          return;
    if (t.numa_node < 0)               return;
    if (t.migrations >= MAX_MIGRATIONS) return;   // ← máximo 1 migración

    int target = (t.numa_node == 0) ? 1 : 0;
    if (target >= g_num_numa_nodes) return;

    // HWLOC_CPUBIND_THREAD → solo este hilo, no el proceso
    int rc = hwloc_set_thread_cpubind(
        g_topology,
        t.pthread_handle,
        g_node_cpusets[target],
        HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT
    );

    // Fallback sin STRICT si el kernel no lo soporta
    if (rc != 0)
        rc = hwloc_set_thread_cpubind(
            g_topology,
            t.pthread_handle,
            g_node_cpusets[target],
            HWLOC_CPUBIND_THREAD
        );

    if (rc == 0) {
        int prev_node = t.numa_node;
        t.numa_node = target;
        t.cooldown  = COOLDOWN_WINDOWS;
        t.migrations++;
        t.has_last  = 0;  // resetear baseline de deltas post-migración
        t.bucket    = 0.0;

        fprintf(stderr,
            "[SCHED] MIGRACIÓN tid=%-6d ompt_id=%-4" PRIu64
            "  nodo %d → nodo %d  (total migraciones: %" PRIu64 ")\n",
            (int)t.tid_linux, t.ompt_id,
            prev_node, target, t.migrations);
    } else {
        fprintf(stderr,
            "[SCHED] FALLO migración tid=%d (rc=%d)\n",
            (int)t.tid_linux, rc);
    }
}

// =============================================================================
// perf_event helpers
// =============================================================================
static int open_counter(pid_t tid, uint32_t type, uint64_t config) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type           = type;
    pe.size           = sizeof(pe);
    pe.config         = config;
    pe.disabled       = 1;
    pe.inherit        = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    return (int)perf_event_open_syscall(&pe, tid, -1, -1, 0);
}

static bool read_counter(int fd, uint64_t& val) {
    val = 0;
    if (fd < 0) return false;
    return read(fd, &val, sizeof(val)) == (ssize_t)sizeof(val);
}

static void close_perf(PerfFDs& p) {
    if (p.rm_misses    >= 0) { close(p.rm_misses);    p.rm_misses    = -1; }
    if (p.instr_fd     >= 0) { close(p.instr_fd);     p.instr_fd     = -1; }
    if (p.cycles_fd >= 0) { close(p.cycles_fd); p.cycles_fd = -1; }
    if (p.all_fills_fd >= 0) { close(p.all_fills_fd); p.all_fills_fd = -1; }
    p.opened = 0;
}

static void open_csv_file(const char* path, const char* header, FILE** out) {
    if (!path || !*path) return;
    struct stat st {};
    const bool write_header = (stat(path, &st) != 0 || st.st_size == 0);
    FILE* f = fopen(path, "a");
    if (!f) return;
    if (write_header && header) {
        fprintf(f, "%s\n", header);
        fflush(f);
    }
    *out = f;
}

static void enable_perf(PerfFDs& p) {
    if (!p.opened) return;
    if (p.rm_misses    >= 0) { ioctl(p.rm_misses,    PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.rm_misses,    PERF_EVENT_IOC_ENABLE, 0); }
    if (p.instr_fd     >= 0) { ioctl(p.instr_fd,     PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.instr_fd,     PERF_EVENT_IOC_ENABLE, 0); }
    if (p.cycles_fd >= 0) { ioctl(p.cycles_fd, PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.cycles_fd, PERF_EVENT_IOC_ENABLE, 0); }
    if (p.all_fills_fd >= 0) { ioctl(p.all_fills_fd, PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.all_fills_fd, PERF_EVENT_IOC_ENABLE, 0); }
}

static void disable_perf(PerfFDs& p) {
    if (!p.opened) return;
    if (p.rm_misses    >= 0) ioctl(p.rm_misses,    PERF_EVENT_IOC_DISABLE, 0);
    if (p.instr_fd     >= 0) ioctl(p.instr_fd,     PERF_EVENT_IOC_DISABLE, 0);
    if (p.cycles_fd >= 0) ioctl(p.cycles_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (p.all_fills_fd >= 0) ioctl(p.all_fills_fd, PERF_EVENT_IOC_DISABLE, 0);
}

static void open_perf_for_thread(ThreadInfo& t) {
    if (t.perf.opened) return;

    // remote fills AMD EPYC: event=0x44 umask=0xD0 → raw 0xD044
    t.perf.rm_misses    = open_counter(t.tid_linux, PERF_TYPE_RAW, 0xD044);
    t.perf.instr_fd     = open_counter(t.tid_linux, PERF_TYPE_HARDWARE,
                                        PERF_COUNT_HW_INSTRUCTIONS);
    t.perf.cycles_fd = open_counter(t.tid_linux, PERF_TYPE_HARDWARE,
                                        PERF_COUNT_HW_CPU_CYCLES);
    t.perf.all_fills_fd = open_counter(t.tid_linux, PERF_TYPE_RAW, 0xFF44);

    if (t.perf.rm_misses < 0 || t.perf.instr_fd < 0 || t.perf.cycles_fd < 0 || t.perf.all_fills_fd < 0) {
        fprintf(stderr,
            "[OMPT][PERF] FALLO abriendo contadores tid=%d "
            "(rm=%d ins=%d miss=%d)\n"
            "  → cat /proc/sys/kernel/perf_event_paranoid debe ser <= 1\n",
            (int)t.tid_linux,
            t.perf.rm_misses, t.perf.instr_fd, t.perf.cycles_fd);
        close_perf(t.perf);
        return;
    }

    t.perf.opened = 1;
    t.has_last    = 0;

    if (g_measuring.load(std::memory_order_acquire)) {
        t.measuring = 1;
        enable_perf(t.perf);
        fprintf(stderr, "[OMPT][PERF] Contadores ACTIVOS tid=%d\n",
                (int)t.tid_linux);
    } else {
        fprintf(stderr, "[OMPT][PERF] Contadores pausados (esperando start) tid=%d\n",
                (int)t.tid_linux);
    }
}

// =============================================================================
// upsert_thread — guarda también pthread_handle
// =============================================================================
static int upsert_thread(pid_t tid, int cpu,
                          uint64_t ompt_id, int ompt_type,
                          pthread_t pt) {
    for (int i = 0; i < g_count; ++i) {
        if (g_threads[i].seen && g_threads[i].tid_linux == tid) {
            g_threads[i].last_cpu       = cpu;
            g_threads[i].numa_node      = cpu_to_numa_node_hwloc(cpu);
            g_threads[i].ompt_id        = ompt_id;
            g_threads[i].pthread_handle = pt;
            return i;
        }
    }
    if (g_count >= MAX_THREADS) {
        fprintf(stderr, "[OMPT] WARNING: MAX_THREADS alcanzado\n");
        return -1;
    }
    ThreadInfo& t    = g_threads[g_count];
    t                = ThreadInfo{};
    t.tid_linux      = tid;
    t.last_cpu       = cpu;
    t.numa_node      = cpu_to_numa_node_hwloc(cpu);
    t.ompt_id        = ompt_id;
    t.ompt_type      = ompt_type;
    t.pthread_handle = pt;
    t.seen           = 1;
    return g_count++;
}

// =============================================================================
// Hilo monitor — mide, decide y migra cada MONITOR_MS
// =============================================================================
static std::atomic<bool> g_monitor_running {false};
static std::atomic<bool> g_finalizing      {false};
static pthread_t         g_monitor_pthread;
static bool              g_monitor_valid = false;

static void* monitor_loop(void*) {
    fprintf(stderr,
        "[OMPT][MON] Monitor (C: integrador con fugas)  período=%dms"
        "  warmup=%d  T=ref+%.2f*MAD  leak=%.2f*MAD  cap=%.2f*MAD  max_mig=%" PRIu64 "\n",
        MONITOR_MS, WARMUP_WINDOWS,
        LEAK_MARGIN_SIG, LEAK_RATE_SIG, LEAK_CAP_SIG, MAX_MIGRATIONS);

    while (g_monitor_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(MONITOR_MS));

        lock();
        for (int i = 0; i < g_count; ++i) {
            ThreadInfo& t = g_threads[i];
            if (!t.seen || !t.perf.opened || !t.measuring) continue;

            // ── Leer contadores ──────────────────────────────────────────
            uint64_t remote = 0, ins = 0, cyc = 0, all = 0;
            bool ok = read_counter(t.perf.rm_misses,    remote)
                   && read_counter(t.perf.instr_fd,     ins)
                   && read_counter(t.perf.cycles_fd, cyc)
                   && read_counter(t.perf.all_fills_fd, all);
            if (!ok) continue;

            // ── Primera lectura: solo baseline ────────────────────────────
            if (!t.has_last) {
                t.last_rm_misses = remote;
                t.last_instr     = ins;
                t.last_cycles    = cyc;
                t.last_all_fills = all;
                t.has_last       = 1;
                continue;
            }

            // ── Calcular deltas ──────────────────────────────────────────
            uint64_t d_rm  = remote - t.last_rm_misses;
            uint64_t d_ins = ins    - t.last_instr;
            uint64_t d_cyc= cyc   - t.last_cycles;
            uint64_t d_all = all - t.last_all_fills;

            t.last_rm_misses = remote;
            t.last_instr     = ins;
            t.last_cycles    = cyc;
            t.last_all_fills = all;

            double ipc  = (d_ins  > 0) ? (double)d_ins  / (double)d_cyc  : 0.0;
            double ratio = (d_all > 0) ? (double)d_rm / (double)d_all : 0.0;

            if (g_window_csv) {
                fprintf(g_window_csv,
                    "%s,%d,%" PRIu64 ",%d,%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f\n",
                    g_tag,
                    MONITOR_MS,
                    t.ompt_id,
                    (int)t.tid_linux,
                    thread_type_str((ompt_thread_t)t.ompt_type),
                    t.last_cpu,
                    t.numa_node,
                    d_rm,
                    d_ins,
                    d_cyc,
                    ipc,
                    ratio);
                fflush(g_window_csv);
            }

            fprintf(stderr,
                "[OMPT][WIN%dms] ompt_id=%-4" PRIu64
                " tid=%-6d tipo=%-22s cpu=%3d numa=%d"
                "  d_rm=%8" PRIu64
                "  d_instr=%10" PRIu64
                "  d_cyc=%10" PRIu64
                "  d_all=%8" PRIu64
                "  IPC=%.3f  ratio_rm=%.4f\n",
                MONITOR_MS,
                t.ompt_id, (int)t.tid_linux,
                thread_type_str((ompt_thread_t)t.ompt_type),
                t.last_cpu, t.numa_node,
                d_rm, d_ins, d_cyc, d_all,
                ipc, ratio);

            // ── Mecanismo C: integrador con fugas + histéresis ────────────
            // Solo ventanas con actividad real (gating anti-ruido).
            if (d_all >= MIN_FILLS) {
                t.win_count++;

                if (!g_ref_init) {
                    g_ref = ratio; g_dev = 0.0; g_ref_init = true;
                }
                double dev = (g_dev > DEV_FLOOR) ? g_dev : DEV_FLOOR;

                double T    = g_ref + LEAK_MARGIN_SIG * dev;   // umbral alto adaptativo
                double leak = LEAK_RATE_SIG  * dev;            // fuga por ventana
                double cap  = LEAK_CAP_SIG   * dev;            // capacidad -> dispara

                if (ratio > T) {
                    t.bucket += (ratio - T);                   // llenar por exceso
                } else {
                    t.bucket -= leak;                          // fuga lenta
                    if (t.bucket < 0.0) t.bucket = 0.0;
                }

                if (t.win_count >= WARMUP_WINDOWS
                    && t.migrations < MAX_MIGRATIONS
                    && !t.needs_migration
                    && t.bucket > cap) {
                    t.needs_migration = 1;
                    fprintf(stderr,
                        "[SCHED][LEAKY] tid=%-6d ratio=%.4f T=%.4f"
                        " bucket=%.4f > cap=%.4f -> PENDIENTE (mig=%" PRIu64 ")\n",
                        (int)t.tid_linux, ratio, T,
                        t.bucket, cap, t.migrations);
                }

                // Actualizar la referencia global DESPUÉS de evaluar
                // (g_ref = EWMA del ratio; g_dev = EWMA de |desviación|).
                double e  = ratio - g_ref;
                g_ref    += REF_BETA * e;
                double ae = (e < 0.0) ? -e : e;
                g_dev    += REF_BETA * (ae - g_dev);
            }
        }
        unlock();
    }

    fprintf(stderr, "[OMPT][MON] Monitor detenido\n");
    return nullptr;
}

// =============================================================================
// Callbacks OMPT
// =============================================================================
static void on_ompt_callback_thread_begin(
    ompt_thread_t  thread_type,
    ompt_data_t*   thread_data)
{
    if (g_finalizing.load(std::memory_order_relaxed)) return;

    uint64_t  uid = g_ompt_get_unique_id ? g_ompt_get_unique_id() : 0;
    thread_data->value = uid;

    pid_t     tid = linux_tid();
    int       cpu = sched_getcpu();
    pthread_t pt  = pthread_self();   // ← necesario para hwloc_set_thread_cpubind

    lock();
    int idx = upsert_thread(tid, cpu, uid, (int)thread_type, pt);
    if (idx >= 0 && !g_finalizing.load(std::memory_order_relaxed))
        open_perf_for_thread(g_threads[idx]);
    unlock();

    fprintf(stderr,
        "[OMPT] thread_begin: tipo=%-22s ompt_id=%" PRIu64
        " tid=%d cpu=%d numa=%d\n",
        thread_type_str(thread_type), uid, (int)tid, cpu,
        cpu_to_numa_node_hwloc(cpu));
}

static void on_ompt_callback_thread_end(ompt_data_t* thread_data) {
    fprintf(stderr,
        "[OMPT] thread_end: ompt_id=%" PRIu64 " tid=%d cpu=%d\n",
        thread_data->value, (int)linux_tid(), sched_getcpu());
}

static int on_ompt_callback_control_tool(
    uint64_t command, uint64_t modifier,
    void* arg, const void* codeptr_ra)
{
    (void)modifier; (void)arg; (void)codeptr_ra;
    if (command == 1) {   // omp_control_tool_start
        fprintf(stderr, "[OMPT] control_tool: INICIANDO medicion\n");
        g_measuring.store(true, std::memory_order_release);
        lock();
        for (int i = 0; i < g_count; ++i) {
            if (g_threads[i].seen && g_threads[i].perf.opened) {
                g_threads[i].measuring = 1;
                g_threads[i].has_last  = 0;
                enable_perf(g_threads[i].perf);
            }
        }
        unlock();
    } else if (command == 2) {   // omp_control_tool_pause
        fprintf(stderr, "[OMPT] control_tool: PAUSANDO medicion\n");
        g_measuring.store(false, std::memory_order_release);
        lock();
        for (int i = 0; i < g_count; ++i) {
            if (g_threads[i].seen && g_threads[i].perf.opened) {
                g_threads[i].measuring = 0;
                disable_perf(g_threads[i].perf);
            }
        }
        unlock();
    }
    return 0;
}

static void on_ompt_callback_parallel_begin(
    ompt_data_t* encountering_task_data,
    const ompt_frame_t* encountering_task_frame,
    ompt_data_t* parallel_data,
    uint32_t requested_team_size,
    int flags, const void* codeptr_ra)
{
    (void)encountering_task_data; (void)encountering_task_frame;
    (void)flags; (void)codeptr_ra;
    if (g_ompt_get_unique_id)
        parallel_data->value = g_ompt_get_unique_id();
    fprintf(stderr,
        "[OMPT] parallel_begin: parallel_id=%" PRIu64 " team_size=%u\n",
        parallel_data->value, requested_team_size);
}

static void on_ompt_callback_parallel_end(
    ompt_data_t* parallel_data,
    ompt_data_t* encountering_task_data,
    int flags, const void* codeptr_ra)
{
    (void)encountering_task_data; (void)flags; (void)codeptr_ra;
    fprintf(stderr,
        "[OMPT] parallel_end: parallel_id=%" PRIu64 "\n",
        parallel_data->value);
}

static void on_ompt_callback_implicit_task(
    ompt_scope_endpoint_t endpoint,
    ompt_data_t*          parallel_data,
    ompt_data_t*          task_data,
    unsigned int          team_size,
    unsigned int          thread_num,
    int                   flags)
{
    (void)flags;
    if (g_finalizing.load(std::memory_order_acquire)) return;

    if (endpoint == ompt_scope_begin) {
        if (g_ompt_get_unique_id)
            task_data->value = g_ompt_get_unique_id();

        pid_t     tid = linux_tid();
        int       cpu = sched_getcpu();
        pthread_t pt  = pthread_self();

        int do_migrate = 0;
        int thread_idx = -1;

        // Activar contadores si ya llegó control_tool_start,
        // y actualizar pthread_handle (puede cambiar entre regiones)
        lock();
        for (int i = 0; i < g_count; ++i) {
            if (g_threads[i].seen && g_threads[i].tid_linux == tid) {
                g_threads[i].last_cpu       = cpu;
                g_threads[i].numa_node      = cpu_to_numa_node_hwloc(cpu);
                g_threads[i].pthread_handle = pt;  // ← actualizar handle

                if (g_threads[i].needs_migration) {
                    g_threads[i].needs_migration = 0;
                    do_migrate = 1;
                    thread_idx = i;
                }

                if (g_measuring.load(std::memory_order_acquire)
                    && g_threads[i].perf.opened
                    && !g_threads[i].measuring) {
                    g_threads[i].measuring = 1;
                    g_threads[i].has_last  = 0;
                    enable_perf(g_threads[i].perf);
                }
                break;
            }
        }
        unlock();

        if (do_migrate && thread_idx >= 0) {
            migrate_to_opposite_node(g_threads[thread_idx]);
        }

        fprintf(stderr,
            "[OMPT] implicit_task_begin: parallel_id=%" PRIu64
            " thread_num=%u team_size=%u\n",
            parallel_data ? parallel_data->value : 0,
            thread_num, team_size);
    } else {
        fprintf(stderr,
            "[OMPT] implicit_task_end: parallel_id=%" PRIu64
            " thread_num=%u\n",
            parallel_data ? parallel_data->value : 0, thread_num);
    }
}

// =============================================================================
// Initialize / Finalize
// =============================================================================
static int ompt_initialize(
    ompt_function_lookup_t lookup,
    int initial_device_num,
    ompt_data_t* tool_data)
{
    (void)initial_device_num; (void)tool_data;

    const char* log_path = getenv("OMPT_LOG_FILE");
    if (log_path && *log_path) {
        (void)freopen(log_path, "w", stderr);
        setvbuf(stderr, nullptr, _IOLBF, 0);
    } else {
        (void)freopen("/dev/null", "w", stderr);
    }

    const char* tag_env = getenv("OMPT_TAG");
    strncpy(g_tag, tag_env ? tag_env : "", sizeof(g_tag) - 1);
    g_tag[sizeof(g_tag) - 1] = '\0';

    open_csv_file(getenv("OMPT_WINDOW_CSV"),
                  "tag,window_ms,ompt_id,tid,thread_type,cpu,numa,d_rm,d_instr,d_cyc,ipc,ratio_rm",
                  &g_window_csv);
    open_csv_file(getenv("OMPT_SUMMARY_CSV"),
                  "tag,tid,ompt_id,thread_type,last_cpu,numa_node,migrations,remote_fills,instructions,cycles,ipc",
                  &g_summary_csv);

    fprintf(stderr, "[OMPT] ompt_initialize (OPCIÓN C: integrador con fugas)\n");

    // ── 1. Function pointers del runtime ────────────────────────────────
    g_ompt_set_callback     = (ompt_set_callback_t)     lookup("ompt_set_callback");
    g_ompt_get_thread_data  = (ompt_get_thread_data_t)  lookup("ompt_get_thread_data");
    g_ompt_get_unique_id    = (ompt_get_unique_id_t)    lookup("ompt_get_unique_id");
    g_ompt_get_proc_id      = (ompt_get_proc_id_t)      lookup("ompt_get_proc_id");
    g_ompt_get_num_procs    = (ompt_get_num_procs_t)    lookup("ompt_get_num_procs");
    g_ompt_get_parallel_info= (ompt_get_parallel_info_t)lookup("ompt_get_parallel_info");

    if (!g_ompt_set_callback) {
        fprintf(stderr, "[OMPT] ERROR: ompt_set_callback no encontrado\n");
        return 0;
    }

    // ── 2. Inicializar hwloc ─────────────────────────────────────────────
    if (hwloc_topology_init(&g_topology) == 0 &&
        hwloc_topology_load(g_topology) == 0) {

        g_topology_valid = true;
        int nb = hwloc_get_nbobjs_by_type(g_topology, HWLOC_OBJ_NUMANODE);
        g_num_numa_nodes = (nb < MAX_NUMA_NODES) ? nb : MAX_NUMA_NODES;

        for (int n = 0; n < g_num_numa_nodes; ++n) {
            g_node_cpusets[n] = hwloc_bitmap_alloc();
            hwloc_bitmap_zero(g_node_cpusets[n]);
        }

        hwloc_obj_t numa = nullptr;
        while ((numa = hwloc_get_next_obj_by_type(
                    g_topology, HWLOC_OBJ_NUMANODE, numa)) != nullptr) {
            int idx = (int)numa->logical_index;
            if (idx >= 0 && idx < g_num_numa_nodes && numa->cpuset)
                hwloc_bitmap_copy(g_node_cpusets[idx], numa->cpuset);
        }

        fprintf(stderr, "[HWLOC] Nodos NUMA detectados: %d\n", g_num_numa_nodes);
        for (int n = 0; n < g_num_numa_nodes; ++n)
            fprintf(stderr, "[HWLOC] Nodo %d: %d CPUs\n",
                    n, hwloc_bitmap_weight(g_node_cpusets[n]));
    } else {
        fprintf(stderr,
            "[HWLOC] WARNING: no se pudo inicializar hwloc. "
            "Migraciones deshabilitadas.\n");
        g_topology_valid = false;
        g_num_numa_nodes = 0;
    }

    // ── 3. Registrar callbacks ───────────────────────────────────────────
    register_callback(ompt_callback_thread_begin);
    register_callback(ompt_callback_thread_end);
    register_callback(ompt_callback_parallel_begin);
    register_callback(ompt_callback_parallel_end);
    register_callback(ompt_callback_implicit_task);
    register_callback(ompt_callback_control_tool);
    fprintf(stderr, "[OMPT] Callbacks registrados\n");

    // ── 4. Lanzar hilo monitor ───────────────────────────────────────────
    g_finalizing.store(false, std::memory_order_release);
    g_monitor_running.store(true, std::memory_order_release);

    if (pthread_create(&g_monitor_pthread, nullptr, monitor_loop, nullptr) != 0) {
        fprintf(stderr, "[OMPT] ERROR: no se pudo crear el hilo monitor\n");
        g_monitor_running.store(false, std::memory_order_release);
        return 0;
    }
    g_monitor_valid = true;

    if (g_ompt_get_num_procs)
        fprintf(stderr, "[OMPT] Sistema: %d procesadores lógicos\n",
                g_ompt_get_num_procs());

    return 1;
}

static void ompt_finalize(ompt_data_t* tool_data) {
    (void)tool_data;
    fprintf(stderr, "[OMPT] ompt_finalize\n");

    g_finalizing.store(true, std::memory_order_release);
    g_monitor_running.store(false, std::memory_order_release);

    if (g_monitor_valid) {
        pthread_join(g_monitor_pthread, nullptr);
        g_monitor_valid = false;
    }

    // Snapshot final con verificaciones defensivas
    fprintf(stderr, "[OMPT] ===== Tabla final: %d hilos =====\n", g_count);
    for (int i = 0; i < g_count; ++i) {
        ThreadInfo& t = g_threads[i];
        if (!t.seen) continue;

        fprintf(stderr,
            "[OMPT] tid=%-6d ompt_id=%-4" PRIu64
            " tipo=%-22s last_cpu=%3d numa=%d"
            " migraciones=%" PRIu64 " perf=%s\n",
            (int)t.tid_linux, t.ompt_id,
            thread_type_str((ompt_thread_t)t.ompt_type),
            t.last_cpu, t.numa_node, t.migrations,
            t.perf.opened ? "abierto" : "cerrado/fallido");

        if (t.perf.opened) {
            uint64_t rm = 0, ins = 0, cyc = 0, all = 0;

            if (t.perf.rm_misses    >= 0)
                ioctl(t.perf.rm_misses,    PERF_EVENT_IOC_DISABLE, 0);
            if (t.perf.instr_fd     >= 0)
                ioctl(t.perf.instr_fd,     PERF_EVENT_IOC_DISABLE, 0);
            if (t.perf.cycles_fd >= 0)
                ioctl(t.perf.cycles_fd, PERF_EVENT_IOC_DISABLE, 0);
            if (t.perf.all_fills_fd >= 0)
                ioctl(t.perf.all_fills_fd, PERF_EVENT_IOC_DISABLE, 0);

            read_counter(t.perf.rm_misses,    rm);
            read_counter(t.perf.instr_fd,     ins);
            read_counter(t.perf.cycles_fd, cyc);
            read_counter(t.perf.all_fills_fd, all);

            double ipc = (cyc > 0) ? (double)ins / (double)cyc : 0.0;

            if (g_summary_csv) {
                fprintf(g_summary_csv,
                    "%s,%d,%" PRIu64 ",%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f\n",
                    g_tag,
                    (int)t.tid_linux,
                    t.ompt_id,
                    thread_type_str((ompt_thread_t)t.ompt_type),
                    t.last_cpu,
                    t.numa_node,
                    t.migrations,
                    rm,
                    ins,
                    cyc,
                    ipc);
                fflush(g_summary_csv);
            }

            fprintf(stderr,
                "        remote_fills=%" PRIu64
                "  instr=%" PRIu64
                "  ipc=%.3f\n",
                rm, ins, ipc);

            if (t.perf.rm_misses    >= 0) { close(t.perf.rm_misses);    t.perf.rm_misses    = -1; }
            if (t.perf.instr_fd     >= 0) { close(t.perf.instr_fd);     t.perf.instr_fd     = -1; }
            if (t.perf.cycles_fd >= 0) { close(t.perf.cycles_fd); t.perf.cycles_fd = -1; }
            if (t.perf.all_fills_fd >= 0) { close(t.perf.all_fills_fd); t.perf.all_fills_fd = -1; }
            t.perf.opened = 0;
        }
    }

    if (g_topology_valid) {
        for (int n = 0; n < g_num_numa_nodes; ++n) {
            if (g_node_cpusets[n]) {
                hwloc_bitmap_free(g_node_cpusets[n]);
                g_node_cpusets[n] = nullptr;
            }
        }
        hwloc_topology_destroy(g_topology);
        g_topology_valid = false;
    }

    if (g_window_csv) {
        fclose(g_window_csv);
        g_window_csv = nullptr;
    }
    if (g_summary_csv) {
        fclose(g_summary_csv);
        g_summary_csv = nullptr;
    }
}

// =============================================================================
// Entry point OMPT
// =============================================================================
#ifdef __cplusplus
extern "C" {
#endif

ompt_start_tool_result_t* ompt_start_tool(
    unsigned int omp_version,
    const char*  runtime_version)
{
    fprintf(stderr,
        "[OMPT] ompt_start_tool: OMP_version=%u runtime=%s\n",
        omp_version, runtime_version ? runtime_version : "(null)");

    if (omp_version < 201811)
        fprintf(stderr,
            "[OMPT] WARNING: OMP version=%u < 5.0\n", omp_version);

    static ompt_start_tool_result_t result = {
        &ompt_initialize, &ompt_finalize, {0}
    };
    return &result;
}

#ifdef __cplusplus
}
#endif
