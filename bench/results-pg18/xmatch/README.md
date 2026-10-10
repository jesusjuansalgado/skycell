# Cross-match on PostgreSQL 18.6, second host

`11_xmatch_ab.sql` (each method warmed on its own block, then timed; blocks in
randomized method order) on the 10M resampled-Gaia catalogue, 200,000 probes
from `04_xmatch.sql` in 8 blocks, radii 1" and 10", 2 repetitions.  Same host
and versions as `../`, skycell with its custom scan on: neither join form uses
it (a join's cone has no constant centre), and their plans are identical with
`skycell.custom_scan` on and off.  `report.md` is `report.py` on
`bench_xm_ab.csv`.  The paper's own run, 50M rows on the first host, is
`../../results-ab/bench_xm_ab.csv`.

`bench_xm_ab_cs.csv` (`report_cs.md`) repeats the run after 6730fd1 with a
fifth method, `skycell_cs`: the same join query as `skycell_slots`, answered
by the custom scan's parameterized node, one covering per probe
(`skycell_slots` now runs with `skycell.custom_scan` off).
