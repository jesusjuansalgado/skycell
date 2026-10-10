# tab:cones' real-corpus columns on PostgreSQL 18.6, custom scan as the default

`../../31_real_cones.sh` (`30_real_cones.sql`), same host and versions as `../`,
on 10M real Gaia DR3 positions (`random_index < 10^7`), 1,456 centres at
tab:cones' radii and counts. `report.md` is `../../realcone_report.py`.

Warm, on the shared heap `gaia_realc`: each of skycell (the custom scan),
skycell-rw (the range rewrite, `skycell.custom_scan = off`) and pgSphere in its
own block, 3 untimed then 5 measured EXPLAIN passes, in two orders. Cold, on one
table per method (`gaia_real_cell`, `gaia_real_sphere`), 1,456 new centres kept
r1 + r2 + 2 deg apart, the server restarted and the page cache dropped before
each method's pass at each radius, first touch only; custom scan and pgSphere
(the rewrite shares `gaia_real_cell`). Row counts agree on every query.

| radius | n | custom / pgSphere, warm | rewrite / pgSphere, warm | custom / rewrite, warm | custom / pgSphere, cold |
|---|---|---|---|---|---|
| 1"   | 400 | **0.83** [0.81, 0.84] | *1.29* [1.25, 1.33] | **0.64** | **0.87** [0.82, 0.94] |
| 10"  | 400 | **0.85** [0.82, 0.86] | *1.34* [1.31, 1.40] | **0.62** | 1.01 [0.94, 1.09] |
| 1'   | 300 | **0.90** [0.87, 0.92] | *1.27* [1.23, 1.31] | **0.70** | 0.98 [0.90, 1.04] |
| 6'   | 200 | **0.96** [0.93, 0.99] | *1.56* [1.49, 1.64] | **0.62** | **0.61** [0.55, 0.65] |
| 30'  | 100 | **0.81** [0.78, 0.86] | *1.52* [1.31, 1.71] | **0.57** | **0.67** [0.60, 0.72] |
| 1deg |  40 | **0.80** [0.75, 0.88] | 1.25 [0.98, 1.51]   | **0.66** | **0.53** [0.44, 0.66] |
| 3deg |  16 | **0.69** [0.64, 0.75] | 0.74 [0.73, 1.06]   | **0.92** | **0.48** [0.43, 0.54] |

The rewrite reproduces the earlier real-corpus deficit (GIST_REGION_DESIGN.md
round sixty-eight: 1.12-1.45 below 1 deg), and its cause: extra planning
(+0.014 to +0.17 ms over pgSphere). The custom scan plans in +0.004 ms over
pgSphere at every radius and executes faster too (0.52-0.82 of pgSphere's
execution time), so the warm real-corpus result now goes skycell's way at
every radius. Cold reads cost about 0.1 ms per page here (pages read and I/O
time are in `report.md`): a cache below the VM, not disk latency, as in round
sixty-nine.
