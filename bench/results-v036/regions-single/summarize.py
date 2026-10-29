"""Per band and seed: median buffers, median ms, and the paired median ratio
skycell/pgSphere with a 95% bootstrap interval over probes (4000 resamples)."""
import csv, random, statistics as st, collections as C, sys
d = sys.argv[1] if len(sys.argv) > 1 else '.'
R = {1: 0.05, 2: 0.1, 3: 0.3, 4: 0.5, 5: 1.0}
t = C.defaultdict(dict)
for r in csv.DictReader(open(f'{d}/bs_time.csv')):
    t[(r['seed'], int(r['band']), int(r['pid']), int(r['rep']))][r['method']] = float(r['ms'])
b = C.defaultdict(list)
for r in csv.DictReader(open(f'{d}/bs_explain.csv')):
    b[(r['seed'], int(r['band']), r['method'])].append(float(r['bufs']))
print('| seed | radius | skycell pages | pgSphere pages | skycell ms | pgSphere ms | ratio [95%] |')
print('|---|---|---|---|---|---|---|')
for seed in sorted({k[0] for k in t}):
    for band in sorted(R):
        per = C.defaultdict(list)
        for (s, bb, pid, rep), v in t.items():
            if s == seed and bb == band:
                per[pid].append(v)
        pids = sorted(per)
        def med_ratio(ps):
            return st.median(v['skycell'] / v['pgsphere'] for p in ps for v in per[p])
        rng = random.Random(4000)
        boots = sorted(med_ratio([rng.choice(pids) for _ in pids]) for _ in range(4000))
        sky = st.median(v['skycell'] for p in pids for v in per[p])
        sph = st.median(v['pgsphere'] for p in pids for v in per[p])
        print(f"| {seed} | {R[band]} deg | {st.median(b[(seed, band, 'skycell')]):g} | {st.median(b[(seed, band, 'pgsphere')]):g} "
              f"| {sky:.4f} | {sph:.4f} | {med_ratio(pids):.2f} [{boots[100]:.2f}, {boots[3899]:.2f}] |")
