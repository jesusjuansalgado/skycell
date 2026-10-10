#!/usr/bin/env python3
"""tab:xmreal from 18_xmatch_sweep.sql's xms table (xms.csv).

Per cell (outer size, target distribution, radius): the median over repetitions
of each method's time; per cell the ratio of each skycell form to q3c_join and
to pgSphere; then the geometric mean of those ratios over the cells at each
radius (three sizes x two distributions) and over all cells.  Row counts are
compared between methods in every cell, and timed-out or sequential-scan plans
are reported.

Usage: report.py xms.csv
"""
import csv
import math
import statistics
import sys
from collections import defaultdict

FORMS = [('skycell_cs', 'cone, custom'), ('skycell_join', 'join, custom'), ('skycell_slots', 'cone, slots')]
RIVALS = [('q3c_join', 'q3c_join'), ('pgsphere', 'pgSphere')]


def gmean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float('nan')


def main():
    rows = list(csv.DictReader(open(sys.argv[1] if len(sys.argv) > 1 else 'xms.csv')))
    ms = defaultdict(list)
    out = defaultdict(set)
    flags = []
    for r in rows:
        cell = (int(r['n']), r['targets'], float(r['radius_arcsec']))
        if r['timed_out'] == 't':
            flags.append(f"timeout: {r['method']} {cell}")
            continue
        if r['plan_shape'] != 'index':
            flags.append(f"plan {r['plan_shape']}: {r['method']} {cell}")
        ms[(cell, r['method'])].append(float(r['ms']))
        out[cell, r['method']].add(r['rows_out'])
    med = {k: statistics.median(v) for k, v in ms.items()}
    cells = sorted({c for c, _ in med})
    radii = sorted({c[2] for c in cells})

    mism = 0
    for c in cells:
        counts = {m: out[c, m] for _, m in [(None, f) for f, _ in FORMS] + [(None, r) for r, _ in RIVALS]
                  if (c, m) in out}
        vals = set().union(*counts.values()) if counts else set()
        if len(vals) > 1:
            mism += 1
            flags.append(f'row counts differ in {c}: {counts}')

    head = ' | '.join(f'{fl} vs {rl}' for f, fl in FORMS for _, rl in RIVALS)
    print('Geometric mean of the per-cell ratio (median over repetitions), real Gaia DR3 positions\n')
    print(f'| radius | {head} |')
    print('|---|' + '---|' * (len(FORMS) * len(RIVALS)))
    allr = defaultdict(list)
    for rad in radii + [None]:
        cs = [c for c in cells if rad is None or c[2] == rad]
        vals = []
        for f, _ in FORMS:
            for rv, _ in RIVALS:
                xs = [med[c, f] / med[c, rv] for c in cs if (c, f) in med and (c, rv) in med]
                vals.append(f'{gmean(xs):.2f}' if xs else 'n/a')
        label = 'all' if rad is None else f'{rad:g}"'
        print(f'| {label} | ' + ' | '.join(vals) + ' |')
    print(f'\nCells: {len(cells)}; row-count mismatches: {mism} (expect 0).')
    for fl in flags[:20]:
        print('-', fl)


if __name__ == '__main__':
    main()
