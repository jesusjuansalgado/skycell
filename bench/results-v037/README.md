# Cone searches with skycell 0.37 (tab:cones, tab:scale, Fig. 1)

skycell 0.37 (tag `v0.37` of jesusjuansalgado/skycell, commit `1465080`) is
0.36 plus jesusjuansalgado/skycell#46. Its two changes are to planning only:
- a cone written with literal coordinates is planned without constant-folding
  its arguments again;
- the covering is computed with about half the instructions, and is the same
  covering.

Its extension catalogue is still extversion 0.36, so each `versions.txt` also
records the tag and the library's checksum (`5793f779…`). Only the cone
searches were re-measured; the paper's other tables stay on 0.36.

`run_all.sh` runs everything here. Host, builds and settings: as
`../results-v036/realcone/` (`server.sh`).

## What ran

- **`cones/`:** `../results-v036/cones/run.sh`'s protocol unchanged, in a new
  database `cone37`:
  - corpora: the designed and Gaia-resampled 10M corpora, then the 5e7 stage
    (`cones/scale50/`);
  - warm: 5 repetitions of q3c, pgsphere, skycell and skycell-rw;
  - cold: one pass of q3c, pgsphere and skycell after a restart and a dropped
    page cache;
  - then the EXPLAIN pass.

  `ab_report.md` is `../ab_report.py`. Index sizes equal 0.36's (pgSphere
  3,438 MB, skycell 1,070 MB at 5e7). Build times differ with the host, and
  Table 5 stays on 0.36.
- **`realcone/`:** `../results-v036/realcone/run.sh`'s protocol unchanged
  (`31_real_cones.sh`), on the same `gaia_realc`, `gaia_real_cell` and
  `gaia_real_sphere` as the 0.36 run. Row counts agree between the methods on
  every query, warm and cold.
- **Fig. 1:** `paper/figures.py` now reads `cones/`.

## The real-DR3 cold column

The cold pass of the first run (`realcone/cold-firsttouch/`) ran on slow
storage. It was the tables' first read since the container restarted, and
reads took 0.1–12 ms (pgSphere 11.7 ms per page at 1″), with stalls of
100–900 ms. That is not the paper's environment (a cold read costs
0.06–0.1 ms), so it is not used.

Four further cold passes ran on local flash (0.07–0.11 ms per page):
`realcone/cold-repeats/cold_run1..4.csv`; `report_run<i>.md` is each pass's
report. `realcone/report.md` is the warm run with cold pass 1. Below 6′ the
passes disagree, while each pass's own interval is narrow. A cold query there
reads two or three pages, and the per-read latency of either method moves the
ratio:

| radius | pass 1 | pass 2 | pass 3 | pass 4 | median [min, max] |
|---|---|---|---|---|---|
| 1″   | 1.07 | 0.78 | 0.68 | 0.66 | 0.73 [0.66, 1.07] |
| 10″  | 1.31 | 0.93 | 0.85 | 0.67 | 0.89 [0.67, 1.31] |
| 1′   | 1.24 | 0.92 | 1.35 | 0.77 | 1.08 [0.77, 1.35] |
| 6′   | 0.73 | 0.82 | 0.64 | 0.95 | 0.78 [0.64, 0.95] |
| 30′  | 0.94 | 0.56 | 0.69 | 0.74 | 0.72 [0.56, 0.94] |
| 1deg | 0.48 | 0.51 | 0.53 | 0.35 | 0.49 [0.35, 0.53] |
| 3deg | 0.37 | 0.49 | 0.44 | 0.36 | 0.41 [0.36, 0.49] |

Table 3's real-DR3 cold column gives the median and range of the four passes
(`cold-repeats/summary.py`, `summary.md`). Bold means skycell is faster in
all four. The 0.36 column was one pass; the same-host study in
`../results-v036/realcone/same-host-026/` had shown the same spread then
(cold 1″ 0.53–1.13).

## 0.36 → 0.37

`compare036.py`, `compare036.md`: every cell of tab:cones and tab:scale under
both versions, with intervals. Four intervals no longer overlap:

- **Better:**
  - Gaia-resampled 1e7, warm, 1″: 0.77 [0.76, 0.80] → 0.71 [0.68, 0.75].
  - Real DR3, cold, 1″: 1.20 [1.13, 1.27] → 0.68 [0.64, 0.73], in the
    first-touch pass. The four-pass median is 0.73 [0.66, 1.07].
- **Worse:**
  - Real DR3, warm, 1″: 0.76 [0.74, 0.77] → 0.81 [0.79, 0.83].
  - Real DR3, cold, 6′: 0.78 [0.74, 0.82] → 0.99 [0.85, 1.18], first-touch
    pass. The four-pass median is 0.78 [0.64, 0.95].

None of them is the version's:
- **Same pages.** 0.37's coverings are 0.36's, so a query reads the same
  pages. Over the real-DR3 warm queries (`realcone/compare036.md`) the buffers
  per query are equal at every radius (1″ 4.3 / 5.6, 6′ 10.6 / 11.8), and cold
  pages read are equal too.
- **Inside the known spread.** The real-DR3 warm 1″ value lies inside the
  spread of four same-host 0.36 runs (0.54–0.86,
  `../results-v036/realcone/same-host-026/spread.md`), and so does the cold 6′
  one (0.71–1.18).
- **Planning.** In the EXPLAIN pass, which follows warm-up and so reuses the
  cached covering, skycell's planning lead over pgSphere at 1″ is level
  (designed: +0.011 ms under 0.36, +0.012 under 0.37). #46's planning savings
  show for cones the cache has not seen, as in the PR's measurements.

All warm real-DR3 ratios now exclude 1, 6′ included (0.95 [0.90, 0.99]), so
"faster at every radius, real Gaia DR3 too" holds again. The paper's abstract,
highlights, conclusions and Sect. 5.1 say so.
