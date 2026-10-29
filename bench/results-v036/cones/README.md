# Cone searches with skycell 0.36 (tab:cones' synthetic columns, Fig. 1, tab:scale, tab:gaia)

`run.sh` is `../../results-pg18/run.sh`'s cone protocol unchanged:

- **Warm:** one untimed pass, then 5 repetitions of q3c, pgsphere, skycell (the
  custom scan) and skycell-rw (the range rewrite, `skycell.custom_scan = off`).
- **Cold:** the server stopped and the page cache dropped, then one pass of
  q3c, pgsphere and skycell.
- **EXPLAIN:** the EXPLAIN pass.

It runs on two 10M corpora:

- **designed:** `01_data.sql` and `02_build.sql`;
- **gaia:** resampled from the DR3 order-9 map, `11_scale_corpus.sql` and
  `02_build.sql`.

`scale50/` will hold the 5e7 stage (tab:scale, tab:gaia).

Host: the cloud machine type of `../../results-pg18/` (x86-64, 4 vCPU),
PostgreSQL 18.6 built from source, Q3C 2.0.5, pgSphere 1.5.2, skycell 0.36
(`versions.txt`), with the server settings of REPRODUCING.md §2.
`ab_report.md` is `../../ab_report.py` on `bench_ab.csv` and `bench_ab_x.csv`.
Every method returned the same row totals on both corpora.
`bench_build_{designed,gaia}.csv` have build times and sizes.

## skycell / pgSphere, paired median ratio (0.26 → 0.36)

| radius | n | designed warm | designed cold | Gaia warm | Gaia cold |
|---|---|---|---|---|---|
| 1"   | 400 | 0.75 → 0.76 | 0.85 → 0.79 | 0.70 → 0.77 | 0.79 → 0.78 |
| 10"  | 400 | 0.76 → 0.76 | 0.88 → 0.88 | 0.71 → 0.78 | 0.88 → 0.86 |
| 1'   | 300 | 0.80 → 0.84 | 0.99 → 0.96 | 0.74 → 0.79 | 0.96 → 0.98 |
| 6'   | 200 | 0.83 → 0.88 | 0.86 → 0.86 | 0.80 → 0.85 | 0.86 → 0.90 |
| 30'  | 100 | 0.79 → 0.84 | 0.75 → 0.78 | 0.75 → 0.79 | 0.69 → 0.67 |
| 1deg |  40 | 0.75 → 0.78 | 0.61 → 0.60 | 0.71 → 0.76 | 0.51 → 0.50 |
| 3deg |  16 | 0.73 → 0.74 | 0.61 → 0.52 | 0.67 → 0.76 | 0.51 → 0.52 |

The intervals are in `ab_report.md` (0.36) and `../../results-pg18/ab_report.md`
(0.26). The 0.26 run was made in another container of the same type.

- **Designed corpus, warm and cold, and the Gaia corpus cold:** every 0.36
  interval overlaps the 0.26 one.
- **Gaia corpus warm:** 0.36's ratios are higher at every radius, though
  skycell's own median times are unchanged or lower (1": 0.065 → 0.063 ms).
  What changed is pgSphere, faster on this host (1": 0.092 → 0.082 ms).

## The same host, 0.26 against 0.36

To separate the host from the version, `same-host-026/cones.sh` built the gaia
corpus in a second database with skycell 0.26's library and catalogue (`CREATE
EXTENSION skycell VERSION '0.26'`). It then ran the warm protocol four times,
alternating 0.26, 0.36, 0.36, 0.26, with pgsphere and skycell in each run.

| radius | 0.26 | 0.36 |
|---|---|---|
| 1"   | 0.76 / 0.77 | 0.76 / 0.75 |
| 10"  | 0.77 / 0.78 | 0.76 / 0.76 |
| 1'   | 0.80 / 0.80 | 0.78 / 0.79 |
| 6'   | 0.83 / 0.83 | 0.85 / 0.86 |
| 30'  | 0.75 / 0.76 | 0.78 / 0.81 |
| 1deg | 0.73 / 0.73 | 0.75 / 0.74 |
| 3deg | 0.71 / 0.70 | 0.72 / 0.75 |

- **Up to 1':** the two versions are level, so the Gaia warm shift above is the
  host's.
- **From 6' up:** 0.36 reads a few hundredths higher. `same-host-026/x30.sql`
  explains every 6', 30' and 1 deg cone under both versions, alternating twice:
  - **Plans:** the same, ordered mode with the same ranges.
  - **Buffers:** within 2.5% (6': 2,052 against 2,102; 30': 3,825 against
    3,839; 1 deg: 3,432 against 3,407).
  - **Planning and execution:** level across the alternations (30' execution,
    100 cones: 19.5 and 18.2 ms with 0.26, 17.8 and 19.4 ms with 0.36).

  The residual comes from each database's own state, not from skycell. Each
  database's `ANALYZE` draws its own sample, so the density estimate, and with
  it the covering, differs slightly: 15,814 against 15,819 rows removed on
  6'. And pgSphere, which skycell's version does not touch, ran about 10%
  slower in the 0.36 database than in the 0.26 one (1": 0.055 against 0.050
  ms).

## 5e7 rows (`scale50/`, tab:scale's 50M column and tab:gaia)

The 5e7 stage is `../../results-pg18/scale50/run.sh`'s protocol:
`11_scale_corpus.sql` at 5e7, `12_scale_build.sql`, then the same cone
protocol. Every method returned the same row totals.

Index sizes and builds (`scale50/bench_build50.csv`; 0.26 in brackets):

- skycell: 1,070 MB in 15.0 s (1,071 MB, 16.7 s);
- Q3C: 1,070 MB in 21.9 s (1,071 MB, 25.3 s);
- pgSphere: 3,439 MB in 363 s (3,439 MB, 387 s).

skycell / pgSphere, paired median ratio, 0.26 → 0.36. Every interval overlaps
the 0.26 one.

| radius | warm | cold |
|---|---|---|
| 1"   | 0.78 → 0.76 | 0.82 → 0.76 |
| 10"  | 0.78 → 0.78 | 0.86 → 0.87 |
| 1'   | 0.86 → 0.83 | 0.98 → 0.93 |
| 6'   | 0.86 → 0.86 | 0.81 → 0.80 |
| 30'  | 0.75 → 0.76 | 0.53 → 0.58 |
| 1deg | 0.69 → 0.72 | 0.37 → 0.40 |
| 3deg | 0.73 → 0.75 | 0.42 → 0.47 |
