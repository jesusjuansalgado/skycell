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


def fig_benchmark():
    centers = {r["qid"]: r for r in load("bench_centers")}
    times, buffers = defaultdict(list), defaultdict(list)
    for r in load("bench_cone"):
        if r["pass"] == "2" and r["variant"] == "":
            times[(r["method"], centers[r["qid"]]["label"])].append(float(r["ms"]))
    for r in load("bench_cone_x"):
        if r["variant"] == "":
            buffers[(r["method"], centers[r["qid"]]["label"])].append(float(r["buffers"]))

    fig, axes = plt.subplots(1, 3, figsize=(10.5, 3.2))
    x = [DEG[l] for l in ORDER]
    for m in ("q3c", "pgsphere", "skycell"):
        axes[0].plot(x, [statistics.median(times[(m, l)]) for l in ORDER], "o-",
                     color=COLOR[m], label=LABEL[m], ms=4)
        axes[1].plot(x, [statistics.mean(buffers[(m, l)]) for l in ORDER], "o-",
                     color=COLOR[m], ms=4)
    for ax, ylab in ((axes[0], "median time per query (ms)"), (axes[1], "buffers per query")):
        ax.set_xscale("log"); ax.set_yscale("log"); ax.set_xlabel("cone radius (deg)")
        ax.set_ylabel(ylab); ax.grid(alpha=0.25, which="both")
    axes[0].legend(frameon=False, fontsize=8)

    xm = [r for r in load("bench_xmatch") if r["pass"] == "2"]
    names = ["q3c", "pgsphere", "skycell_slots", "skycell_lateral"]
    pretty = ["Q3C", "pgSphere", "skycell\n(join)", "skycell\n(lateral)"]
    w = 0.38
    for k, rad in enumerate(["1", "10"]):
        vals = [float(next(r for r in xm if r["method"] == n and r["radius_arcsec"] == rad)["ms"]) / 1000
                for n in names]
        axes[2].bar(np.arange(len(names)) + (k - 0.5) * w, vals, w,
                    color=["#c1440e", "#1f6f8b", "#7fbf7f", "#1b7f3b"],
                    alpha=1.0 if k else 0.55, label=f'{rad}"')
    axes[2].set_xticks(range(len(names))); axes[2].set_xticklabels(pretty, fontsize=7)
    axes[2].set_ylabel("cross-match, 200k probes (s)"); axes[2].grid(alpha=0.25, axis="y")
    axes[2].legend(frameon=False, fontsize=8, title="radius", title_fontsize=8)
    for ax, t in zip(axes, ("(a) cone searches", "(b) pages touched", "(c) cross-match")):
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
