"""tab:crossover from bench_cross_seed*.csv: total time skycell/pgSphere (and
the range rewrite, skycell-rw, /pgSphere) per class and size, for each run.

Usage: python3 summarize.py bench_cross_seed019.csv bench_cross_seed053.csv
"""
import csv
import statistics as st
import sys
from collections import defaultdict

SIZES = ['0.5', '2', '10']
for path in sys.argv[1:]:
    t = defaultdict(float); b = defaultdict(list); n = defaultdict(list)
    for r in csv.DictReader(open(path)):
        k = (r['rows_m'], r['class'], r['method'])
        t[k] += float(r['plan_ms']) + float(r['exec_ms'])
        b[k].append(float(r['buffers'])); n[k].append(int(r['n']))
    print(f'\n{path}')
    print('| class | skycell/pgSphere 0.5M | 2M | 10M | rewrite/pgSphere 0.5M | 2M | 10M '
          '| median rows 0.5M | 2M | 10M | median buffers at 10M pgSphere / skycell | counts agree |')
    print('|---' * 12 + '|')
    for c in ['Q05', 'Q06', 'Q07', 'Q12']:
        sky = [t[(s, c, 'skycell')] / t[(s, c, 'pgsphere')] for s in SIZES]
        rw = [t[(s, c, 'skycell-rw')] / t[(s, c, 'pgsphere')] for s in SIZES]
        rows = [st.median(n[(s, c, 'pgsphere')]) for s in SIZES]
        buf = [st.median(b[('10', c, m)]) for m in ('pgsphere', 'skycell')]
        # as multisets: rows reusing freed heap space need not come back in trial order
        agree = all(sorted(n[(s, c, 'skycell')]) == sorted(n[(s, c, 'pgsphere')])
                    == sorted(n[(s, c, 'skycell-rw')]) for s in SIZES)
        print(f'| {c} | ' + ' | '.join(f'{x:.2f}' for x in sky + rw) + ' | '
              + ' | '.join(f'{x:.0f}' for x in rows)
              + f' | {buf[0]:.0f} / {buf[1]:.0f} | {agree} |')
