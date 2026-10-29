# Real-DR3 cross-match with skycell 0.36 (tab:xmreal)

`run.sh` is `../../18_xmatch_sweep.sql`'s `xms_run('{1000,10000,100000}',
'{0.5,1,1.5,5}', '{clustered,uniform}')` unchanged, the protocol of
`../../results-pg18/xmreal/`, on `gaia_realc` (10M real DR3 positions, one
shared heap with the cell expression index, Q3C's index and pgSphere's GiST).
Per cell (size, targets, radius) and repetition (3) the five methods run in
random order, each warmed on the cell's query and then timed.

Methods: `skycell_cs`, the join written with `skycell_cone`, through the
custom scan; `skycell_join`, Q3C's spelling, through the custom scan;
`skycell_slots`, the `skycell_cone` join through the range rewrite
(`skycell.custom_scan` off); `q3c_join`; pgSphere. Every plan reaches
`gaia_realc` through an index (`plan_shape` in `xms.csv`), and row counts
agree in all 24 cells.

Host, builds and settings: as `../realcone/` (`versions.txt`, `server.sh`,
`setup.sh`). `report.md` is `report.py xms.csv` (a copy of results-pg18's).
`report.py` gives no intervals, so `intervals.py` adds them: each (cell,
method)'s repetitions resampled with replacement, 4,000 times, seed 18, and
the table recomputed (`intervals.md`, for both runs).

## 0.26 → 0.36, geometric mean of the per-cell ratio [95% bootstrap]

| radius | cone, custom vs q3c_join | cone, custom vs pgSphere | join, custom vs q3c_join | join, custom vs pgSphere | cone, slots vs q3c_join | cone, slots vs pgSphere |
|---|---|---|---|---|---|---|
| 0.5" | 0.81 → 0.89 | 0.32 → 0.29 | 0.77 → 0.84 | 0.30 → 0.27 | 1.17 → 1.40 | 0.45 → 0.45 |
| 1"   | 0.77 → 0.81 | 0.31 → 0.28 | 0.76 → 0.77 | 0.30 → 0.26 | 1.21 → 1.27 | 0.48 → 0.44 |
| 1.5" | 0.77 → 0.87 | 0.33 → 0.28 | 0.78 → 0.87 | 0.34 → 0.28 | 1.16 → 1.44 | 0.50 → 0.46 |
| 5"   | 0.84 → 0.81 | 0.35 → 0.29 | 0.83 → 0.80 | 0.35 → 0.28 | 1.22 → 1.37 | 0.51 → 0.48 |
| all  | 0.80 [0.77, 0.83] → 0.85 [0.77, 0.89] | 0.33 [0.32, 0.34] → **0.28** [0.26, 0.30] | 0.79 [0.77, 0.82] → 0.82 [0.76, 0.88] | 0.32 [0.31, 0.34] → **0.27** [0.26, 0.30] | 1.19 [1.15, 1.25] → *1.37* [1.24, 1.43] | 0.49 [0.47, 0.52] → 0.46 [0.42, 0.49] |

The per-radius intervals are in `intervals.md`; every one of them overlaps
its 0.26 counterpart. Over all cells, three no longer overlap:

- **Better:** the custom scan against pgSphere, both spellings (cone 0.33 →
  0.28, join 0.32 → 0.27).
- **Worse:** the range rewrite against `q3c_join` (1.19 → 1.37).

## The same host, 0.26 against 0.36

`same-host-026.sh` ran `xms_run` four times, alternating 0.26, 0.36, 0.36,
0.26, each version in its own database with the same `gaia_realc` heap
(`../realcone/same-host-026/load.sh`: skycell 0.26's library from commit
`4ea17ee` and its catalogue). Raw `same-host-026/xms_run<i>_0<v>.csv`, reports
`same-host-026/report_run<i>_0<v>.md`, intervals in `intervals.md`.

| all cells | 0.26, results-pg18 | 0.36, this run | 0.26, run 1 / run 4 | 0.36, run 2 / run 3 |
|---|---|---|---|---|
| cone, custom vs q3c_join | 0.80 | 0.85 | 0.87 / 0.79 | 0.79 / 0.83 |
| cone, custom vs pgSphere | 0.33 | 0.28 | 0.32 / 0.30 | 0.28 / 0.28 |
| join, custom vs q3c_join | 0.79 | 0.82 | 0.86 / 0.79 | 0.81 / 0.83 |
| join, custom vs pgSphere | 0.32 | 0.27 | 0.32 / 0.30 | 0.28 / 0.28 |
| cone, slots vs q3c_join  | 1.19 | 1.37 | 1.29 / 1.23 | 1.29 / 1.32 |
| cone, slots vs pgSphere  | 0.49 | 0.46 | 0.48 / 0.47 | 0.45 / 0.44 |

Each method's own time, as the geometric mean over the 24 cells of the ratio
of its medians (`methods.py`, `methods.txt`):

| method | results-pg18 0.26 → this 0.36 run | same host: 0.26 (runs 1, 4) → 0.36 (runs 2, 3) |
|---|---|---|
| q3c_join      | 1.08 | 1.05 |
| skycell_cs    | 1.15 | 1.01 |
| skycell_join  | 1.13 | 0.99 |
| skycell_slots | 1.24 | 1.08 |
| pgSphere      | 1.32 | 1.13 |

- **The slots' shift is this run's.** On the same host the rewrite reads
  1.29 and 1.23 of `q3c_join` under 0.26 and 1.29 and 1.32 under 0.36; the
  intervals of all four overlap, and this run's 1.37 is above all of them.
  The run-to-run spread is the host's: over the four runs it got faster,
  run 4 by 14–21% for every method against run 1, and a run's methods do not
  all gain alike.
- **Against pgSphere, the change is pgSphere's.** The ratio's drop holds on
  the same host, 0.32 / 0.30 under 0.26 against 0.28 / 0.28 under 0.36. But
  skycell's own times are level there (custom scan 1.01, join 0.99, against
  `q3c_join`'s 1.05): it is pgSphere that ran 13% slower in the 0.36
  database, which skycell's version does not touch. `../cones/` found the
  same on its same-host test (pgSphere about 10% slower in its 0.36
  database). The two databases' GiST indexes on the same heap differ
  slightly (717.8 MB in `real26` against 716.0 MB in `real36`), which shows
  the trees differ, though the 0.6% difference in buffers below is too small
  to account for 13%.
- **Plans:** `same-host-026/plans.sh` ran `plans.sql` under both versions,
  alternating 26, 36, 36, 26. Each run explained every method's 1" cross-match
  of one fixed set of 10,000 probes (every 1,000th row of `gaia_realc` by
  source_id) with `ANALYZE, BUFFERS`: `same-host-026/plans_run<i>_0<v>.txt`.
  With the planner's statistics constants (the rewrite's histogram arrays)
  stripped, the plans are identical between the versions: custom scan in
  ordered mode for both skycell spellings, a four-slot BitmapOr for the
  rewrite, Q3C's bitmap scan, the GiST index scan. Buffers are identical
  for `q3c_join` (130,098), `skycell_cs` and `skycell_join` (47,015) and
  `skycell_slots` (130,090); pgSphere's are 64,081 in `real26` and 64,439 in
  `real36`. These timings, with per-node timing on, vary 2–3× between runs and
  are not used.

## Files

- `run.sh`, `server.sh`, `setup.sh`: the commands, as run; `run.log`.
- `xms.csv`: raw; `report.md` (`report.py`); `intervals.py`, `intervals.md`;
  `methods.py`, `methods.txt`; `versions.txt`.
- `same-host-026.sh`, `same-host-026.out`, `same-host-026/`: the four
  alternating runs and the plan comparison (`plans.sql`, `plans.sh`).
