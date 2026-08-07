#!/usr/bin/env python3
"""
Contraste estadistico de la campana (Fase 4 de la metodologia).

Lee los ficheros metrics/<kernel>_t<N>_<config>_times.csv, que contienen las REPS
repeticiones individuales de cada corrida, y produce:

  estadistica.csv  -> prueba t de Welch por pareja de configuraciones
  anova.csv        -> ANOVA de un factor entre las configuraciones que compiten

LIMITACION QUE HAY QUE DECLARAR EN LA MEMORIA
---------------------------------------------
La muestra es INTRA-PROCESO: son las 150 iteraciones de UNA ejecucion. Captura la
variabilidad iteracion a iteracion, pero NO la variabilidad entre procesos
(colocacion de paginas, loteria de binding, estado de la maquina). Ademas las
observaciones estan autocorrelacionadas, asi que los grados de libertad efectivos
son menores que n-1 y el p-valor sale optimista.

Con n=150 casi cualquier diferencia sale "significativa": por eso se reporta
SIEMPRE el tamano del efecto (d de Cohen) junto al p-valor, y la interpretacion
debe apoyarse en el primero. La convencion habitual: |d|<0.2 despreciable,
0.2-0.5 pequeno, 0.5-0.8 medio, >0.8 grande.

Sin dependencias obligatorias mas alla de la biblioteca estandar (usa scipy si
esta disponible, pero trae su propia implementacion de la beta incompleta).
"""

import argparse
import csv
import glob
import math
import os
import re
from collections import defaultdict

# ── Configuraciones y su papel (ver la cabecera de run_perf_metricsV4.sh) ─────
CONFIGS = ["spread", "close", "nobind", "obs", "ovh", "scheduler"]

# Parejas que se contrastan. Cada una responde a UNA pregunta concreta.
COMPARACIONES = [
    ("scheduler", "spread", "propuesta vs baseline estatico"),
    ("scheduler", "close",  "propuesta vs baseline secundario"),
    ("ovh",       "spread", "overhead del tool a igual afinidad"),
    ("nobind",    "spread", "coste de no fijar afinidad"),
    ("obs",       "nobind", "coste de monitorizar"),
    ("scheduler", "obs",    "efecto de migrar"),
]

# Solo estas compiten de verdad; los controles no entran en el ANOVA.
CONFIGS_ANOVA = ["spread", "close", "scheduler"]

TAG_RE = re.compile(r"^(?P<kernel>.+)_t(?P<threads>\d+)_(?P<config>[a-z]+)_times\.csv$")


# ─────────────────────────────────────────────────────────────────────────────
# Distribuciones: beta incompleta regularizada -> colas de t y de F.
# Se implementa a mano para no exigir scipy (mismo criterio que analyze_plans.py).
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
    if df <= 0 or not math.isfinite(t):
        return float("nan")
    return betainc(df / 2.0, 0.5, df / (df + t * t))


def p_upper_f(f, df1, df2):
    """Cola superior de una F(df1, df2)."""
    if f <= 0 or df1 <= 0 or df2 <= 0 or not math.isfinite(f):
        return float("nan")
    return betainc(df2 / 2.0, df1 / 2.0, df2 / (df2 + df1 * f))


# ─────────────────────────────────────────────────────────────────────────────
# Estadisticos
# ─────────────────────────────────────────────────────────────────────────────
def media_var(xs):
    n = len(xs)
    m = sum(xs) / n
    v = sum((x - m) ** 2 for x in xs) / (n - 1) if n > 1 else 0.0
    return m, v


def welch(a, b):
    """t de Welch (no asume varianzas iguales) + gl de Welch-Satterthwaite."""
    n1, n2 = len(a), len(b)
    m1, v1 = media_var(a)
    m2, v2 = media_var(b)
    s1, s2 = v1 / n1, v2 / n2
    denom = math.sqrt(s1 + s2)
    if denom == 0:
        return m1, m2, float("nan"), float("nan"), float("nan"), float("nan")
    t = (m1 - m2) / denom
    num = (s1 + s2) ** 2
    den = (s1 * s1 / (n1 - 1) if n1 > 1 else 0.0) + (s2 * s2 / (n2 - 1) if n2 > 1 else 0.0)
    df = num / den if den > 0 else float("nan")
    # d de Cohen con desviacion agrupada
    sp = math.sqrt(((n1 - 1) * v1 + (n2 - 1) * v2) / (n1 + n2 - 2)) if n1 + n2 > 2 else 0.0
    d = (m1 - m2) / sp if sp > 0 else float("nan")
    return m1, m2, t, df, p_two_sided_t(t, df), d


def anova_un_factor(grupos):
    """ANOVA de un factor. grupos = lista de listas de observaciones."""
    grupos = [g for g in grupos if len(g) > 1]
    k = len(grupos)
    if k < 2:
        return (float("nan"),) * 5
    n_tot = sum(len(g) for g in grupos)
    gran = sum(sum(g) for g in grupos) / n_tot
    ss_entre = sum(len(g) * (sum(g) / len(g) - gran) ** 2 for g in grupos)
    ss_dentro = sum(sum((x - sum(g) / len(g)) ** 2 for x in g) for g in grupos)
    df1, df2 = k - 1, n_tot - k
    if df2 <= 0 or ss_dentro <= 0:
        return float("nan"), df1, df2, float("nan"), float("nan")
    f = (ss_entre / df1) / (ss_dentro / df2)
    # eta cuadrado: fraccion de la varianza explicada por la configuracion
    eta2 = ss_entre / (ss_entre + ss_dentro)
    return f, df1, df2, p_upper_f(f, df1, df2), eta2


def magnitud(d):
    a = abs(d)
    if not math.isfinite(a):
        return "NA"
    if a < 0.2:
        return "despreciable"
    if a < 0.5:
        return "pequeno"
    if a < 0.8:
        return "medio"
    return "grande"


# ─────────────────────────────────────────────────────────────────────────────
def cargar_tiempos(outdir):
    """{(kernel, threads, config): [tiempos_ms]}"""
    datos = {}
    patron = os.path.join(outdir, "metrics", "*_times.csv")
    for ruta in sorted(glob.glob(patron)):
        m = TAG_RE.match(os.path.basename(ruta))
        if not m:
            continue
        cfg = m.group("config")
        if cfg not in CONFIGS:
            continue          # p.ej. las corridas seriales, fuera del contraste
        with open(ruta, newline="") as f:
            xs = [float(r["time_ms"]) for r in csv.DictReader(f) if r.get("time_ms")]
        if len(xs) > 1:
            datos[(m.group("kernel"), int(m.group("threads")), cfg)] = xs
    return datos


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--outdir", default="perf_out_v4")
    ap.add_argument("--destino", default="analisis")
    ap.add_argument("--alfa", type=float, default=0.05)
    args = ap.parse_args()

    datos = cargar_tiempos(args.outdir)
    if not datos:
        print(f"[!] No hay ficheros *_times.csv en {args.outdir}/metrics/")
        return 1
    os.makedirs(args.destino, exist_ok=True)

    kernels = sorted({k for k, _, _ in datos})
    hilos = sorted({t for _, t, _ in datos})
    print(f"[i] {len(datos)} corridas | kernels: {kernels} | hilos: {hilos}")

    # ── Welch por pareja ────────────────────────────────────────────────────
    ruta_t = os.path.join(args.destino, "estadistica.csv")
    with open(ruta_t, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kernel", "threads", "config_a", "config_b", "pregunta",
                    "n_a", "n_b", "media_a_ms", "media_b_ms", "diff_pct",
                    "t", "gl", "p", "cohen_d", "magnitud", "significativo"])
        for kernel in kernels:
            for th in hilos:
                for ca, cb, pregunta in COMPARACIONES:
                    a = datos.get((kernel, th, ca))
                    b = datos.get((kernel, th, cb))
                    if not a or not b:
                        continue
                    ma, mb, t, df, p, d = welch(a, b)
                    # diff_pct en TIEMPO: negativo = la config A es mas rapida
                    diff = (ma / mb - 1.0) * 100.0 if mb else float("nan")
                    sig = "si" if (math.isfinite(p) and p < args.alfa) else "no"
                    w.writerow([kernel, th, ca, cb, pregunta, len(a), len(b),
                                f"{ma:.6f}", f"{mb:.6f}", f"{diff:+.3f}",
                                f"{t:.4f}", f"{df:.1f}", f"{p:.3e}",
                                f"{d:.3f}", magnitud(d), sig])
    print(f"[ok] {ruta_t}")

    # ── ANOVA de un factor entre las configuraciones que compiten ───────────
    ruta_a = os.path.join(args.destino, "anova.csv")
    with open(ruta_a, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kernel", "threads", "configs", "F", "gl1", "gl2", "p",
                    "eta2", "significativo"])
        for kernel in kernels:
            for th in hilos:
                grupos, usadas = [], []
                for c in CONFIGS_ANOVA:
                    g = datos.get((kernel, th, c))
                    if g:
                        grupos.append(g)
                        usadas.append(c)
                if len(grupos) < 2:
                    continue
                fval, df1, df2, p, eta2 = anova_un_factor(grupos)
                sig = "si" if (math.isfinite(p) and p < args.alfa) else "no"
                w.writerow([kernel, th, "|".join(usadas), f"{fval:.4f}",
                            df1, df2, f"{p:.3e}", f"{eta2:.4f}", sig])
    print(f"[ok] {ruta_a}")

    # ── Resumen legible de lo que de verdad importa ─────────────────────────
    print("\n  scheduler vs spread (tiempo; negativo = el scheduler es mas rapido)")
    print(f"  {'kernel':<14}{'hilos':>6}{'diff':>10}{'p':>12}{'d':>9}  magnitud")
    for kernel in kernels:
        for th in hilos:
            a, b = datos.get((kernel, th, "scheduler")), datos.get((kernel, th, "spread"))
            if not a or not b:
                continue
            ma, mb, t, df, p, d = welch(a, b)
            print(f"  {kernel:<14}{th:>6}{(ma/mb-1)*100:>+9.2f}%{p:>12.2e}"
                  f"{d:>9.2f}  {magnitud(d)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
