#!/usr/bin/env python3
"""perfcheck report: per workload, the candidate's time over the baseline's.

The unit of comparison is the round: within one, the two libraries run back to
back, so the host's drift from one session to the next -- the larger part of
the noise, and shared by every centre of a session -- falls on both alike.
Per round and workload the median over centres of the candidate's time over
the baseline's at the same centre (robust to a centre that stalls); the
estimate is the geometric mean of those over the rounds, with a 95% t interval
on their logs.  A workload whose interval lies wholly above
the limit is a regression, wholly below 1/limit faster.  Exit status 1 on a
regression or when the two libraries returned different counts."""
import csv, collections, math, statistics as st, sys

T975 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26,
        10: 2.23, 12: 2.18, 15: 2.13, 20: 2.09, 30: 2.04}

def t975(df):
    return T975.get(df) or T975[max(k for k in T975 if k <= df)]

path, label, limit = sys.argv[1], sys.argv[2], float(sys.argv[3])
tot = collections.defaultdict(float)
per = {}
n = collections.defaultdict(set)
for r in csv.DictReader(open(path)):
    if r.get('n') is None or r.get('c') is None or not r['c'].isdigit():
        continue
    tot[(r['workload'], r['lib'], int(r['round']))] += float(r['us'])
    per[(r['workload'], r['lib'], int(r['round']), int(r['c']))] = float(r['us'])
    n[(r['workload'], int(r['c']))].add(r['n'])
workloads = list(dict.fromkeys(w for (w, _, _) in tot))
bad = False
print(f"| workload | baseline {label}, ms a round | candidate | candidate / baseline [95%] | |")
print("|---|---|---|---|---|")
for w in workloads:
    rounds = sorted({r for (ww, l, r) in tot if ww == w and (w, 'cand', r) in tot and (w, 'base', r) in tot})
    cs = sorted({c for (ww, l, rr, c) in per if ww == w})
    logs = [st.median(math.log(per[(w, 'cand', r, c)] / per[(w, 'base', r, c)])
                      for c in cs if (w, 'cand', r, c) in per and (w, 'base', r, c) in per)
            for r in rounds]
    m = st.mean(logs)
    h = t975(len(logs) - 1) * st.stdev(logs) / math.sqrt(len(logs)) if len(logs) > 1 else float('inf')
    est, lo, hi = math.exp(m), math.exp(m - h), math.exp(m + h)
    mism = sorted({c for (ww, c), v in n.items() if ww == w and len(v) > 1})
    verdict = ''
    if mism:
        verdict, bad = f'DIFFERENT ROWS at {len(mism)} centres', True
    elif lo > limit:
        verdict, bad = 'REGRESSION', True
    elif hi < 1 / limit:
        verdict = 'faster'
    b = st.median(tot[(w, 'base', r)] for r in rounds) / 1000
    a = st.median(tot[(w, 'cand', r)] for r in rounds) / 1000
    print(f"| {w} | {b:.2f} | {a:.2f} | {est:.2f} [{lo:.2f}, {hi:.2f}] | {verdict} |")
sys.exit(1 if bad else 0)
