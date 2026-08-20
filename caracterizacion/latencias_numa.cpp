// latencias_numa.cpp — Caracterización de latencias de la jerarquía de memoria
//
// Sustituye a latenciaMemoria.cpp, que medía un único punto de 128 MiB sin control
// de afinidad ni de nodo NUMA, usaba std::random_shuffle (eliminado en C++17) y
// reportaba ticks de TSC como si fueran ciclos de núcleo.
//
// Mide los cinco niveles que necesita el proyecto en exadell (2x AMD EPYC 9554,
// Zen 4): L1d, L2, L3 del propio chiplet, L3 de OTRO chiplet del mismo nodo, y
// memoria remota. Ningún método único los cubre todos, así que hay tres modos:
//
//   Modo A (--curva)    Pointer chasing de un hilo, barrido de tamaños, memoria
//                       fijada al nodo local o al remoto. Da L1 / L2 / L3 propia /
//                       DRAM local / DRAM remota como mesetas de la curva.
//                       NO alcanza la L3 de otro chiplet: los datos de un hilo
//                       solo se cachean en su propio CCD.
//
//   Modo B (--c2c)      Ping-pong sobre una línea compartida entre dos hilos
//                       fijados. Mide TRANSFERENCIA DE PROPIEDAD de la línea
//                       (coherencia). Con --matriz recorre los 128x128 núcleos
//                       físicos y produce el mapa de calor que hace visible la
//                       estructura de chiplets.
//
//   Modo C (--victima)  Un productor en la CPU A escribe un buffer que cabe en la
//                       L3 de su CCD; un consumidor en la CPU B lo recorre. Mide
//                       LATENCIA DE LECTURA desde la caché ajena, que es lo que
//                       hacen de verdad los kernels del proyecto, a diferencia del
//                       ping-pong atómico del modo B.
//
// La topología NO se asume: se descubre con hwloc y se imprime, de modo que la
// hipótesis "el núcleo físico c está en el CCD c/8" queda verificada o refutada
// antes de usarla en el diseño experimental.
//
// Compilación:
//   clang++ -std=c++17 -O2 -pthread caracterizacion/latencias_numa.cpp \
//       -o latencias_numa \
//       -I${HWLOC_INCLUDE:-/opt/ohpc/pub/libs/hwloc/include} \
//       -L${HWLOC_LIB:-/opt/ohpc/pub/libs/hwloc/lib} -lhwloc
//
// Nota sobre -O2: el bucle de persecución de punteros NO debe vectorizarse ni
// desenrollarse de forma que rompa la dependencia serie. Es imposible por
// construcción (cada iteración depende del puntero de la anterior), pero el
// resultado se consume en un sumidero global para que el compilador no lo elimine.

#include <hwloc.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Utilidades básicas
// ─────────────────────────────────────────────────────────────────────────────

// Reloj monótono en nanosegundos. Se usa clock_gettime y NO __rdtsc porque el TSC
// avanza a frecuencia constante, distinta de la frecuencia del núcleo: reportar
// ticks de TSC como "ciclos" era el error de la herramienta anterior.
static inline double now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

static inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

// Sumidero: impide que el compilador elimine los bucles de medida.
static volatile std::uintptr_t g_sumidero = 0;

// Un nodo ocupa exactamente una línea de caché para que el prefetcher no traiga
// varios de golpe.
struct Nodo {
    Nodo* next;
    char  pad[56];
};
static_assert(sizeof(Nodo) == 64, "Nodo debe ocupar una linea de cache de 64 B");

static constexpr std::size_t LINEA = 64;

// ─────────────────────────────────────────────────────────────────────────────
// Descubrimiento de topología (no se asume nada)
// ─────────────────────────────────────────────────────────────────────────────

struct Topologia {
    hwloc_topology_t t{};

    int n_nucleos = 0;   // núcleos FÍSICOS
    int n_ccd     = 0;   // instancias de L3 (un CCD por L3 en Zen 4)
    int n_nodos   = 0;   // nodos NUMA

    // Indexado por núcleo lógico de hwloc (0..n_nucleos-1)
    std::vector<int> pu_principal;  // índice OS del primer PU del núcleo
    std::vector<int> pu_hermano;    // índice OS del hermano SMT (-1 si no hay)
    std::vector<int> ccd_de;        // índice lógico de la L3 que lo contiene
    std::vector<int> nodo_de;       // nodo NUMA

    std::size_t l1d_bytes = 0;
    std::size_t l2_bytes  = 0;
    std::size_t l3_bytes  = 0;
};

static bool descubrir_topologia(Topologia& topo) {
    if (hwloc_topology_init(&topo.t) != 0) {
        std::fprintf(stderr, "[ERROR] hwloc_topology_init fallo\n");
        return false;
    }
    if (hwloc_topology_load(topo.t) != 0) {
        std::fprintf(stderr, "[ERROR] hwloc_topology_load fallo\n");
        return false;
    }

    topo.n_nucleos = hwloc_get_nbobjs_by_type(topo.t, HWLOC_OBJ_CORE);
    topo.n_ccd     = hwloc_get_nbobjs_by_type(topo.t, HWLOC_OBJ_L3CACHE);
    topo.n_nodos   = hwloc_get_nbobjs_by_type(topo.t, HWLOC_OBJ_NUMANODE);

    if (topo.n_nucleos <= 0) {
        std::fprintf(stderr, "[ERROR] hwloc no reporta nucleos\n");
        return false;
    }
    if (topo.n_nodos <= 0) topo.n_nodos = 1;

    topo.pu_principal.assign(topo.n_nucleos, -1);
    topo.pu_hermano.assign(topo.n_nucleos, -1);
    topo.ccd_de.assign(topo.n_nucleos, -1);
    topo.nodo_de.assign(topo.n_nucleos, 0);

    for (int i = 0; i < topo.n_nucleos; ++i) {
        hwloc_obj_t nucleo = hwloc_get_obj_by_type(topo.t, HWLOC_OBJ_CORE, i);
        if (!nucleo) continue;

        // PUs del núcleo (hermanos SMT)
        std::vector<int> pus;
        hwloc_obj_t pu = nullptr;
        while ((pu = hwloc_get_next_obj_inside_cpuset_by_type(
                    topo.t, nucleo->cpuset, HWLOC_OBJ_PU, pu)) != nullptr) {
            pus.push_back(static_cast<int>(pu->os_index));
        }
        std::sort(pus.begin(), pus.end());
        if (!pus.empty()) topo.pu_principal[i] = pus[0];
        if (pus.size() > 1) topo.pu_hermano[i] = pus[1];

        // CCD = instancia de L3 que contiene al núcleo
        hwloc_obj_t l3 = hwloc_get_ancestor_obj_by_type(topo.t, HWLOC_OBJ_L3CACHE, nucleo);
        if (l3) {
            topo.ccd_de[i] = static_cast<int>(l3->logical_index);
            if (topo.l3_bytes == 0) topo.l3_bytes = l3->attr->cache.size;
        }

        // Nodo NUMA
        if (nucleo->nodeset && !hwloc_bitmap_iszero(nucleo->nodeset)) {
            topo.nodo_de[i] = hwloc_bitmap_first(nucleo->nodeset);
        }

        // Tamaños de L1d y L2 (del primer núcleo que los reporte)
        if (topo.l2_bytes == 0) {
            hwloc_obj_t l2 = hwloc_get_ancestor_obj_by_type(topo.t, HWLOC_OBJ_L2CACHE, nucleo);
            if (l2) topo.l2_bytes = l2->attr->cache.size;
        }
        if (topo.l1d_bytes == 0) {
            hwloc_obj_t l1 = hwloc_get_ancestor_obj_by_type(topo.t, HWLOC_OBJ_L1CACHE, nucleo);
            if (l1) topo.l1d_bytes = l1->attr->cache.size;
        }
    }
    return true;
}

static void imprimir_topologia(const Topologia& topo) {
    std::printf("\n=== Topologia descubierta (hwloc) ===\n");
    std::printf("Nucleos fisicos : %d\n", topo.n_nucleos);
    std::printf("Instancias L3   : %d  (un CCD por L3 en Zen 4)\n", topo.n_ccd);
    std::printf("Nodos NUMA      : %d\n", topo.n_nodos);
    std::printf("L1d por nucleo  : %zu KiB\n", topo.l1d_bytes / 1024);
    std::printf("L2  por nucleo  : %zu KiB\n", topo.l2_bytes / 1024);
    std::printf("L3  por CCD     : %zu MiB\n", topo.l3_bytes / (1024 * 1024));

    // Verificación explícita de la hipótesis "nucleo c -> CCD c/8": si falla, todo
    // el diseño experimental que dependa de ella hay que rehacerlo.
    int por_ccd = (topo.n_ccd > 0) ? topo.n_nucleos / topo.n_ccd : 0;
    bool hipotesis_ok = (por_ccd > 0);
    for (int i = 0; i < topo.n_nucleos && hipotesis_ok; ++i) {
        if (topo.ccd_de[i] != i / por_ccd) hipotesis_ok = false;
    }
    std::printf("Nucleos por CCD : %d\n", por_ccd);
    std::printf("Hipotesis 'nucleo c esta en el CCD c/%d': %s\n",
                por_ccd, hipotesis_ok ? "SE CUMPLE" : "NO SE CUMPLE (revisar el diseno)");

    std::printf("\nMapa nucleo -> (PU, hermano SMT, CCD, nodo)  [primeros 16 y ultimos 4]\n");
    for (int i = 0; i < topo.n_nucleos; ++i) {
        if (i >= 16 && i < topo.n_nucleos - 4) continue;
        std::printf("  nucleo %3d -> PU %3d  SMT %3d  CCD %2d  nodo %d\n",
                    i, topo.pu_principal[i], topo.pu_hermano[i],
                    topo.ccd_de[i], topo.nodo_de[i]);
    }
    std::printf("\n");
}

// Fija el hilo actual al PU con el índice de OS dado.
static bool fijar_hilo_a_pu(const Topologia& topo, int pu_os_index) {
    hwloc_obj_t pu = hwloc_get_pu_obj_by_os_index(topo.t, pu_os_index);
    if (!pu) return false;
    hwloc_cpuset_t set = hwloc_bitmap_dup(pu->cpuset);
    hwloc_bitmap_singlify(set);
    int rc = hwloc_set_cpubind(topo.t, set, HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT);
    hwloc_bitmap_free(set);
    return rc == 0;
}

// Reserva memoria fijada (MPOL_BIND) al nodo NUMA indicado. STRICT para que falle
// en vez de caer silenciosamente en otro nodo, que arruinaría la medida.
//
// Se pide de más y se alinea a 2 MiB para poder activar páginas enormes
// transparentes (THP). Sin ellas, un recorrido aleatorio de unos pocos MiB toca
// cientos de páginas de 4 KiB, la TLB no da abasto y CADA acceso paga un recorrido
// de tablas: la curva salta a latencia de DRAM mucho antes de que se acabe la L3 y
// el escalón de la L3 desaparece. Medido en local: con 4 KiB el salto a ~158 ns
// ocurría a 3 MiB, con una L3 de 12 MiB medio vacía.
struct Reserva {
    void*       bruto = nullptr;   // lo que hay que devolver a hwloc
    void*       datos = nullptr;   // puntero alineado y utilizable
    std::size_t bytes_bruto = 0;
    bool        thp = false;
};

static constexpr std::size_t ALINEACION_THP = 2 * 1024 * 1024;

static Reserva reservar_en_nodo(const Topologia& topo, std::size_t bytes, int nodo,
                                bool usar_thp) {
    Reserva r;
    r.bytes_bruto = bytes + ALINEACION_THP;

    hwloc_nodeset_t ns = hwloc_bitmap_alloc();
    hwloc_bitmap_only(ns, static_cast<unsigned>(nodo));
    r.bruto = hwloc_alloc_membind(topo.t, r.bytes_bruto, ns, HWLOC_MEMBIND_BIND,
                                  HWLOC_MEMBIND_BYNODESET | HWLOC_MEMBIND_STRICT);
    hwloc_bitmap_free(ns);

    if (!r.bruto) {
        std::fprintf(stderr, "[ERROR] no se pudo reservar %zu bytes en el nodo %d\n",
                     r.bytes_bruto, nodo);
        return r;
    }

    std::uintptr_t dir = reinterpret_cast<std::uintptr_t>(r.bruto);
    std::uintptr_t alineada = (dir + ALINEACION_THP - 1) & ~(ALINEACION_THP - 1);
    r.datos = reinterpret_cast<void*>(alineada);

    if (usar_thp) {
#ifdef MADV_HUGEPAGE
        r.thp = (madvise(r.datos, bytes, MADV_HUGEPAGE) == 0);
#endif
    } else {
#ifdef MADV_NOHUGEPAGE
        madvise(r.datos, bytes, MADV_NOHUGEPAGE);
#endif
        r.thp = false;
    }
    return r;
}

static void liberar(const Topologia& topo, Reserva& r) {
    if (r.bruto) hwloc_free(topo.t, r.bruto, r.bytes_bruto);
    r.bruto = r.datos = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Motor de medida común a los tres modos
// ─────────────────────────────────────────────────────────────────────────────

// Construye una cadena cíclica aleatoria sobre los primeros n_nodos elementos.
// Semilla fija: la caracterizacion tiene que ser reproducible.
static void construir_cadena(Nodo* base, std::size_t n_nodos, std::uint64_t semilla) {
    std::vector<std::uint32_t> idx(n_nodos);
    std::iota(idx.begin(), idx.end(), 0u);

    std::mt19937_64 gen(semilla);
    // std::shuffle, no std::random_shuffle: este ultimo se elimino en C++17 y solo
    // compilaba porque libstdc++ aun lo arrastra como obsoleto.
    std::shuffle(idx.begin(), idx.end(), gen);

    for (std::size_t i = 0; i + 1 < n_nodos; ++i) {
        base[idx[i]].next = &base[idx[i + 1]];
    }
    base[idx[n_nodos - 1]].next = &base[idx[0]];  // cerrar el ciclo
}

// El bucle de persecución. noinline para que quede aislado y sea fácil de inspeccionar.
__attribute__((noinline))
static Nodo* perseguir(Nodo* p, std::uint64_t saltos) {
    for (std::uint64_t i = 0; i < saltos; ++i) p = p->next;
    return p;
}

struct Muestra {
    double ns_min = 0.0;
    double ns_media = 0.0;
    double ns_p95 = 0.0;
    std::uint64_t saltos = 0;
};

static Muestra resumir(std::vector<double>& ns_por_acceso, std::uint64_t saltos) {
    Muestra m;
    m.saltos = saltos;
    if (ns_por_acceso.empty()) return m;
    std::sort(ns_por_acceso.begin(), ns_por_acceso.end());
    m.ns_min = ns_por_acceso.front();
    double suma = 0.0;
    for (double v : ns_por_acceso) suma += v;
    m.ns_media = suma / static_cast<double>(ns_por_acceso.size());
    std::size_t k = static_cast<std::size_t>(0.95 * (ns_por_acceso.size() - 1) + 0.5);
    m.ns_p95 = ns_por_acceso[k];
    return m;
}

// Mide la latencia media por acceso persiguiendo punteros. Calibra el número de
// saltos para que cada repetición dure ~objetivo_ms, de modo que un punto de L1 no
// tarde lo mismo que uno de DRAM remota.
static Muestra medir_persecucion(Nodo* inicio, int repeticiones, double objetivo_ms) {
    // Calentamiento + calibración
    Nodo* p = perseguir(inicio, 200000);
    g_sumidero = reinterpret_cast<std::uintptr_t>(p);

    double t0 = now_ns();
    p = perseguir(p, 200000);
    double t1 = now_ns();
    g_sumidero = reinterpret_cast<std::uintptr_t>(p);

    double ns_estimado = (t1 - t0) / 200000.0;
    if (ns_estimado < 0.3) ns_estimado = 0.3;
    std::uint64_t saltos =
        static_cast<std::uint64_t>((objetivo_ms * 1e6) / ns_estimado);
    if (saltos < 100000) saltos = 100000;

    std::vector<double> muestras;
    muestras.reserve(repeticiones);
    for (int r = 0; r < repeticiones; ++r) {
        double a = now_ns();
        p = perseguir(p, saltos);
        double b = now_ns();
        g_sumidero = reinterpret_cast<std::uintptr_t>(p);
        muestras.push_back((b - a) / static_cast<double>(saltos));
    }
    return resumir(muestras, saltos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Salida CSV
// ─────────────────────────────────────────────────────────────────────────────

static FILE* g_csv = nullptr;

static void csv_abrir(const std::string& ruta) {
    bool nuevo = true;
    if (FILE* f = std::fopen(ruta.c_str(), "r")) {
        std::fseek(f, 0, SEEK_END);
        nuevo = (std::ftell(f) == 0);
        std::fclose(f);
    }
    g_csv = std::fopen(ruta.c_str(), "a");
    if (!g_csv) {
        std::fprintf(stderr, "[ERROR] no se pudo abrir %s\n", ruta.c_str());
        return;
    }
    if (nuevo) {
        std::fprintf(g_csv,
                     "modo,clase,thp,bytes,nivel_esperado,cpu_mide,cpu_calienta,"
                     "nodo_mem,saltos,ns_min,ns_media,ns_p95\n");
    }
}

static void csv_fila(const char* modo, const char* clase, int thp, std::size_t bytes,
                     const char* nivel, int cpu_mide, int cpu_calienta, int nodo_mem,
                     const Muestra& m) {
    if (!g_csv) return;
    std::fprintf(g_csv, "%s,%s,%d,%zu,%s,%d,%d,%d,%llu,%.4f,%.4f,%.4f\n",
                 modo, clase, thp, bytes, nivel, cpu_mide, cpu_calienta, nodo_mem,
                 static_cast<unsigned long long>(m.saltos),
                 m.ns_min, m.ns_media, m.ns_p95);
    std::fflush(g_csv);
}

// Etiqueta del nivel que se espera estar midiendo para un tamaño dado. Es una
// expectativa declarada, no una medida: sirve para leer la curva, y si la curva la
// desmiente, la que manda es la curva.
static const char* nivel_esperado(std::size_t bytes, const Topologia& topo) {
    if (bytes <= topo.l1d_bytes) return "L1d";
    if (bytes <= topo.l2_bytes)  return "L2";
    if (bytes <= topo.l3_bytes)  return "L3_propia";
    return "DRAM";
}

// ─────────────────────────────────────────────────────────────────────────────
// MODO A — curva de latencia frente al tamaño del working set
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::size_t> tamanos_barrido() {
    // Pasos de ~1.5x, con puntos extra alrededor de los limites de cache de Zen 4
    // (32 KiB, 1 MiB, 32 MiB) para que los escalones se vean con nitidez.
    static const std::size_t K = 1024, M = 1024 * 1024;
    return {
        8*K, 12*K, 16*K, 24*K, 28*K, 32*K, 40*K, 48*K, 64*K, 96*K,
        128*K, 192*K, 256*K, 384*K, 512*K, 768*K,
        1*M, 1536*K, 2*M, 3*M, 4*M, 6*M, 8*M, 12*M, 16*M, 24*M, 28*M,
        32*M, 40*M, 48*M, 64*M, 96*M, 128*M, 192*M, 256*M, 384*M, 512*M,
        768*M, 1024*M, 1536*M, 2048*M
    };
}

static void modo_curva(const Topologia& topo, int repeticiones, double objetivo_ms,
                       std::uint64_t semilla, const std::vector<bool>& variantes_thp) {
    std::printf("=== MODO A: curva de latencia frente al tamano ===\n");

    const auto tamanos = tamanos_barrido();
    const std::size_t bytes_max = tamanos.back();

    // El hilo que mide vive en el primer núcleo del nodo 0.
    const int nucleo_medida = 0;
    const int cpu_medida = topo.pu_principal[nucleo_medida];
    const int nodo_local = topo.nodo_de[nucleo_medida];

    if (!fijar_hilo_a_pu(topo, cpu_medida)) {
        std::fprintf(stderr, "[ERROR] no se pudo fijar el hilo a la CPU %d\n", cpu_medida);
        return;
    }

    // Un nodo remoto distinto del local; si la maquina tuviera un solo nodo, se
    // omite la mitad remota del barrido.
    int nodo_remoto = -1;
    for (int n = 0; n < topo.n_nodos; ++n) {
        if (n != nodo_local) { nodo_remoto = n; break; }
    }

    struct Colocacion { const char* nombre; int nodo; };
    std::vector<Colocacion> colocaciones = {{"local", nodo_local}};
    if (nodo_remoto >= 0) colocaciones.push_back({"remoto", nodo_remoto});

    for (bool thp : variantes_thp) {
        for (const auto& col : colocaciones) {
            // Se reserva una sola vez el buffer mas grande y se reutiliza para cada
            // tamano: reservar 2 GiB en cada punto dominaria el tiempo de ejecucion.
            Reserva r = reservar_en_nodo(topo, bytes_max, col.nodo, thp);
            if (!r.datos) continue;
            Nodo* base = static_cast<Nodo*>(r.datos);

            // Primer toque desde el hilo que mide, para que las paginas existan y,
            // con THP pedido, para que el nucleo las promocione a 2 MiB.
            std::memset(r.datos, 0, bytes_max);

            if (thp && !r.thp) {
                std::printf("  [aviso] madvise(MADV_HUGEPAGE) fallo; la curva "
                            "llevara fallos de TLB\n");
            }

            for (std::size_t bytes : tamanos) {
                std::size_t n = bytes / sizeof(Nodo);
                if (n < 2) continue;

                construir_cadena(base, n, semilla);
                Muestra m = medir_persecucion(&base[0], repeticiones, objetivo_ms);

                const char* nivel = nivel_esperado(bytes, topo);
                csv_fila("A", col.nombre, thp ? 1 : 0, bytes, nivel,
                         cpu_medida, -1, col.nodo, m);

                std::printf("  [A/%-6s/thp=%d] %8zu KiB  %-9s  "
                            "min %7.2f ns   media %7.2f ns\n",
                            col.nombre, thp ? 1 : 0, bytes / 1024, nivel,
                            m.ns_min, m.ns_media);
            }
            liberar(topo, r);
        }
    }
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// MODO B — latencia núcleo a núcleo (ping-pong)
// ─────────────────────────────────────────────────────────────────────────────

struct alignas(64) Celda {
    std::atomic<std::uint64_t> v{0};
    char pad[64 - sizeof(std::atomic<std::uint64_t>)];
};

// Devuelve ns por transferencia entre las CPUs a y b.
static double ping_pong(const Topologia& topo, int cpu_a, int cpu_b,
                        std::uint64_t rondas) {
    // La celda se reserva alineada a linea; su nodo no se fija a proposito, porque
    // lo que se mide es la transferencia entre cachés, no el acceso a DRAM.
    void* bruto = nullptr;
    if (posix_memalign(&bruto, 64, sizeof(Celda)) != 0 || !bruto) return -1.0;
    Celda* c = new (bruto) Celda();
    c->v.store(0, std::memory_order_relaxed);

    std::atomic<bool> listo{false};
    double ns = -1.0;

    std::thread responde([&]() {
        fijar_hilo_a_pu(topo, cpu_b);
        listo.store(true, std::memory_order_release);
        for (std::uint64_t r = 0; r < rondas; ++r) {
            const std::uint64_t espera = 2 * r + 1;
            while (c->v.load(std::memory_order_acquire) != espera) cpu_relax();
            c->v.store(2 * r + 2, std::memory_order_release);
        }
    });

    fijar_hilo_a_pu(topo, cpu_a);
    while (!listo.load(std::memory_order_acquire)) cpu_relax();

    // Calentamiento fuera del cronómetro: las primeras rondas pagan el coste de
    // traer la línea y de que el hilo par arranque de verdad.
    const std::uint64_t calienta = rondas / 10 + 1;
    for (std::uint64_t r = 0; r < calienta; ++r) {
        c->v.store(2 * r + 1, std::memory_order_release);
        while (c->v.load(std::memory_order_acquire) != 2 * r + 2) cpu_relax();
    }

    double t0 = now_ns();
    for (std::uint64_t r = calienta; r < rondas; ++r) {
        c->v.store(2 * r + 1, std::memory_order_release);
        while (c->v.load(std::memory_order_acquire) != 2 * r + 2) cpu_relax();
    }
    double t1 = now_ns();

    responde.join();

    std::uint64_t medidas = rondas - calienta;
    if (medidas > 0) ns = (t1 - t0) / static_cast<double>(2 * medidas);

    c->~Celda();
    std::free(bruto);
    return ns;
}

// Clasifica la relación topológica entre dos núcleos físicos.
static const char* clase_de(const Topologia& topo, int nucleo_a, int nucleo_b) {
    if (nucleo_a == nucleo_b) return "smt";
    if (topo.nodo_de[nucleo_a] != topo.nodo_de[nucleo_b]) return "otro_nodo";
    if (topo.ccd_de[nucleo_a] == topo.ccd_de[nucleo_b]) return "mismo_ccd";
    return "otro_ccd";
}

static void modo_c2c(const Topologia& topo, int repeticiones, bool matriz_completa) {
    std::printf("=== MODO B: latencia nucleo a nucleo (ping-pong) ===\n");

    const std::uint64_t rondas = 60000;

    if (matriz_completa) {
        std::printf("  Recorriendo la matriz completa de %d x %d nucleos fisicos...\n",
                    topo.n_nucleos, topo.n_nucleos);
        for (int a = 0; a < topo.n_nucleos; ++a) {
            for (int b = a + 1; b < topo.n_nucleos; ++b) {
                double ns = ping_pong(topo, topo.pu_principal[a], topo.pu_principal[b],
                                      rondas);
                if (ns < 0) continue;
                Muestra m;
                m.ns_min = m.ns_media = m.ns_p95 = ns;
                m.saltos = rondas;
                csv_fila("B", clase_de(topo, a, b), 0, LINEA, "c2c",
                         topo.pu_principal[a], topo.pu_principal[b],
                         topo.nodo_de[a], m);
            }
            if ((a % 8) == 0) std::printf("    nucleo %d/%d\n", a, topo.n_nucleos);
        }
        std::printf("\n");
        return;
    }

    // Pares representativos: uno por clase topológica. Se eligen a partir de la
    // topología descubierta, no de índices escritos a mano.
    struct Par { const char* etiqueta; int nucleo_a; int cpu_a; int cpu_b; };
    std::vector<Par> pares;

    const int a = 0;
    // Hermano SMT del mismo núcleo físico
    if (topo.pu_hermano[a] >= 0)
        pares.push_back({"smt", a, topo.pu_principal[a], topo.pu_hermano[a]});

    // Mismo CCD, otro CCD del mismo nodo, otro CCD lejano, otro nodo
    int b_mismo_ccd = -1, b_otro_ccd = -1, b_otro_ccd_lejano = -1, b_otro_nodo = -1;
    for (int b = 1; b < topo.n_nucleos; ++b) {
        if (topo.nodo_de[b] != topo.nodo_de[a]) {
            if (b_otro_nodo < 0) b_otro_nodo = b;
            continue;
        }
        if (topo.ccd_de[b] == topo.ccd_de[a]) {
            if (b_mismo_ccd < 0) b_mismo_ccd = b;
        } else {
            if (b_otro_ccd < 0) b_otro_ccd = b;
            b_otro_ccd_lejano = b;  // el último del mismo nodo = el más lejano
        }
    }
    if (b_mismo_ccd >= 0)
        pares.push_back({"mismo_ccd", a, topo.pu_principal[a], topo.pu_principal[b_mismo_ccd]});
    if (b_otro_ccd >= 0)
        pares.push_back({"otro_ccd", a, topo.pu_principal[a], topo.pu_principal[b_otro_ccd]});
    if (b_otro_ccd_lejano >= 0 && b_otro_ccd_lejano != b_otro_ccd)
        pares.push_back({"otro_ccd_lejano", a, topo.pu_principal[a], topo.pu_principal[b_otro_ccd_lejano]});
    if (b_otro_nodo >= 0)
        pares.push_back({"otro_nodo", a, topo.pu_principal[a], topo.pu_principal[b_otro_nodo]});

    for (const auto& p : pares) {
        std::vector<double> muestras;
        for (int r = 0; r < repeticiones; ++r) {
            double ns = ping_pong(topo, p.cpu_a, p.cpu_b, rondas);
            if (ns > 0) muestras.push_back(ns);
        }
        Muestra m = resumir(muestras, rondas);
        csv_fila("B", p.etiqueta, 0, LINEA, "c2c", p.cpu_a, p.cpu_b,
                 topo.nodo_de[p.nucleo_a], m);
        std::printf("  [B] %-16s CPU %3d <-> %3d   min %7.2f ns   media %7.2f ns\n",
                    p.etiqueta, p.cpu_a, p.cpu_b, m.ns_min, m.ns_media);
    }
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// MODO C — lectura desde la caché de otro núcleo (víctima)
// ─────────────────────────────────────────────────────────────────────────────
//
// El productor ESCRIBE el buffer (no solo lo lee): una escritura invalida las
// copias limpias que el consumidor pudiera haberse quedado de la medida anterior,
// que es lo que obliga a que la siguiente lectura vuelva a cruzar el fabric. Si el
// productor solo leyera, a partir de la segunda repeticion el consumidor estaria
// midiendo su propia L3 y el resultado seria indistinguible de "mismo_ccd".

static void modo_victima(const Topologia& topo, int repeticiones,
                         std::size_t bytes, std::uint64_t semilla) {
    std::printf("=== MODO C: lectura desde la cache de otro nucleo ===\n");

    const int nucleo_consumidor = 0;
    const int cpu_consumidor = topo.pu_principal[nucleo_consumidor];
    const int nodo_local = topo.nodo_de[nucleo_consumidor];

    // Por defecto, la mitad de una L3 para que el buffer quepa holgadamente en el
    // CCD del productor.
    if (bytes == 0) bytes = (topo.l3_bytes > 0) ? topo.l3_bytes / 2 : 16 * 1024 * 1024;
    const std::size_t n = bytes / sizeof(Nodo);
    if (n < 2) return;

    // Un productor por clase topológica
    struct Prod { const char* etiqueta; int nucleo; };
    std::vector<Prod> productores;
    int p_mismo_ccd = -1, p_otro_ccd = -1, p_otro_nodo = -1;
    for (int b = 1; b < topo.n_nucleos; ++b) {
        if (topo.nodo_de[b] != topo.nodo_de[nucleo_consumidor]) {
            if (p_otro_nodo < 0) p_otro_nodo = b;
        } else if (topo.ccd_de[b] == topo.ccd_de[nucleo_consumidor]) {
            if (p_mismo_ccd < 0) p_mismo_ccd = b;
        } else {
            if (p_otro_ccd < 0) p_otro_ccd = b;
        }
    }
    if (p_mismo_ccd >= 0) productores.push_back({"mismo_ccd", p_mismo_ccd});
    if (p_otro_ccd >= 0)  productores.push_back({"otro_ccd", p_otro_ccd});
    if (p_otro_nodo >= 0) productores.push_back({"otro_nodo", p_otro_nodo});

    // THP siempre en el modo C: el buffer cabe en una L3 y lo que se quiere medir es
    // la transferencia entre cachés, no recorridos de tablas de páginas.
    Reserva r = reservar_en_nodo(topo, bytes, nodo_local, true);
    if (!r.datos) return;
    Nodo* base = static_cast<Nodo*>(r.datos);
    std::memset(r.datos, 0, bytes);
    construir_cadena(base, n, semilla);

    for (const auto& prod : productores) {
        const int cpu_productor = topo.pu_principal[prod.nucleo];
        std::vector<double> muestras;

        for (int r = 0; r < repeticiones; ++r) {
            // El productor escribe todo el buffer desde SU nucleo: al terminar, las
            // lineas estan modificadas en la cache de su CCD.
            std::atomic<bool> hecho{false};
            std::thread productor([&]() {
                fijar_hilo_a_pu(topo, cpu_productor);
                for (std::size_t i = 0; i < n; ++i) base[i].pad[0] = static_cast<char>(r);
                hecho.store(true, std::memory_order_release);
            });
            productor.join();
            while (!hecho.load(std::memory_order_acquire)) cpu_relax();

            // El consumidor recorre la cadena UNA sola vez: a partir de la segunda
            // pasada los datos ya estarian en su propia cache.
            fijar_hilo_a_pu(topo, cpu_consumidor);
            double t0 = now_ns();
            Nodo* p = perseguir(&base[0], n);
            double t1 = now_ns();
            g_sumidero = reinterpret_cast<std::uintptr_t>(p);
            muestras.push_back((t1 - t0) / static_cast<double>(n));
        }

        Muestra m = resumir(muestras, n);
        csv_fila("C", prod.etiqueta, r.thp ? 1 : 0, bytes, "lectura_cache_ajena",
                 cpu_consumidor, cpu_productor, nodo_local, m);
        std::printf("  [C] %-12s productor CPU %3d -> consumidor CPU %3d  "
                    "min %7.2f ns   media %7.2f ns\n",
                    prod.etiqueta, cpu_productor, cpu_consumidor, m.ns_min, m.ns_media);
    }

    liberar(topo, r);
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────

static void uso(const char* prog) {
    std::printf(
        "Uso: %s [opciones]\n"
        "  --curva            modo A: latencia frente al tamano (local y remoto)\n"
        "  --c2c              modo B: latencia nucleo a nucleo, pares representativos\n"
        "  --matriz           modo B sobre la matriz completa NxN (para el mapa de calor)\n"
        "  --victima          modo C: lectura desde la cache de otro nucleo\n"
        "  --csv RUTA         fichero de salida (por defecto caracterizacion/latencias_exadell.csv)\n"
        "  --reps N           repeticiones por punto (por defecto 5)\n"
        "  --ms N             milisegundos objetivo por repeticion en el modo A (por defecto 50)\n"
        "  --semilla N        semilla del barajado (por defecto 42)\n"
        "  --bytes N          tamano del buffer del modo C (por defecto media L3)\n"
        "  --thp si|no|ambos  paginas enormes en el modo A (por defecto ambos)\n"
        "Sin modos explicitos se ejecutan A, B (pares) y C.\n", prog);
}

int main(int argc, char* argv[]) {
    bool hacer_curva = false, hacer_c2c = false, hacer_victima = false, matriz = false;
    std::string ruta_csv = "caracterizacion/latencias_exadell.csv";
    int repeticiones = 5;
    double objetivo_ms = 50.0;
    std::uint64_t semilla = 42;
    std::size_t bytes_victima = 0;
    // Por defecto el modo A se recorre DOS veces, con y sin paginas enormes. La
    // diferencia entre ambas curvas es el coste de los fallos de TLB, y hace falta
    // declararla: los kernels del proyecto corren con la politica de THP que tenga
    // el sistema, asi que hay que saber en cual de las dos curvas caen.
    std::vector<bool> variantes_thp = {true, false};

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--curva") hacer_curva = true;
        else if (a == "--c2c") hacer_c2c = true;
        else if (a == "--matriz") { hacer_c2c = true; matriz = true; }
        else if (a == "--victima") hacer_victima = true;
        else if (a == "--csv" && i + 1 < argc) ruta_csv = argv[++i];
        else if (a == "--reps" && i + 1 < argc) repeticiones = std::atoi(argv[++i]);
        else if (a == "--ms" && i + 1 < argc) objetivo_ms = std::atof(argv[++i]);
        else if (a == "--semilla" && i + 1 < argc) semilla = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--bytes" && i + 1 < argc) bytes_victima = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--thp" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "si") variantes_thp = {true};
            else if (v == "no") variantes_thp = {false};
            else if (v == "ambos") variantes_thp = {true, false};
            else { uso(argv[0]); return 1; }
        }
        else { uso(argv[0]); return (a == "--help" || a == "-h") ? 0 : 1; }
    }
    if (!hacer_curva && !hacer_c2c && !hacer_victima) {
        hacer_curva = hacer_c2c = hacer_victima = true;
    }
    if (repeticiones < 1) repeticiones = 1;

    Topologia topo;
    if (!descubrir_topologia(topo)) return 1;
    imprimir_topologia(topo);

    csv_abrir(ruta_csv);

    if (hacer_curva)   modo_curva(topo, repeticiones, objetivo_ms, semilla, variantes_thp);
    if (hacer_c2c)     modo_c2c(topo, repeticiones, matriz);
    if (hacer_victima) modo_victima(topo, repeticiones, bytes_victima, semilla);

    if (g_csv) std::fclose(g_csv);
    hwloc_topology_destroy(topo.t);

    std::printf("Resultados en: %s\n", ruta_csv.c_str());
    return 0;
}
