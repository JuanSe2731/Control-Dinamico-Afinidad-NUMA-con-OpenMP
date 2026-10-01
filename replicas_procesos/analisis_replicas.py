#!/usr/bin/env python3
"""
Analisis estadistico de la campana de replicas de proceso (replicas_procesos/).

LA UNIDAD DE ANALISIS ES EL PROCESO
-----------------------------------
analisis/estadistica.py, y con el la seccion 4.8 del documento, usa como muestra
las 150 repeticiones de UNA ejecucion. Eso mide el ruido de iteracion a iteracion,
pero no lo que cambia de un proceso a otro (en que nodo cae el hilo maestro y con
el las paginas, que CPU elige el sistema para cada hilo sin afinidad, el estado de
la maquina), y ademas las repeticiones estan autocorrelacionadas: el error
estandar sale optimista y casi cualquier diferencia resulta "significativa".

Aqui cada grupo (kernel, tamano, hilos, configuracion) tiene N procesos
independientes, uno por replica, y la OBSERVACION es la media de las repeticiones
de cada proceso. Todas las pruebas se hacen con n = N por grupo.

SALIDAS (en --destino, por defecto analisis/)
---------------------------------------------
  procesos.csv         una fila por proceso: la tabla limpia de la que sale todo
  resumen_celdas.csv   por grupo: media entre procesos con su IC95, dispersion
                       entre procesos, FACTOR DE SUBESTIMACION del error estandar
                       intra-proceso, ratio_rm y migraciones
  contrastes.csv       t de Welch por pareja (las tres de la Tabla 9 y las de las
                       estaticas): IC95 de Welch y bootstrap, p corregida por Holm,
                       d de Cohen, g de Hedges y diferencia minima detectable
  descomposicion.csv   C_monitor, G_migrar, G_neta, G_interleave y G_spread en ms,
                       con el signo del documento y sus IC95
  anova.csv            ANOVA de un factor clasica (la del documento) y de Welch
  contraste_29390.csv  donde cae la ejecucion unica de la campana 29390 dentro de
                       la distribucion de las replicas (si existe ../perf_out_v5)
  resumen.txt          el resumen que se imprime por pantalla

Solo necesita numpy (para el bootstrap). Las colas de t y F salen de la misma
beta incompleta que analisis/estadistica.py, copiada aqui para que el directorio
sea autonomo.

Uso:
  python3 analisis_replicas.py
  python3 analisis_replicas.py --resultados prueba_humo --destino prueba_humo/analisis
"""

import argparse
import csv
import glob
import math
import os
import re
import sys
from collections import Counter, defaultdict

try:
    import numpy as np
except ImportError:
    sys.exit("[!] Falta numpy, que se usa para el bootstrap: pip install --user numpy")

DIR = os.path.dirname(os.path.abspath(__file__))
NAN = float("nan")

# El orden de las tablas del documento: SpMV primero.
KERNELS = ["spmv_static", "stencil"]
CONFIGS = ["base", "obs", "scheduler", "interleave", "bind_spread",
           "bind_close", "bind_close_il", "bind_spread_il"]
CONFIGS_CON_TOOL = {"obs", "scheduler"}

# (A, B, pregunta). dif = T_A - T_B en tiempo: NEGATIVO = A es mas rapida, el mismo
# convenio que la Tabla 9 del documento. Cada pareja es una familia para Holm.
COMPARACIONES = [
    ("scheduler",   "base",        "propuesta vs OpenMP puro"),
    ("scheduler",   "obs",         "efecto de migrar (scheduler vs control)"),
    ("obs",         "base",        "coste de monitorizar (control vs base)"),
    ("interleave",  "base",        "intercalado de memoria vs base"),
    ("bind_spread", "base",        "afinidad dispersa vs base"),
    ("scheduler",   "interleave",  "propuesta vs intercalado"),
    ("scheduler",   "bind_spread", "propuesta vs afinidad dispersa"),
]

# Los terminos de la descomposicion (Metodologia, ec. 7) con el signo del
# documento: termino = T_minuendo - T_sustraendo. G > 0 ahorra tiempo, C > 0 cuesta.
TERMINOS = [
    ("C_monitor",    "obs",  "base"),
    ("G_migrar",     "obs",  "scheduler"),
    ("G_neta",       "base", "scheduler"),
    ("G_interleave", "base", "interleave"),
    ("G_spread",     "base", "bind_spread"),
]

CONJUNTOS_ANOVA = [
    # El del documento (seccion 3.4.2): las configuraciones que compiten.
    ("tesis",        ["base", "bind_spread", "scheduler"]),
    # Con la mejor estatica en S5 dentro.
    ("competidoras", ["base", "bind_spread", "interleave", "scheduler"]),
]

POTENCIA = 0.80      # para la diferencia minima detectable

# Claves de procedencia.txt que deben ser IGUALES en todas las replicas.
CLAVES_CONSTANTES = ["host", "cpu", "compilador", "kernel_linux", "reps", "sizes",
                     "threads", "configs", "nmi_watchdog", "perf_event_paranoid",
                     "numa_balancing", "thp_enabled", "thp_defrag", "governor",
                     "boost", "entorno_openmp"]

# <kernel>_<S#>_t<hilos>_<config>[_dmnd]
TAG_RE = re.compile(r"^(?P<kernel>stencil|spmv_static)_(?P<tam>S\d+)_t(?P<hilos>\d+)_"
                    r"(?P<config>[a-z_]+?)(?P<dmnd>_dmnd)?$")


# ─────────────────────────────────────────────────────────────────────────────
# Distribuciones. Beta incompleta regularizada -> colas de t y F (copiadas de
# analisis/estadistica.py) y el cuantil de t por biseccion.
# ─────────────────────────────────────────────────────────────────────────────
def _betacf(a, b, x, itmax=200, eps=3e-16):
    """Fraccion continua de la beta incompleta (metodo de Lentz)."""
    tiny = 1e-300
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < tiny:
        d = tiny
    d = 1.0 / d
    h = d
    for m in range(1, itmax + 1):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < tiny:
            d = tiny
        c = 1.0 + aa / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < tiny:
            d = tiny
        c = 1.0 + aa / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < eps:
            break
    return h


def betainc(a, b, x):
    """Beta incompleta regularizada I_x(a,b)."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    lbeta = (math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
             + a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1.0) / (a + b + 2.0):
        return math.exp(lbeta) * _betacf(a, b, x) / a
    return 1.0 - math.exp(lbeta) * _betacf(b, a, 1.0 - x) / b


def p_two_sided_t(t, df):
    """p bilateral de una t de Student con df grados de libertad."""
    if df <= 0 or not math.isfinite(t) or not math.isfinite(df):
        return NAN
    return betainc(df / 2.0, 0.5, df / (df + t * t))


def p_upper_f(f, df1, df2):
    """Cola superior de una F(df1, df2)."""
    if f <= 0 or df1 <= 0 or df2 <= 0 or not math.isfinite(f) or not math.isfinite(df2):
        return NAN
    return betainc(df2 / 2.0, df1 / 2.0, df2 / (df2 + df1 * f))


def t_cdf(t, df):
    cola = 0.5 * betainc(df / 2.0, 0.5, df / (df + t * t))
    return 1.0 - cola if t >= 0 else cola


def t_ppf(q, df):
    """Cuantil q de la t de Student, por biseccion sobre t_cdf."""
    if not (0.0 < q < 1.0) or not math.isfinite(df) or df <= 0:
        return NAN
    if q < 0.5:
        return -t_ppf(1.0 - q, df)
    lo, hi = 0.0, 1.0
    while t_cdf(hi, df) < q:
        lo, hi = hi, 2.0 * hi
        if hi > 1e8:
            return NAN
    for _ in range(100):
        mid = 0.5 * (lo + hi)
        if t_cdf(mid, df) < q:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


# ─────────────────────────────────────────────────────────────────────────────
# Estadisticos
# ─────────────────────────────────────────────────────────────────────────────
def media_var(xs):
    n = len(xs)
    if n == 0:
        return NAN, NAN
    m = math.fsum(xs) / n
    v = math.fsum((x - m) ** 2 for x in xs) / (n - 1) if n > 1 else NAN
    return m, v


def welch(a, b, alfa):
    """t de Welch entre dos listas de medias de proceso. dif = media(a) - media(b)."""
    n1, n2 = len(a), len(b)
    m1, v1 = media_var(a)
    m2, v2 = media_var(b)
    r = dict(n_a=n1, n_b=n2, media_a=m1, media_b=m2, dif=m1 - m2, ee=NAN, t=NAN,
             gl=NAN, p=NAN, ic_lo=NAN, ic_hi=NAN, d=NAN, g=NAN, dmd=NAN)
    if n1 < 2 or n2 < 2:
        return r
    s1, s2 = v1 / n1, v2 / n2
    ee = math.sqrt(s1 + s2)
    if ee == 0:
        return r
    gl = (s1 + s2) ** 2 / (s1 * s1 / (n1 - 1) + s2 * s2 / (n2 - 1))
    t = (m1 - m2) / ee
    tq = t_ppf(1.0 - alfa / 2.0, gl)
    # d de Cohen con la definicion del documento: sqrt((s1^2 + s2^2) / 2).
    sd_prom = math.sqrt((v1 + v2) / 2.0)
    d = (m1 - m2) / sd_prom if sd_prom > 0 else NAN
    r.update(ee=ee, t=t, gl=gl, p=p_two_sided_t(t, gl),
             ic_lo=(m1 - m2) - tq * ee, ic_hi=(m1 - m2) + tq * ee, d=d,
             # g de Hedges: la d corregida por muestra pequena. Con n=10 por grupo
             # el factor es 0.958, no despreciable.
             g=d * (1.0 - 3.0 / (4.0 * (n1 + n2) - 9.0)),
             # Diferencia minima detectable: la que esta prueba, con esta
             # dispersion y este n, detectaria con probabilidad POTENCIA. Es lo que
             # da sentido a un "no significativo".
             dmd=(tq + t_ppf(POTENCIA, gl)) * ee)
    return r


def anova_clasica(grupos):
    """ANOVA de un factor (varianzas iguales), la del documento."""
    k = len(grupos)
    n_tot = sum(len(g) for g in grupos)
    if k < 2 or any(len(g) < 2 for g in grupos):
        return NAN, NAN, NAN, NAN, NAN
    gran = math.fsum(math.fsum(g) for g in grupos) / n_tot
    medias = [math.fsum(g) / len(g) for g in grupos]
    ss_entre = math.fsum(len(g) * (m - gran) ** 2 for g, m in zip(grupos, medias))
    ss_dentro = math.fsum(math.fsum((x - m) ** 2 for x in g) for g, m in zip(grupos, medias))
    gl1, gl2 = k - 1, n_tot - k
    if ss_dentro <= 0:
        return NAN, gl1, gl2, NAN, NAN
    f = (ss_entre / gl1) / (ss_dentro / gl2)
    return f, gl1, gl2, p_upper_f(f, gl1, gl2), ss_entre / (ss_entre + ss_dentro)


def anova_welch(grupos):
    """ANOVA de Welch: no supone varianzas iguales entre grupos. Aqui importa:
    la dispersion entre procesos del scheduler no tiene por que parecerse a la del
    base (la de uno depende de si migra, la del otro de donde cae el maestro)."""
    k = len(grupos)
    if k < 2 or any(len(g) < 2 for g in grupos):
        return NAN, NAN, NAN, NAN
    ns = [len(g) for g in grupos]
    mv = [media_var(g) for g in grupos]
    if any(v <= 0 for _, v in mv):
        return NAN, NAN, NAN, NAN
    w = [n / v for n, (_, v) in zip(ns, mv)]
    sw = math.fsum(w)
    mw = math.fsum(wi * m for wi, (m, _) in zip(w, mv)) / sw
    a = math.fsum(wi * (m - mw) ** 2 for wi, (m, _) in zip(w, mv)) / (k - 1)
    lam = math.fsum((1.0 - wi / sw) ** 2 / (n - 1) for wi, n in zip(w, ns))
    b = 1.0 + 2.0 * (k - 2) / (k * k - 1) * lam
    f = a / b
    gl2 = (k * k - 1) / (3.0 * lam)
    return f, k - 1, gl2, p_upper_f(f, k - 1, gl2)


def holm(ps):
    """p ajustadas por Holm-Bonferroni (controla el error de familia)."""
    validos = sorted((p, i) for i, p in enumerate(ps) if math.isfinite(p))
    m = len(validos)
    ajust = [NAN] * len(ps)
    acumulado = 0.0
    for rango, (p, i) in enumerate(validos):
        acumulado = max(acumulado, min(1.0, (m - rango) * p))
        ajust[i] = acumulado
    return ajust


def mediana(xs):
    o = sorted(xs)
    n = len(o)
    return o[n // 2] if n % 2 else 0.5 * (o[n // 2 - 1] + o[n // 2])


def magnitud(x):
    a = abs(x)
    if not math.isfinite(a):
        return "NA"
    if a < 0.2:
        return "despreciable"
    if a < 0.5:
        return "pequeno"
    if a < 0.8:
        return "medio"
    return "grande"


def fmt(x, patron="{:.4f}"):
    if isinstance(x, (int, float)) and math.isfinite(x):
        return patron.format(x)
    return "NA"


def fp(p):
    return fmt(p, "{:.3e}")


# ─────────────────────────────────────────────────────────────────────────────
# Lectura
# ─────────────────────────────────────────────────────────────────────────────
def leer_csv(ruta):
    with open(ruta, newline="") as f:
        return list(csv.DictReader(f))


def num(x):
    try:
        v = float(x)
    except (TypeError, ValueError):
        return NAN
    return v if math.isfinite(v) else NAN


def tag_de_fila(r):
    sufijo = "_dmnd" if r.get("familia") == "dmnd" else ""
    return (f"{r.get('kernel')}_{r.get('size_tag')}_t{r.get('threads')}_"
            f"{r.get('config')}{sufijo}")


def ultima_fila_valida(ruta):
    """kernel_metrics.csv solo se anexa: un reintento deja la fila vieja (con NA si
    fallo) y la nueva debajo. Vale la ULTIMA fila con avg_ms numerico."""
    filas = {}
    if os.path.exists(ruta):
        for r in leer_csv(ruta):
            if math.isfinite(num(r.get("avg_ms"))):
                filas[tag_de_fila(r)] = r
    return filas


def leer_tiempos(ruta):
    with open(ruta, newline="") as f:
        return [v for v in (num(r.get("time_ms")) for r in csv.DictReader(f))
                if math.isfinite(v)]


def leer_por_tag(ruta):
    """{tag: ultima fila}. Para cronologia.csv, donde un reintento anexa otra fila."""
    return {r["tag"]: r for r in leer_csv(ruta)} if os.path.exists(ruta) else {}


def leer_procedencia(ruta):
    """{clave: {valores}}; las huellas van como 'sha256:<fichero>'."""
    datos = defaultdict(set)
    if os.path.exists(ruta):
        with open(ruta) as f:
            for linea in f:
                linea = linea.rstrip("\n")
                if linea and not linea.startswith("#") and "=" in linea:
                    clave, valor = linea.split("=", 1)
                    datos[clave].add(valor.strip())
    return datos


def ratio_valido(x):
    """-1 es el centinela de 'no medible', no un cociente."""
    return x if math.isfinite(x) and x >= 0 else NAN


def cargar_procesos(dir_resultados, descartes):
    procesos, procedencias = [], {}
    replicas = sorted(d for d in glob.glob(os.path.join(dir_resultados, "*"))
                      if os.path.isdir(os.path.join(d, "metrics")))
    for dir_rep in replicas:
        rep = os.path.basename(dir_rep)
        km = ultima_fila_valida(os.path.join(dir_rep, "kernel_metrics.csv"))
        crono = leer_por_tag(os.path.join(dir_rep, "cronologia.csv"))
        procedencias[rep] = leer_procedencia(os.path.join(dir_rep, "procedencia.txt"))
        for ruta in sorted(glob.glob(os.path.join(dir_rep, "metrics", "*_times.csv"))):
            tag = os.path.basename(ruta)[:-len("_times.csv")]
            m = TAG_RE.match(tag)
            if not m:
                descartes["nombre de fichero no reconocido"] += 1
                continue
            if m.group("dmnd"):
                descartes["familia dmnd (fuera del diseno)"] += 1
                continue
            if m.group("config") not in CONFIGS:
                descartes[f"configuracion '{m.group('config')}'"] += 1
                continue
            # Sin marcador la ejecucion fallo o la corto SLURM: sus ficheros, si
            # existen, son de un intento incompleto. La suite los borra antes de
            # reintentar, asi que lo que queda sin .done no es fiable.
            if not os.path.exists(os.path.join(dir_rep, ".done", tag)):
                descartes["sin marcador .done (fallida o cortada)"] += 1
                continue
            xs = leer_tiempos(ruta)
            if len(xs) < 2:
                descartes["menos de 2 repeticiones"] += 1
                continue
            media, var = media_var(xs)
            fila = km.get(tag, {})
            c = crono.get(tag, {})
            ini, fin = num(c.get("inicio_epoch")), num(c.get("fin_epoch"))
            p = dict(replica=rep, tag=tag, kernel=m.group("kernel"), tam=m.group("tam"),
                     hilos=int(m.group("hilos")), config=m.group("config"),
                     n_reps=len(xs), media_ms=media, sd_intra_ms=math.sqrt(var),
                     mediana_ms=mediana(xs), min_ms=min(xs), max_ms=max(xs),
                     avg_ms_kernel=num(fila.get("avg_ms")),
                     ratio_rm=ratio_valido(num(fila.get("ratio_rm"))),
                     migraciones=num(fila.get("migrations")),
                     posicion=num(c.get("posicion")), job=c.get("job", "NA"),
                     inicio_epoch=ini, duracion_s=fin - ini,
                     cb_us_total=NAN, monitor_us_total=NAN, monitor_ticks=NAN,
                     hilos_seguidos=NAN)
            if p["config"] in CONFIGS_CON_TOOL:
                ruta_ovh = os.path.join(dir_rep, "metrics", f"{tag}_ompt_overhead.csv")
                if os.path.exists(ruta_ovh):
                    filas_ovh = leer_csv(ruta_ovh)
                    if filas_ovh:
                        o = filas_ovh[-1]
                        p.update(cb_us_total=num(o.get("callback_us_total")),
                                 monitor_us_total=num(o.get("monitor_us_total")),
                                 monitor_ticks=num(o.get("monitor_ticks")),
                                 hilos_seguidos=num(o.get("threads_tracked")))
            procesos.append(p)
    return procesos, procedencias


def comprobar_procedencia(procedencias, avisos):
    """Todas las replicas deben venir de los mismos binarios y del mismo entorno."""
    union = defaultdict(lambda: defaultdict(list))   # clave -> valor -> [replicas]
    for rep, datos in procedencias.items():
        if not datos:
            avisos.append(f"{rep}: sin procedencia.txt (no se puede verificar la version)")
            continue
        for clave, valores in datos.items():
            if clave.startswith("sha256:") or clave in CLAVES_CONSTANTES:
                for v in valores:
                    union[clave][v].append(rep)
    for clave in sorted(union):
        if len(union[clave]) < 2:
            continue
        partes = []
        for valor, reps in sorted(union[clave].items()):
            corto = valor if len(valor) <= 24 else valor[:12] + "..."
            partes.append(f"'{corto}' en {','.join(sorted(set(reps)))}")
        gravedad = "MEZCLA DE VERSIONES" if clave.startswith("sha256:") else "entorno distinto"
        avisos.append(f"{gravedad} en '{clave}': " + "; ".join(partes))


# ─────────────────────────────────────────────────────────────────────────────
def orden_celda(celda):
    kernel, tam, hilos = celda
    return (KERNELS.index(kernel) if kernel in KERNELS else 99, int(tam[1:]), hilos)


def ruta_legible(ruta):
    rel = os.path.relpath(ruta)
    return os.path.abspath(ruta) if rel.startswith("..") else rel


def escribir_csv(ruta, cabecera, filas):
    with open(ruta, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(cabecera)
        w.writerows(filas)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--resultados", default=os.path.join(DIR, "resultados"),
                    help="directorio con una carpeta por replica (r01, r02...)")
    ap.add_argument("--destino", default=os.path.join(DIR, "analisis"))
    ap.add_argument("--referencia", default=os.path.join(DIR, "..", "perf_out_v5"),
                    help="campana de UNA ejecucion por punto con la que comparar (29390)")
    ap.add_argument("--alfa", type=float, default=0.05)
    ap.add_argument("--bootstrap", type=int, default=10000,
                    help="remuestreos de procesos para los IC bootstrap")
    ap.add_argument("--semilla", type=int, default=20260930,
                    help="semilla del bootstrap (el resultado es reproducible)")
    ap.add_argument("--min-procesos", type=int, default=2,
                    help="procesos minimos por grupo para hacer una prueba")
    args = ap.parse_args()
    alfa = args.alfa

    salida = []

    def out(linea=""):
        salida.append(linea)
        print(linea)

    descartes = Counter()
    procesos, procedencias = cargar_procesos(args.resultados, descartes)
    if not procesos:
        print(f"[!] No hay procesos validos en {args.resultados}/<rNN>/metrics/")
        if descartes:
            print("    descartados: " + ", ".join(f"{k} x{v}" for k, v in descartes.items()))
        return 1

    # Todas las ejecuciones deben tener el mismo numero de repeticiones: mezclar una
    # prueba de humo (REPS=20) con la campana (150) mezclaria dos experimentos.
    n_modal = Counter(p["n_reps"] for p in procesos).most_common(1)[0][0]
    otras = [p for p in procesos if p["n_reps"] != n_modal]
    if otras:
        descartes[f"repeticiones != {n_modal}"] += len(otras)
        procesos = [p for p in procesos if p["n_reps"] == n_modal]

    avisos = []
    comprobar_procedencia(procedencias, avisos)
    incoherentes = [p for p in procesos if math.isfinite(p["avg_ms_kernel"]) and
                    abs(p["media_ms"] / p["avg_ms_kernel"] - 1.0) > 1e-3]
    if incoherentes:
        avisos.append(f"{len(incoherentes)} procesos cuya media de _times.csv no coincide con "
                      f"el avg_ms de kernel_metrics.csv (ej. {incoherentes[0]['replica']}/"
                      f"{incoherentes[0]['tag']}): ficheros de intentos distintos")

    grupos = defaultdict(lambda: defaultdict(list))    # celda -> config -> [procesos]
    for p in sorted(procesos, key=lambda q: q["replica"]):
        grupos[(p["kernel"], p["tam"], p["hilos"])][p["config"]].append(p)
    celdas = sorted(grupos, key=orden_celda)
    configs_vistas = [c for c in CONFIGS if any(c in grupos[k] for k in celdas)]
    replicas = sorted({p["replica"] for p in procesos})
    tamanos_n = [len(grupos[k][c]) for k in celdas for c in grupos[k]]

    os.makedirs(args.destino, exist_ok=True)

    out(f"=== Campana de replicas: {ruta_legible(args.resultados)} ===")
    out(f"    replicas : {len(replicas)} ({', '.join(replicas)})")
    out(f"    procesos : {len(procesos)} validos, {n_modal} repeticiones cada uno")
    out(f"    celdas   : {len(celdas)}  (kernel x tamano x hilos)")
    out(f"    configs  : {', '.join(configs_vistas)}")
    out(f"    procesos por grupo: min {min(tamanos_n)}, max {max(tamanos_n)}")
    if descartes:
        out("    descartados: " + ", ".join(f"{k} x{v}" for k, v in sorted(descartes.items())))
    if any(procedencias.values()) and not any(a.startswith("MEZCLA") for a in avisos):
        out("    procedencia: mismos binarios (sha256) en todas las replicas")
    for a in avisos:
        out(f"    AVISO: {a}")
    incompletos = [(k, c, len(grupos[k][c])) for k in celdas for c in configs_vistas
                   if len(grupos[k].get(c, [])) < max(tamanos_n)]
    if incompletos:
        out(f"    AVISO: {len(incompletos)} grupos con menos de {max(tamanos_n)} procesos, p.ej. "
            + ", ".join(f"{k[0]}_{k[1]}_t{k[2]}_{c}={n}" for k, c, n in incompletos[:4]))

    # ── procesos.csv ────────────────────────────────────────────────────────
    cols_p = ["kernel", "tam", "hilos", "config", "replica", "n_reps", "media_ms",
              "sd_intra_ms", "mediana_ms", "min_ms", "max_ms", "avg_ms_kernel",
              "ratio_rm", "migraciones", "posicion", "job", "inicio_epoch",
              "duracion_s", "cb_us_total", "monitor_us_total", "monitor_ticks",
              "hilos_seguidos"]
    filas = []
    for k in celdas:
        for c in configs_vistas:
            for p in grupos[k].get(c, []):
                filas.append([p["kernel"], p["tam"], p["hilos"], p["config"], p["replica"],
                              p["n_reps"], fmt(p["media_ms"], "{:.6f}"),
                              fmt(p["sd_intra_ms"], "{:.6f}"), fmt(p["mediana_ms"], "{:.6f}"),
                              fmt(p["min_ms"], "{:.6f}"), fmt(p["max_ms"], "{:.6f}"),
                              fmt(p["avg_ms_kernel"], "{:.6f}"), fmt(p["ratio_rm"], "{:.6f}"),
                              fmt(p["migraciones"], "{:.0f}"), fmt(p["posicion"], "{:.0f}"),
                              p["job"], fmt(p["inicio_epoch"], "{:.3f}"),
                              fmt(p["duracion_s"], "{:.3f}"), fmt(p["cb_us_total"], "{:.3f}"),
                              fmt(p["monitor_us_total"], "{:.3f}"),
                              fmt(p["monitor_ticks"], "{:.0f}"), fmt(p["hilos_seguidos"], "{:.0f}")])
    escribir_csv(os.path.join(args.destino, "procesos.csv"), cols_p, filas)

    # ── resumen_celdas.csv ──────────────────────────────────────────────────
    # factor_ee = DE entre procesos / (DE intra / sqrt(reps)). El denominador es el
    # error estandar que el analisis intra-proceso atribuia a la media de UN
    # proceso; el numerador, cuanto varia de verdad esa media de un proceso a otro.
    # 1 = el analisis intra-proceso acertaba; 10 = subestimaba la incertidumbre 10x
    # (por la variacion entre procesos o por la autocorrelacion de las repeticiones:
    # las dos razones por las que el documento lo declaraba como limitacion).
    resumen = {}
    filas = []
    for k in celdas:
        for c in configs_vistas:
            ps = grupos[k].get(c, [])
            if not ps:
                continue
            medias = [p["media_ms"] for p in ps]
            n = len(medias)
            m, v = media_var(medias)
            sd = math.sqrt(v) if math.isfinite(v) else NAN
            semi = t_ppf(1.0 - alfa / 2.0, n - 1) * sd / math.sqrt(n) if n > 1 else NAN
            s2_intra = math.fsum(p["sd_intra_ms"] ** 2 for p in ps) / n
            ee_intra = math.sqrt(s2_intra / n_modal)
            factor = sd / ee_intra if ee_intra > 0 else NAN
            sigma_b2 = max(0.0, v - s2_intra / n_modal) if math.isfinite(v) else NAN
            icc = sigma_b2 / (sigma_b2 + s2_intra) if math.isfinite(sigma_b2) else NAN
            ratios = [p["ratio_rm"] for p in ps if math.isfinite(p["ratio_rm"])]
            rm_m, rm_v = media_var(ratios) if ratios else (NAN, NAN)
            migs = [p["migraciones"] for p in ps if math.isfinite(p["migraciones"])]
            con_tool = c in CONFIGS_CON_TOOL
            ordenadas = sorted(medias)
            resumen[(k, c)] = dict(n=n, media=m, sd=sd, factor=factor, icc=icc,
                                   rm_media=rm_m, rm_min=min(ratios) if ratios else NAN,
                                   rm_max=max(ratios) if ratios else NAN,
                                   migran=sum(1 for x in migs if x > 0) if con_tool else None,
                                   n_mig=len(migs), medias=medias,
                                   min=ordenadas[0], max=ordenadas[-1])
            filas.append([k[0], k[1], k[2], c, n, fmt(m), fmt(m - semi), fmt(m + semi),
                          fmt(sd), fmt(100.0 * sd / m, "{:.3f}"), fmt(mediana(medias)),
                          fmt(ordenadas[0]), fmt(ordenadas[-1]),
                          fmt(100.0 * (ordenadas[-1] - ordenadas[0]) / m, "{:.3f}"),
                          fmt(math.sqrt(s2_intra)), fmt(ee_intra, "{:.6f}"),
                          fmt(factor, "{:.2f}"), fmt(icc, "{:.4f}"),
                          fmt(rm_m, "{:.6f}"),
                          fmt(math.sqrt(rm_v) if math.isfinite(rm_v) else NAN, "{:.6f}"),
                          fmt(min(ratios) if ratios else NAN, "{:.6f}"),
                          fmt(max(ratios) if ratios else NAN, "{:.6f}"),
                          (sum(1 for x in migs if x > 0) if con_tool else ""),
                          (fmt(sum(migs) / len(migs), "{:.2f}") if con_tool and migs else ""),
                          (fmt(min(migs), "{:.0f}") if con_tool and migs else ""),
                          (fmt(max(migs), "{:.0f}") if con_tool and migs else "")])
    escribir_csv(os.path.join(args.destino, "resumen_celdas.csv"),
                 ["kernel", "tam", "hilos", "config", "n_procesos", "media_ms",
                  "ic95_lo_ms", "ic95_hi_ms", "sd_entre_ms", "cv_entre_pct", "mediana_ms",
                  "min_ms", "max_ms", "rango_pct", "sd_intra_ms", "ee_intra_ms",
                  "factor_ee", "icc", "ratio_rm_media", "ratio_rm_sd", "ratio_rm_min",
                  "ratio_rm_max", "procesos_con_migracion", "migraciones_media",
                  "migraciones_min", "migraciones_max"], filas)

    # ── Bootstrap: medias remuestreadas por (celda, config) ─────────────────
    # Se remuestrean PROCESOS, independientemente en cada configuracion, una sola
    # vez por celda: todas las diferencias de la celda salen de los mismos
    # remuestreos, asi que G_neta = G_migrar - C_monitor se cumple en cada uno.
    rng = np.random.default_rng(args.semilla)
    boot = {}
    for k in celdas:
        for c in configs_vistas:
            ps = grupos[k].get(c, [])
            if len(ps) >= args.min_procesos:
                x = np.array([p["media_ms"] for p in ps])
                idx = rng.integers(0, len(x), size=(args.bootstrap, len(x)))
                boot[(k, c)] = x[idx].mean(axis=1)
    q_lo, q_hi = 100.0 * alfa / 2.0, 100.0 * (1.0 - alfa / 2.0)

    def ic_boot(k, a, b):
        if (k, a) not in boot or (k, b) not in boot:
            return NAN, NAN
        lo, hi = np.percentile(boot[(k, a)] - boot[(k, b)], [q_lo, q_hi])
        return float(lo), float(hi)

    def medias_de(k, c):
        ps = grupos[k].get(c, [])
        return [p["media_ms"] for p in ps] if len(ps) >= args.min_procesos else []

    # ── contrastes.csv ──────────────────────────────────────────────────────
    reg_contrastes = []
    for ca, cb, pregunta in COMPARACIONES:
        for k in celdas:
            a, b = medias_de(k, ca), medias_de(k, cb)
            if not a or not b:
                continue
            w = welch(a, b, alfa)
            reg_contrastes.append((ca, cb, pregunta, k, w, ic_boot(k, ca, cb)))
    for ca, cb, _ in COMPARACIONES:
        familia = [i for i, r in enumerate(reg_contrastes) if (r[0], r[1]) == (ca, cb)]
        for i, ph in zip(familia, holm([reg_contrastes[i][4]["p"] for i in familia])):
            reg_contrastes[i][4]["p_holm"] = ph
    # Filas por celda y, dentro de la celda, en el orden de COMPARACIONES.
    orden_cmp = {(ca, cb): i for i, (ca, cb, _) in enumerate(COMPARACIONES)}
    reg_contrastes.sort(key=lambda r: (orden_celda(r[3]), orden_cmp[(r[0], r[1])]))
    filas = []
    for ca, cb, pregunta, k, w, (blo, bhi) in reg_contrastes:
        mb = w["media_b"]
        filas.append([k[0], k[1], k[2], ca, cb, pregunta, w["n_a"], w["n_b"],
                      fmt(w["media_a"]), fmt(mb), fmt(w["dif"]),
                      fmt(100.0 * w["dif"] / mb, "{:+.3f}"),
                      fmt(w["ic_lo"]), fmt(w["ic_hi"]), fmt(blo), fmt(bhi),
                      fmt(w["t"]), fmt(w["gl"], "{:.2f}"), fp(w["p"]), fp(w["p_holm"]),
                      "si" if w["p_holm"] < alfa else "no",
                      fmt(w["d"], "{:.3f}"), fmt(w["g"], "{:.3f}"), magnitud(w["g"]),
                      fmt(w["dmd"]), fmt(100.0 * w["dmd"] / mb, "{:.3f}")])
    escribir_csv(os.path.join(args.destino, "contrastes.csv"),
                 ["kernel", "tam", "hilos", "config_a", "config_b", "pregunta", "n_a", "n_b",
                  "media_a_ms", "media_b_ms", "dif_ms", "dif_pct", "ic95_lo_ms", "ic95_hi_ms",
                  "ic95_boot_lo_ms", "ic95_boot_hi_ms", "t", "gl", "p", "p_holm",
                  "significativo", "cohen_d", "hedges_g", "magnitud", "dmd_ms", "dmd_pct"],
                 filas)

    # ── descomposicion.csv ──────────────────────────────────────────────────
    terminos = {}
    reg_terminos = []
    for nombre, c1, c2 in TERMINOS:
        for k in celdas:
            a, b = medias_de(k, c1), medias_de(k, c2)
            if not a or not b:
                continue
            w = welch(a, b, alfa)
            reg_terminos.append((nombre, c1, c2, k, w, ic_boot(k, c1, c2)))
    for nombre, _, _ in TERMINOS:
        familia = [i for i, r in enumerate(reg_terminos) if r[0] == nombre]
        for i, ph in zip(familia, holm([reg_terminos[i][4]["p"] for i in familia])):
            reg_terminos[i][4]["p_holm"] = ph
    orden_ter = {nombre: i for i, (nombre, _, _) in enumerate(TERMINOS)}
    reg_terminos.sort(key=lambda r: (orden_celda(r[3]), orden_ter[r[0]]))
    filas = []
    for nombre, c1, c2, k, w, (blo, bhi) in reg_terminos:
        terminos[(k, nombre)] = (w, blo, bhi)
        t_base = resumen.get((k, "base"), {}).get("media", NAN)
        filas.append([k[0], k[1], k[2], nombre, f"T_{c1} - T_{c2}", w["n_a"], w["n_b"],
                      fmt(w["dif"]), fmt(100.0 * w["dif"] / t_base, "{:+.3f}"),
                      fmt(w["ic_lo"]), fmt(w["ic_hi"]), fmt(blo), fmt(bhi),
                      fp(w["p"]), fp(w["p_holm"]), "si" if w["p_holm"] < alfa else "no"])
    escribir_csv(os.path.join(args.destino, "descomposicion.csv"),
                 ["kernel", "tam", "hilos", "termino", "definicion", "n_1", "n_2",
                  "valor_ms", "valor_pct_de_base", "ic95_lo_ms", "ic95_hi_ms",
                  "ic95_boot_lo_ms", "ic95_boot_hi_ms", "p", "p_holm", "significativo"],
                 filas)
    # La identidad del documento, G_neta = G_migrar - C_monitor, es exacta con las
    # medias por proceso si los tres terminos salen de los mismos procesos. Si no
    # cuadra, faltan procesos en alguna configuracion o hay grupos incompletos.
    fuera = 0
    comprobadas = 0
    for k in celdas:
        if all((k, t) in terminos for t in ("G_neta", "G_migrar", "C_monitor")):
            comprobadas += 1
            res = (terminos[(k, "G_neta")][0]["dif"] - terminos[(k, "G_migrar")][0]["dif"]
                   + terminos[(k, "C_monitor")][0]["dif"])
            if abs(res) > 1e-9 * max(1.0, abs(terminos[(k, "G_neta")][0]["media_a"])):
                fuera += 1

    # ── anova.csv ───────────────────────────────────────────────────────────
    reg_anova = []
    for conjunto, cfgs in CONJUNTOS_ANOVA:
        for k in celdas:
            gs = [medias_de(k, c) for c in cfgs]
            if any(not g for g in gs):
                continue
            reg_anova.append(dict(conjunto=conjunto, cfgs=cfgs, celda=k, ns=[len(g) for g in gs],
                                  clasica=anova_clasica(gs), welch=anova_welch(gs)))
    for conjunto, _ in CONJUNTOS_ANOVA:
        familia = [r for r in reg_anova if r["conjunto"] == conjunto]
        for r, ph in zip(familia, holm([r["clasica"][3] for r in familia])):
            r["p_holm"] = ph
        for r, ph in zip(familia, holm([r["welch"][3] for r in familia])):
            r["p_welch_holm"] = ph
    reg_anova.sort(key=lambda r: (orden_celda(r["celda"]), r["conjunto"] != "tesis"))
    filas = []
    for r in reg_anova:
        f, g1, g2, p, eta2 = r["clasica"]
        fw, gw1, gw2, pw = r["welch"]
        k = r["celda"]
        filas.append([k[0], k[1], k[2], r["conjunto"], "|".join(r["cfgs"]),
                      "|".join(str(n) for n in r["ns"]), fmt(f), fmt(g1, "{:.0f}"),
                      fmt(g2, "{:.0f}"), fp(p), fp(r["p_holm"]), fmt(eta2),
                      fmt(fw), fmt(gw1, "{:.0f}"), fmt(gw2, "{:.2f}"), fp(pw),
                      fp(r["p_welch_holm"]), "si" if r["p_welch_holm"] < alfa else "no"])
    escribir_csv(os.path.join(args.destino, "anova.csv"),
                 ["kernel", "tam", "hilos", "conjunto", "configs", "n_por_grupo", "F", "gl1",
                  "gl2", "p", "p_holm", "eta2", "F_welch", "gl1_welch", "gl2_welch", "p_welch",
                  "p_welch_holm", "significativo_welch"], filas)

    # ── Resumen legible ─────────────────────────────────────────────────────
    out()
    out("--- 1. Cuanto subestimaba el error estandar intra-proceso ---")
    out("    factor = DE entre procesos / (DE intra / sqrt(reps)); 1 = el analisis de")
    out("    las 150 repeticiones acertaba, 10 = subestimaba la incertidumbre 10 veces")
    out(f"    {'kernel':<13}{'tam':<5}{'mediana':>9}{'min':>9}{'max':>9}   (sobre hilos y configs)")
    por_kt = defaultdict(list)
    for (k, c), r in resumen.items():
        if math.isfinite(r["factor"]):
            por_kt[(k[0], k[1])].append(r["factor"])
    for (kern, tam) in sorted(por_kt, key=lambda x: (KERNELS.index(x[0]), x[1])):
        fs = por_kt[(kern, tam)]
        out(f"    {kern:<13}{tam:<5}{mediana(fs):>9.1f}{min(fs):>9.1f}{max(fs):>9.1f}")
    peores = sorted(((r["factor"], k, c) for (k, c), r in resumen.items()
                     if math.isfinite(r["factor"])), reverse=True)[:5]
    if peores:
        out("    mayores: " + ", ".join(f"{k[0]}_{k[1]}_t{k[2]}_{c}={f:.1f}" for f, k, c in peores))

    out()
    out(f"--- 2. Descomposicion en ms (IC{int(round(100 * (1 - alfa)))} de Welch); "
        f"* = significativo con Holm ---")
    out("    G > 0: la propuesta ahorra tiempo.  C > 0: monitorizar cuesta tiempo.")
    out(f"    {'kernel':<13}{'tam':<4}{'hilos':>5}  {'G_neta':>28}  {'C_monitor':>28}  "
        f"{'G_migrar':>28}  migran")

    def celda_termino(k, nombre):
        if (k, nombre) not in terminos:
            return f"{'NA':>28}"
        w = terminos[(k, nombre)][0]
        marca = "*" if w["p_holm"] < alfa else " "
        return f"{w['dif']:>+10.2f}{marca} [{w['ic_lo']:+8.2f},{w['ic_hi']:+8.2f}]"

    for k in celdas:
        r = resumen.get((k, "scheduler"))
        migran = f"{r['migran']}/{r['n_mig']}" if r and r["migran"] is not None else "NA"
        out(f"    {k[0]:<13}{k[1]:<4}{k[2]:>5}  {celda_termino(k, 'G_neta')}  "
            f"{celda_termino(k, 'C_monitor')}  {celda_termino(k, 'G_migrar')}  {migran:>6}")
    if comprobadas:
        out(f"    identidad G_neta = G_migrar - C_monitor: {comprobadas - fuera}/{comprobadas} "
            f"celdas cuadran" + ("" if not fuera else "  <-- REVISAR"))

    out()
    out("--- 3. ANOVA de un factor por celda (Welch, p corregida por Holm) ---")
    for conjunto, cfgs in CONJUNTOS_ANOVA:
        rs = [r for r in reg_anova if r["conjunto"] == conjunto]
        if not rs:
            out(f"    {conjunto} ({', '.join(cfgs)}): faltan configuraciones")
            continue
        sig = [r for r in rs if r["p_welch_holm"] < alfa]
        out(f"    {conjunto:<13}({', '.join(cfgs)}): significativo en {len(sig)}/{len(rs)} celdas")
        no_sig = [r["celda"] for r in rs if not r["p_welch_holm"] < alfa]
        if no_sig and len(no_sig) <= 8:
            out("        no significativas: " + ", ".join(f"{k[0]}_{k[1]}_t{k[2]}" for k in no_sig))

    # ── contraste_29390.csv ─────────────────────────────────────────────────
    ref = args.referencia
    con_referencia = False
    if os.path.isdir(os.path.join(ref, "metrics")):
        km_ref = ultima_fila_valida(os.path.join(ref, "kernel_metrics.csv"))
        filas, fuera_rango, extremos = [], 0, []
        for k in celdas:
            for c in configs_vistas:
                r = resumen.get((k, c))
                tag = f"{k[0]}_{k[1]}_t{k[2]}_{c}"
                ruta = os.path.join(ref, "metrics", f"{tag}_times.csv")
                if not r or r["n"] < 2 or not os.path.exists(ruta):
                    continue
                xs = leer_tiempos(ruta)
                if len(xs) < 2:
                    continue
                v = math.fsum(xs) / len(xs)
                z = (v - r["media"]) / r["sd"] if r["sd"] > 0 else NAN
                debajo = sum(1 for x in r["medias"] if x < v)
                empates = sum(1 for x in r["medias"] if x == v)
                percentil = 100.0 * (debajo + 0.5 * empates) / r["n"]
                dentro = r["min"] <= v <= r["max"]
                fuera_rango += 0 if dentro else 1
                if math.isfinite(z):
                    extremos.append((abs(z), z, tag))
                fr = km_ref.get(tag, {})
                filas.append([k[0], k[1], k[2], c, fmt(v), fmt(r["media"]), fmt(r["sd"]),
                              fmt(r["min"]), fmt(r["max"]), fmt(z, "{:+.2f}"),
                              fmt(percentil, "{:.1f}"), "si" if dentro else "no",
                              fmt(ratio_valido(num(fr.get("ratio_rm"))), "{:.6f}"),
                              fmt(r["rm_media"], "{:.6f}"), fmt(r["rm_min"], "{:.6f}"),
                              fmt(r["rm_max"], "{:.6f}"), fmt(num(fr.get("migrations")), "{:.0f}"),
                              ("instrumentacion anterior (con IPC)" if c in CONFIGS_CON_TOOL
                               else "")])
        if filas:
            con_referencia = True
            escribir_csv(os.path.join(args.destino, "contraste_29390.csv"),
                         ["kernel", "tam", "hilos", "config", "valor_29390_ms",
                          "media_replicas_ms", "sd_replicas_ms", "min_replicas_ms",
                          "max_replicas_ms", "z", "percentil", "dentro_rango",
                          "ratio_rm_29390", "ratio_rm_replicas_media", "ratio_rm_replicas_min",
                          "ratio_rm_replicas_max", "migraciones_29390", "nota"], filas)
            out()
            out(f"--- 4. La ejecucion unica de 29390 frente a las replicas ({ruta_legible(ref)}) ---")
            out(f"    fuera del rango [min, max] de las replicas: {fuera_rango} de {len(filas)} grupos")
            extremos.sort(reverse=True)
            if extremos:
                out("    mas atipicas: " + ", ".join(f"{t} z={z:+.1f}" for _, z, t in extremos[:5]))
            out("    (obs y scheduler de 29390 son de la build con IPC: mismo disparador,")
            out("     el doble de lecturas de contadores por ventana)")

    out()
    out(f"[ok] {len(procesos)} procesos -> {ruta_legible(args.destino)}/: procesos.csv, "
        "resumen_celdas.csv, contrastes.csv, descomposicion.csv, anova.csv"
        + (", contraste_29390.csv" if con_referencia else ""))
    with open(os.path.join(args.destino, "resumen.txt"), "w") as f:
        f.write("\n".join(salida) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
