import sys, csv, random, statistics as st
from collections import defaultdict
rows = list(csv.DictReader(open(sys.argv[1])))
g = defaultdict(lambda: defaultdict(list))
for r in rows:
    g[(r['run'], int(r['k']))][(r['qid'], r['method'])].append(float(r['us']))
print("| table | k | skycell us | pgSphere us | ratio [95%] |"); print("|---|---|---|---|---|")
for key in sorted(g):
    d = g[key]; qids = sorted({q for q, _ in d})
    per = {q: (st.median(d[(q,'skycell')]), st.median(d[(q,'pgsphere')])) for q in qids}
    def ratio(qs): return sum(per[q][0] for q in qs) / sum(per[q][1] for q in qs)
    random.seed(1); bs = sorted(ratio(random.choices(qids, k=len(qids))) for _ in range(2000))
    print(f"| {key[0]} | {key[1]} | {sum(p[0] for p in per.values())/len(qids):.1f} | {sum(p[1] for p in per.values())/len(qids):.1f} | {ratio(qids):.2f} [{bs[50]:.2f}, {bs[1949]:.2f}] |")
