// =============================================================================
// sched_NUMA_optC_leaky.cpp   (OPCIÓN C)
//
// Scheduler NUMA adaptativo via OMPT + perf_event_open + hwloc
//
//   MECANISMO DE DISPARO: integrador con fugas + histéresis sobre una
//                         REFERENCIA GLOBAL adaptativa
//   --------------------------------------------------------------------------
//   - Se evalúa CADA ventana de 100 ms.
//   - g_ref = EWMA del ratio_rm AGREGADO de la ventana => referencia "típica" de
//     la corrida (autoajustable; sin la constante mágica 0.11).
//   - g_dev = EWMA de la desviación absoluta media (MAD) entre hilos => dispersión
//     robusta que escala el umbral automáticamente.
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
//   PRINCIPIO DE DISEÑO: el instrumento no debe perturbar lo que mide.
//     * Ninguna E/S ocurre con el spinlock global tomado.
//     * Las filas del CSV de ventanas se acumulan en memoria y se vuelcan una
//       sola vez en ompt_finalize.
//     * Todo el trazo por ventana/región está detrás de OMPT_VERBOSE.
//     * El syscall de rebinding (hwloc_set_thread_cpubind) se ejecuta SIN lock;
//       solo las escrituras de estado, que son un puñado de stores, van dentro.
//
// Variables de entorno:
//   OMPT_TAG                identificador de la corrida, va en cada fila de CSV
//   OMPT_WINDOW_CSV         ruta del CSV por ventana (si falta, no se acumula)
//   OMPT_SUMMARY_CSV        ruta del CSV de resumen por hilo
//   OMPT_OVERHEAD_CSV       ruta del CSV de overhead intrínseco del tool
//   OMPT_LOG_FILE           si está, stderr va ahí; si no, a /dev/null
//   OMPT_VERBOSE=1          reactiva el trazo por ventana y por región
//   OMPT_DISABLE_MIGRATION=1  monitorea y registra igual, pero nunca migra
//                             (configuraciones de control para medir overhead)
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
#include <cerrno>
#include <cmath>
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

// Techo de filas del buffer en memoria del CSV de ventanas. A 128 hilos y 100 ms
// una corrida típica genera ~5k filas; 262144 deja dos órdenes de magnitud de
// margen y evita cualquier realloc con el lock tomado.
static constexpr size_t   MAX_WINDOW_ROWS   = 262144;

// Estado de los contadores de un hilo. Con el IPC retirado solo quedan dos
// estados posibles: o el par 0xFF44/0xD044 abre y el hilo puede decidir, o no
// abre y el hilo no cuenta para nada. El antiguo PERF_IPC_ONLY describia un hilo
// que aportaba IPC pero no podia disparar migraciones; sin IPC ese estado ya no
// tiene contenido.
enum PerfStatus { PERF_FAILED = 0, PERF_FULL = 1 };
static const char* perf_status_str(int s) {
    return (s == PERF_FULL) ? "full" : "failed";
}

// =============================================================================
// Salida a archivos
// =============================================================================
static FILE*       g_window_csv   = nullptr;
static FILE*       g_summary_csv  = nullptr;
static FILE*       g_overhead_csv = nullptr;
// char[] en lugar de std::string: los atexit del runtime OpenMP pueden llamar a
// ompt_finalize DESPUÉS de que los destructores de estáticos de C++ hayan corrido,
// lo que deja std::string en estado inválido y corrompe el campo tag en el CSV.
static char        g_tag[1024]    = {};

static bool        g_verbose            = false;
static bool        g_disable_migration  = false;

// Trazo condicional: sin OMPT_VERBOSE no se formatea ni se escribe nada. Antes
// estos fprintf corrían siempre (aun con stderr en /dev/null) y a 128 hilos
// suponían ~38k llamadas formateadas por corrida dentro de la región medida.
#define VLOG(...)  do { if (g_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

// =============================================================================
// hwloc — topología global
// =============================================================================
static hwloc_topology_t g_topology;
static bool             g_topology_valid = false;
static int              g_num_numa_nodes = 0;
static hwloc_cpuset_t   g_node_cpusets[MAX_NUMA_NODES];

// =============================================================================
// Referencia global adaptativa (compartida por todos los hilos)
//   - Protegida por g_lock; se actualiza UNA vez por ventana en monitor_loop.
// =============================================================================
static double g_ref      = 0.0;   // EWMA del ratio_rm agregado de la ventana
static double g_dev      = 0.0;   // EWMA de la MAD entre hilos
static bool   g_ref_init = false;

// =============================================================================
// Contadores de diagnóstico y de overhead intrínseco
// =============================================================================
static uint64_t g_win_idx          = 0;   // índice de ventana del monitor
static uint64_t g_monitor_ticks    = 0;
static double   g_monitor_us_total = 0.0;
static double   g_monitor_us_max   = 0.0;
static std::atomic<uint64_t> g_callback_calls   {0};
static std::atomic<uint64_t> g_callback_ns_total{0};
static double   g_migration_us_total = 0.0;
static uint64_t g_resyncs          = 0;   // ventanas descartadas por retroceso del contador
static uint64_t g_mux_scaled_reads = 0;   // lecturas corregidas por multiplexado
// Los eventos crudos 0xD044/0xFF44 son específicos de AMD Zen. En otra PMU el
// perf_event_open SUELE TENER ÉXITO y devolver 0 para siempre: el tool creería
// estar midiendo y simplemente no migraría nunca. Contrastar ventanas con ciclos
// contra ventanas con fills permite detectar ese caso y gritarlo al final.
static uint64_t g_win_with_fills   = 0;   // muestras (hilo x ventana) con fills
// Los 4 eventos se abren COMO GRUPO, y un grupo entra en la PMU entero o no
// entra. Si no cabe, todas las lecturas devuelven time_running=0 y el monitor
// descarta al hilo en cada ventana: CSV vacíos, cero migraciones y ni un solo
// mensaje de error. Hay que contarlo para poder gritarlo al final.
static uint64_t g_reads_never_scheduled = 0;
static uint64_t g_reads_ok              = 0;
static uint64_t g_window_rows_dropped = 0;
static int      g_perf_failed_threads = 0;
static int      g_perf_partial_threads = 0;

static std::chrono::steady_clock::time_point g_mon_t0;

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
static ompt_get_unique_id_t     g_ompt_get_unique_id      = nullptr;
static ompt_get_num_procs_t     g_ompt_get_num_procs      = nullptr;

// =============================================================================
// Estructuras de datos por hilo
// =============================================================================
// Rellenos DESGLOSADOS POR ORIGEN. PMCx044 (`ls_any_fills_from_sys`) tiene una
// máscara por procedencia de la línea, y eso es lo que permite separar el tráfico
// por distancia topológica — que es la magnitud que de verdad importa, no el
// "ancho de banda" analítico de los kernels.
//
// Estos contadores son SOLO PARA INFORMAR: el mecanismo de decisión sigue usando
// ratio_rm exactamente igual que antes. Lo único que cambia es de dónde salen sus
// dos números: en vez de abrir 0xFF44 y 0xD044 aparte (que sumarían 10 eventos
// sobre 6 PMC programables y forzarían un multiplexado agresivo justo sobre la
// variable de decisión), se derivan de las seis máscaras:
//     all    = suma de las seis
//     remoto = far_cache + far_dram
// La única diferencia con el antiguo 0xD044 es el bit 0x80 (memoria alternativa,
// CXL), que en esta máquina es cero.
//
// Con OMPT_MASCARAS=0 se vuelve al par 0xFF44/0xD044 exacto, para poder reproducir
// las campañas anteriores bit a bit.
enum FillOrigen {
    FO_L2 = 0,          // umask 0x01  L2 propia
    FO_L3_CCD,          // umask 0x02  L3 del propio chiplet
    FO_CCD_VECINO,      // umask 0x04  L3 de otro chiplet, mismo socket
    FO_DRAM_LOCAL,      // umask 0x08  DRAM local
    FO_FAR_CACHE,       // umask 0x10  caché del otro socket
    FO_FAR_DRAM,        // umask 0x40  DRAM remota
    FO_N
};

// Mascaras de origen (byte alto). CODIGOS VERIFICADOS en exadell el 2026-08-19
// con `perf stat -e ls_any_fills_from_sys.<nombre> -vv -- true`, que imprime el
// config ya resuelto. No estan en sysfs: en AMD vienen de las tablas JSON que
// perf lleva compiladas para la familia 19h, por eso van cableados.
//
// Comprobado de paso que `remote_cache` = mascara 0x14 = 0x10|0x04, o sea la SUMA
// de far_cache y near_cache, no un septimo origen.
static const uint64_t FILL_UMASK[FO_N] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x40
};

// Dos familias del mismo evento:
//   0x44  ls_any_fills_from_sys   demanda + prefetch hardware + prefetch software
//   0x43  ls_dmnd_fills_from_sys  SOLO demanda
//
// El TRAFICO se mide con `any` (un prefetch consume enlace igual que una demanda);
// el TIEMPO MUERTO con `dmnd` (un prefetch que llega a tiempo no para al nucleo).
// No caben las dos a la vez sobre 6 PMC, asi que se conmuta con PERF_FAMILIA.
//
// El scheduler usa SIEMPRE la que le digan, pero la campana lo deja en `any`: es
// la variable de decision del mecanismo y cambiarla invalidaria la comparacion
// con las campanas 29223 y 29311.
static constexpr uint64_t EVENTO_ANY  = 0x44;
static constexpr uint64_t EVENTO_DMND = 0x43;

static uint64_t evento_familia() {
    static uint64_t cache = 0;
    if (cache == 0) {
        const char* f = getenv("PERF_FAMILIA");
        cache = (f && strcmp(f, "dmnd") == 0) ? EVENTO_DMND : EVENTO_ANY;
    }
    return cache;
}

static bool familia_es_demanda() { return evento_familia() == EVENTO_DMND; }

static uint64_t fill_config(int i) {
    return (FILL_UMASK[i] << 8) | evento_familia();
}

// Orden en que se conservan mascaras cuando no caben todas: primero las que CRUZAN
// una interconexion, que son las que este trabajo estudia.
static const int PRIORIDAD_FILL[FO_N] = {
    FO_FAR_DRAM, FO_FAR_CACHE, FO_CCD_VECINO,
    FO_DRAM_LOCAL, FO_L3_CCD, FO_L2
};

// Cuantas mascaras del desglose se abren ADEMAS del par. Por defecto 3, que es lo
// que queda libre: Zen 4 tiene 6 contadores programables, el nmi_watchdog ocupa uno
// y el par de ratio_rm ocupa dos. Pedir mas obligaria al kernel a multiplexar el
// desglose contra el par, y aunque el cociente remoto/total seguiria siendo exacto
// (van en el mismo grupo y se escalan igual), pasaria a muestrearse a rafagas: mas
// varianza en la senal del detector y conteos absolutos extrapolados justo debajo
// del filtro MIN_FILLS. La variable de decision del mecanismo no debe depender de
// una extrapolacion.
// SUBE DE 3 A 5 al retirar el IPC. Zen 4 tiene 6 contadores programables y el
// nmi_watchdog ocupa uno, asi que quedan 5 libres. Antes, ciclos e instrucciones
// se llevaban 2 y el par exacto otros 2, de modo que solo sobraba 1... y aun asi
// el tope estaba en 3, con lo que el grupo del desglose no se planificaba entero
// y salian columnas a cero. Sin IPC, el par exacto va en su grupo y el desglose
// dispone del resto: 5 origenes de los 6 son alcanzables.
static int tope_origenes() {
    static int cache = -1;
    if (cache < 0) {
        cache = 5;
        if (const char* e = getenv("PERF_MAX_ORIGENES")) {
            int v = atoi(e);
            if (v >= 0 && v <= FO_N) cache = v;
        }
    }
    return cache;
}

// Los nombres de columna correspondientes viven en la cabecera de
// OMPT_SUMMARY_CSV y deben coincidir con los de perf_region.hpp, para que el
// script de campaña agregue por igual los dos caminos de medida.

struct PerfFDs {
    int rm_misses    {-1};   // ANY_DATA_CACHE_FILLS_REMOTE_ALL (raw 0xD044)
    int all_fills_fd {-1};   // ALL DATA CACHE FILLS (raw 0xFF44)
    int origen_fd[FO_N] {-1, -1, -1, -1, -1, -1};   // desglose por procedencia
    int opened       {0};
    int status       {PERF_FAILED};
};

struct ThreadInfo {
    // ── Identificación ──────────────────────────────────────────────────────
    pid_t     tid_linux       {0};
    uint64_t  ompt_id         {0};
    int       ompt_type       {0};
    int       last_cpu        {-1};
    int       numa_node       {-1};
    int       seen            {0};
    int       alive           {0};   // 0 tras thread_end: ya no se muestrea
    int       measuring       {0};
    int       needs_migration {0};
    int       migrating       {0};   // rebinding en curso: el monitor lo salta

    // ── Estado del detector (Mecanismo C: integrador con fugas) ──────────
    int       win_count       {0};     // ventanas válidas vistas (warmup)
    double    bucket          {0.0};    // nivel del balde con fugas

    // ── Handle pthread — necesario para hwloc_set_thread_cpubind ─────────
    pthread_t pthread_handle {0};

    // ── Contadores hardware ──────────────────────────────────────────────
    PerfFDs   perf           {};

    // ── Últimas lecturas (para deltas) ────────────────────────────────────
    uint64_t  last_rm_misses {0};
    uint64_t  last_all_fills {0};
    int       has_last       {0};

    // ── Totales finales (se congelan en thread_end o en finalize) ─────────
    uint64_t  fin_rm  {0}, fin_all {0};
    uint64_t  fin_origen[FO_N] {0, 0, 0, 0, 0, 0};
    int       fin_valid {0};

    // ── Estado del scheduler ─────────────────────────────────────────────
    uint64_t  migrations         {0};   // migraciones aplicadas con éxito
    uint64_t  mig_failures       {0};   // rebindings que devolvieron error
    int64_t   first_migration_win{-1};  // ventana de la primera migración
};

static ThreadInfo g_threads[MAX_THREADS];
static int        g_count = 0;

// Spinlock liviano — evita malloc en callbacks
static std::atomic<bool> g_measuring {false};
static std::atomic_flag  g_lock = ATOMIC_FLAG_INIT;
static inline void lock()   { while (g_lock.test_and_set(std::memory_order_acquire)); }
static inline void unlock() { g_lock.clear(std::memory_order_release); }

// =============================================================================
// Buffer en memoria del CSV de ventanas
//   Se vuelca entero en ompt_finalize. Contrapartida asumida: si el proceso
//   muere de forma anormal se pierden las ventanas; a cambio, el camino caliente
//   queda sin E/S. El resumen y el overhead sí se escriben al final igualmente.
//
//   MEMORIA CRUDA A PROPÓSITO, no std::vector: el runtime OpenMP llama a
//   ompt_finalize desde un atexit que corre DESPUÉS de los destructores de los
//   estáticos de C++, así que un contenedor global ya estaría destruido y
//   recorrerlo aquí es un segfault (verificado). Es la misma razón por la que
//   g_tag es char[] y no std::string. La memoria de malloc sobrevive a esos
//   destructores; no se libera porque el proceso termina justo después.
// =============================================================================
struct WindowRow {
    uint64_t win_idx;
    double   t_ms;
    uint64_t ompt_id;
    int      tid;
    int      ompt_type;
    // cpu/numa son LO ULTIMO CONOCIDO, capturado al entrar el hilo en una region
    // paralela (implicit_task). El monitor no puede consultar el cpu de otro hilo
    // sin leer /proc/<tid>/stat, que seria carisimo cada 100 ms. La DECISION de
    // migrar si usa un sched_getcpu() fresco; esta columna es solo informativa.
    int      cpu;
    int      numa;
    uint64_t d_rm, d_all;
    double   ratio;      // -1 => el hilo no tiene los eventos NUMA
    double   bucket, thr_T, ref, dev;
    int      flagged;    // 1 => en esta ventana el balde cruzó la capacidad
};
static WindowRow* g_window_rows = nullptr;
static size_t     g_window_n    = 0;

// Muestras de la ventana en curso: pase 1 las llena, pase 2 las evalúa.
struct WinSample {
    int      idx;
    uint64_t d_rm, d_all;
    double   ratio;
    bool     gated;      // pasó el filtro MIN_FILLS y tiene eventos NUMA
};
static WinSample g_samples[MAX_THREADS];

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

static inline double us_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - t0).count();
}

// =============================================================================
// perf_event helpers
// =============================================================================
//
// read_format pide time_enabled/time_running para poder CORREGIR el multiplexado:
// si se piden más eventos de los que hay registros contadores en la PMU, el
// kernel los rota y cada uno solo mide una fracción del intervalo. Sin esta
// corrección los conteos salen bajos y —lo que es peor para nosotros— cada
// evento por un factor distinto, con lo que ratio_rm = d_rm/d_all e IPC pasan a
// ser cocientes de números medidos en intervalos diferentes.
//
// Además los 4 eventos se abren COMO GRUPO (group_fd = líder): así entran y
// salen del hardware juntos y cubren exactamente el mismo intervalo, que es
// justo lo que un cociente necesita.
struct PerfRead { uint64_t value, time_enabled, time_running; };

static int open_counter(pid_t tid, uint32_t type, uint64_t config, int group_fd) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type           = type;
    pe.size           = sizeof(pe);
    pe.config         = config;
    pe.disabled       = (group_fd == -1) ? 1 : 0;  // solo el líder arranca parado
    pe.inherit        = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    pe.read_format    = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    return (int)perf_event_open_syscall(&pe, tid, -1, group_fd, 0);
}

static bool read_counter(int fd, uint64_t& val) {
    val = 0;
    if (fd < 0) return false;
    PerfRead r {};
    if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) return false;
    if (r.time_running == 0) {
        // El evento nunca llegó a estar en el hardware en este intervalo. Con
        // eventos agrupados esto suele significar que el grupo entero no cabe
        // en la PMU, y entonces NO habrá ningún dato en toda la corrida.
        val = 0;
        ++g_reads_never_scheduled;
        return false;
    }
    ++g_reads_ok;
    if (r.time_running < r.time_enabled) {
        // Escalado por multiplexado. Se hace en double a propósito: value *
        // time_enabled desbordaría uint64 y aquí la precisión sobra.
        val = (uint64_t)((double)r.value *
                         ((double)r.time_enabled / (double)r.time_running));
        ++g_mux_scaled_reads;
    } else {
        val = r.value;
    }
    return true;
}

static void close_perf(PerfFDs& p) {
    if (p.rm_misses    >= 0) { close(p.rm_misses);    p.rm_misses    = -1; }
    if (p.all_fills_fd >= 0) { close(p.all_fills_fd); p.all_fills_fd = -1; }
    // Los seis del desglose por origen TAMBIÉN. Sin esto se filtrarían 6 fd por
    // hilo: a 128 hilos son 768, muy por encima del RLIMIT_NOFILE habitual de
    // 1024, y es exactamente la fuga que ya obligó a reescribir thread_end.
    for (int i = 0; i < FO_N; ++i)
        if (p.origen_fd[i] >= 0) { close(p.origen_fd[i]); p.origen_fd[i] = -1; }
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
    if (p.all_fills_fd >= 0) { ioctl(p.all_fills_fd, PERF_EVENT_IOC_RESET,  0);
                                ioctl(p.all_fills_fd, PERF_EVENT_IOC_ENABLE, 0); }
    // Solo el lider del grupo del desglose: PERF_IOC_FLAG_GROUP arrastra al resto.
    const int lider_fill = p.origen_fd[PRIORIDAD_FILL[0]];
    if (lider_fill >= 0) { ioctl(lider_fill, PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
                           ioctl(lider_fill, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP); }
}

static void disable_perf(PerfFDs& p) {
    if (!p.opened) return;
    if (p.rm_misses    >= 0) ioctl(p.rm_misses,    PERF_EVENT_IOC_DISABLE, 0);
    if (p.all_fills_fd >= 0) ioctl(p.all_fills_fd, PERF_EVENT_IOC_DISABLE, 0);
    for (int i = 0; i < FO_N; ++i)
        if (p.origen_fd[i] >= 0) ioctl(p.origen_fd[i], PERF_EVENT_IOC_DISABLE, 0);
}

// ¿Se abre el desglose por origen? Por defecto sí. Con OMPT_MASCARAS=0 se vuelve
// al par 0xFF44/0xD044 exacto, para reproducir las campañas anteriores.
// Se pone a 1 si algun hilo tuvo que caer al par degradado; sirve para que la
// columna 'mascaras' del resumen diga la verdad.
static int g_mascaras_degradadas = 0;

static bool usar_mascaras_origen() {
    static int cache = -1;
    if (cache < 0) {
        const char* e = getenv("OMPT_MASCARAS");
        cache = (e && atoi(e) == 0) ? 0 : 1;
    }
    return cache != 0;
}

// Lee los rellenos y devuelve los DOS números que el mecanismo de decisión
// necesita, venga de donde venga el desglose. Opcionalmente devuelve también el
// vector por origen para poder informarlo.
//
// Esto es lo que mantiene intacto el mecanismo: ratio_rm = remoto/all sigue
// significando lo mismo, con las seis máscaras o con el par antiguo.
static bool read_fills(PerfFDs& p, uint64_t& all, uint64_t& remoto,
                       uint64_t* por_origen /* FO_N o nullptr */) {
    all = 0; remoto = 0;
    if (por_origen) for (int i = 0; i < FO_N; ++i) por_origen[i] = 0;

    // El desglose es opcional y solo informa; si esta, se rellena.
    //
    // El guardian NO puede mirar origen_fd[0]: ese indice es FO_L2, que es la
    // ULTIMA en PRIORIDAD_FILL y por tanto la primera en quedarse fuera cuando
    // solo caben 3 mascaras. Mirarlo hacia que la condicion fuese siempre falsa
    // y el desglose no se leyera nunca (campana 29390: fill_* todo a cero en
    // ompt_summary.csv, y con ello bw_remoto_gibs=0 para obs y scheduler).
    // El bucle ya comprueba cada fd por separado, asi que basta con eso.
    if (por_origen) {
        uint64_t v = 0;
        for (int i = 0; i < FO_N; ++i)
            if (p.origen_fd[i] >= 0 && read_counter(p.origen_fd[i], v))
                por_origen[i] = v;
    }

    // all y remoto vienen SIEMPRE del par exacto: es lo que sostiene ratio_rm.
    if (!read_counter(p.all_fills_fd, all))    return false;
    if (!read_counter(p.rm_misses,    remoto)) return false;
    return true;
}


// IPC RETIRADO (campana V6). Ya no se abren ciclos ni instrucciones. El IPC no
// se usaba para decidir nada —la variable de decision es y fue siempre ratio_rm—
// y a cambio ocupaba DOS de los cinco contadores programables libres. Con ellos
// fuera, el desglose de rellenos por origen sube de 3 mascaras a 5.
static void open_perf_for_thread(ThreadInfo& t) {
    if (t.perf.opened) return;

    int e_rm = 0, e_all = 0;
    bool have_numa = false;

    // EL PAR PRIMERO, SIEMPRE. 0xFF44 y 0xD044 son dos eventos, caben seguro, y de
    // aqui sale ratio_rm EXACTO: los dos van en el mismo grupo, asi que su cociente
    // cubre el mismo intervalo. Es la variable de decision del mecanismo y no puede
    // depender de que quepa el desglose.
    //
    // En la campana 29355 no era asi y salio mal: las seis mascaras colgaban del
    // grupo de ciclos, o sea OCHO eventos en un grupo. Zen 4 tiene 6 contadores
    // programables y el nmi_watchdog ocupa uno, asi que solo hay CINCO libres: el
    // grupo nunca se planifico, 12 096 de 12 096 hilos quedaron en ipc_only, y el
    // scheduler corrio sin poder migrar ni una vez.
    t.perf.all_fills_fd = open_counter(t.tid_linux, PERF_TYPE_RAW,
                                       (0xFFULL << 8) | evento_familia(), -1);
    e_all = (t.perf.all_fills_fd < 0) ? errno : 0;
    t.perf.rm_misses    = open_counter(t.tid_linux, PERF_TYPE_RAW,
                                       (0xD0ULL << 8) | evento_familia(),
                                       t.perf.all_fills_fd);
    e_rm = (t.perf.rm_misses < 0) ? errno : 0;
    have_numa = (t.perf.all_fills_fd >= 0 && t.perf.rm_misses >= 0);

    // El desglose por origen es un EXTRA para informar: se abre en su propio grupo y
    // solo si cabe entero. Si no cabe, se cierra y no pasa nada — ratio_rm sigue en
    // pie y lo unico que se pierde es la figura de trafico por distancia.
    if (usar_mascaras_origen() && tope_origenes() > 0) {
        int lider_fill = -1;
        bool todas = true;
        for (int i = 0; i < tope_origenes(); ++i) {
            const int org = PRIORIDAD_FILL[i];
            t.perf.origen_fd[org] = open_counter(t.tid_linux, PERF_TYPE_RAW,
                                                 fill_config(org), lider_fill);
            if (t.perf.origen_fd[org] < 0) todas = false;
            if (i == 0) lider_fill = t.perf.origen_fd[org];
        }
        if (!todas) {
            for (int i = 0; i < FO_N; ++i)
                if (t.perf.origen_fd[i] >= 0) { close(t.perf.origen_fd[i]);
                                                t.perf.origen_fd[i] = -1; }
            g_mascaras_degradadas = 1;
        }
        if (tope_origenes() < FO_N) g_mascaras_degradadas = 1;
    }

    if (!have_numa) {
        // Sin el par no hay ratio_rm y por tanto no hay ninguna decision posible:
        // el hilo no aporta nada. Antes este caso se marcaba "ipc_only" porque el
        // hilo seguia dando IPC; retirado el IPC, es simplemente un fallo.
        fprintf(stderr,
            "[OMPT][PERF] FALLO abriendo el par de rellenos tid=%d\n"
            "    0xFF44 fd=%-3d %s\n"
            "    0xD044 fd=%-3d %s\n"
            "  → los eventos crudos son específicos de AMD Zen; en otra PMU no existen\n"
            "  → comprobar perf_event_paranoid <= 2 y `perf list | grep -i fills`\n",
            (int)t.tid_linux,
            t.perf.all_fills_fd, e_all ? strerror(e_all) : "ok",
            t.perf.rm_misses,    e_rm  ? strerror(e_rm)  : "ok");
        close_perf(t.perf);
        t.perf.status = PERF_FAILED;
        ++g_perf_failed_threads;
    } else {
        t.perf.status = PERF_FULL;
    }

    t.perf.opened = 1;
    t.has_last    = 0;

    if (g_measuring.load(std::memory_order_acquire)) {
        t.measuring = 1;
        enable_perf(t.perf);
        VLOG("[OMPT][PERF] Contadores ACTIVOS tid=%d (%s)\n",
             (int)t.tid_linux, perf_status_str(t.perf.status));
    } else {
        VLOG("[OMPT][PERF] Contadores pausados (esperando start) tid=%d (%s)\n",
             (int)t.tid_linux, perf_status_str(t.perf.status));
    }
}

// =============================================================================
// upsert_thread — guarda también pthread_handle
//   Solo empareja con entradas VIVAS: Linux recicla los tid, y sin este filtro
//   un hilo nuevo heredaba el bucket y el contador de migraciones de un hilo
//   muerto que tuvo el mismo tid.
// =============================================================================
static int upsert_thread(pid_t tid, int cpu,
                          uint64_t ompt_id, int ompt_type,
                          pthread_t pt) {
    for (int i = 0; i < g_count; ++i) {
        if (g_threads[i].seen && g_threads[i].alive &&
            g_threads[i].tid_linux == tid) {
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
    t.alive          = 1;
    return g_count++;
}

// =============================================================================
// Hilo monitor — mide, decide y marca cada MONITOR_MS
// =============================================================================
static std::atomic<bool> g_monitor_running {false};
static std::atomic<bool> g_finalizing      {false};
static pthread_t         g_monitor_pthread;
static bool              g_monitor_valid = false;

static void* monitor_loop(void*) {
    fprintf(stderr,
        "[OMPT][MON] Monitor (C: integrador con fugas)  período=%dms"
        "  warmup=%d  T=ref+%.2f*MAD  leak=%.2f*MAD  cap=%.2f*MAD  max_mig=%" PRIu64
        "  migracion=%s\n",
        MONITOR_MS, WARMUP_WINDOWS,
        LEAK_MARGIN_SIG, LEAK_RATE_SIG, LEAK_CAP_SIG, MAX_MIGRATIONS,
        g_disable_migration ? "DESACTIVADA" : "activa");

    g_mon_t0 = std::chrono::steady_clock::now();

    while (g_monitor_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(MONITOR_MS));

        const double t_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - g_mon_t0).count();
        const auto tick_t0 = std::chrono::steady_clock::now();

        lock();

        // ── PASE 1: leer contadores y calcular deltas ───────────────────────
        int      ns      = 0;
        uint64_t sum_rm  = 0;
        uint64_t sum_all = 0;

        for (int i = 0; i < g_count; ++i) {
            ThreadInfo& t = g_threads[i];
            if (!t.seen || !t.alive || !t.perf.opened || !t.measuring) continue;
            if (t.migrating) continue;   // rebinding en curso, no tocar su estado

            const bool numa_ok = (t.perf.status == PERF_FULL);

            uint64_t remote = 0, all = 0;
            if (numa_ok) {
                // read_fills devuelve all y remoto con el mismo significado tanto
                // con las seis máscaras como con el par 0xFF44/0xD044: el mecanismo
                // de decisión no se entera de cuál está en uso.
                if (!read_fills(t.perf, all, remote, nullptr)) continue;
            }

            // ── Primera lectura: solo baseline ────────────────────────────
            if (!t.has_last) {
                t.last_rm_misses = remote;
                t.last_all_fills = all;
                t.has_last       = 1;
                continue;
            }

            // Guarda de underflow: los deltas son uint64, así que un contador
            // que retroceda (un PERF_EVENT_IOC_RESET intercalado, o el
            // prctl(PR_TASK_PERF_EVENTS_ENABLE) que hacen los kernels y que el
            // tool no ve) daría la vuelta a ~1.8e19 y envenenaría g_ref durante
            // las ~20 ventanas siguientes. Se resincroniza y se descarta.
            if (numa_ok && (remote < t.last_rm_misses || all < t.last_all_fills)) {
                t.last_rm_misses = remote;
                t.last_all_fills = all;
                ++g_resyncs;
                continue;
            }

            const uint64_t d_rm  = numa_ok ? (remote - t.last_rm_misses) : 0;
            const uint64_t d_all = numa_ok ? (all - t.last_all_fills) : 0;

            t.last_rm_misses = remote;
            t.last_all_fills = all;

            WinSample& s = g_samples[ns++];
            s.idx   = i;
            s.d_rm  = d_rm;
            s.d_all = d_all;
            // -1 = NO MEDIBLE (sin eventos NUMA, o ventana sin fills). Se usa un
            // centinela en vez de 0 porque un ratio de 0 es un valor legítimo:
            // significa "todos los fills fueron locales", que es justo lo que se
            // quiere poder distinguir de "no hay dato".
            s.ratio = (numa_ok && d_all > 0) ? (double)d_rm / (double)d_all : -1.0;
            s.gated = numa_ok && (d_all >= MIN_FILLS);

            if (d_all > 0) ++g_win_with_fills;

            if (s.gated) { sum_rm += d_rm; sum_all += d_all; }
        }

        // ── Sembrar la referencia global si es la primera ventana con datos ─
        // Se siembra con el AGREGADO de la ventana, no con la primera muestra
        // de un hilo arbitrario: esa muestra podía ser atípica y la referencia
        // tardaba ~1/REF_BETA = 20 ventanas en recuperarse.
        if (!g_ref_init && sum_all > 0) {
            g_ref = (double)sum_rm / (double)sum_all;
            double acc = 0.0; int c = 0;
            for (int k = 0; k < ns; ++k)
                if (g_samples[k].gated) { acc += fabs(g_samples[k].ratio - g_ref); ++c; }
            g_dev      = c ? acc / c : 0.0;
            g_ref_init = true;
        }

        // Umbral ÚNICO para toda la ventana. Antes g_ref se actualizaba dentro
        // del bucle por hilo, así que el hilo 0 se comparaba contra la
        // referencia de la ventana anterior y el hilo 127 contra una ya
        // desplazada 127 veces: el veredicto dependía del orden del arreglo.
        const double dev  = (g_dev > DEV_FLOOR) ? g_dev : DEV_FLOOR;
        const double thrT = g_ref + LEAK_MARGIN_SIG * dev;
        const double leak = LEAK_RATE_SIG * dev;
        const double cap  = LEAK_CAP_SIG  * dev;

        // ── PASE 2: evaluar a todos contra el MISMO umbral ──────────────────
        for (int k = 0; k < ns; ++k) {
            WinSample&  s = g_samples[k];
            ThreadInfo& t = g_threads[s.idx];
            int flagged = 0;

            if (s.gated) {
                t.win_count++;

                if (s.ratio > thrT) {
                    t.bucket += (s.ratio - thrT);        // llenar por exceso
                } else {
                    t.bucket -= leak;                    // fuga lenta
                    if (t.bucket < 0.0) t.bucket = 0.0;
                }

                if (!g_disable_migration
                    && t.win_count >= WARMUP_WINDOWS
                    && t.migrations < MAX_MIGRATIONS
                    && !t.needs_migration
                    && !t.migrating
                    && t.bucket > cap) {
                    t.needs_migration = 1;
                    flagged = 1;
                    VLOG("[SCHED][LEAKY] tid=%-6d ratio=%.4f T=%.4f"
                         " bucket=%.4f > cap=%.4f -> PENDIENTE (mig=%" PRIu64 ")\n",
                         (int)t.tid_linux, s.ratio, thrT, t.bucket, cap, t.migrations);
                }
            }

            if (g_window_rows) {
                if (g_window_n < MAX_WINDOW_ROWS) {
                    g_window_rows[g_window_n++] = WindowRow{
                        g_win_idx, t_ms, t.ompt_id, (int)t.tid_linux, t.ompt_type,
                        t.last_cpu, t.numa_node,
                        s.d_rm, s.d_all, s.ratio,
                        t.bucket, thrT, g_ref, g_dev, flagged};
                } else {
                    ++g_window_rows_dropped;
                }
            }

            VLOG("[OMPT][WIN%dms] ompt_id=%-4" PRIu64
                 " tid=%-6d cpu=%3d numa=%d  d_rm=%8" PRIu64
                 "  d_all=%8" PRIu64 "  ratio_rm=%.4f  bucket=%.4f/%.4f\n",
                 MONITOR_MS, t.ompt_id, (int)t.tid_linux, t.last_cpu, t.numa_node,
                 s.d_rm, s.d_all, s.ratio, t.bucket, cap);
        }

        // ── Actualizar la referencia UNA vez, con el agregado de la ventana ─
        if (sum_all > 0) {
            const double win_ratio = (double)sum_rm / (double)sum_all;
            double acc = 0.0; int c = 0;
            for (int k = 0; k < ns; ++k)
                if (g_samples[k].gated) { acc += fabs(g_samples[k].ratio - g_ref); ++c; }
            const double win_mad = c ? acc / c : 0.0;

            g_ref += REF_BETA * (win_ratio - g_ref);
            g_dev += REF_BETA * (win_mad   - g_dev);
        }

        ++g_win_idx;
        unlock();

        const double tick_us = us_since(tick_t0);
        ++g_monitor_ticks;
        g_monitor_us_total += tick_us;
        if (tick_us > g_monitor_us_max) g_monitor_us_max = tick_us;
    }

    fprintf(stderr, "[OMPT][MON] Monitor detenido tras %" PRIu64 " ventanas\n", g_win_idx);
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

    VLOG("[OMPT] thread_begin: tipo=%-22s ompt_id=%" PRIu64
         " tid=%d cpu=%d numa=%d\n",
         thread_type_str(thread_type), uid, (int)tid, cpu,
         cpu_to_numa_node_hwloc(cpu));
}

// Antes este callback solo imprimía. Ahora congela los totales del hilo y CIERRA
// sus fds. Sin esto: (a) se filtraban 4 fds por hilo (512 a 128 hilos, cerca del
// RLIMIT_NOFILE por defecto de 1024); (b) el monitor seguía leyendo los fds de
// hilos muertos y emitía ventanas con deltas 0, que desinflaban los promedios;
// (c) un tid reciclado heredaba el estado del hilo difunto.
static void on_ompt_callback_thread_end(ompt_data_t* thread_data) {
    const pid_t tid = linux_tid();

    lock();
    for (int i = 0; i < g_count; ++i) {
        ThreadInfo& t = g_threads[i];
        if (!t.seen || !t.alive || t.tid_linux != tid) continue;

        if (t.perf.opened) {
            disable_perf(t.perf);
            if (t.perf.status == PERF_FULL) {
                read_fills(t.perf, t.fin_all, t.fin_rm, t.fin_origen);
            }
            t.fin_valid = 1;
            close_perf(t.perf);
        }
        t.alive     = 0;   // seen se mantiene: el hilo debe salir en el resumen
        t.measuring = 0;
        break;
    }
    unlock();

    VLOG("[OMPT] thread_end: ompt_id=%" PRIu64 " tid=%d cpu=%d\n",
         thread_data->value, (int)tid, sched_getcpu());
}

static int on_ompt_callback_control_tool(
    uint64_t command, uint64_t modifier,
    void* arg, const void* codeptr_ra)
{
    (void)modifier; (void)arg; (void)codeptr_ra;
    if (command == omp_control_tool_start) {
        fprintf(stderr, "[OMPT] control_tool: INICIANDO medicion\n");
        g_measuring.store(true, std::memory_order_release);
        lock();
        for (int i = 0; i < g_count; ++i) {
            if (g_threads[i].seen && g_threads[i].alive && g_threads[i].perf.opened) {
                g_threads[i].measuring = 1;
                g_threads[i].has_last  = 0;
                enable_perf(g_threads[i].perf);
            }
        }
        unlock();
    } else if (command == omp_control_tool_pause) {
        fprintf(stderr, "[OMPT] control_tool: PAUSANDO medicion\n");
        g_measuring.store(false, std::memory_order_release);
        lock();
        for (int i = 0; i < g_count; ++i) {
            if (g_threads[i].seen && g_threads[i].alive && g_threads[i].perf.opened) {
                g_threads[i].measuring = 0;
                disable_perf(g_threads[i].perf);
            }
        }
        unlock();
    } else {
        VLOG("[OMPT] control_tool: comando %" PRIu64 " ignorado\n", command);
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
    VLOG("[OMPT] parallel_begin: parallel_id=%" PRIu64 " team_size=%u\n",
         parallel_data->value, requested_team_size);
}

static void on_ompt_callback_parallel_end(
    ompt_data_t* parallel_data,
    ompt_data_t* encountering_task_data,
    int flags, const void* codeptr_ra)
{
    (void)encountering_task_data; (void)flags; (void)codeptr_ra;
    VLOG("[OMPT] parallel_end: parallel_id=%" PRIu64 "\n", parallel_data->value);
}

static void on_ompt_callback_implicit_task(
    ompt_scope_endpoint_t endpoint,
    ompt_data_t*          parallel_data,
    ompt_data_t*          task_data,
    unsigned int          team_size,
    unsigned int          thread_num,
    int                   flags)
{
    (void)flags; (void)team_size;
    if (g_finalizing.load(std::memory_order_acquire)) return;

    if (endpoint != ompt_scope_begin) {
        VLOG("[OMPT] implicit_task_end: parallel_id=%" PRIu64 " thread_num=%u\n",
             parallel_data ? parallel_data->value : 0, thread_num);
        return;
    }

    const auto cb_t0 = std::chrono::steady_clock::now();

    if (g_ompt_get_unique_id)
        task_data->value = g_ompt_get_unique_id();

    pid_t     tid = linux_tid();
    int       cpu = sched_getcpu();
    pthread_t pt  = pthread_self();

    // ── Fase 1: bajo lock, solo stores. Se decide si toca migrar y se reserva
    //            el hilo con la bandera 'migrating'. ─────────────────────────
    int        thread_idx = -1;
    int        mig_target = -1;
    pthread_t  mig_handle = 0;

    lock();
    for (int i = 0; i < g_count; ++i) {
        ThreadInfo& t = g_threads[i];
        if (!t.seen || !t.alive || t.tid_linux != tid) continue;

        t.last_cpu       = cpu;
        t.numa_node      = cpu_to_numa_node_hwloc(cpu);
        t.pthread_handle = pt;   // ← el handle puede cambiar entre regiones

        if (t.needs_migration) {
            t.needs_migration = 0;
            const int target = (t.numa_node == 0) ? 1 : 0;
            if (g_topology_valid && !g_disable_migration &&
                t.numa_node >= 0 && target < g_num_numa_nodes &&
                t.migrations < MAX_MIGRATIONS && pt != 0) {
                t.migrating = 1;          // el monitor saltará este hilo
                thread_idx  = i;
                mig_target  = target;
                mig_handle  = pt;
            }
        }

        if (g_measuring.load(std::memory_order_acquire)
            && t.perf.opened && !t.measuring) {
            t.measuring = 1;
            t.has_last  = 0;
            enable_perf(t.perf);
        }
        break;
    }
    unlock();

    // ── Fase 2: SIN lock. hwloc_set_thread_cpubind es en el fondo un
    //            sched_setaffinity; sostener aquí el spinlock global dejaría a
    //            todo el equipo girando sobre él en cada frontera de región. ──
    if (thread_idx >= 0) {
        const auto mig_t0 = std::chrono::steady_clock::now();

        int rc = hwloc_set_thread_cpubind(
            g_topology, mig_handle, g_node_cpusets[mig_target],
            HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT);

        // Fallback sin STRICT si el kernel no lo soporta
        if (rc != 0)
            rc = hwloc_set_thread_cpubind(
                g_topology, mig_handle, g_node_cpusets[mig_target],
                HWLOC_CPUBIND_THREAD);

        const double mig_us = us_since(mig_t0);

        // ── Fase 3: bajo lock otra vez, solo para confirmar el estado. ──────
        int prev_node = -1;
        uint64_t total_mig = 0;
        lock();
        {
            ThreadInfo& t = g_threads[thread_idx];
            if (rc == 0) {
                prev_node = t.numa_node;
                t.numa_node = mig_target;
                t.migrations++;
                t.has_last = 0;     // el baseline de deltas ya no vale
                t.bucket   = 0.0;
                if (t.first_migration_win < 0)
                    t.first_migration_win = (int64_t)g_win_idx;
                total_mig = t.migrations;
            } else {
                t.mig_failures++;   // los fallos antes no se contaban en ningún sitio
            }
            t.migrating = 0;
            g_migration_us_total += mig_us;
        }
        unlock();

        if (rc == 0)
            VLOG("[SCHED] MIGRACION tid=%-6d nodo %d -> nodo %d"
                 " (total=%" PRIu64 ", %.1f us)\n",
                 (int)tid, prev_node, mig_target, total_mig, mig_us);
        else
            VLOG("[SCHED] FALLO migracion tid=%d rc=%d (%.1f us)\n",
                 (int)tid, rc, mig_us);
    }

    g_callback_calls.fetch_add(1, std::memory_order_relaxed);
    g_callback_ns_total.fetch_add(
        (uint64_t)(us_since(cb_t0) * 1000.0), std::memory_order_relaxed);

    VLOG("[OMPT] implicit_task_begin: parallel_id=%" PRIu64 " thread_num=%u\n",
         parallel_data ? parallel_data->value : 0, thread_num);
}

// =============================================================================
// Volcado de los CSV (solo en finalize; nunca en el camino caliente)
// =============================================================================
static void flush_window_csv() {
    if (!g_window_csv || !g_window_rows) return;
    for (size_t k = 0; k < g_window_n; ++k) {
        const WindowRow& r = g_window_rows[k];
        fprintf(g_window_csv,
            "%s,%" PRIu64 ",%.3f,%" PRIu64 ",%d,%s,%d,%d,"
            "%" PRIu64 ",%" PRIu64 ","
            "%.6f,%.6f,%.6f,%.6f,%.6f,%d\n",
            g_tag, r.win_idx, r.t_ms, r.ompt_id, r.tid,
            thread_type_str((ompt_thread_t)r.ompt_type), r.cpu, r.numa,
            r.d_rm, r.d_all, r.ratio, r.bucket, r.thr_T, r.ref, r.dev, r.flagged);
    }
    fflush(g_window_csv);
    fprintf(stderr, "[OMPT] CSV de ventanas: %zu filas volcadas (%" PRIu64 " descartadas)\n",
            g_window_n, g_window_rows_dropped);
}

static void write_overhead_csv() {
    if (!g_overhead_csv) return;
    const uint64_t calls = g_callback_calls.load(std::memory_order_relaxed);
    const double   cb_us = g_callback_ns_total.load(std::memory_order_relaxed) / 1000.0;
    fprintf(g_overhead_csv,
        "%s,%" PRIu64 ",%.3f,%.3f,%" PRIu64 ",%.3f,%.3f,%d,%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
        g_tag,
        g_monitor_ticks, g_monitor_us_total, g_monitor_us_max,
        calls, cb_us, g_migration_us_total,
        g_count, g_resyncs, g_mux_scaled_reads, g_window_rows_dropped,
        g_perf_failed_threads, g_perf_partial_threads,
        g_win_with_fills,
        g_reads_ok, g_reads_never_scheduled);
    fflush(g_overhead_csv);
}

// =============================================================================
// Initialize / Finalize
// =============================================================================
static bool env_flag(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

static int ompt_initialize(
    ompt_function_lookup_t lookup,
    int initial_device_num,
    ompt_data_t* tool_data)
{
    (void)initial_device_num; (void)tool_data;

    g_verbose           = env_flag("OMPT_VERBOSE");
    g_disable_migration = env_flag("OMPT_DISABLE_MIGRATION");

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
                  "tag,win_idx,t_ms,ompt_id,tid,thread_type,cpu,numa,"
                  "d_rm,d_all,ratio_rm,bucket,thr_T,g_ref,g_dev,flagged",
                  &g_window_csv);
    open_csv_file(getenv("OMPT_SUMMARY_CSV"),
                  "tag,tid,ompt_id,thread_type,last_cpu,numa_node,migrations,mig_failures,"
                  "remote_fills,all_fills,ratio_rm,"
                  "windows_counted,first_migration_win,perf_status,"
                  "fill_l2,fill_l3_ccd,fill_ccd_vecino,fill_dram_local,"
                  "fill_far_cache,fill_far_dram,mascaras,familia",
                  &g_summary_csv);
    open_csv_file(getenv("OMPT_OVERHEAD_CSV"),
                  "tag,monitor_ticks,monitor_us_total,monitor_us_max,callback_calls,"
                  "callback_us_total,migration_us_total,threads_tracked,resyncs,"
                  "mux_scaled_reads,window_rows_dropped,perf_failed_threads,"
                  "perf_partial_threads,win_with_fills,"
                  "reads_ok,reads_never_scheduled",
                  &g_overhead_csv);

    if (g_window_csv) {
        g_window_rows = (WindowRow*)malloc(MAX_WINDOW_ROWS * sizeof(WindowRow));
        if (!g_window_rows)
            fprintf(stderr, "[OMPT] WARNING: sin memoria para el buffer de ventanas "
                            "(%zu MB); no se registrarán ventanas\n",
                    (MAX_WINDOW_ROWS * sizeof(WindowRow)) >> 20);
    }

    fprintf(stderr, "[OMPT] ompt_initialize (OPCIÓN C: integrador con fugas)"
                    "  verbose=%d  migracion=%s\n",
            (int)g_verbose, g_disable_migration ? "DESACTIVADA" : "activa");

    // ── 1. Function pointers del runtime ────────────────────────────────
    g_ompt_set_callback     = (ompt_set_callback_t)  lookup("ompt_set_callback");
    g_ompt_get_unique_id    = (ompt_get_unique_id_t) lookup("ompt_get_unique_id");
    g_ompt_get_num_procs    = (ompt_get_num_procs_t) lookup("ompt_get_num_procs");

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

        // El mecanismo migra "al nodo contrario", lo que solo tiene sentido con
        // 2 nodos. En una máquina con más, las CPU de los nodos 2..n-1 mapean a
        // numa_node = -1 y las migraciones quedaban desactivadas EN SILENCIO.
        if (nb > MAX_NUMA_NODES)
            fprintf(stderr,
                "[HWLOC] AVISO: la máquina tiene %d nodos NUMA y este scheduler "
                "solo maneja %d (migra al nodo contrario). Los hilos fuera de los "
                "nodos 0/1 no podrán migrar.\n", nb, MAX_NUMA_NODES);
        if (nb < 2)
            fprintf(stderr,
                "[HWLOC] AVISO: solo %d nodo NUMA visible; no hay destino de "
                "migración posible. El tool medirá pero nunca migrará.\n", nb);

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

        fprintf(stderr, "[HWLOC] Nodos NUMA detectados: %d (usados: %d)\n",
                nb, g_num_numa_nodes);
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

    flush_window_csv();

    fprintf(stderr, "[OMPT] ===== Tabla final: %d hilos =====\n", g_count);
    uint64_t total_migrations = 0;

    // Con el lock tomado: un hilo trabajador que ya hubiera pasado el chequeo de
    // g_finalizing puede seguir dentro de la fase 3 de una migración, escribiendo
    // en g_threads[] mientras aquí se leen y se cierran sus fds. Aquí ya no hay
    // nada que optimizar, así que sostenerlo durante el volcado sale gratis.
    lock();
    for (int i = 0; i < g_count; ++i) {
        ThreadInfo& t = g_threads[i];
        if (!t.seen) continue;

        // Los hilos que ya terminaron congelaron sus totales en thread_end.
        if (!t.fin_valid && t.perf.opened) {
            disable_perf(t.perf);
            if (t.perf.status == PERF_FULL) {
                read_fills(t.perf, t.fin_all, t.fin_rm, t.fin_origen);
            }
            t.fin_valid = 1;
            close_perf(t.perf);
        }

        // El ratio agregado por hilo: antes all_fills se leía y se descartaba,
        // así que esta métrica —central para el objetivo del proyecto— no era
        // calculable desde el CSV de resumen.
        const double ratio = (t.fin_all > 0) ? (double)t.fin_rm / (double)t.fin_all : -1.0;
        total_migrations += t.migrations;

        if (g_summary_csv) {
            fprintf(g_summary_csv,
                "%s,%d,%" PRIu64 ",%s,%d,%d,%" PRIu64 ",%" PRIu64 ","
                "%" PRIu64 ",%" PRIu64 ",%.6f,%d,%" PRId64 ",%s",
                g_tag, (int)t.tid_linux, t.ompt_id,
                thread_type_str((ompt_thread_t)t.ompt_type),
                t.last_cpu, t.numa_node,
                t.migrations, t.mig_failures,
                t.fin_rm, t.fin_all, ratio,
                t.win_count, t.first_migration_win,
                perf_status_str(t.perf.status));
            // Desglose por origen: las mismas seis columnas que emite
            // perf_region.hpp en las configuraciones sin tool, para que el script
            // de campaña agregue los dos caminos con el mismo código.
            for (int k = 0; k < FO_N; ++k)
                fprintf(g_summary_csv, ",%" PRIu64, t.fin_origen[k]);
            // 'mascaras' = cuantas del desglose se midieron de verdad. Con el par
            // exacto siempre abierto, 0 aqui NO significa que falte ratio_rm.
            fprintf(g_summary_csv, ",%d,%s\n",
                    usar_mascaras_origen() ? tope_origenes() : 0,
                    familia_es_demanda() ? "dmnd" : "any");
        }

        fprintf(stderr,
            "[OMPT] tid=%-6d ompt_id=%-4" PRIu64 " tipo=%-22s last_cpu=%3d numa=%d"
            " mig=%" PRIu64 " fallos=%" PRIu64 " perf=%s ratio_rm=%.4f\n",
            (int)t.tid_linux, t.ompt_id,
            thread_type_str((ompt_thread_t)t.ompt_type),
            t.last_cpu, t.numa_node, t.migrations, t.mig_failures,
            perf_status_str(t.perf.status), ratio);
    }
    unlock();
    if (g_summary_csv) fflush(g_summary_csv);

    write_overhead_csv();

    fprintf(stderr,
        "[OMPT] ===== Overhead intrínseco =====\n"
        "  monitor: %" PRIu64 " ticks, %.1f us total (max %.1f us/tick)\n"
        "  callbacks implicit_task: %" PRIu64 ", %.1f us total\n"
        "  migraciones: %" PRIu64 " (%.1f us total)\n"
        "  resyncs=%" PRIu64 "  lecturas escaladas por multiplexado=%" PRIu64 "\n"
        "  hilos con perf fallido=%d  parcial=%d\n"
        "  ventanas con fills=%" PRIu64 "\n",
        g_monitor_ticks, g_monitor_us_total, g_monitor_us_max,
        g_callback_calls.load(), g_callback_ns_total.load() / 1000.0,
        total_migrations, g_migration_us_total,
        g_resyncs, g_mux_scaled_reads,
        g_perf_failed_threads, g_perf_partial_threads,
        g_win_with_fills);

    // El grupo de eventos nunca llegó a planificarse: sin esto la corrida sale
    // completamente muda (CSV vacíos, cero migraciones, ningún error).
    if (g_reads_never_scheduled > 0 && g_reads_ok == 0)
        fprintf(stderr,
            "\n[OMPT] *** AVISO GRAVE ***\n"
            "  Las %" PRIu64 " lecturas de contadores devolvieron time_running=0:\n"
            "  el GRUPO de 4 eventos nunca llegó a entrar en la PMU (no caben, o\n"
            "  hay otro perf compitiendo por los registros). No se midió NADA en\n"
            "  toda la corrida. Revisar que no haya un 'perf stat' simultáneo.\n\n",
            g_reads_never_scheduled);
    else if (g_reads_never_scheduled > 0)
        fprintf(stderr,
            "[OMPT] AVISO: %" PRIu64 " de %" PRIu64 " lecturas sin planificar "
            "(la PMU está sobresuscrita en parte de la corrida)\n",
            g_reads_never_scheduled, g_reads_never_scheduled + g_reads_ok);

    // Fallo silencioso clásico: los eventos abrieron pero la PMU no los conoce.
    // El canario era "ventanas con ciclos y ninguna con fills"; retirado el IPC, es
    // "lecturas que tuvieron exito y ninguna ventana con fills", que ademas apunta
    // mas directamente a la causa: el fd es valido y el contador devuelve cero.
    if (g_reads_ok > 0 && g_win_with_fills == 0)
        fprintf(stderr,
            "\n[OMPT] *** AVISO GRAVE ***\n"
            "  %" PRIu64 " lecturas de contador con exito y NINGUNA ventana con data\n"
            "  cache fills. Los eventos crudos 0xD044/0xFF44 abrieron pero cuentan\n"
            "  siempre 0: esta PMU no los implementa (son de AMD Zen). ratio_rm no\n"
            "  existe y el scheduler NO PUEDE decidir migraciones. Los resultados de\n"
            "  localidad de esta corrida no son válidos.\n\n", g_reads_ok);

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

    if (g_window_csv)   { fclose(g_window_csv);   g_window_csv   = nullptr; }
    if (g_summary_csv)  { fclose(g_summary_csv);  g_summary_csv  = nullptr; }
    if (g_overhead_csv) { fclose(g_overhead_csv); g_overhead_csv = nullptr; }
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
