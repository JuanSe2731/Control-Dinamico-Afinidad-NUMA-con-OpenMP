// perf_region.hpp — Auto-instrumentación por hilo, acotada a la región medida.
//
// POR QUÉ EXISTE
// --------------
// Hasta ahora el IPC y el ratio_rm de las configuraciones sin tool OMPT se
// obtenían con un `perf stat` externo. Eso NO funciona: perf cuenta el proceso
// entero, incluidas la carga/generación de la matriz, la construcción del CSR y la
// validación, y el `prctl(PR_TASK_PERF_EVENTS_DISABLE)` interno no puede apagar
// eventos creados por otro proceso. Medido en la campaña 29311: 1.516e12
// instrucciones de preparación frente a 2.99e11 del bucle medido (5.1x), con un
// IPC agregado de 2.648 frente al IPC real del kernel, 1.277. Las columnas estaban
// declaradas como inutilizables en METODOLOGIA_EXPERIMENTAL.md §4.3.
//
// Este cabecero abre los contadores DENTRO del proceso, uno por hilo OpenMP, y los
// lee exactamente alrededor del bucle medido. Consecuencias:
//   - mismo instrumento en TODAS las configuraciones -> por fin son comparables
//   - acotado a la región por construcción, sin fase de preparación
//   - por hilo, sin root, sin proceso externo compitiendo por los registros del PMU
//
// UN SOLO PROPIETARIO DE CONTADORES POR HILO
// ------------------------------------------
// Cuando el tool OMPT está cargado, cada hilo YA tiene contadores abiertos: el
// scheduler los necesita para decidir. Abrir aquí otro juego daría ~14 eventos
// sobre 6 PMC programables, multiplexado agresivo y precisión degradada en ambos.
// Por eso `init()` se desactiva sola si detecta el tool, y en ese caso los datos
// salen de ompt_summary.csv. Nunca se mide dos veces lo mismo.
//
// EVENTOS (AMD Zen 4, familia 19h) — CODIGOS VERIFICADOS EN exadell
// -----------------------------------------------------------------
// Verificados el 2026-08-19 sobre el propio nodo con
//     perf stat -e ls_any_fills_from_sys.<nombre> -vv -- true
// que imprime el `config` ya resuelto por las tablas JSON de perf.
//
// OJO: estos eventos NO estan expuestos en
// /sys/bus/event_source/devices/cpu/events/ — en AMD vienen de las tablas que
// perf lleva compiladas dentro para la familia 19h. Por eso los codigos van
// cableados aqui y no se leen de sysfs en tiempo de ejecucion: no hay de donde.
//
// El evento es el byte bajo y el ORIGEN de la linea es la mascara (byte alto):
//
//   mascara  origen                                        que interconexion cruza
//   0x01     Local L2            L2 propia                 ninguna
//   0x02     Local CCX           L3 del PROPIO chiplet     ninguna
//   0x04     Near cache          L3 de OTRO chiplet,       Infinity Fabric
//                                 mismo nodo NUMA
//   0x08     DRAM/IO near        DRAM del nodo local       IOD
//   0x10     Far cache           cache de otro CCX,        xGMI
//                                 otro nodo NUMA
//   0x40     DRAM/IO far         DRAM del otro nodo        xGMI
//   0x80     Alternate memories  memoria de extension (CXL); cero en esta maquina
//   0xFF     todos
//
// Comprobado ademas que `remote_cache` = 0x1444, o sea mascara 0x14 = 0x10|0x04:
// es la SUMA de far_cache y near_cache, no un septimo origen. Y la mascara 0x20
// no tiene nombre en las tablas de perf (se presume reservada), asi que la suma de
// las seis mas 0x80 deberia dar practicamente el total de 0xFF.
//
// DOS FAMILIAS, y cada una sirve para una magnitud distinta:
//
//   0x44  ls_any_fills_from_sys   demanda + prefetch hardware + prefetch software
//   0x43  ls_dmnd_fills_from_sys  SOLO demanda
//
// Para el TRAFICO manda `any`: un prefetch consume ancho de banda del enlace
// igual que una demanda. Para el TIEMPO MUERTO manda `dmnd`: un prefetch que
// llega a tiempo no para al nucleo, que es justo su proposito, asi que
// multiplicar rellenos `any` por la latencia remota sobreestimaria la espera.
//
// No caben las dos a la vez (6 mascaras x 2 familias = 12 eventos sobre 6
// registros programables), asi que se conmuta con PERF_FAMILIA=any|dmnd. Por
// defecto `any`, que es lo que usa la campana y lo que mantiene la
// comparabilidad con las campanas anteriores.
//
// VERIFICAR EN OTRA MAQUINA antes de fiarse: `perf list | grep -i fills`. Si las
// mascaras no coinciden, PERF_REGION_MASCARAS=0 vuelve al par 0xFF44/0xD044, que
// es el que ya estaba validado en produccion.
//
// FALLO SILENCIOSO A VIGILAR: si perf_event_open es denegado (kernel.
// perf_event_paranoid demasiado alto) los fd salen < 0, todos los conteos quedan a
// cero y no se reporta ningun error. Comprobar perf_event_paranoid <= 2.

#ifndef PERF_REGION_HPP
#define PERF_REGION_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

namespace perf_region {

// ─────────────────────────────────────────────────────────────────────────────
// Configuración
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MAX_HILOS = 512;   // la máquina objetivo tiene 256 PU
static constexpr int N_ORIGENES = 6;

// Índices dentro del vector de rellenos, en orden de distancia topológica.
enum Origen {
    ORG_L2          = 0,  // umask 0x01  local L2
    ORG_L3_CCD      = 1,  // umask 0x02  L3 del propio chiplet
    ORG_CCD_VECINO  = 2,  // umask 0x04  L3 de otro chiplet, mismo socket
    ORG_DRAM_LOCAL  = 3,  // umask 0x08  DRAM local
    ORG_FAR_CACHE   = 4,  // umask 0x10  caché del otro socket
    ORG_FAR_DRAM    = 5   // umask 0x40  DRAM remota
};

static const char* const NOMBRE_ORIGEN[N_ORIGENES] = {
    "fill_l2", "fill_l3_ccd", "fill_ccd_vecino",
    "fill_dram_local", "fill_far_cache", "fill_far_dram"
};

// Mascaras de origen (byte alto del config). El evento (byte bajo) lo pone la
// familia, asi que conmutar entre `any` y `dmnd` es cambiar un solo byte.
static const std::uint64_t UMASK_ORIGEN[N_ORIGENES] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x40
};

static constexpr std::uint64_t EVENTO_ANY  = 0x44;   // ls_any_fills_from_sys
static constexpr std::uint64_t EVENTO_DMND = 0x43;   // ls_dmnd_fills_from_sys

// Familia activa. `any` por defecto: es la que usa la campana y la que mantiene
// la comparabilidad con las campanas anteriores.
inline std::uint64_t evento_familia() {
    static std::uint64_t cache = 0;
    if (cache == 0) {
        const char* f = std::getenv("PERF_FAMILIA");
        cache = (f && std::strcmp(f, "dmnd") == 0) ? EVENTO_DMND : EVENTO_ANY;
    }
    return cache;
}

inline bool familia_es_demanda() { return evento_familia() == EVENTO_DMND; }

inline std::uint64_t config_origen(int i) {
    return (UMASK_ORIGEN[i] << 8) | evento_familia();
}

// Orden en que se SACRIFICAN mascaras cuando no caben todas: se conservan primero
// las lejanas, que son las que importan para NUMA, y se sueltan antes las cercanas.
// Si solo caben 3, quedan far_dram + far_cache + ccd_vecino, que es justo el tramo
// caro de esta maquina.
static const int PRIORIDAD_ORIGEN[N_ORIGENES] = {
    ORG_FAR_DRAM, ORG_FAR_CACHE, ORG_CCD_VECINO,
    ORG_DRAM_LOCAL, ORG_L3_CCD, ORG_L2
};

inline int& n_origenes_activos() { static int n = -1; return n; }

// TOPE de mascaras del desglose. Por defecto 3, y no es una limitacion tecnica sino
// una DECISION: con 5 contadores libres, el par de ratio_rm ocupa 2 y quedan 3. Si
// se piden mas, el kernel multiplexa el desglose contra el par, y aunque el cociente
// remoto/total sigue siendo exacto (ambos van en el mismo grupo, se escalan igual),
// pasa a muestrearse a rafagas: mete varianza en la senal que alimenta el detector y
// deja los conteos absolutos extrapolados, justo debajo del filtro MIN_FILLS.
//
// Las 3 que se conservan son las que CRUZAN UNA INTERCONEXION, que es lo que este
// trabajo estudia: near_cache (Infinity Fabric), far_cache y dram_io_far (xGMI). Las
// tres cercanas se descartan porque no cruzan nada, y la banda "local" de la figura
// de trafico se DERIVA como all - (las tres lejanas), con all exacto.
//
// PERF_MAX_ORIGENES lo cambia si algun dia hay mas contadores libres (por ejemplo si
// se pudiera apagar el nmi_watchdog, que hoy ocupa uno de forma permanente).
inline int tope_origenes() {
    if (const char* e = std::getenv("PERF_MAX_ORIGENES")) {
        int v = std::atoi(e);
        if (v >= 0 && v <= N_ORIGENES) return v;
    }
    return 3;
}

// Modo degradado: el par que ya estaba validado en produccion (0xFF/0xD0), con
// el evento de la familia activa.
inline std::uint64_t config_all_fills()    { return (0xFFULL << 8) | evento_familia(); }
inline std::uint64_t config_remote_fills() { return (0xD0ULL << 8) | evento_familia(); }

// ─────────────────────────────────────────────────────────────────────────────
// Estado por hilo
// ─────────────────────────────────────────────────────────────────────────────

struct EstadoHilo {
    int fd_ciclos = -1;
    int fd_instr  = -1;
    int fd_origen[N_ORIGENES] = {-1, -1, -1, -1, -1, -1};

    // Modo degradado (PERF_REGION_MASCARAS=0)
    int fd_all_fills = -1;
    int fd_remote_fills = -1;

    std::uint64_t base_ciclos = 0, base_instr = 0;
    std::uint64_t base_origen[N_ORIGENES] = {0, 0, 0, 0, 0, 0};
    std::uint64_t base_all = 0, base_remote = 0;

    // Acumulado de la región (delta fin - inicio)
    std::uint64_t d_ciclos = 0, d_instr = 0;
    std::uint64_t d_origen[N_ORIGENES] = {0, 0, 0, 0, 0, 0};
    std::uint64_t d_all = 0, d_remote = 0;

    long tid = 0;
    int  usado = 0;
};

// Almacenamiento estático: nada de contenedores dinámicos, para que se pueda
// volcar sin depender del orden de destrucción de objetos globales.
inline EstadoHilo* tabla() {
    static EstadoHilo t[MAX_HILOS];
    return t;
}

inline int& n_hilos_registrados() { static int n = 0; return n; }
inline bool& esta_activo()        { static bool a = false; return a; }
inline bool& usa_mascaras()       { static bool m = true;  return m; }
inline int&  hilos_con_fallo()    { static int f = 0; return f; }
inline int&  hilos_degradados()   { static int d = 0; return d; }

// ─────────────────────────────────────────────────────────────────────────────
// perf_event_open
// ─────────────────────────────────────────────────────────────────────────────

#if defined(__linux__)

struct LecturaPerf { std::uint64_t valor, tiempo_activo, tiempo_corriendo; };

inline long perf_event_open_syscall(struct perf_event_attr* attr, pid_t pid, int cpu,
                                    int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// read_format pide time_enabled/time_running para poder CORREGIR el multiplexado:
// si se piden más eventos que registros hay en la PMU, el kernel los rota y cada
// uno mide solo una fracción del intervalo. Sin corregirlo los conteos salen bajos
// y, peor, cada uno por un factor distinto, con lo que el IPC y las proporciones
// pasarían a ser cocientes de números medidos en intervalos diferentes.
inline int abrir_contador(std::uint32_t tipo, std::uint64_t config, int group_fd) {
    struct perf_event_attr pe;
    std::memset(&pe, 0, sizeof(pe));
    pe.type           = tipo;
    pe.size           = sizeof(pe);
    pe.config         = config;
    pe.disabled       = (group_fd == -1) ? 1 : 0;   // solo el líder arranca parado
    pe.inherit        = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    pe.read_format    = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    // pid = 0 -> el hilo que llama; cpu = -1 -> siga a donde siga el hilo.
    return static_cast<int>(perf_event_open_syscall(&pe, 0, -1, group_fd, 0));
}

inline bool leer_contador(int fd, std::uint64_t& valor) {
    valor = 0;
    if (fd < 0) return false;
    LecturaPerf r{};
    if (read(fd, &r, sizeof(r)) != static_cast<ssize_t>(sizeof(r))) return false;
    if (r.tiempo_corriendo == 0) return false;   // nunca entró en el hardware
    if (r.tiempo_corriendo < r.tiempo_activo) {
        // Escalado por multiplexado, en double a propósito: valor * tiempo_activo
        // desbordaría uint64 y aquí sobra precisión.
        valor = static_cast<std::uint64_t>(
            static_cast<double>(r.valor) *
            (static_cast<double>(r.tiempo_activo) /
             static_cast<double>(r.tiempo_corriendo)));
    } else {
        valor = r.valor;
    }
    return true;
}

#else  // no Linux: todo queda inerte

inline int  abrir_contador(std::uint32_t, std::uint64_t, int) { return -1; }
inline bool leer_contador(int, std::uint64_t& v) { v = 0; return false; }

#endif

// ─────────────────────────────────────────────────────────────────────────────
// Sonda: ¿cuantos eventos raw caben en UN grupo que el kernel llegue a planificar?
// ─────────────────────────────────────────────────────────────────────────────
//
// No basta con que perf_event_open devuelva un fd. Un grupo tiene que caber ENTERO
// y a la vez en la PMU; si no cabe, el kernel lo abre pero NUNCA lo planifica, y
// entonces time_running queda a 0 y no hay dato. Eso es exactamente lo que paso en
// la campana 29355: Zen 4 tiene 6 contadores programables pero el nmi_watchdog
// ocupa uno de forma permanente, asi que solo hay CINCO libres y el grupo de seis
// nunca corrio. `perf stat` lo reporta como "<not counted> (0.00%)".
//
// Por eso se prueba de verdad: se abre el grupo, se habilita, se hace algo de
// trabajo y se comprueba que time_running > 0. Se repite bajando el tamano hasta
// que uno funcione. Se hace UNA vez, en el hilo principal, antes de la region
// paralela.

#if defined(__linux__)
inline int probar_tamano_grupo() {
    // El par 0xFF44/0xD044 se abre SIEMPRE y ocupa 2 contadores, asi que la sonda
    // tiene que medir el hueco QUE QUEDA, no el total. Se abre un par de mentira y
    // se prueba cuantas mascaras caben ADEMAS, que es la condicion real de no
    // multiplexar: par + desglose <= contadores libres.
    int par_a = abrir_contador(PERF_TYPE_RAW, config_all_fills(), -1);
    int par_b = (par_a >= 0) ? abrir_contador(PERF_TYPE_RAW, config_remote_fills(),
                                              par_a) : -1;
    if (par_a >= 0) {
        ioctl(par_a, PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
        ioctl(par_a, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }

    int resultado = 0;
    for (int n = N_ORIGENES; n >= 1; --n) {
        int fds[N_ORIGENES];
        for (int i = 0; i < N_ORIGENES; ++i) fds[i] = -1;
        int lider = -1;
        bool ok = true;
        for (int i = 0; i < n && ok; ++i) {
            fds[i] = abrir_contador(PERF_TYPE_RAW,
                                    config_origen(PRIORIDAD_ORIGEN[i]), lider);
            if (fds[i] < 0) ok = false;
            if (i == 0) lider = fds[0];
        }
        if (ok && lider >= 0) {
            ioctl(lider, PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
            ioctl(lider, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
            volatile double x = 0.0;
            for (int k = 0; k < 300000; ++k) x += static_cast<double>(k);
            LecturaPerf r{};
            ok = (read(fds[0], &r, sizeof(r)) == static_cast<ssize_t>(sizeof(r)))
                 && r.tiempo_corriendo > 0;
            ioctl(lider, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        }
        for (int i = 0; i < N_ORIGENES; ++i) if (fds[i] >= 0) close(fds[i]);
        if (ok) { resultado = n; break; }
    }

    if (par_b >= 0) close(par_b);
    if (par_a >= 0) close(par_a);
    return resultado;
}
#else
inline int probar_tamano_grupo() { return 0; }
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Detección del tool OMPT
// ─────────────────────────────────────────────────────────────────────────────

inline bool tool_ompt_cargado() {
    const char* t = std::getenv("OMP_TOOL");
    const char* l = std::getenv("OMP_TOOL_LIBRARIES");
    return t && l && *l && std::strcmp(t, "enabled") == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Ciclo de vida
// ─────────────────────────────────────────────────────────────────────────────

inline int slot_de_este_hilo() {
#ifdef _OPENMP
    return omp_get_thread_num();
#else
    return 0;
#endif
}

// Abre los contadores del hilo que llama. Debe invocarse DENTRO de una región
// paralela para que cada hilo abra los suyos.
inline void abrir_este_hilo() {
    if (!esta_activo()) return;
    int s = slot_de_este_hilo();
    if (s < 0 || s >= MAX_HILOS) return;
    EstadoHilo& e = tabla()[s];
    if (e.usado) return;
    e.usado = 1;
#if defined(__linux__)
    e.tid = static_cast<long>(syscall(SYS_gettid));

    // Grupo 1: ciclos (líder) + instrucciones. Separado del grupo de rellenos a
    // propósito: si el grupo grande no cupiera en la PMU, el IPC se salva igual.
    e.fd_ciclos = abrir_contador(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1);
    e.fd_instr  = abrir_contador(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,
                                 e.fd_ciclos);

    // Grupo 2: el par 0xFF44/0xD044. SIEMPRE se abre, pase lo que pase con el
    // desglose. Son dos eventos, caben seguro, y de aqui sale ratio_rm EXACTO, que
    // es la variable de decision del mecanismo y la que alimenta la figura de
    // localidad. Perder el desglose es una molestia; perder ratio_rm invalida la
    // campana entera, que es lo que ocurrio en la 29355.
    e.fd_all_fills    = abrir_contador(PERF_TYPE_RAW, config_all_fills(), -1);
    e.fd_remote_fills = abrir_contador(PERF_TYPE_RAW, config_remote_fills(),
                                       e.fd_all_fills);
    if (e.fd_all_fills < 0) ++hilos_con_fallo();

    if (usa_mascaras() && n_origenes_activos() > 0) {
        // Grupo 3: el desglose por origen, con el tamano que la sonda determino que
        // el kernel llega a planificar de verdad, y en orden de prioridad (primero
        // las lejanas). Van agrupadas para que cubran el mismo intervalo entre si.
        int lider = -1;
        for (int i = 0; i < n_origenes_activos(); ++i) {
            const int org = PRIORIDAD_ORIGEN[i];
            e.fd_origen[org] = abrir_contador(PERF_TYPE_RAW, config_origen(org), lider);
            if (i == 0) lider = e.fd_origen[org];
        }
    } else if (usa_mascaras()) {
        ++hilos_degradados();
    }

    if (e.fd_ciclos >= 0) {
        ioctl(e.fd_ciclos, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(e.fd_ciclos, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }
    const int lideres[2] = {
        e.fd_all_fills,
        (n_origenes_activos() > 0) ? e.fd_origen[PRIORIDAD_ORIGEN[0]] : -1
    };
    for (int j = 0; j < 2; ++j) {
        if (lideres[j] >= 0) {
            ioctl(lideres[j], PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
            ioctl(lideres[j], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        }
    }
#endif
}

// Toma la instantánea de inicio de región para el hilo que llama.
inline void inicio_este_hilo() {
    if (!esta_activo()) return;
    int s = slot_de_este_hilo();
    if (s < 0 || s >= MAX_HILOS) return;
    EstadoHilo& e = tabla()[s];
    leer_contador(e.fd_ciclos, e.base_ciclos);
    leer_contador(e.fd_instr,  e.base_instr);
    for (int i = 0; i < N_ORIGENES; ++i)
        if (e.fd_origen[i] >= 0) leer_contador(e.fd_origen[i], e.base_origen[i]);
    leer_contador(e.fd_all_fills,    e.base_all);
    leer_contador(e.fd_remote_fills, e.base_remote);
}

// Acumula el delta de la región para el hilo que llama. Se ACUMULA en vez de
// asignar para que begin/end se puedan anidar en varias fases si hiciera falta.
inline void fin_este_hilo() {
    if (!esta_activo()) return;
    int s = slot_de_este_hilo();
    if (s < 0 || s >= MAX_HILOS) return;
    EstadoHilo& e = tabla()[s];
    std::uint64_t v = 0;

    if (leer_contador(e.fd_ciclos, v) && v >= e.base_ciclos) e.d_ciclos += v - e.base_ciclos;
    if (leer_contador(e.fd_instr,  v) && v >= e.base_instr)  e.d_instr  += v - e.base_instr;

    for (int i = 0; i < N_ORIGENES; ++i)
        if (e.fd_origen[i] >= 0 && leer_contador(e.fd_origen[i], v)
            && v >= e.base_origen[i])
            e.d_origen[i] += v - e.base_origen[i];
    if (leer_contador(e.fd_all_fills, v)    && v >= e.base_all)    e.d_all += v - e.base_all;
    if (leer_contador(e.fd_remote_fills, v) && v >= e.base_remote) e.d_remote += v - e.base_remote;
}

inline void cerrar_este_hilo() {
#if defined(__linux__)
    int s = slot_de_este_hilo();
    if (s < 0 || s >= MAX_HILOS) return;
    EstadoHilo& e = tabla()[s];
    if (e.fd_ciclos >= 0) { close(e.fd_ciclos); e.fd_ciclos = -1; }
    if (e.fd_instr  >= 0) { close(e.fd_instr);  e.fd_instr  = -1; }
    for (int i = 0; i < N_ORIGENES; ++i)
        if (e.fd_origen[i] >= 0) { close(e.fd_origen[i]); e.fd_origen[i] = -1; }
    if (e.fd_all_fills    >= 0) { close(e.fd_all_fills);    e.fd_all_fills = -1; }
    if (e.fd_remote_fills >= 0) { close(e.fd_remote_fills); e.fd_remote_fills = -1; }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// API que usan los kernels
// ─────────────────────────────────────────────────────────────────────────────

// Se llama UNA vez, antes del warm-up. Abre un juego de contadores por hilo del
// equipo OpenMP, salvo que el tool OMPT esté cargado (entonces él es el
// propietario y estas funciones quedan inertes).
inline void init() {
    if (tool_ompt_cargado()) {
        esta_activo() = false;
        std::fprintf(stderr, "[perf_region] tool OMPT detectado: los contadores los "
                             "lleva el tool, no se abren aqui\n");
        return;
    }
    if (const char* m = std::getenv("PERF_REGION_MASCARAS")) {
        usa_mascaras() = (std::atoi(m) != 0);
    }
    esta_activo() = true;

    // La sonda va ANTES de la region paralela: determina una sola vez cuantas
    // mascaras caben de verdad en esta PMU.
    if (usa_mascaras()) {
        const int caben = probar_tamano_grupo();
        n_origenes_activos() = (caben < tope_origenes()) ? caben : tope_origenes();
        std::fprintf(stderr,
            "[perf_region] contadores: par 0xFF44/0xD044 (ratio_rm exacto) + %d "
            "mascaras de desglose\n"
            "              (caben %d ademas del par; tope por diseno %d). "
            "SIN multiplexado.\n",
            n_origenes_activos(), caben, tope_origenes());
    }

#ifdef _OPENMP
    #pragma omp parallel
    {
        abrir_este_hilo();
        #pragma omp master
        n_hilos_registrados() = omp_get_num_threads();
    }
#else
    abrir_este_hilo();
    n_hilos_registrados() = 1;
#endif

    if (hilos_degradados() > 0) {
        std::fprintf(stderr,
            "[perf_region] AVISO: %d hilos no pudieron abrir el grupo de 6 mascaras\n"
            "              y cayeron al par 0xFF44/0xD044. Hay ratio remoto pero NO\n"
            "              desglose por origen. Causa tipica: menos de 6 contadores\n"
            "              programables libres (comprobar kernel.nmi_watchdog).\n",
            hilos_degradados());
    }
    if (hilos_con_fallo() > 0) {
        std::fprintf(stderr,
            "[perf_region] AVISO: %d hilos no pudieron abrir los eventos de relleno.\n"
            "              Comprobar perf_event_paranoid <= 2 y que las mascaras de\n"
            "              PMCx044 coinciden con esta PMU (perf list | grep -i fills).\n"
            "              Con PERF_REGION_MASCARAS=0 se usa el par 0xFF44/0xD044.\n",
            hilos_con_fallo());
    }
}

inline void begin() {
    if (!esta_activo()) return;
#ifdef _OPENMP
    #pragma omp parallel
    { inicio_este_hilo(); }
#else
    inicio_este_hilo();
#endif
}

inline void end() {
    if (!esta_activo()) return;
#ifdef _OPENMP
    #pragma omp parallel
    { fin_este_hilo(); }
#else
    fin_este_hilo();
#endif
}

inline void shutdown() {
    if (!esta_activo()) return;
#ifdef _OPENMP
    #pragma omp parallel
    { cerrar_este_hilo(); }
#else
    cerrar_este_hilo();
#endif
    esta_activo() = false;
}

inline bool activo() { return esta_activo(); }

// Detecta el fallo silencioso que este proyecto ya ha sufrido: los eventos crudos
// 0x..44 son de AMD Zen, y en otra PMU perf_event_open SUELE TENER ÉXITO y devolver
// cero para siempre. Los fd salen válidos, no hay error, y todas las columnas de
// tráfico quedan a cero sin que nadie se entere. Ciclos > 0 con rellenos == 0 es
// la firma inequívoca de ese caso (o de perf_event_paranoid demasiado alto).
inline void diagnostico() {
    if (!esta_activo()) return;
    std::uint64_t total_ciclos = 0, total_rellenos = 0;
    const int n = n_hilos_registrados();
    for (int s = 0; s < n && s < MAX_HILOS; ++s) {
        const EstadoHilo& e = tabla()[s];
        if (!e.usado) continue;
        total_ciclos += e.d_ciclos;
        // Mira el PAR, no el desglose: el par es el que siempre debe estar. Antes
        // miraba el desglose y habria dado una falsa alarma en cuanto el desglose
        // se recortara a 3 mascaras (o a 0) aunque el par funcionara bien.
        total_rellenos += e.d_all;
    }
    if (total_ciclos == 0) {
        std::fprintf(stderr,
            "[perf_region] ERROR: cero ciclos contados. perf_event_open no esta\n"
            "              midiendo nada. Comprobar perf_event_paranoid <= 2.\n");
    } else if (total_rellenos == 0) {
        std::fprintf(stderr,
            "[perf_region] ERROR: %llu ciclos contados pero CERO rellenos.\n"
            "              Los eventos PMCx044 son especificos de AMD Zen; en otra\n"
            "              PMU perf_event_open tiene exito y devuelve cero siempre.\n"
            "              Las columnas de trafico de esta corrida NO son validas.\n",
            static_cast<unsigned long long>(total_ciclos));
    }
}

// Vuelca una fila por hilo. Formato deliberadamente parecido a ompt_summary.csv
// para que el script de campaña agregue los dos igual.
inline void escribir_csv(const char* ruta, const char* etiqueta) {
    if (!esta_activo() || !ruta || !*ruta) return;
    diagnostico();

    bool nuevo = true;
    if (FILE* f = std::fopen(ruta, "r")) {
        std::fseek(f, 0, SEEK_END);
        nuevo = (std::ftell(f) == 0);
        std::fclose(f);
    }
    FILE* f = std::fopen(ruta, "a");
    if (!f) return;
    if (nuevo) {
        // Nombres IDÉNTICOS a los de ompt_summary.csv a propósito: así el script de
        // campaña agrega los dos caminos de medida con el mismo awk.
        std::fprintf(f, "tag,slot,tid,instructions,cycles");
        for (int i = 0; i < N_ORIGENES; ++i) std::fprintf(f, ",%s", NOMBRE_ORIGEN[i]);
        std::fprintf(f, ",fill_all,fill_remoto,mascaras,familia\n");
    }

    const int n = n_hilos_registrados();
    for (int s = 0; s < n && s < MAX_HILOS; ++s) {
        const EstadoHilo& e = tabla()[s];
        if (!e.usado) continue;

        // all y remoto salen SIEMPRE del par exacto: los dos estan en el mismo
        // grupo, asi que su cociente cubre el mismo intervalo y ratio_rm es valido
        // aunque el desglose se haya quedado corto o vacio.
        const std::uint64_t all = e.d_all, remoto = e.d_remote;

        std::fprintf(f, "%s,%d,%ld,%llu,%llu",
                     etiqueta ? etiqueta : "-", s, e.tid,
                     static_cast<unsigned long long>(e.d_instr),
                     static_cast<unsigned long long>(e.d_ciclos));
        for (int i = 0; i < N_ORIGENES; ++i)
            std::fprintf(f, ",%llu", static_cast<unsigned long long>(e.d_origen[i]));
        std::fprintf(f, ",%llu,%llu,%d,%s\n",
                     static_cast<unsigned long long>(all),
                     static_cast<unsigned long long>(remoto),
                     n_origenes_activos() > 0 ? n_origenes_activos() : 0,
                     familia_es_demanda() ? "dmnd" : "any");
    }
    std::fclose(f);
}

}  // namespace perf_region

#endif  // PERF_REGION_HPP
