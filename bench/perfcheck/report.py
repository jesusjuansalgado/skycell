#!/usr/bin/env python3
"""perfcheck report: per workload, the candidate's time over the baseline's.

Per centre and library the median over rounds; per workload the ratio of the
summed medians, with a 95% bootstrap interval (2000 resamples of the centres).
A workload whose interval lies wholly above the limit is a regression, wholly
below 1/limit an improvement.  Exit status 1 on a regression or when the two
libraries returned different counts."""
import csv, collections, random, statistics as st, sys

path, label, limit = sys.argv[1], sys.argv[2], float(sys.argv[3])
t = collections.defaultdict(list)
n = collections.defaultdict(set)
for r in csv.DictReader(open(path)):
    t[(r['workload'], r['lib'], int(r['c']))].append(float(r['us']))
    n[(r['workload'], int(r['c']))].add(r['n'])
med = {k: st.median(v) for k, v in t.items()}
workloads = list(dict.fromkeys(w for (w, _, _) in t))
bad = False
print(f"| workload | baseline {label} us | candidate us | candidate / baseline [95%] | |")
print("|---|---|---|---|---|")
for w in workloads:
    cs = sorted({c for (ww, _, c) in t if ww == w})
    ratio = lambda ks: sum(med[(w, 'cand', c)] for c in ks) / sum(med[(w, 'base', c)] for c in ks)
    rnd = random.Random(7)
    bs = sorted(ratio([rnd.choice(cs) for _ in cs]) for _ in range(2000))
    lo, hi, r = bs[50], bs[1949], ratio(cs)
    mism = [c for c in cs if len(n[(w, c)]) > 1]
    verdict = ''
    if mism:
        verdict, bad = f'DIFFERENT ROWS at {len(mism)} centres', True
    elif lo > limit:
        verdict, bad = 'REGRESSION', True
    elif hi < 1 / limit:
        verdict = 'faster'
    b = st.median(med[(w, 'base', c)] for c in cs)
    a = st.median(med[(w, 'cand', c)] for c in cs)
    print(f"| {w} | {b:.1f} | {a:.1f} | {r:.2f} [{lo:.2f}, {hi:.2f}] | {verdict} |")
sys.exit(1 if bad else 0)
