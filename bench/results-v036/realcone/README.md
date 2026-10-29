# tab:cones' real-corpus columns with skycell 0.36

`run.sh` is `../../31_real_cones.sh` (`30_real_cones.sql`) unchanged, the
protocol of `../../results-pg18/realcone/`, on 10M real Gaia DR3 positions
(`random_index < 10^7`), 1,456 centres at tab:cones' radii and counts:

- **Warm**, on the shared heap `gaia_realc`: skycell (the custom scan),
  skycell-rw (the range rewrite, `skycell.custom_scan = off`) and pgSphere,
  each in its own block, 3 untimed then 5 measured EXPLAIN passes, in two
  orders.
- **Cold**, on one table per method (`gaia_real_cell`, `gaia_real_sphere`,
  `../../19_gaia_load_split.sh`): 1,456 new centres kept r1 + r2 + 2 deg apart,
  the server restarted and the page cache dropped before each method's pass at
  each radius, first touch only; custom scan and pgSphere.

`report.md` is `../../realcone_report.py`. Row counts agree on every query.
Both centre files are byte-identical to the 0.26 run's, so the heaps, which the
centres are sampled from, are the same.

Host: the cloud machine type of `../../results-pg18/` (x86-64, 4 vCPU Intel Xeon
@ 2.80 GHz, 15 GiB). PostgreSQL 18.6 built from source (`--without-llvm`, gcc
13.3.0, `-O2`), Q3C 2.0.5, pgSphere 1.5.2 with Ubuntu's HEALPix C++, skycell
0.36 from tag `v0.36` (`versions.txt`). REPRODUCING.md §2's settings on the
command line (`server.sh`). `setup.sh` has the build and load commands as run.
19_gaia_load.sh gave 10,000,000 / 10,000,000 / 1,593,958 rows.

## 0.26 → 0.36, paired median ratio [95% bootstrap over queries]

| radius | n | custom / pgSphere, warm | rewrite / pgSphere, warm | custom / rewrite, warm | custom / pgSphere, cold |
|---|---|---|---|---|---|
| 1"   | 400 | 0.83 [0.81, 0.84] → **0.76** [0.74, 0.77] | 1.29 [1.25, 1.33] → 1.24 [1.20, 1.28] | 0.64 [0.63, 0.65] → 0.61 [0.60, 0.63] | 0.87 [0.82, 0.94] → *1.20* [1.13, 1.27] |
| 10"  | 400 | 0.85 [0.82, 0.86] → 0.81 [0.79, 0.83] | 1.34 [1.31, 1.40] → 1.39 [1.35, 1.47] | 0.62 [0.60, 0.64] → **0.57** [0.55, 0.59] | 1.01 [0.94, 1.09] → **0.85** [0.81, 0.89] |
| 1'   | 300 | 0.90 [0.87, 0.92] → **0.84** [0.82, 0.86] | 1.27 [1.23, 1.31] → *1.57* [1.51, 1.63] | 0.70 [0.68, 0.72] → **0.53** [0.51, 0.55] | 0.98 [0.90, 1.04] → 0.93 [0.87, 1.00] |
| 6'   | 200 | 0.96 [0.93, 0.99] → 0.95 [0.91, 1.00] | 1.56 [1.49, 1.64] → *2.07* [1.99, 2.20] | 0.62 [0.60, 0.64] → **0.47** [0.44, 0.48] | 0.61 [0.55, 0.65] → *0.78* [0.74, 0.82] |
| 30'  | 100 | 0.81 [0.78, 0.86] → 0.88 [0.79, 0.95] | 1.52 [1.31, 1.71] → 1.51 [1.30, 1.70] | 0.57 [0.52, 0.61] → 0.62 [0.56, 0.65] | 0.67 [0.60, 0.72] → *0.84* [0.78, 0.97] |
| 1deg |  40 | 0.80 [0.75, 0.88] → 0.78 [0.75, 0.86] | 1.25 [0.98, 1.51] → 1.12 [0.92, 1.34] | 0.66 [0.63, 0.76] → 0.73 [0.61, 0.87] | 0.53 [0.44, 0.66] → 0.69 [0.54, 0.85] |
| 3deg |  16 | 0.69 [0.64, 0.75] → 0.70 [0.69, 0.75] | 0.74 [0.73, 1.06] → 0.69 [0.64, 0.95] | 0.92 [0.69, 0.98] → 1.02 [0.75, 1.06] | 0.48 [0.43, 0.54] → *0.57* [0.55, 0.64] |

Bold: the 0.36 interval lies entirely below the 0.26 one (better for the
numerator). Italic: entirely above it (worse).

Intervals that no longer overlap:

- **Better:** custom / pgSphere warm at 1" and 1'; custom / rewrite warm at
  10", 1' and 6'; custom / pgSphere cold at 10".
- **Worse:** rewrite / pgSphere warm at 1' and 6'; custom / pgSphere cold at
  1", 6', 30' and 3 deg.

None of these is skycell's version. The investigation below shows that the
bootstrap intervals, which resample queries inside one run, are far narrower
than the spread between runs on this host, and every shift above is inside
that spread.

## The same host, 0.26 against 0.36

`same-host-026/load.sh` built a second database, `real26`, under skycell
0.26's library and catalogue (`CREATE EXTENSION skycell VERSION '0.26'`). The
library was built from skycell commit `4ea17ee`, the 0.26 the `results-pg18`
real-corpus runs used (there is no `v0.26` tag); the 0.26 catalogue script it
ships is identical to the one 0.36 installs. Its tables are built from the
0.36 database's rows with 19_gaia_load.sh's and 19_gaia_load_split.sh's SQL,
and `same-host-026/heaps.txt` checks that all three heaps are identical in
both databases (row count, MD5 of the source_id order, size). The library file
is swapped between runs; each version runs in its own database.

`same-host-026/run.sh` then ran the whole protocol, warm and cold, eight
times, alternating 0.26 and 0.36: runs 1–4 as 26, 36, 36, 26, runs 5–8 as
36, 26, 26, 36 (`FIRST=4 VERSIONS="36 26 26 36"`). Before runs 5–8 the 0.36
database was re-ANALYZEd (below). Each run's CSVs and report are in
`same-host-026/run<i>_0<v>/`; `summary.md` has every run's ratios side by
side, `spread.md` (`spread.py`) the following:

| radius | 0.26, results-pg18 | 0.36, this run | same host, 0.26: median [min, max] of 4 runs | same host, 0.36: median [min, max] of 4 runs |
|---|---|---|---|---|
| **custom / pgSphere, warm** |||||
| 1"   | 0.83 | 0.76 | 0.74 [0.58, 0.77] | 0.76 [0.54, 0.86] |
| 10"  | 0.85 | 0.81 | 0.73 [0.70, 0.78] | 0.75 [0.52, 0.80] |
| 1'   | 0.90 | 0.84 | 0.82 [0.82, 0.92] | 0.72 [0.57, 0.84] |
| 6'   | 0.96 | 0.95 | 0.94 [0.88, 1.03] | 0.84 [0.57, 0.87] |
| 30'  | 0.81 | 0.88 | 0.79 [0.66, 0.89] | 0.71 [0.57, 0.79] |
| 1deg | 0.80 | 0.78 | 0.73 [0.64, 0.74] | 0.73 [0.60, 0.86] |
| 3deg | 0.69 | 0.70 | 0.68 [0.56, 0.72] | 0.69 [0.59, 0.72] |
| **rewrite / pgSphere, warm** |||||
| 1"   | 1.29 | 1.24 | 1.23 [0.97, 1.44] | 1.34 [1.00, 1.55] |
| 10"  | 1.34 | 1.39 | 1.22 [1.17, 1.50] | 1.25 [1.00, 1.34] |
| 1'   | 1.27 | 1.57 | 1.32 [1.18, 1.66] | 1.17 [0.93, 1.22] |
| 6'   | 1.56 | 2.07 | 1.64 [1.50, 1.76] | 1.50 [1.38, 1.61] |
| 30'  | 1.52 | 1.51 | 1.33 [1.27, 1.46] | 1.22 [1.04, 1.47] |
| 1deg | 1.25 | 1.12 | 1.04 [0.89, 1.07] | 1.00 [0.81, 1.14] |
| 3deg | 0.74 | 0.69 | 0.69 [0.54, 0.74] | 0.67 [0.57, 0.81] |
| **custom / rewrite, warm** |||||
| 1"   | 0.64 | 0.61 | 0.61 [0.50, 0.62] | 0.55 [0.52, 0.62] |
| 10"  | 0.62 | 0.57 | 0.61 [0.46, 0.66] | 0.58 [0.51, 0.65] |
| 1'   | 0.70 | 0.53 | 0.66 [0.48, 0.70] | 0.64 [0.63, 0.68] |
| 6'   | 0.62 | 0.47 | 0.58 [0.50, 0.64] | 0.55 [0.41, 0.58] |
| 30'  | 0.57 | 0.62 | 0.57 [0.50, 0.72] | 0.58 [0.55, 0.61] |
| 1deg | 0.66 | 0.73 | 0.73 [0.70, 0.80] | 0.73 [0.70, 0.89] |
| 3deg | 0.92 | 1.02 | 1.05 [0.90, 1.07] | 1.02 [0.81, 1.06] |
| **custom / pgSphere, cold** |||||
| 1"   | 0.87 | 1.20 | 0.58 [0.53, 1.13] | 0.73 [0.57, 0.82] |
| 10"  | 1.01 | 0.85 | 0.66 [0.54, 0.90] | 0.77 [0.61, 1.06] |
| 1'   | 0.98 | 0.93 | 0.81 [0.71, 0.88] | 0.85 [0.81, 1.05] |
| 6'   | 0.61 | 0.78 | 0.81 [0.66, 0.91] | 0.81 [0.71, 1.18] |
| 30'  | 0.67 | 0.84 | 0.78 [0.70, 0.81] | 0.81 [0.72, 0.90] |
| 1deg | 0.53 | 0.69 | 0.51 [0.42, 0.62] | 0.52 [0.44, 0.56] |
| 3deg | 0.48 | 0.57 | 0.57 [0.46, 0.58] | 0.54 [0.41, 0.67] |

- **Run-to-run spread:** four runs of one version on one host span, for
  example, 0.54–0.86 (custom / pgSphere warm at 1") and 0.53–1.13 (cold at
  1"), against bootstrap intervals about 0.03 wide. Each run takes under a
  minute, so the per-method blocks last seconds and a change in the host's
  speed between blocks moves a ratio. Over the session the host got faster:
  in `../xmreal/` the last of four runs was 14–21% faster than the first for
  every method.
- **No version regression:** on every line but one the two versions' ranges
  over four runs overlap. The exception goes 0.36's way: custom / pgSphere
  warm at 6': 0.87, 0.85, 0.57, 0.83 under 0.36 against 0.91, 1.03, 0.97,
  0.88 under 0.26. Leaving out 0.36's outlier (run 5, 0.57), that is about
  0.08 lower, possibly a small improvement; this run's own 0.95 does not show
  it, so it is not claimed.
- **Rewrite at 1' and 6' in this run:** 1.57 and 2.07 lie above all eight
  same-host runs, of either version. Runs 2 and 3 used the same database,
  library and statistics as this run, and their buffers equal this run's on
  all 4,368 warm (method, centre) queries (`compare026.py . same-host-026/run2_036`),
  so the plans are the same. Only the time differs: the rewrite's median
  planning at 1' was 0.050 ms here against 0.036 ms in run 2, and 0.065 against
  0.052 ms at 6'. This run was the first after the load, the earliest and
  slowest stretch of the session.
- **Cold:** pages read per query are the same in both versions and in both
  runs (`compare026.md`: 1.7 / 2.5 at 1", 432.5 / 800.6 at 3 deg), and a read
  costs 0.02–0.12 ms, so a cold pass is a few dozen sub-millisecond queries
  after a restart and moves with the host as much as warm does.
  `results-pg18` saw the same: this storage is fast local flash, not a
  droppable cache.

### Plans

`same-host-026/plans.sql` explains every warm centre's query, custom scan and
rewrite, under 0.26 in `real26` and under 0.36 in `real36`
(`plans_026.csv`, `plans_036.csv`; `plans_diff.txt`):

- **Identical** for 2,793 of the 2,912 (centre, method) plans. No 1" plan
  differs; 1 of 800 at 10"; the rest are at 1' and up (at most 30 of 200, the
  rewrite at 6').
- **The differences are each database's ANALYZE sample, not the version.**
  Re-ANALYZEing the 0.36 database and explaining again under 0.36 changed 131
  plans (`plans_036_reanalyzed.csv`), more than the 119 the version change
  did. The differences are the coverings the cost model picks from the
  density estimate (e.g. 4 ranges against 3 for one 10" cone; a coarser
  bounding range for one 1' rewrite).
- **Buffers:** skycell's mean buffers per query are within 1% of 0.26's at
  every radius (`compare026.md`), and identical between runs that share
  statistics. pgSphere's differ by up to 4% (12.3 against 11.8 at 6'): its
  GiST is rebuilt with each load and the trees differ slightly (`../xmreal/`).

## Files

- `run.sh`, `server.sh`, `setup.sh`: the commands, as run.
- `bench_realcone.csv`, `bench_realcone_cold.csv`, `realcone_centers.csv`,
  `realcone_cold_centers.csv`: raw results; `report.md`; `run.log`;
  `versions.txt`.
- `compare026.py`, `compare026.md`: per radius and method, 0.26's and 0.36's
  median planning and execution time and buffers on the same queries.
- `same-host-026/`: `load.sh`, `run.sh`, `heaps.txt`, the eight runs,
  `summary.md`, `spread.py`, `spread.md`, `plans.sql`, `plans_*.csv`,
  `plans_diff.txt`.
