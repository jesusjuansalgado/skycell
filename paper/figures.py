#!/usr/bin/env python3
"""The paper's figures: the benchmark curves, and a covering seen up close.

    python3 paper/figures.py            # needs bench/results/*.csv
    CONTAINER=skycell-pg python3 paper/figures.py --covering

The covering figure asks a live database for real coverings (the ones the
planner would use), so it needs the benchmark corpus loaded.
"""
import csv, os, subprocess, sys, statistics
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "bench", "results")
CONTAINER = os.environ.get("CONTAINER", "skycell-pg")
COLOR = {"q3c": "#c1440e", "pgsphere": "#1f6f8b", "skycell": "#1b7f3b"}
LABEL = {"q3c": "Q3C 2.0.5", "pgsphere": "pgSphere 1.5.2", "skycell": "skycell"}
ORDER = ['1"', '10"', "1'", "6'", "30'", "1deg", "3deg"]
DEG = {'1"': 1 / 3600, '10"': 10 / 3600, "1'": 1 / 60, "6'": 0.1, "30'": 0.5, "1deg": 1.0, "3deg": 3.0}


def load(name):
    with open(os.path.join(RES, name + ".csv")) as f:
        return list(csv.DictReader(f))


def psql(db, sql):
    out = subprocess.run(["docker", "exec", "-i", CONTAINER, "psql", "-U", "postgres",
                          "-d", db, "-X", "-At", "-F", ",", "-c", sql],
                         capture_output=True, text=True, check=True).stdout
    return [line.split(",") for line in out.strip().split("\n") if line]


RESAB = os.path.join(HERE, "..", "bench", "results-ab")


def load_ab(name):
    with open(os.path.join(RESAB, name + ".csv")) as f:
        return list(csv.DictReader(f))


def boot_ci(xs, b=4000, seed=7):
    """Percentile bootstrap of the median, the same estimator as ab_report.py."""
    import random
    rnd = random.Random(seed)
    n = len(xs)
    if n < 2:
        return (float("nan"), float("nan"))
    vals = sorted(statistics.median([xs[rnd.randrange(n)] for _ in range(n)])
                  for _ in range(b))
    return vals[int(0.025 * b)], vals[int(0.975 * b)]


def paired_ratios(rows, corpus, cache, rival="pgsphere"):
    """Per query: median over repetitions, then the skycell/rival ratio."""
    per_q = defaultdict(list)
    for r in rows:
        if r["corpus"] == corpus and r["cache"] == cache:
            per_q[(r["label"], int(r["qid"]), r["method"])].append(float(r["ms"]))
    out = {}
    for label in ORDER:
        rs = []
        qids = {k[1] for k in per_q if k[0] == label}
        for q in qids:
            a = per_q.get((label, q, "skycell"))
            b = per_q.get((label, q, rival))
            if a and b:
                mb = statistics.median(b)
                if mb > 0:
                    rs.append(statistics.median(a) / mb)
        if rs:
            lo, hi = boot_ci(rs)
            out[label] = (statistics.median(rs), lo, hi)
    return out


def fig_benchmark():
    """The controlled result: paired ratios with intervals, and what drives them."""
    ab = load_ab("bench_ab")
    abx = load_ab("bench_ab_x")

    fig, axes = plt.subplots(1, 3, figsize=(10.5, 3.3))
    styles = [("designed", "warm", "#1b7f3b", "o", "-", "designed, warm"),
              ("designed", "cold", "#1b7f3b", "s", "--", "designed, cold"),
              ("gaia", "warm", "#1f6f8b", "o", "-", "Gaia, warm"),
              ("gaia", "cold", "#1f6f8b", "s", "--", "Gaia, cold")]
    for corpus, cache, color, marker, ls, lab in styles:
        d = paired_ratios(ab, corpus, cache)
        labels = [l for l in ORDER if l in d]
        x = np.array([DEG[l] for l in labels])
        y = np.array([d[l][0] for l in labels])
        lo = np.array([d[l][1] for l in labels])
        hi = np.array([d[l][2] for l in labels])
        axes[0].errorbar(x, y, yerr=[y - lo, hi - y], color=color, marker=marker,
                         ls=ls, ms=4, lw=1.2, capsize=2, label=lab,
                         alpha=1.0 if cache == "cold" else 0.75)
    axes[0].axhline(1.0, color="0.35", lw=1, zorder=0)
    axes[0].set_xscale("log")
    axes[0].set_ylim(0.2, 1.3)
    axes[0].set_xlabel("cone radius (deg)")
    axes[0].set_ylabel("skycell / pgSphere (paired)")
    axes[0].legend(frameon=False, fontsize=7)
    axes[0].text(0.98, 0.96, "pgSphere faster", fontsize=7, color="0.35",
                 ha="right", va="top", transform=axes[0].transAxes)
    axes[0].text(0.98, 0.04, "skycell faster", fontsize=7, color="0.35",
                 ha="right", va="bottom", transform=axes[0].transAxes)

    buffers = defaultdict(list)
    for r in abx:
        if r["corpus"] == "designed":
            buffers[(r["method"], r["label"])].append(float(r["buffers"]))
    for m in ("q3c", "pgsphere", "skycell"):
        labels = [l for l in ORDER if (m, l) in buffers]
        axes[1].plot([DEG[l] for l in labels],
                     [statistics.mean(buffers[(m, l)]) for l in labels],
                     "o-", color=COLOR[m], label=LABEL[m], ms=4)
    axes[1].set_xscale("log"); axes[1].set_yscale("log")
    axes[1].set_xlabel("cone radius (deg)"); axes[1].set_ylabel("buffers per query")
    axes[1].legend(frameon=False, fontsize=7)
    axes[1].grid(alpha=0.25, which="both")

    sweep = defaultdict(dict)
    for r in load_ab("cm_sweep"):
        if r["corpus"] in ("designed", "gaia"):
            sweep[(r["corpus"], r["label"])][float(r["range_cost"])] = float(r["ms"])
    for (corpus, label), series in sorted(sweep.items()):
        if label not in ("30'", "1deg"):
            continue
        xs = sorted(series)
        ys = [series[x] / min(series.values()) for x in xs]
        axes[2].plot(xs, ys, marker="o", ms=3.5, lw=1.2,
                     color="#1b7f3b" if corpus == "designed" else "#1f6f8b",
                     ls="-" if label == "1deg" else "--",
                     label=f"{corpus}, {label}")
    axes[2].axvline(30, color="0.35", lw=1, ls=":")
    axes[2].text(33, 1.9, "default", fontsize=7, color="0.35", rotation=90)
    axes[2].set_xscale("log")
    axes[2].set_xlabel(r"skycell.range_cost (rows)")
    axes[2].set_ylabel("time / best at that radius")
    axes[2].legend(frameon=False, fontsize=6.5)
    axes[2].grid(alpha=0.25, which="both")

    for ax, t in zip(axes, ("(a) controlled cone searches",
                            "(b) pages touched",
                            "(c) cost-model calibration")):
        ax.set_title(t, fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(HERE, "fig_benchmark.pdf"))
    print("wrote fig_benchmark.pdf")


def cells_of_range(lo, hi, maxorder=29):
    """A range of order-29 ids as the largest aligned cells that tile it."""
    out = []
    while lo <= hi:
        order = maxorder
        while order > 0:
            shift = 2 * (maxorder - (order - 1))
            size = 1 << shift
            if lo % size != 0 or lo + size - 1 > hi:
                break
            order -= 1
        shift = 2 * (maxorder - order)
        out.append((order, lo >> shift))
        lo += 1 << shift
    return out


def fig_covering(db="skycell", radius=0.25):
    """The same cone radius in empty sky and in a crowded field: the cost
    model cuts the crowded one finer, because there the false positives it
    removes are worth more than the extra index ranges they cost."""
    # the Galactic bulge, and a field at high latitude where the catalogue is
    # sparse: the same cone radius, the same settings, different densities
    fields = [("high latitude", 40.0, -75.0), ("Galactic bulge", 266.405, -28.936)]
    fig, axes = plt.subplots(1, 2, figsize=(8.2, 4.0))
    for ax, (title, ra0, dec0) in zip(axes, fields):
        rows = psql(db, f"SELECT lo, hi FROM skycell_cone_ranges({ra0}, {dec0}, {radius}, 'cat_cell')")
        cells = [c for lo, hi in rows for c in cells_of_range(int(lo), int(hi))]
        orders = sorted({o for o, _ in cells})
        vals = psql(db, "SELECT " + " || ',' || ".join(
            f"array_to_string(skycell_cell_corners({o}, {p}::int8), ',')" for o, p in cells))
        flat = [float(x) for x in vals[0]]
        for i, (order, pix) in enumerate(cells):
            v = flat[8 * i:8 * i + 8]
            xs = [((v[2 * i] - ra0 + 180) % 360 - 180) * np.cos(np.radians(dec0)) for i in range(4)]
            ys = [v[2 * i + 1] - dec0 for i in range(4)]
            ax.fill(xs + [xs[0]], ys + [ys[0]], facecolor="#1b7f3b",
                    alpha=0.10 + 0.05 * (order - min(orders)), edgecolor="#1b7f3b", lw=0.4)
        pts = psql(db, f"SELECT ra, dec FROM cat_cell WHERE skycell_cone(cell, ra, dec, {ra0}, {dec0}, {radius * 1.6})")
        if pts:
            px = np.array([((float(p[0]) - ra0 + 180) % 360 - 180) * np.cos(np.radians(dec0)) for p in pts])
            py = np.array([float(p[1]) - dec0 for p in pts])
            ax.plot(px, py, ".", color="#333333", ms=0.7, alpha=0.45, zorder=3)
        th = np.linspace(0, 2 * np.pi, 400)
        ax.plot(radius * np.cos(th), radius * np.sin(th), "-", color="#c1440e", lw=1.4, zorder=4)
        lim = radius * 1.75
        ax.set_xlim(-lim, lim); ax.set_ylim(-lim, lim); ax.set_aspect("equal")
        info = psql(db, f"SELECT nranges, area_ratio FROM skycell_cover_info({ra0}, {dec0}, {radius}, 'cat_cell')")
        rows = psql(db, f"SELECT count(*) FROM cat_cell WHERE skycell_cone(cell, ra, dec, {ra0}, {dec0}, {radius})")
        ax.set_title(f"{title}: {rows[0][0]} sources in the cone\n"
                     f"{len(cells)} cells, orders {min(orders)}\u2013{max(orders)}, "
                     f"{info[0][0]} index ranges, {float(info[0][1]):.1f}x the cone's area",
                     fontsize=8.5)
        ax.set_xlabel(r"$\Delta\alpha\cos\delta$ (deg)"); ax.set_ylabel(r"$\Delta\delta$ (deg)")
    fig.tight_layout()
    fig.savefig(os.path.join(HERE, "fig_covering.pdf"))
    print("wrote fig_covering.pdf")


if __name__ == "__main__":
    if "--covering" in sys.argv:
        fig_covering()
    else:
        fig_benchmark()
        fig_covering()
