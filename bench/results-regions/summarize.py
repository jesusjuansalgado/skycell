#!/usr/bin/env python3
"""summary.tsv from runs.tsv (33_region_table.sh): for each operator and
library, skycell's box-key GiST time over pgSphere's native GiST time.

Each repeat gives one ratio, the median of its skycell passes over the median
of its pgSphere passes; the summary is the median of those ratios, with a 95%
bootstrap interval over the repeats (4000 resamples, fixed seed).

    python3 summarize.py            # in bench/results-regions/
"""
import csv
import random
import statistics as st

rows = list(csv.DictReader(open('runs.tsv'), delimiter='\t'))
libs = list(dict.fromkeys(r['library'] for r in rows))

# operator: (script, scope, method) for skycell, then for pgSphere
OPS = [('&&', ('22', '', 'gist'), ('21', '', 'pgsphere')),
       ('@>(region,point)', ('23', '', 'gist'), ('23', '', 'pgsphere')),
       ('@>(region,region)', ('24', 'contains', 'gist'), ('24', 'contains', 'pgsphere')),
       ('<@(region,region)', ('24', 'covered_by', 'gist'), ('24', 'covered_by', 'pgsphere'))]


def sel(lib, script, scope, method):
    return [r for r in rows if r['library'] == lib and r['script'] == script
            and r['scope'] == scope and r['method'] == method]


def ms(rs, rep=None):
    return st.median(float(r['ms']) for r in rs if rep is None or r['rep'] == rep)


random.seed(1)
with open('summary.tsv', 'w') as f:
    f.write('operator\tlibrary\tskycell_ms\tpgsphere_ms\tratio_median_per_rep'
            '\tci95_lo\tci95_hi\trows\treps\n')
    for name, sk, pg in OPS:
        for lib in libs:
            a, b = sel(lib, *sk), sel(lib, *pg)
            reps = sorted({r['rep'] for r in a}, key=int)
            rr = [ms(a, k) / ms(b, k) for k in reps]
            boots = sorted(st.median(random.choices(rr, k=len(rr))) for _ in range(4000))
            n = ','.join(sorted({r['n'] for r in a} | {r['n'] for r in b}))
            f.write(f'{name}\t{lib}\t{ms(a):.2f}\t{ms(b):.2f}\t{st.median(rr):.3f}'
                    f'\t{boots[100]:.3f}\t{boots[3899]:.3f}\t{n}\t{len(rr)}\n')
