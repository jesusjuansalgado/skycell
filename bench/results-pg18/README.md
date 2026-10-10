# Paper tables rerun on PostgreSQL 18.6, custom scan as the default

PostgreSQL 18.6, pgSphere 1.5.2, Q3C 2.0.5, skycell 0.26 with
`skycell.custom_scan = on` (versions.txt), built from source in a cloud
container: 4 vCPU, 15 GB RAM, shared_buffers 2 GB.  `run.sh` is the driver:
`vm/run_paper.sh`'s cone protocol (07_ab.sql: warm, one untimed pass then 5
repetitions; cold, server stopped and the page cache dropped, one repetition;
the EXPLAIN pass) on the designed corpus (01/02, already built) and on a fresh
10M-row corpus resampled from the DR3 order-9 map (11/02), then the ObsCore
crossover (10) at 0.5M, 2M and 10M rows, 30 centres, 3 repetitions.

Methods: `skycell` is the default (the custom scan), `skycell-rw` the range
rewrite (`skycell.custom_scan = off`).  The cold pass runs q3c, pgsphere and
skycell only: skycell-rw shares `cat_cell`, so whichever ran second would find
it warm.

The real-DR3 columns and the 50M column of tab:scale were run later, on the
same container type: `realcone/` (tab:cones' real columns), `xmreal/`
(tab:xmreal), `estimator/` (tab:estimator) and `scale50/` (tab:scale).

## Files

- `bench_ab.csv`, `bench_ab_x.csv`: both corpora; `ab_report.md` is
  `ab_report.py` on them.
- `rewrite_vs_custom.txt`: `pair.py`, the warm paired ratios of the rewrite to
  pgSphere and of the custom scan to the rewrite.
- `bench_cross.csv`: the crossover, one row per (size, class, method, trial).
- `bench_build_{designed,gaia}.csv`: build times and sizes.

## Cone searches, skycell / pgSphere (paired median ratio, 95% bootstrap)

| radius | n | designed warm | designed cold | Gaia warm | Gaia cold |
|---|---|---|---|---|---|
| 1"   | 400 | **0.75** [0.70, 0.79] | **0.85** [0.80, 0.89] | **0.70** [0.67, 0.74] | **0.79** [0.76, 0.83] |
| 10"  | 400 | **0.76** [0.69, 0.79] | **0.88** [0.81, 0.94] | **0.71** [0.68, 0.76] | **0.88** [0.83, 0.92] |
| 1'   | 300 | **0.80** [0.76, 0.85] | 0.99 [0.91, 1.07]     | **0.74** [0.70, 0.79] | 0.96 [0.90, 1.04]     |
| 6'   | 200 | **0.83** [0.78, 0.88] | **0.86** [0.80, 0.94] | **0.80** [0.74, 0.86] | **0.86** [0.81, 0.90] |
| 30'  | 100 | **0.79** [0.75, 0.86] | **0.75** [0.71, 0.84] | **0.75** [0.73, 0.82] | **0.69** [0.62, 0.76] |
| 1deg |  40 | **0.75** [0.71, 0.78] | **0.61** [0.52, 0.68] | **0.71** [0.69, 0.77] | **0.51** [0.45, 0.72] |
| 3deg |  16 | **0.73** [0.66, 0.82] | **0.61** [0.50, 0.66] | **0.67** [0.63, 0.76] | **0.51** [0.41, 0.60] |

The rewrite alone measures 0.88-1.07 against pgSphere warm up to 30' (0.69-0.91
at 1-3 deg), where the published PostgreSQL 16 table has it, and the custom
scan is 0.78-0.84 of the rewrite up to 30', 0.87-0.97 above
(`rewrite_vs_custom.txt`).  All methods return the same row totals on both
corpora.

Q3C 2.0.5 is not comparable with the Q3C 2.0.1 of the PostgreSQL 16 runs: it
plans a cone into about 100 bitmap index scans, most over empty ranges, and
spends ~2.7 ms planning, so it runs ~3 ms per cone at every radius.

## ObsCore crossover, total time skycell / pgSphere

| class | 0.5M | 2M | 10M | rewrite at 10M | buffers at 10M (pgSphere / skycell), median |
|---|---|---|---|---|---|
| Q05 (0.15 deg)        | 1.16 | 0.97 | 0.95 | 1.28 |   437 /  172 |
| Q06 (2 deg)           | 0.79 | 0.70 | 0.49 | 0.48 | 43688 / 4846 |
| Q07 (0.5 deg + cuts)  | 0.89 | 0.75 | 0.73 | 0.79 |  3223 /  571 |
| Q12 (empty 0.7")      | 0.61 | 0.66 | 0.64 | 1.05 |     5 /    6 |

Row counts agree across the three methods at every size and class.  The centre
draw here is far denser than in the published run (median Q06 cone 117k rows at
10M against 17k in `results-ab/bench_cross.csv`), because the crossover draws
its centres from whichever `src` corpus is live.  On cones that size pgSphere
estimates a fixed 3046 rows, picks a plain GiST index scan and visits ~60k heap
pages, where the custom scan goes through a TID bitmap; that, not the covering,
is most of the Q06/Q07 margin, and the rewrite shows the same margin on Q06.
