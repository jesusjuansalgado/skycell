#!/usr/bin/env python3
"""Report for ../36_polygon_scan.sql: per shape and size band, median ms per
method, and the scan's summed time over each other method's with a 95%
bootstrap interval (4000 resamples of the shapes)."""
import csv, collections, random, statistics as st, sys

rows = list(csv.DictReader(open(sys.argv[1])))
t = collections.defaultdict(lambda: collections.defaultdict(list))
size = {}
for r in rows:
    k = (int(r['c']), r['shape'])
    t[k][r['method']].append(float(r['us']) / 1000)
    size[k] = float(r['s'])
shape_ms = {k: {m: st.mean(v) for m, v in ms.items()} for k, ms in t.items()}

def ratio_ci(keys, a, b):
    keys = [k for k in keys if a in shape_ms[k] and b in shape_ms[k]]
    if not keys:
        return None
    f = lambda ks: sum(shape_ms[k][a] for k in ks) / sum(shape_ms[k][b] for k in ks)
    rnd = random.Random(36)
    bs = sorted(f([rnd.choice(keys) for _ in keys]) for _ in range(4000))
    return f(keys), bs[100], bs[3899]

def fmt(x):
    return '—' if x is None else f'{x[0]:.2f} [{x[1]:.2f}, {x[2]:.2f}]'

methods = ['scan', 'rewrite', 'adql', 'pgsphere', 'q3c']
bands = [('all sizes', 0, 9), ('0.05-0.15 deg', 0, 0.15), ('0.15-0.5 deg', 0.15, 0.5), ('0.5-2 deg', 0.5, 9)]
for label, lo, hi in bands:
    print(f'| shape, {label} | shapes | ' + ' | '.join(f'{m} ms' for m in methods)
          + ' | scan / rewrite | scan / pgSphere | scan / Q3C | ADQL / scan |')
    print('|---' * (2 + len(methods) + 4) + '|')
    for shape in ['quad', 'box', 'ushape']:
        keys = [k for k in shape_ms if k[1] == shape and lo <= size[k] < hi]
        med = [('—' if not any(m in shape_ms[k] for k in keys)
                else f'{st.median(shape_ms[k][m] for k in keys if m in shape_ms[k]):.3f}') for m in methods]
        print(f'| {shape} | {len(keys)} | ' + ' | '.join(med) + ' | '
              + ' | '.join(fmt(ratio_ci(keys, a, b)) for a, b in
                           [('scan', 'rewrite'), ('scan', 'pgsphere'), ('scan', 'q3c'), ('adql', 'scan')]) + ' |')
    print()
