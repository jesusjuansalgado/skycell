#!/usr/bin/env python3
"""Report for 30_real_cones.sql / 31_real_cones.sh: tab:cones' real-corpus columns.

Per radius: the median paired skycell/pgSphere ratio of total (planning +
execution) time with a 4,000-resample percentile bootstrap over queries, the
execution-only ratio, skycell's extra planning, buffers (and, cold, pages read
and I/O time), and a row-count check between the methods on every query.

Usage: realcone_report.py DIR   (DIR holds the CSVs 31_real_cones.sh exports)
"""
import csv
import os
import random
import statistics
import sys
from collections import defaultdict

LABELS = ['1"', '10"', "1'", "6'", "30'", '1deg', '3deg']
B = 4000


def load(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def ci(xs, seed=68):
    rnd = random.Random(seed)
    n = len(xs)
    v = sorted(statistics.median([xs[rnd.randrange(n)] for _ in range(n)]) for _ in range(B))
    return v[int(0.025 * B)], v[int(0.975 * B) - 1]


def fmt(xs):
    if len(xs) < 2:
        return 'n/a'
    lo, hi = ci(xs)
    return f'{statistics.median(xs):.2f} [{lo:.2f}, {hi:.2f}]'


def report(title, rows, label_of, extra, a='skycell', b='pgsphere'):
    # per (method, qid): medians over measurements
    per = defaultdict(list)
    for r in rows:
        per[(r['method'], int(r['qid']))].append(r)
    med = {}
    for k, rs in per.items():
        f = lambda c: statistics.median(float(x[c]) for x in rs if x[c] not in ('', None))
        med[k] = {'tot': statistics.median(float(x['plan_ms']) + float(x['exec_ms']) for x in rs),
                  'plan': f('plan_ms'), 'exec': f('exec_ms'),
                  'n': {x['n'] for x in rs}, 'rows': rs}
    print(f'\n## {title}: {a} / {b}\n')
    print(f'| radius | n | total, {a}/{b} | execution only | planning, {a} - {b} (ms) | '
          + ' | '.join(h for h, _ in extra) + ' |')
    print('|' + '---|' * (5 + len(extra)))
    mism = 0
    qids = sorted({q for (_, q) in med})
    for lab in LABELS:
        tot, ex, dp, pairs = [], [], [], []
        for q in qids:
            s, p = med.get((a, q)), med.get((b, q))
            if not s or not p or label_of(q) != lab:
                continue
            if s['n'] != p['n']:
                mism += 1
            tot.append(s['tot'] / p['tot'])
            ex.append(s['exec'] / p['exec'])
            dp.append(s['plan'] - p['plan'])
            pairs.append((s, p))
        if not tot:
            continue
        cells = [fn(pairs) for _, fn in extra]
        print(f'| {lab} | {len(tot)} | {fmt(tot)} | {fmt(ex)} | {statistics.median(dp):+.3f} | '
              + ' | '.join(cells) + ' |')
    print(f'\nRow-count mismatches between the methods: {mism} (expect 0).')


def mean_of(col, transform=float):
    def fn(pairs):
        s = statistics.mean(statistics.mean(transform(r[col]) for r in a['rows']) for a, _ in pairs)
        p = statistics.mean(statistics.mean(transform(r[col]) for r in b['rows']) for _, b in pairs)
        return f'{s:.1f} / {p:.1f}'
    return fn


def med_of(col):
    def fn(pairs):
        vals = lambda side: [statistics.median(float(r[col]) for r in x['rows'] if r[col]) for x in side if any(r[col] for r in x['rows'])]
        s, p = vals([a for a, _ in pairs]), vals([b for _, b in pairs])
        return f'{statistics.median(s):.3f} / {statistics.median(p):.3f}' if s and p else 'n/a'
    return fn


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else 'results-realcone'
    warm = load(os.path.join(d, 'bench_realcone.csv'))
    cold = load(os.path.join(d, 'bench_realcone_cold.csv'))
    wc = {int(r['qid']): r['label'] for r in load(os.path.join(d, 'realcone_centers.csv'))}
    if warm:
        methods = {r['method'] for r in warm}
        pairs = [('skycell', 'pgsphere')]
        if 'skycell-rw' in methods:
            pairs += [('skycell-rw', 'pgsphere'), ('skycell', 'skycell-rw')]
        for a, b in pairs:
            report('Real corpus, warm (gaia_realc, per-method blocks, two orders)', warm,
                   lambda q: wc.get(q), [('buffers, a / b', mean_of('buffers'))], a, b)
    if cold:
        lab = {int(r['qid']): r['label'] for r in cold}
        report('Real corpus, cold (gaia_real_cell / gaia_real_sphere, restart before each pass)', cold,
               lambda q: lab.get(q),
               [('pages read, skycell / pgSphere', mean_of('rd')),
                ('buffers, skycell / pgSphere', lambda pairs: mean_of('hit')(pairs) and
                 f"{statistics.mean(int(a['rows'][0]['hit']) + int(a['rows'][0]['rd']) for a, _ in pairs):.1f} / "
                 f"{statistics.mean(int(b['rows'][0]['hit']) + int(b['rows'][0]['rd']) for _, b in pairs):.1f}"),
                ('I/O ms, skycell / pgSphere', med_of('io_ms'))])
        print('\nCold reads only count as cold if the I/O time per page read is disk-like '
              '(milliseconds); round sixty-nine saw about 0.08 ms, a host cache.')


if __name__ == '__main__':
    main()
