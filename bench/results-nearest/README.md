# Nearest neighbours against pgSphere (README, Performance)

Not in the paper. `run.sh` times `ORDER BY distance LIMIT k` with skycell's
`SkycellNearest` scan (`skycell_dist(ra, dec, ra0, dec0)`) against pgSphere's
GiST ordering (`pos <-> spoint(...)`).

- **Tables:**
  - `gaia_realc`: the 10M real Gaia DR3 positions of `../results-v036/realcone/`,
    with an expression index on `skycell_ang2cell(ra, dec)` and pgSphere's
    GiST on `pos`. Its heap follows the cell order (the ordered mode).
  - `oc`: the 0.5M-row ObsCore-shaped table of `../10_crossover.sql` (`CALL
    build_obscore(500000)`), observations grouped in fields, its heap out of
    cell order (the bitmap mode).
- **Centres:** 30 per table (`mk_centres`, set up by `run.sh`).
- **Protocol** (`micro_knn.sql`): per centre, k and method, 20 executions,
  planned and run, each at a new centre 1e-9 deg away, so that no plan or
  covering is reused; the methods in random order per centre; two passes of
  three repetitions. `k = -10` is the 10 nearest more than 2 deg away, the
  same condition for both methods: no row passes until the search gets past
  2 deg.
- **Report** (`report_knn.py`, `report.md`): per table and k, skycell's summed
  per-centre median time over pgSphere's, with a 95% bootstrap interval over
  the centres.
- **Host and server:** as `../results-v036/realcone/` (`../REPRODUCING.md`
  section 2), skycell built from main after #52 (`versions.txt`).

## skycell / pgSphere

Rerun with 0.45 (`report.md`, `versions.txt`); the first run, built from main
after #52, is in `report-v036.md`. Both runs on the same host and tables.

| k | real DR3, 10M, #52 | real DR3, 0.45 | ObsCore, #52 | ObsCore, 0.45 |
|---|---|---|---|---|
| 1 | 0.55 [0.53, 0.57] | 0.56 [0.54, 0.59] | 1.00 [0.89, 1.12] | 1.05 [0.95, 1.17] |
| 10 | 0.47 [0.45, 0.49] | 0.48 [0.45, 0.51] | 0.98 [0.88, 1.09] | 1.01 [0.90, 1.13] |
| 100 | 0.59 [0.56, 0.61] | 0.57 [0.54, 0.61] | 1.16 [1.04, 1.31] | 1.15 [1.05, 1.28] |
| 1000 | 0.69 [0.68, 0.71] | 0.67 [0.66, 0.68] | 1.59 [1.37, 1.83] | 1.55 [1.34, 1.78] |
| 10 beyond 2° | 1.74 [1.71, 1.76] | 0.25 [0.24, 0.26] | 1.90 [1.58, 2.35] | 1.00 [0.66, 1.63] |

- **Real DR3:** skycell takes about half of pgSphere's time up to k = 100.
- **Beyond 2°:** since 0.45 the rings start at the distance the condition
  sets, instead of growing out to it from the centre: a quarter of pgSphere's
  time on DR3, level on ObsCore.
- **ObsCore:** level for a few rows, slower for hundreds. The scan reads about
  2.5 times the rows it returns there: the first ring, sized from the density
  of a cell round the centre, holds more rows than wanted where observations
  cluster in fields, each read through a bitmap over an unordered heap.
- **Tried and dropped (`ab.csv`, `ab.sh`):** testing each index entry by its
  key, an order-29 cell, before the heap is read, and cutting the first ring
  to the nearest keys. It cut the rows read to 1.2-1.6 per row returned, but
  on this table, which is in memory, a skipped heap fetch saves less than the
  key's decoding, the sort and the bitmap's entry-by-entry filling cost: 12-15%
  slower at k = 100-1000 in a same-day A/B against v0.44.
- **A regression found and fixed:** 0.44 read the index's lowest and highest
  key at every nearest query (for partitions), about 25 µs, a third of a
  k = 1 search (`bisect-v042-v044.txt`: v0.42 77 µs, v0.44 104 µs). Since
  0.45 only a partition reads them (`bisect-fix.txt`: 78 µs).

The answers were checked separately against PostgreSQL's own sort (the
`nearest` regression test, and #50's sweeps); this run counts rows only.
