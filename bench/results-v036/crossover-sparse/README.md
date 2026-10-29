# The ObsCore crossover: table size against result size

In Table 8, skycell's ratio to pgSphere falls with the relation's size, from
about 1.1 at 0.5M rows to 0.5-0.9 at 10M. Across every crossover dataset in
this repository it is lower at 10M than at 0.5M in 46 of 48 (dataset, class)
pairs. In `build_obscore()` a larger relation also means more fields, so
every cone returns more rows (Q05: 48 at 0.5M, 126 at 10M). This directory
separates the two: does the gain follow the table's size or the number of
rows a query returns?

## The sparse table

`sparse.sql`'s `build_obscore_sparse()` builds a 10M-row relation whose
cones return exactly what they return at 0.5M:
- **Base rows:** `build_obscore(500000)`'s rows, the first 3,906 fields, from
  its own query and seed.
- **Filler:** 9.5M rows in fields that no stored query cone can reach. Each
  filler field centre lies at least 2.6 deg from every `oc_queries` centre:
  Q06's 2 deg plus a field's 0.28 deg maximum offset, with margin. 65,585 of
  the 78,125 stored fields qualify; at 145 rows each they make up the 10M.
- **Build steps:** as `build_obscore()`: the same column formulas, the heap in
  (collection, field) order, `pos` added by ALTER and UPDATE, the same indexes
  and statistics target.

Checked when built:
- the base rows are bit-identical to `build_obscore(500000)`'s;
- every class at every one of the 40 query centres returns the same count as
  on the 0.5M table, 160 of 160;
- heap and indexes are the dense 10M table's size (`*_size.txt`): heap
  1,528 MB, GiST 1,065-1,070 MB, cell index 405 MB.

## The runs

`run.sh` builds four tables in turn and measures each:
- `dense05`: `build_obscore(500000)`, Table 8's 0.5M relation;
- `sparse10`: the sparse table;
- `dense10`: `build_obscore(10000000)`, Table 8's 10M relation;
- `sparse10b`: the sparse table rebuilt, for a fresh ANALYZE sample and a
  second point after `dense10`, against drift.

Each table gets two measurements:
- **Table 8's protocol:** `bench_cross_run`, 30 stored centres, 3
  repetitions, pgsphere, skycell and skycell-rw, seeds 0.19 and 0.53.
- **`threeway.sql`:** pgSphere as planned (an index scan on its GiST),
  pgSphere forced to a bitmap heap scan on the same GiST
  (`enable_indexscan = off`), and skycell; warmed, 3 repetitions, buffer
  reads recorded apart from hits.

The whole sequence ran twice, with two libraries, both extversion 0.36:
- **this directory:** skycell#46 at `2cc6c6e`;
- **`v036/`:** tag `v0.36`, the library the paper's tables use
  (`LIB=... OUT=v036 ./run.sh`).

#46 returns the same coverings as 0.36, so the buffer counts agree between
the two runs to within each build's ANALYZE sample. Host, builds and
settings: as `../realcone/` (`server.sh`). `compare.md` is `compare.py`, in
each directory. Row counts agree between the methods everywhere, and every
buffer access in the three-way runs was a hit.

## Results with 0.36 (`v036/`)

Table 8's measure, summed total time, skycell / pgSphere (seed 0.19 / 0.53):

| class | dense 0.5M | sparse 10M | dense 10M | sparse 10M, rebuilt |
|---|---|---|---|---|
| Q05 | 1.18 / 1.29 | 0.85 / 0.88 | 0.96 / 0.99 | 0.98 / 0.96 |
| Q06 | 0.92 / 0.93 | 0.55 / 0.59 | 0.79 / 0.82 | 0.59 / 0.55 |
| Q07 | 0.98 / 0.96 | 0.82 / 0.67 | 0.86 / 0.89 | 0.78 / 0.83 |
| Q12 | 0.79 / 0.84 | 0.59 / 0.58 | 0.61 / 0.58 | 0.63 / 0.59 |

Buffers per query (mean, pgSphere as planned / pgSphere bitmap / skycell):

| class | dense 0.5M | sparse 10M | dense 10M |
|---|---|---|---|
| Q05 | 7 / 6 / 13   | 10 / 9 / 13     | 53 / 24 / 45      |
| Q06 | 59 / 41 / 86 | 227 / 105 / 144 | 3709 / 737 / 1180 |
| Q07 | 11 / 9 / 21  | 15 / 12 / 28    | 285 / 76 / 139    |
| Q12 | 3 / 3 / 3    | 6 / 6 / 3       | 6 / 6 / 3         |

## Results with skycell#46 (this directory)

Table 8's measure, summed total time, skycell / pgSphere (seed 0.19 / 0.53):

| class | dense 0.5M | sparse 10M | dense 10M | sparse 10M, rebuilt |
|---|---|---|---|---|
| Q05 | 1.09 / 1.14 | 0.93 / 0.87 | 0.71 / 0.85 | 0.88 / 0.85 |
| Q06 | 0.90 / 0.97 | 0.59 / 0.54 | 0.76 / 0.75 | 0.53 / 0.55 |
| Q07 | 1.01 / 1.00 | 0.80 / 0.51 | 0.74 / 0.76 | 0.79 / 0.90 |
| Q12 | 0.77 / 0.79 | 0.54 / 0.63 | 0.49 / 0.50 | 0.56 / 0.69 |

Buffers per query (mean, pgSphere as planned / pgSphere bitmap / skycell)
and rows returned (median), from `threeway.sql` and the Table 8 runs:

| class | dense 0.5M | sparse 10M | dense 10M | rows: 0.5M and sparse / dense 10M |
|---|---|---|---|---|
| Q05 | 7 / 6 / 11  | 10 / 9 / 13   | 60 / 26 / 45      | 48 / 126 |
| Q06 | 59 / 41 / 86 | 231 / 105 / 143 | 3841 / 741 / 1180 | 576 / 8350 |
| Q07 | 11 / 9 / 21 | 16 / 12 / 28  | 309 / 79 / 138    | 128 / 502 |
| Q12 | 3 / 3 / 3   | 6 / 6 / 3     | 8 / 8 / 3         | 0 / 0 |

`compare.md` also has the planning and execution split and the three-way
time ratios, including against bitmap pgSphere.

## What it shows

- **The gain follows the table's size.** On the sparse table every cone
  returns what it returns at 0.5M, and skycell's ratio falls as far as on the
  dense 10M table, or further. Q06 reads 0.53-0.59 sparse against 0.75-0.76
  dense, and Q12 0.54-0.69 against 0.49-0.50. Both sparse builds agree.
- **pgSphere's work grows with its index, for the same result.** From 0.5M to
  the sparse 10M table:
  - Q06: pgSphere's buffers grow from 59 to 218-231 (bitmap 41 to 101-105)
    for the same 576 rows; skycell's from 86 to 143.
  - Q12, which returns nothing and so measures the index alone: pgSphere's
    grow from 3 to 5-6; skycell's stay at 3.
  - The GiST is a larger tree whose regions near the query are shared with
    more of the sky. The B-tree's descent gains at most a level, and the
    covering's ranges still bound exactly the cells near the cone.
- **Not pgSphere's plan, not the buffer pool.** Against pgSphere forced to a
  bitmap scan the picture is the same (`compare.md`), and no page was read
  from outside shared buffers.
- **A large result dilutes the gain.** Per row returned, the two methods
  cost about the same, so the dense 10M table's larger results (Q06: 8,350
  rows) bring the ratio back towards 1: 0.75-0.76 there, against 0.53-0.59
  sparse. This is the opposite of the earlier reading, that larger results
  dilute a fixed overhead of skycell's.

The 0.36 run (`v036/`) shows the same throughout. Q06 reads 0.55-0.59 on
the sparse table against 0.92-0.93 at 0.5M and 0.79-0.82 dense at 10M; for
the same 576 rows pgSphere touches 224-227 pages against 59, and skycell
143-144 against 86.

So for a Q-like cone, the advantage over pgSphere grows with the size of the
table the index covers, and shrinks with the number of rows the query
returns. The measurements stop at 10M rows.

## Files

- `sparse.sql`: `build_obscore_sparse()`.
- `run.sh`, `server.sh`, `threeway.sql`: the commands, as run; `run.log`;
  `versions.txt`.
- `<table>_cross_seed0<seed>.csv`: Table 8's protocol, raw; `<table>_threeway.txt`:
  `threeway.sql`'s output; `<table>_size.txt`: rows and relation sizes (bytes).
- `compare.py`, `compare.md`: the tables.
