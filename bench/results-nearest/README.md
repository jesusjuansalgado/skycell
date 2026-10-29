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

| k | real DR3, 10M | ObsCore, 0.5M |
|---|---|---|
| 1 | 0.55 [0.53, 0.57] | 1.00 [0.89, 1.12] |
| 10 | 0.47 [0.45, 0.49] | 0.98 [0.88, 1.09] |
| 100 | 0.59 [0.56, 0.61] | 1.16 [1.04, 1.31] |
| 1000 | 0.69 [0.68, 0.71] | 1.59 [1.37, 1.83] |
| 10 beyond 2° | 1.74 [1.71, 1.76] | 1.90 [1.58, 2.35] |

- **Real DR3:** skycell takes about half of pgSphere's time up to k = 100.
- **ObsCore:** level for a few rows, slower for hundreds. Over the 30 centres
  the scan reads about twice the rows it returns (the ring's margin and the
  cells its edge crosses), each through a bitmap over an unordered heap; GiST
  reads close to exactly the rows it returns.
- **Beyond 2°:** per row read skycell is the cheaper, but it reads about 3×
  more rows: the last ring overshoots, and cells on the rings' edges are read
  twice.

The answers were checked separately against PostgreSQL's own sort (the
`nearest` regression test, and #50's sweeps); this run counts rows only.
