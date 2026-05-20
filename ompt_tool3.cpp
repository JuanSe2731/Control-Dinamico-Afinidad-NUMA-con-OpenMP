// =============================================================================
// numa_sched_ompt.cpp
//
// Herramienta OMPT para monitoreo de hilos OpenMP con contadores hardware
// via perf_event_open. Diseñada para el proyecto de scheduler NUMA-aware.
//
// Extraído y adaptado del patrón de referencia de LLVM OpenMP test suite,
// eliminando todas las dependencias internas (ompt-signal.h, kmp_platform.h).
//
// Compilar:
//   clang++ -std=c++17 -fPIC -shared -fopenmp -pthread \
//           numa_sched_ompt.cpp -o libnuma_sched_ompt.so
//
// Usar:
//   OMP_TOOL=enabled \
//   OMP_TOOL_LIBRARIES=$(pwd)/libnuma_sched_ompt.so \
//   ./spmv_dynamic2 Queen_4147.mtx 12 30 salida
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <string>
#include <sys/stat.h>

// OMPT
#include <omp-tools.h>
#include <omp.h>

// Linux per-thread
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <pthread.h>

// Hilo monitor
#include <atomic>
#include <chrono>
#include <thread>

// =============================================================================
// Configuración
// =============================================================================
static constexpr int MAX_THREADS  = 1024;
static constexpr int MONITOR_MS   = 100;   // periodo del hilo monitor

// =============================================================================
// Tabla de nombres legibles para tipos de hilo
// (patrón extraído directamente del LLVM callback.h)
// =============================================================================
static const char* thread_type_name[] = {
    "ompt_thread_unknown",   // 0 — no definido en el enum pero lo ponemos
    "ompt_thread_initial",   // 1
    "ompt_thread_worker",    // 2
    "ompt_thread_other"      // 3
};

static const char* thread_type_str(ompt_thread_t t) {
    if (t >= 1 && t <= 3) return thread_type_name[t];
    return thread_type_name[0];
}

// =============================================================================
// Configuración runtime (silencioso por defecto)
// =============================================================================
static bool g_verbose = false;
static bool g_monitor_enabled = false;
static std::string g_csv_path;
static std::string g_csv_tag;

#define OMPT_LOG(...) do { if (g_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

static bool env_flag_enabled(const char* name) {
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0') return false;
    if (std::strcmp(v, "0") == 0) return false;
    if (std::strcmp(v, "false") == 0 || std::strcmp(v, "FALSE") == 0) return false;
    return true;
}

// =============================================================================
// Macro de registro de callbacks
// (patrón extraído directamente del LLVM callback.h)
//
// Uso: register_callback(ompt_callback_thread_begin);
//      register_callback_t(ompt_callback_parallel_begin,
//                          ompt_callback_parallel_begin_t);
// =============================================================================
static ompt_set_callback_t g_ompt_set_callback = nullptr;

#define register_callback_t(name, type)                                   \
  do {                                                                     \
    type f_##name = &on_##name;                                            \
    ompt_set_result_t r =                                                  \
        g_ompt_set_callback(name, (ompt_callback_t)f_##name);             \
    if (r == ompt_set_never)                                               \
      OMPT_LOG("[OMPT] WARNING: callback '%s' no soportado\n", #name);     \
    else                                                                   \
      OMPT_LOG("[OMPT] callback '%s' registrado (result=%d)\n",            \
               #name, (int)r);                                             \
  } while (0)

#define register_callback(name) \
    register_callback_t(name, name##_t)

// =============================================================================
// Function pointers del runtime OMPT
// (patrón del LLVM callback.h — lookup() en ompt_initialize)
// =============================================================================
static ompt_get_thread_data_t  g_ompt_get_thread_data  = nullptr;
static ompt_get_unique_id_t    g_ompt_get_unique_id     = nullptr;
static ompt_get_proc_id_t      g_ompt_get_proc_id       = nullptr;
static ompt_get_num_procs_t    g_ompt_get_num_procs     = nullptr;
static ompt_get_parallel_info_t g_ompt_get_parallel_info = nullptr;

// =============================================================================
// Estructuras de datos por hilo
// =============================================================================
struct PerfFDs {
    int cycles_fd    {-1};
    int instr_fd     {-1};
    int cache_ref_fd {-1};
    int cachemiss_fd {-1};
    int cpu_mig_fd   {-1};
    int tlb_fd       {-1};
    int opened       {0};
};

struct ThreadInfo {
    // Identificación
    pid_t    tid_linux  {0};
    uint64_t ompt_id    {0};   // ID único de OMPT (ompt_get_unique_id)
    int      ompt_type  {0};   // ompt_thread_initial / worker / other
    int      last_cpu   {-1};
    int      seen       {0};
    int      measuring  {0};

    // Contadores hardware
    PerfFDs  perf {};

    // Últimos valores leídos (para calcular deltas)
    uint64_t last_cycles {0};
    uint64_t last_instr  {0};
    uint64_t last_miss   {0};
    int      has_last    {0};
};

static ThreadInfo g_threads[MAX_THREADS];
static int        g_count = 0;

// Spinlock liviano — evita malloc en callbacks
static std::atomic<bool> g_measuring       {false};
static std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
static inline void lock()   { while (g_lock.test_and_set(std::memory_order_acquire)); }
static inline void unlock() { g_lock.clear(std::memory_order_release); }

// =============================================================================
// Helpers Linux
// =============================================================================
static inline pid_t linux_tid() {
    return (pid_t)syscall(SYS_gettid);
}

static long perf_event_open(struct perf_event_attr* attr,
                             pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int open_counter(pid_t tid, uint32_t type, uint64_t config) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type           = type;
    pe.size           = sizeof(pe);
    pe.config         = config;
    pe.disabled       = 1;
    pe.inherit        = 0;      // solo este hilo
    pe.exclude_kernel = 0;      // igual que perf stat (include kernel)
    pe.exclude_hv     = 0;
    return (int)perf_event_open(&pe, tid, -1, -1, 0);
}

static bool read_counter(int fd, uint64_t& val) {
    val = 0;
    if (fd < 0) return false;
    return read(fd, &val, sizeof(val)) == (ssize_t)sizeof(val);
}

static void close_perf(PerfFDs& p) {
    if (p.cycles_fd    >= 0) { close(p.cycles_fd);    p.cycles_fd    = -1; }
    if (p.instr_fd     >= 0) { close(p.instr_fd);     p.instr_fd     = -1; }
    if (p.cache_ref_fd >= 0) { close(p.cache_ref_fd); p.cache_ref_fd = -1; }
    if (p.cachemiss_fd >= 0) { close(p.cachemiss_fd); p.cachemiss_fd = -1; }
    if (p.cpu_mig_fd   >= 0) { close(p.cpu_mig_fd);   p.cpu_mig_fd   = -1; }
    if (p.tlb_fd       >= 0) { close(p.tlb_fd);       p.tlb_fd       = -1; }
    p.opened = 0;
}

static void enable_perf(PerfFDs& p) {
    if (!p.opened) return;
    if (p.cycles_fd    >= 0) { ioctl(p.cycles_fd,    PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.cycles_fd,    PERF_EVENT_IOC_ENABLE, 0); }
    if (p.instr_fd     >= 0) { ioctl(p.instr_fd,     PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.instr_fd,     PERF_EVENT_IOC_ENABLE, 0); }
    if (p.cache_ref_fd >= 0) { ioctl(p.cache_ref_fd, PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.cache_ref_fd, PERF_EVENT_IOC_ENABLE, 0); }
    if (p.cachemiss_fd >= 0) { ioctl(p.cachemiss_fd, PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.cachemiss_fd, PERF_EVENT_IOC_ENABLE, 0); }
    if (p.cpu_mig_fd   >= 0) { ioctl(p.cpu_mig_fd,   PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.cpu_mig_fd,   PERF_EVENT_IOC_ENABLE, 0); }
    if (p.tlb_fd       >= 0) { ioctl(p.tlb_fd,       PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.tlb_fd,       PERF_EVENT_IOC_ENABLE, 0); }
}

static void disable_perf(PerfFDs& p) {
    if (!p.opened) return;
    if (p.cycles_fd    >= 0) ioctl(p.cycles_fd,    PERF_EVENT_IOC_DISABLE, 0);
    if (p.instr_fd     >= 0) ioctl(p.instr_fd,     PERF_EVENT_IOC_DISABLE, 0);
    if (p.cache_ref_fd >= 0) ioctl(p.cache_ref_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (p.cachemiss_fd >= 0) ioctl(p.cachemiss_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (p.cpu_mig_fd   >= 0) ioctl(p.cpu_mig_fd,   PERF_EVENT_IOC_DISABLE, 0);
    if (p.tlb_fd       >= 0) ioctl(p.tlb_fd,       PERF_EVENT_IOC_DISABLE, 0);
}

// Abre los tres contadores para un hilo dado su TID de Linux
static void open_perf_for_thread(ThreadInfo& t) {
    if (t.perf.opened) return;

    t.perf.cycles_fd    = open_counter(t.tid_linux,
                              PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
    t.perf.instr_fd     = open_counter(t.tid_linux,
                              PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    t.perf.cache_ref_fd = open_counter(t.tid_linux,
                              PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES);
    t.perf.cachemiss_fd = open_counter(t.tid_linux,
                              PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
    t.perf.cpu_mig_fd   = open_counter(t.tid_linux,
                              PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS);
    // dTLB misses: PERF_COUNT_HW_CACHE_DTLB with MISS
    t.perf.tlb_fd       = open_counter(t.tid_linux,
                              PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_DTLB);

    if (t.perf.cycles_fd < 0 || t.perf.instr_fd < 0 ||
        t.perf.cache_ref_fd < 0 || t.perf.cachemiss_fd < 0 ||
        t.perf.cpu_mig_fd < 0 || t.perf.tlb_fd < 0) {
        fprintf(stderr,
            "[OMPT][PERF] FALLO abriendo contadores para tid=%d "
            "(cyc=%d ins=%d ref=%d miss=%d mig=%d tlb=%d)\n"
            "             Verifica: cat /proc/sys/kernel/perf_event_paranoid\n"
            "             Debe ser <= 1. Ejecuta: "
            "sudo sysctl -w kernel.perf_event_paranoid=1\n",
            (int)t.tid_linux,
            t.perf.cycles_fd, t.perf.instr_fd,
            t.perf.cache_ref_fd, t.perf.cachemiss_fd,
            t.perf.cpu_mig_fd, t.perf.tlb_fd);
        close_perf(t.perf);
        return;
    }

    t.perf.opened = 1;
    t.has_last    = 0;
    //enable_perf(t.perf);

    if (g_measuring.load(std::memory_order_acquire)) {
        t.measuring = 1;
        enable_perf(t.perf);
        OMPT_LOG("[OMPT][PERF] Contadores ACTIVOS (start ya llegó): tid=%d\n",
                 (int)t.tid_linux);
    } else {
        OMPT_LOG("[OMPT][PERF] Contadores pausados (esperando start): tid=%d\n",
                 (int)t.tid_linux);
    }

    OMPT_LOG("[OMPT][PERF] Contadores abiertos: tid=%d ompt_id=%" PRIu64
             " tipo=%s cpu=%d\n",
             (int)t.tid_linux, t.ompt_id,
             thread_type_str((ompt_thread_t)t.ompt_type), t.last_cpu);
}

// Busca o inserta un hilo en la tabla global
static int upsert_thread(pid_t tid, int cpu, uint64_t ompt_id, int ompt_type) {
    // Buscar existente
    for (int i = 0; i < g_count; ++i) {
        if (g_threads[i].seen && g_threads[i].tid_linux == tid) {
            g_threads[i].last_cpu  = cpu;
            g_threads[i].ompt_id   = ompt_id;
            return i;
        }
    }
    // Insertar nuevo
    if (g_count >= MAX_THREADS) {
        OMPT_LOG("[OMPT] WARNING: MAX_THREADS alcanzado\n");
        return -1;
    }
    ThreadInfo& t = g_threads[g_count];
    t            = ThreadInfo{};   // reset limpio
    t.tid_linux  = tid;
    t.last_cpu   = cpu;
    t.ompt_id    = ompt_id;
    t.ompt_type  = ompt_type;
    t.seen       = 1;
    return g_count++;
}

// =============================================================================
// Hilo monitor (ventana de 100 ms)
// =============================================================================
static std::atomic<bool> g_monitor_running {false};
static std::atomic<bool> g_finalizing      {false}; 
static pthread_t         g_monitor_pthread;
static bool              g_monitor_valid = false;

static void* monitor_loop(void*) {
    OMPT_LOG("[OMPT][MON] Hilo monitor iniciado (período=%d ms)\n",
             MONITOR_MS);

    while (g_monitor_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(MONITOR_MS));

        lock();
        for (int i = 0; i < g_count; ++i) {
            ThreadInfo& t = g_threads[i];
            if (!t.seen || !t.perf.opened || !t.measuring) continue;

            uint64_t cyc = 0, ins = 0, miss = 0;
            bool ok = read_counter(t.perf.cycles_fd,    cyc)
                   && read_counter(t.perf.instr_fd,     ins)
                   && read_counter(t.perf.cachemiss_fd, miss);
            if (!ok) continue;

            // Primera lectura: solo guarda baseline
            if (!t.has_last) {
                t.last_cycles = cyc;
                t.last_instr  = ins;
                t.last_miss   = miss;
                t.has_last    = 1;
                continue;
            }

            // Delta respecto a la ventana anterior
            uint64_t d_cyc  = cyc  - t.last_cycles;
            uint64_t d_ins  = ins  - t.last_instr;
            uint64_t d_miss = miss - t.last_miss;

            t.last_cycles = cyc;
            t.last_instr  = ins;
            t.last_miss   = miss;

            double ipc        = (d_cyc  > 0) ? (double)d_ins  / d_cyc  : 0.0;
            double mpki       = (d_ins  > 0) ? (double)d_miss / d_ins * 1000.0 : 0.0;

            OMPT_LOG(
                "[OMPT][WIN%dms] ompt_id=%-4" PRIu64
                " tid=%-6d tipo=%-22s cpu=%2d"
                "  d_cycles=%10" PRIu64
                "  d_instr=%10" PRIu64
                "  d_cachemiss=%8" PRIu64
                "  IPC=%.3f  MPKI=%.1f\n",
                MONITOR_MS,
                t.ompt_id, (int)t.tid_linux,
                thread_type_str((ompt_thread_t)t.ompt_type),
                t.last_cpu,
                d_cyc, d_ins, d_miss,
                ipc, mpki);
        }
        unlock();
    }

    OMPT_LOG("[OMPT][MON] Hilo monitor detenido\n");
    return nullptr;
}

// =============================================================================
// Callbacks OMPT
// (firmas exactas según OpenMP 5.0 spec — igual que LLVM callback.h)
// =============================================================================

// ------------------------------------------------------------------
// thread_begin: el runtime crea un hilo OpenMP
//   → Aquí abrimos los contadores perf_event para ese hilo
// ------------------------------------------------------------------
static void on_ompt_callback_thread_begin(
    ompt_thread_t  thread_type,
    ompt_data_t*   thread_data)
{
    if (g_finalizing.load(std::memory_order_relaxed)) return;

    // ompt_get_unique_id() — patrón del LLVM callback.h
    // Asigna un ID único y persistente al hilo en thread_data->value
    uint64_t uid = g_ompt_get_unique_id ? g_ompt_get_unique_id() : 0;
    thread_data->value = uid;

    pid_t tid = linux_tid();
    int   cpu = sched_getcpu();

    lock();
    int idx = upsert_thread(tid, cpu, uid, (int)thread_type);
    if (idx >= 0 && !g_finalizing.load(std::memory_order_relaxed))
        open_perf_for_thread(g_threads[idx]);
    unlock();

    OMPT_LOG(
        "[OMPT] thread_begin: tipo=%-22s ompt_id=%" PRIu64
        " tid=%d cpu=%d\n",
        thread_type_str(thread_type), uid, (int)tid, cpu);
}

// ------------------------------------------------------------------
// thread_end: el runtime destruye el hilo
// ------------------------------------------------------------------
static void on_ompt_callback_thread_end(
    ompt_data_t* thread_data)
{
    pid_t tid = linux_tid();
    int   cpu = sched_getcpu();

    OMPT_LOG(
        "[OMPT] thread_end:   ompt_id=%" PRIu64 " tid=%d cpu=%d\n",
        thread_data->value, (int)tid, cpu);

    // Los contadores se cierran en ompt_finalize para poder leer
    // la snapshot final. No los cerramos aquí.
}

static int on_ompt_callback_control_tool(
    uint64_t    command,   // omp_control_tool_start, pause, flush, end
    uint64_t    modifier,  // el segundo argumento que pasaste (1 en este caso)
    void*       arg,
    const void* codeptr_ra)
{
    int i;
    if (command == 1) {  // omp_control_tool_start
        OMPT_LOG("[OMPT] control_tool: INICIANDO medicion\n");
        g_measuring.store(true, std::memory_order_release);
        lock();
        for ( i = 0; i < g_count; ++i){
            if (g_threads[i].seen && g_threads[i].perf.opened){
                g_threads[i].measuring = 1;
                g_threads[i].has_last = 0;  // resetear baseline
                enable_perf(g_threads[i].perf);
            }
        }
        unlock();
    }
    else if (command == 2) {  // omp_control_tool_pause
        OMPT_LOG("[OMPT] control_tool: PAUSANDO medicion\n");
        g_measuring.store(false, std::memory_order_release);
        lock();
        for (i = 0; i < g_count; ++i){
            if (g_threads[i].seen && g_threads[i].perf.opened){
                g_threads[i].measuring = 0;
                disable_perf(g_threads[i].perf);
            }
        }
        unlock();
    }
    return 0;
}

// ------------------------------------------------------------------
// parallel_begin: empieza una región paralela
//   → Usamos ompt_get_unique_id() igual que LLVM callback.h
//     para asignar un ID a la región
// ------------------------------------------------------------------
static void on_ompt_callback_parallel_begin(
    ompt_data_t*        encountering_task_data,
    const ompt_frame_t* encountering_task_frame,
    ompt_data_t*        parallel_data,
    uint32_t            requested_team_size,
    int                 flags,
    const void*         codeptr_ra)
{
    (void)encountering_task_data;
    (void)encountering_task_frame;
    (void)flags;
    (void)codeptr_ra;

    // Patrón LLVM: asignar ID único a la región paralela
    if (g_ompt_get_unique_id)
        parallel_data->value = g_ompt_get_unique_id();

    pid_t tid = linux_tid();
    int   cpu = sched_getcpu();

    OMPT_LOG(
        "[OMPT] parallel_begin: parallel_id=%" PRIu64
        " team_size=%u  encuentro_tid=%d cpu=%d\n",
        parallel_data->value, requested_team_size, (int)tid, cpu);
}

// ------------------------------------------------------------------
// parallel_end: termina una región paralela
// ------------------------------------------------------------------
static void on_ompt_callback_parallel_end(
    ompt_data_t* parallel_data,
    ompt_data_t* encountering_task_data,
    int          flags,
    const void*  codeptr_ra)
{
    (void)encountering_task_data;
    (void)flags;
    (void)codeptr_ra;

    pid_t tid = linux_tid();
    int   cpu = sched_getcpu();

    OMPT_LOG(
        "[OMPT] parallel_end:   parallel_id=%" PRIu64
        " encuentro_tid=%d cpu=%d\n",
        parallel_data->value, (int)tid, cpu);
}

// ------------------------------------------------------------------
// implicit_task: tarea implícita (una por hilo dentro de parallel)
//   → Patrón LLVM: asigna ID único y reporta thread_num
//     Esto permite saber qué hilo OpenMP (0..N-1) es cada tid
// ------------------------------------------------------------------
static void on_ompt_callback_implicit_task(
    ompt_scope_endpoint_t endpoint,
    ompt_data_t*          parallel_data,
    ompt_data_t*          task_data,
    unsigned int          team_size,
    unsigned int          thread_num,
    int                   flags)
{
    (void)flags;

    if (endpoint == ompt_scope_begin) {
        if (g_ompt_get_unique_id)
            task_data->value = g_ompt_get_unique_id();

        // thread_num es el índice OpenMP del hilo (0-based) dentro del equipo
        // Muy útil para correlacionar con omp_get_thread_num() en la app
        OMPT_LOG(
            "[OMPT] implicit_task_begin: parallel_id=%" PRIu64
            " task_id=%" PRIu64
            " thread_num=%u team_size=%u\n",
            parallel_data ? parallel_data->value : 0,
            task_data->value, thread_num, team_size);
    } else {
        OMPT_LOG(
            "[OMPT] implicit_task_end:   parallel_id=%" PRIu64
            " task_id=%" PRIu64
            " thread_num=%u\n",
            parallel_data ? parallel_data->value : 0,
            task_data->value, thread_num);
    }
}

// =============================================================================
// Initialize / Finalize
// =============================================================================
static int ompt_initialize(
    ompt_function_lookup_t lookup,
    int                    initial_device_num,
    ompt_data_t*           tool_data)
{
    (void)initial_device_num;
    (void)tool_data;

    g_verbose = env_flag_enabled("OMPT_VERBOSE");
    g_monitor_enabled = env_flag_enabled("OMPT_MONITOR");
    const char* csv = std::getenv("OMPT_CSV");
    if (csv && csv[0]) {
        g_csv_path = std::string(csv);
    } else {
        g_csv_path = "";
    }
    const char* tag = std::getenv("OMPT_TAG");
    if (tag && tag[0]) {
        g_csv_tag = std::string(tag);
    } else {
        g_csv_tag = "";
    }

    OMPT_LOG("[OMPT] ompt_initialize\n");

    // ── 1. Obtener function pointers del runtime
    //       Patrón exacto del LLVM callback.h: cast con lookup()
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

    // ── 2. Registrar callbacks con el macro del patrón LLVM
    register_callback(ompt_callback_thread_begin);
    register_callback(ompt_callback_thread_end);
    register_callback(ompt_callback_parallel_begin);
    register_callback(ompt_callback_parallel_end);
    register_callback(ompt_callback_implicit_task);
    register_callback(ompt_callback_control_tool);


    OMPT_LOG("[OMPT] Callbacks registrados\n");

    // ── 3. Lanzar hilo monitor con pthread (no std::thread estático)
    g_finalizing.store(false, std::memory_order_release);
    if (g_monitor_enabled) {
        g_monitor_running.store(true,  std::memory_order_release);
        if (pthread_create(&g_monitor_pthread, nullptr, monitor_loop, nullptr) != 0) {
            fprintf(stderr, "[OMPT] ERROR: no se pudo crear el hilo monitor\n");
            g_monitor_running.store(false, std::memory_order_release);
            return 0;
        }
        g_monitor_valid = true;
    } else {
        g_monitor_running.store(false, std::memory_order_release);
        g_monitor_valid = false;
    }

    // ── 4. Respaldo: si ompt_finalize no es llamado por libgomp,
    //       atexit garantiza el join antes de que el proceso muera
    atexit([]() {
        if (g_finalizing.load(std::memory_order_acquire)) return; // ya hecho
        g_monitor_running.store(false, std::memory_order_release);
        if (g_monitor_valid) {
            pthread_join(g_monitor_pthread, nullptr);
            g_monitor_valid = false;
        }
        // NO accedas a g_threads aquí — puede estar destruido
        g_finalizing.store(true, std::memory_order_release);
    });

    if (g_ompt_get_num_procs)
        OMPT_LOG("[OMPT] Sistema: %d procesadores lógicos\n",
                 g_ompt_get_num_procs());

    return 1;
}

static void ompt_finalize(ompt_data_t* tool_data) {
    (void)tool_data;
    OMPT_LOG("[OMPT] ompt_finalize\n");

    // 1. Señalizar y esperar al monitor
    g_finalizing.store(true,  std::memory_order_release);
    g_monitor_running.store(false, std::memory_order_release);

    if (g_monitor_valid) {
        pthread_join(g_monitor_pthread, nullptr);
        g_monitor_valid = false;
    }

    // 2. Snapshot final — SIN lock (monitor ya terminó, no hay races)
    OMPT_LOG(
        "[OMPT] ===== Tabla final: %d hilos =====\n", g_count);

    uint64_t total_cycles = 0;
    uint64_t total_instr  = 0;
    uint64_t total_ref    = 0;
    uint64_t total_miss   = 0;
    uint64_t total_mig    = 0;
    uint64_t total_tlb    = 0;

    for (int i = 0; i < g_count; ++i) {
        ThreadInfo& t = g_threads[i];
        if (!t.seen) continue;

        OMPT_LOG(
            "[OMPT] tid=%-6d ompt_id=%-4" PRIu64
            " tipo=%-22s last_cpu=%2d perf=%s\n",
            (int)t.tid_linux, t.ompt_id,
            thread_type_str((ompt_thread_t)t.ompt_type),
            t.last_cpu,
            t.perf.opened ? "abierto" : "cerrado/fallido");

        if (t.perf.opened) {
            uint64_t cyc = 0, ins = 0, ref = 0, miss = 0, mig = 0, tlb = 0;
            disable_perf(t.perf);
            read_counter(t.perf.cycles_fd,    cyc);
            read_counter(t.perf.instr_fd,     ins);
            read_counter(t.perf.cache_ref_fd, ref);
            read_counter(t.perf.cachemiss_fd, miss);
            read_counter(t.perf.cpu_mig_fd,   mig);
            read_counter(t.perf.tlb_fd,       tlb);

            total_cycles += cyc;
            total_instr  += ins;
            total_ref    += ref;
            total_miss   += miss;
            total_mig    += mig;
            total_tlb    += tlb;

            double ipc  = (cyc > 0) ? (double)ins  / cyc        : 0.0;
            double mpki = (ins > 0) ? (double)miss / ins * 1000.0 : 0.0;

            OMPT_LOG(
                "        cycles=%" PRIu64 "  instr=%" PRIu64
                "  cache_refs=%" PRIu64 "  cache_misses=%" PRIu64
                "  cpu_migrations=%" PRIu64 "  tlb_misses=%" PRIu64
                "  IPC=%.3f  MPKI=%.1f\n",
                cyc, ins, ref, miss, mig, tlb, ipc, mpki);

            close_perf(t.perf);
        }
    }

    if (!g_csv_path.empty()) {
        struct stat st;
        const bool need_header = (stat(g_csv_path.c_str(), &st) != 0 || st.st_size == 0);
        FILE* f = std::fopen(g_csv_path.c_str(), "a");
        if (!f) {
            fprintf(stderr, "[OMPT] ERROR: no se pudo abrir CSV: %s\n",
                    g_csv_path.c_str());
            return;
        }
        if (need_header) {
            std::fprintf(f, "tag,cycles,instructions,cache_references,cache_misses,cpu_migrations,tlb_misses,ipc,mpki\n");
        }
        double total_ipc  = (total_cycles > 0) ? (double)total_instr / total_cycles : 0.0;
        double total_mpki = (total_instr > 0) ? (double)total_miss / total_instr * 1000.0 : 0.0;
        const char* tag = g_csv_tag.empty() ? "ompt" : g_csv_tag.c_str();
        std::fprintf(
            f, "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%.1f\n",
            tag, total_cycles, total_instr, total_ref, total_miss, total_mig, total_tlb, total_ipc, total_mpki);
        std::fclose(f);
    }
}

// =============================================================================
// Entry point — obligatorio y con extern "C"
// (patrón exacto del LLVM callback.h — el #ifdef __cplusplus es necesario)
// =============================================================================
#ifdef __cplusplus
extern "C" {
#endif

ompt_start_tool_result_t* ompt_start_tool(
    unsigned int omp_version,
    const char*  runtime_version)
{
    g_verbose = env_flag_enabled("OMPT_VERBOSE");
    OMPT_LOG(
        "[OMPT] ompt_start_tool: OMP_version=%u runtime=%s\n",
        omp_version, runtime_version ? runtime_version : "(null)");

    // Verificación de versión — patrón del LLVM callback.h
    if (omp_version < 201811) {   // 201811 = OpenMP 5.0
        OMPT_LOG(
            "[OMPT] WARNING: runtime OMP version=%u < 5.0 (201811). "
            "Algunos callbacks pueden no estar disponibles.\n", omp_version);
    }

    static ompt_start_tool_result_t result = {
        &ompt_initialize,
        &ompt_finalize,
        {0}
    };
    return &result;
}

#ifdef __cplusplus
}
#endif
