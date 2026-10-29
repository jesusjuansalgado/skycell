# Polygons and MOC footprints with skycell 0.36 (Sect. 5.4's polygon paragraph, Sect. 5.7's MOC sentence)

`run.sh` is `../../run.sh`'s protocol for `05_poly.sql` and `06_footprints.sql`,
the one the paper's laptop numbers came from, run on the cloud machine:

- each method runs alone, in sequence: pass 1, then pass 2, which is the
  reported pass;
- the sequence then runs again in reverse order (passes 3 and 4), to show
  whether the order matters;
- two 10M corpora: **gaia** (resampled from the DR3 order-9 map, `11`, `02`;
  the paper's 0.28 / 0.77 / 1.86 ms and its 200,474 pairs) and **designed**
  (`01`, `02`);
- per corpus, `03_cone.sql` for the query pool, `04_xmatch.sql` for the
  200,000-row probe table.

`summary.md` has every pass; raw rows are in `bench_poly_*.csv`, `bench_polys_*.csv`
and `bench_fp_*.csv`. Host and versions as `../cones/` (`versions.txt`).

The first designed run overlapped other work on the host (`run.log`), so the
designed stage was run again on a quiet machine. Its files are from that second
run, and agree with the first within a few per cent.

## Polygons (median ms, pass 2)

| corpus | skycell | pgSphere | Q3C | pgSphere / skycell | Q3C / skycell |
|---|---|---|---|---|---|
| gaia     | 0.262 | 1.255 | 3.413 | 4.8 | 13 |
| designed | 0.240 | 1.042 | 3.380 | 4.3 | 14 |
| laptop, gaia (0.6) | 0.281 | 0.767 | 1.864 | 2.7 | 6.6 |
| laptop, designed (0.6) | 0.142 | 0.631 | 1.526 | 4.4 | 11 |

Every method returned the same rows for every polygon, in every pass. The
reverse order (passes 3, 4) gives the same medians within 11%. pgSphere's tail
is long, as on the laptop: its mean is 7 times its median on the Gaia corpus.

## Footprints: point-in-region join, 200,000 probes against 20,000 circles

| corpus | pairs | skycell s (passes 2, 3, 4) | pgSphere s (passes 2, 3, 4) |
|---|---|---|---|
| gaia     | 198,596 | 1.42, 1.40, 1.55 | 1.46, 1.55, 1.45 |
| designed | 142,645 | 1.24, 1.30, 1.32 | 1.42, 1.37, 1.44 |

Both methods returned the same pairs. On this host the MOC table in a B-tree
is level with pgSphere's GiST on the Gaia corpus and 5–13% faster on the
designed one; on the laptop it took 0.76 s against 0.97 s (gaia) and 0.67 s
against 1.10 s (designed). The plans are the laptop's: pgSphere a nested loop
over its GiST, skycell a nested loop over `skycell_ancestors` (orders 4–11,
eight B-tree probes per point) hash-joined to the footprints. The pair count
differs from the laptop's 200,474 because the corpus differs (see below).

## The same host, 0.26 against 0.36 (`same-host-026/`)

`pf.sh` built the gaia corpus in a database with skycell 0.26's library and
catalogue and again in the 0.36 one. It then ran passes 1 and 2 of pgsphere
and skycell, for both scripts, four times, alternating 0.26, 0.36, 0.36, 0.26.

| run | version | polygons skycell / pgSphere (median ms) | footprints skycell / pgSphere (s) |
|---|---|---|---|
| 1 | 0.26 | 0.225 / 1.255 = 0.18 | 1.33 / 1.30 = 1.03 |
| 2 | 0.36 | 0.243 / 1.208 = 0.20 | 1.44 / 1.47 = 0.98 |
| 3 | 0.36 | 0.241 / 1.183 = 0.20 | 1.39 / 1.58 = 0.88 |
| 4 | 0.26 | 0.265 / 1.425 = 0.19 | 1.31 / 1.53 = 0.86 |

The two versions are level on both. The change from the laptop is the host's,
not 0.36's. The footprint join is one timed query per pass, and its ratio
moves by ±10% between runs of the same version.

The two databases did not get identical corpora (`check_026.txt`,
`check_036.txt`: 10,001,306 against 9,994,125 rows). `11_scale_corpus.sql` is
seeded, but its plan puts a Memoize node over
`generate_series(1, floor(n * g.n / tot.s + random()))`, keyed on `g.n`. Cells
with the same count then reuse a cached row count instead of drawing
`random()` again, and which draws are reused depends on the cache's sizing
and evictions, which can differ between databases. Within each database both methods
query the same rows, so each ratio above is fair; the 0.26 database's corpus
is the one `run.sh` measured (198,596 pairs in both).
