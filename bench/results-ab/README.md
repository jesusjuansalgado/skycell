# Controlled benchmark results

Produced by `bench/07_ab.sql` … `bench/10_crossover.sql`, analysed by
`bench/ab_report.py`. Unlike `../results/`, these are *randomized paired
trials*: all three methods answer each query back to back in an order drawn per
trial, repeated, warm and with caches dropped.

| file | what |
|---|---|
| `bench_ab.csv` | one row per (corpus, cache, repetition, query, method): time and the position it drew in the trial |
| `bench_ab_x.csv` | randomized `EXPLAIN (ANALYZE, BUFFERS)` pass: planning, execution, buffers, estimate vs actual |
| `cm_sweep.csv` | `range_cost` over four decades, both corpora, plus a cold sweep (`gaia-cold`) |
| `cm_rho.csv` | the histogram's density estimate against the counted truth in the same footprint |
| `cm_curve.csv` | query time at every order around the model's choice (`skycell.force_order`) |
| `bench_join.csv` | spatial + non-spatial predicates and joins: estimate, plan strategy, time |
| `bench_cross.csv` | ObsCore-shaped relation at 0.5M / 2M / 10M rows, both indexes |
| `bench_poly.csv`, `bench_xmatch.csv`, `bench_fp.csv`, `bench_build.csv` | polygons, cross-match, footprints, build cost (Gaia corpus, measured per method in sequence) |

Corpora: `designed` is the synthetic one; `gaia` is resampled from Gaia DR3
source counts per order-9 cell.

## Scale run (added after the referee round)

`11_scale_corpus.sql` / `12_scale_build.sql` build a 50M-row corpus from the same
Gaia density field and the three index layouts on it; the A/B is then the same
`bench_ab_run('gaia50', …)`. At that size pgSphere's GiST index is 3442 MB
against a 2 GB `shared_buffers` and skycell's B-tree is 1072 MB, which is the
point of the size: one fits, the other does not.

Result: the advantage **grows** with the catalogue for regions ≥30′ (1° cold:
0.53 → 0.27) and **reverses** below a few arcminutes (1′ 0.93 → 1.07, 6′ 1.03 →
1.13, warm), because skycell's planning cost grows with source density while
pgSphere's does not. `bench_build50.csv` holds the build times and sizes.
