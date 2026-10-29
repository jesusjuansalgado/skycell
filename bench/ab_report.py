#!/usr/bin/env python3
"""Analysis for the controlled cone benchmark (bench/07_ab.sql).

Paired: for each query the three methods ran back to back in a random order, so
a ratio is formed within a trial and the query's own difficulty cancels.  The
reported interval is a bootstrap over queries (BCa would need scipy; the
percentile interval is enough to say whether a ratio is distinguishable from 1).

Usage: ab_report.py results/bench_ab.csv results/bench_ab_x.csv [> report.md]
"""
import csv
import random
import statistics
import sys
from collections import defaultdict

RADII = ['1"', '10"', "1'", "6'", "30'", '1deg', '3deg']
METHODS = ['q3c', 'pgsphere', 'skycell']
B = 4000


def med(xs):
    return statistics.median(xs) if xs else float('nan')


def boot_ci(xs, stat=med, b=B, seed=12345):
    """Percentile bootstrap interval for `stat` over the sample."""
    if len(xs) < 2:
        return (float('nan'), float('nan'))
    rnd = random.Random(seed)
    n = len(xs)
    vals = []
    for _ in range(b):
        vals.append(stat([xs[rnd.randrange(n)] for _ in range(n)]))
    vals.sort()
    return (vals[int(0.025 * b)], vals[int(0.975 * b)])


def load(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def main():
    rows = load(sys.argv[1])
    xrows = load(sys.argv[2]) if len(sys.argv) > 2 else []

    # per (corpus, cache, label, qid, method): median over repetitions
    per_q = defaultdict(list)
    slot = defaultdict(list)
    for r in rows:
        key = (r['corpus'], r['cache'], r['label'], int(r['qid']), r['method'])
        per_q[key].append(float(r['ms']))
        slot[(r['corpus'], r['cache'], r['method'], int(r['slot']))].append(float(r['ms']))
    qmed = {k: med(v) for k, v in per_q.items()}

    corpora = sorted({r['corpus'] for r in rows})
    caches = sorted({r['cache'] for r in rows}, reverse=True)

    for corpus in corpora:
        for cache in caches:
            labels = [l for l in RADII
                      if any(k[:3] == (corpus, cache, l) for k in qmed)]
            if not labels:
                continue
            print(f'\n## {corpus} corpus, {cache} cache\n')
            print('Median query time (ms) with a 95% bootstrap interval, and the '
                  'paired ratio of skycell to each rival\n')
            print('| radius | n | q3c | pgsphere | skycell | skycell/pgsphere | skycell/q3c |')
            print('|---|---|---|---|---|---|---|')
            for label in labels:
                qids = sorted({k[3] for k in qmed if k[:3] == (corpus, cache, label)})
                cols = []
                for m in METHODS:
                    xs = [qmed[(corpus, cache, label, q, m)] for q in qids
                          if (corpus, cache, label, q, m) in qmed]
                    lo, hi = boot_ci(xs)
                    cols.append(f'{med(xs):.3f} [{lo:.3f}, {hi:.3f}]')
                ratios = {}
                for rival in ('pgsphere', 'q3c'):
                    rs = []
                    for q in qids:
                        a = qmed.get((corpus, cache, label, q, 'skycell'))
                        b = qmed.get((corpus, cache, label, q, rival))
                        if a and b and b > 0:
                            rs.append(a / b)
                    lo, hi = boot_ci(rs)
                    mark = '' if (lo < 1 < hi) else ('**' if med(rs) < 1 else '*')
                    ratios[rival] = f'{mark}{med(rs):.2f}{mark} [{lo:.2f}, {hi:.2f}]'
                print(f'| {label} | {len(qids)} | ' + ' | '.join(cols) + ' | '
                      + ratios["pgsphere"] + ' | ' + ratios["q3c"] + ' |')
            print('\nBold: skycell faster, interval excludes 1. '
                  'Plain: not distinguishable from 1.')

            # did the randomisation leave an order effect?
            print('\nMean ms by position in the trial (order effect check)\n')
            print('| method | 1st | 2nd | 3rd |')
            print('|---|---|---|---|')
            for m in METHODS:
                cells = []
                for s in (1, 2, 3):
                    xs = slot.get((corpus, cache, m, s), [])
                    cells.append(f'{statistics.mean(xs):.3f}' if xs else '-')
                print(f'| {m} | ' + ' | '.join(cells) + ' |')

    if xrows:
        print('\n## Planning and execution, separated (EXPLAIN ANALYZE, randomized order)\n')
        print('| corpus | radius | method | plan ms | exec ms | buffers | est/act |')
        print('|---|---|---|---|---|---|---|')
        agg = defaultdict(lambda: defaultdict(list))
        for r in xrows:
            k = (r['corpus'], r['label'], r['method'])
            agg[k]['plan'].append(float(r['plan_ms']))
            agg[k]['exec'].append(float(r['exec_ms']))
            agg[k]['buf'].append(float(r['buffers']))
            est, act = float(r['est']), float(r['act'])
            if act > 0 and est > 0:
                agg[k]['err'].append(max(est / act, act / est))
        for corpus in corpora:
            for label in RADII:
                for m in METHODS:
                    k = (corpus, label, m)
                    if k not in agg:
                        continue
                    a = agg[k]
                    print(f'| {corpus} | {label} | {m} | {med(a["plan"]):.3f} | '
                          f'{med(a["exec"]):.3f} | {med(a["buf"]):.0f} | '
                          f'{med(a["err"]):.2f} |')


if __name__ == '__main__':
    main()
