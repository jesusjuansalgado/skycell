#!/usr/bin/env python3
"""Size against result size: run.sh's four tables side by side.

1. Table 8's measure (summed total time, skycell / pgSphere) per table and
   seed, from <table>_cross_seed0*.csv.
2. Per-query medians from the same files: planning difference, execution
   ratio, buffers, rows.
3. threeway.sql's output per table: skycell against pgSphere as planned and
   against pgSphere forced to a bitmap heap scan, as ratios of median times.

Usage: compare.py   (from this directory)
"""
import csv
import glob
import re
import statistics as st
from collections import defaultdict

TABLES = [('dense05', 'dense 0.5M'), ('sparse10', 'sparse 10M'), ('dense10', 'dense 10M'),
          ('sparse10b', 'sparse 10M, rebuilt')]
CLASSES = ['Q05', 'Q06', 'Q07', 'Q12']


def cross(t):
    out = []
    for p in sorted(glob.glob(f'{t}_cross_seed0*.csv')):
        rows = defaultdict(list)
        for r in csv.DictReader(open(p)):
            rows[r['class'], r['method']].append(r)
        out.append(rows)
    return out


data = {t: cross(t) for t, _ in TABLES}

print('## Table 8\'s measure: summed total time, skycell / pgSphere (seed 0.19 / 0.53)\n')
print('| class | ' + ' | '.join(name for _, name in TABLES) + ' |')
print('|---|' + '---|' * len(TABLES))
for c in CLASSES:
    cells = []
    for t, _ in TABLES:
        xs = []
        for rows in data[t]:
            tot = lambda m: sum(float(r['plan_ms']) + float(r['exec_ms']) for r in rows[c, m])
            xs.append(tot('skycell') / tot('pgsphere'))
        cells.append(' / '.join(f'{x:.2f}' for x in xs))
    print(f'| {c} | ' + ' | '.join(cells) + ' |')

print('\n## Per query, both seeds pooled: planning skycell - pgSphere (ms), execution skycell / pgSphere '
      '(medians), buffers pgSphere / skycell (means), rows (median)\n')
print('| class | ' + ' | '.join(name for _, name in TABLES) + ' |')
print('|---|' + '---|' * len(TABLES))
for c in CLASSES:
    cells = []
    for t, _ in TABLES:
        P = [r for rows in data[t] for r in rows[c, 'pgsphere']]
        S = [r for rows in data[t] for r in rows[c, 'skycell']]
        med = lambda rs, k: st.median(float(r[k]) for r in rs)
        cells.append(f"{med(S, 'plan_ms') - med(P, 'plan_ms'):+.4f}, {med(S, 'exec_ms') / med(P, 'exec_ms'):.2f}, "
                     f"{st.mean(float(r['buffers']) for r in P):.0f} / {st.mean(float(r['buffers']) for r in S):.0f}, "
                     f"{st.median(int(r['n']) for r in P):.0f}")
    print(f'| {c} | ' + ' | '.join(cells) + ' |')

# row counts: the methods answer the same queries, so the same multiset of counts
bad = 0
for t, _ in TABLES:
    for rows in data[t]:
        for c in CLASSES:
            counts = [sorted(int(r['n']) for r in rows[c, m]) for m in ('pgsphere', 'skycell', 'skycell-rw')]
            bad += any(x != counts[0] for x in counts)
print(f'\n(table, seed, class) cells where the methods returned different row counts: {bad} (expect 0).')

print('\n## threeway.sql: median time, skycell / pgSphere as planned, skycell / pgSphere bitmap; '
      'buffers pgSphere / pgSphere bitmap / skycell\n')
print('| class | ' + ' | '.join(name for _, name in TABLES) + ' |')
print('|---|' + '---|' * len(TABLES))
three = {}
for t, _ in TABLES:
    three[t] = {}
    for line in open(f'{t}_threeway.txt'):
        f = [x.strip() for x in line.split('|')]
        if len(f) == 8 and re.match(r'Q\d\d$', f[0]):
            three[t][f[0], f[1]] = (float(f[4]), float(f[5]), float(f[6]))
for c in CLASSES:
    cells = []
    for t, _ in TABLES:
        a, b, s = three[t][c, 'pgsphere'], three[t][c, 'pgsphere-bitmap'], three[t][c, 'skycell']
        cells.append(f'{s[0] / a[0]:.2f}, {s[0] / b[0]:.2f}; {a[1]:.0f} / {b[1]:.0f} / {s[1]:.0f}')
    print(f'| {c} | ' + ' | '.join(cells) + ' |')
reads = sum(v[2] for t in three for v in three[t].values())
print(f'\nShared buffer reads in the three-way runs, all tables: {reads:.1f} (0: every page a hit).')
