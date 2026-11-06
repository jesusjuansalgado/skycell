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
  section 2), skycell as `versions.txt` records (0.52 for `report.md`).

## skycell / pgSphere

Rerun with 0.52 (`report.md`, `versions.txt`); the first run, built from main
after #52, is in `report-v036.md`, and 0.45's figures are kept below. All runs
on the same host and tables.

| k | real DR3, 10M, #52 | real DR3, 0.45 | real DR3, 0.52 | ObsCore, #52 | ObsCore, 0.45 | ObsCore, 0.52 |
|---|---|---|---|---|---|---|
| 1 | 0.55 [0.53, 0.57] | 0.56 [0.54, 0.59] | 0.56 [0.55, 0.57] | 1.00 [0.89, 1.12] | 1.05 [0.95, 1.17] | 1.01 [0.94, 1.08] |
| 10 | 0.47 [0.45, 0.49] | 0.48 [0.45, 0.51] | 0.48 [0.46, 0.49] | 0.98 [0.88, 1.09] | 1.01 [0.90, 1.13] | 1.01 [0.94, 1.07] |
| 100 | 0.59 [0.56, 0.61] | 0.57 [0.54, 0.61] | 0.53 [0.51, 0.56] | 1.16 [1.04, 1.31] | 1.15 [1.05, 1.28] | 0.94 [0.88, 1.00] |
| 1000 | 0.69 [0.68, 0.71] | 0.67 [0.66, 0.68] | 0.61 [0.60, 0.62] | 1.59 [1.37, 1.83] | 1.55 [1.34, 1.78] | 0.95 [0.86, 1.06] |
| 10 beyond 2° | 1.74 [1.71, 1.76] | 0.25 [0.24, 0.26] | 0.24 [0.23, 0.24] | 1.90 [1.58, 2.35] | 1.00 [0.66, 1.63] | 0.97 [0.64, 1.53] |

- **Real DR3:** skycell takes about half of pgSphere's time up to k = 100.
- **Beyond 2°:** since 0.45 the rings start at the distance the condition
  sets, instead of growing out to it from the centre: a quarter of pgSphere's
  time on DR3, level on ObsCore.
- **ObsCore:** level with pgSphere at every k since 0.52 (below); until
  0.51, level for a few rows and slower for hundreds. The scan read about
  2.5 times the rows it returned: the first ring, sized from the density of
  a cell round the centre, held more rows than wanted where observations
  cluster in fields, each read through a bitmap over an unordered heap.
- **Tried and dropped (`ab.csv`, `ab.sh`):** testing each index entry by its
  key, an order-29 cell, before the heap is read, and cutting the first ring
  to the nearest keys. It cut the rows read to 1.2-1.6 per row returned, but
  on this table, which is in memory, a skipped heap fetch saves less than the
  key's decoding, the sort and the bitmap's entry-by-entry filling cost: 12-15%
  slower at k = 100-1000 in a same-day A/B against v0.44.
- **Tried in 0.51 (`micro_oc.sql`, on real36's copy of the ObsCore table):**
  - **Kept:** sorting only the rows the query wants (quickselect, then sort
    the prefix; the rest only if asked for), and rejecting a row whose
    declination is farther from the centre's than the ring reaches, before
    the exact distance. perfcheck against v0.50: ObsCore k = 100 0.93
    [0.90, 0.96], k = 1000 0.98 [0.96, 1.00], Gaia k = 1 0.95 [0.91, 0.99].
    Against pgSphere on ObsCore: 1.16–1.20 at k = 100, 1.43–1.48 at
    k = 1000.
  - **Dropped:** finer and coarser ring coverings (slower at every k), and
    shrinking the first ring by the count of index entries its bitmap holds.
    That shrinking cut rows read at k = 100 from 3.9 to 2.6 per row
    returned, but the discarded bitmaps and extra coverings cost as much:
    neutral to 10% slower.
  - The problem, the profile and everything tried are written up for a
    second opinion in [`open-problem.md`](open-problem.md).
- **Best-first, 0.52** (the GUC `skycell.nearest_bestfirst`, on by default),
  after the second opinion on `open-problem.md`. On a heap out of cell order
  the first ring is replaced by a walk over HEALPix cells, nearest first,
  ordered by the least distance a point of each can have from the centre:
  - a cell the statistics expect to hold many rows is split into its four
    children (a key range each, without touching the index);
  - the others are gathered into a batch, read through one TID bitmap
    (adjacent cells merged into one range) once the batch should complete
    the k rows, their exact distances computed;
  - once k rows are held, D_k (the k-th distance) prunes: a row beyond it is
    not copied, a cell crossing it is split finer, a cell within it is read
    whole, and every cell left nearer than D_k goes into one more batch;
  - it stops when the nearest cell left is farther than D_k. The rows within
    D_k, ties included, are the first ring, and rings carry on beyond it if
    the query wants more.

  At k = 1000 (`micro_oc.sql`, real36's copy) rows read fell from 2,150 to
  about 1,480 per query (1,000 copied, against 1,680), in 2.7 batches.
  Profiling then found two costs common to both modes, fixed with it:
  - the scan's projection computed the distance a second time for every row
    returned, for the `ORDER BY` column (13% of a k = 1000 search). The
    projection now reads it from a parameter the scan sets, for queries of 32
    rows or more (below that, compiling the projection again costs more);
  - sorting compared NULL and NaN flags for every pair; rows whose distances
    are all numbers now use a comparison of two doubles.

  Split and batch thresholds (a cell is split above max(8, k/8) expected
  rows, and once D_k is known above 8 if it crosses it) were tuned over
  4–32 and k/4–k/16; the times were flat within the noise round 8 and k/8.
  The walk holds its rows in memory, so it is used only when about three
  times the rows wanted fit in `work_mem` at the heap's average width; the
  rings, which spill to a tuplesort, otherwise. perfcheck against v0.51
  (`perfcheck-v051-v052.md`): ObsCore k = 100 0.84 [0.78, 0.90], k = 1000
  0.74 [0.69, 0.79]; nothing else moved (Gaia k = 1 1.02 [0.99, 1.04]).
  Exactness: the `nearest` regression test with the walk forced on, and
  comparisons against PostgreSQL's sort at random and field centres, with
  conditions, offsets, ties, NULL positions and partitions by cell range.
- **A regression found and fixed:** 0.44 read the index's lowest and highest
  key at every nearest query (for partitions), about 25 µs, a third of a
  k = 1 search (`bisect-v042-v044.txt`: v0.42 77 µs, v0.44 104 µs). Since
  0.45 only a partition reads them (`bisect-fix.txt`: 78 µs).

The answers were checked separately against PostgreSQL's own sort (the
`nearest` regression test, and #50's sweeps); this run counts rows only.
