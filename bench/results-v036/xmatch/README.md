# Resampled-catalogue cross-match with skycell 0.36

`run.sh` uses `../../results-pg18/xmatch/`'s protocol for `report_join.md`:

- the 10M Gaia-density corpus (`11_scale_corpus.sql`, `02_build.sql`);
- 200,000 probes from `04_xmatch.sql`, in 8 blocks;
- `11_xmatch_ab.sql`'s `bench_xm_ab_run(10, {1,10}, 2)`;
- methods q3c, pgsphere, skycell_slots (the range rewrite), skycell_cs (the
  custom scan) and skycell_join (Q3C's spelling).

Host: the cloud machine type, PostgreSQL 18.6, Q3C 2.0.5, pgSphere 1.5.2,
skycell 0.36 (`versions.txt`). `report_join.md` is
`../../results-pg18/xmatch/report.py` on `bench_xm_ab_join.csv`. Every skycell
method returned q3c's rows in all 32 trials; pgSphere differed by one row in 2.

## Ratios, 0.26 → 0.36 (paired median, 95% bootstrap in the reports)

| | 1" | 10" |
|---|---|---|
| skycell_join / q3c      | 0.67 → 0.73 | 0.73 → 0.83 |
| skycell_join / pgsphere | 0.26 → 0.26 | 0.31 → 0.33 |
| skycell_cs / q3c        | 0.71 → 0.75 | 0.75 → 0.84 |
| skycell_cs / pgsphere   | 0.27 → 0.28 | 0.31 → 0.33 |

The intervals overlap, though barely at 10" against q3c. The 0.26 run was made
on another container. Here every method is faster, and q3c most of all: its
median block took 226 → 148 ms at 1", skycell_join 153 → 109 ms.

## The same host, 0.26 against 0.36

`same-host-026/xm.sh` built the corpus and the probes in a second database
under skycell 0.26's library and catalogue. It then ran `bench_xm_ab_run`
four times, alternating 0.26, 0.36, 0.36, 0.26, with q3c, pgsphere,
skycell_cs and skycell_join.

| | 0.26 (runs 1 / 4) | 0.36 (runs 2 / 3) |
|---|---|---|
| skycell_join / q3c, 1"       | 0.77 / 0.75 | 0.78 / 0.76 |
| skycell_join / q3c, 10"      | 0.81 / 0.80 | 0.85 / 0.81 |
| skycell_cs / q3c, 1"         | 0.69 / 0.75 | 0.75 / 0.78 |
| skycell_cs / q3c, 10"        | 0.79 / 0.81 | 0.87 / 0.85 |
| skycell_join / pgsphere, 1"  | 0.29 / 0.29 | 0.31 / 0.29 |
| skycell_join / pgsphere, 10" | 0.31 / 0.33 | 0.33 / 0.32 |

On the same host the versions are level within the spread between runs, and
every skycell method returns q3c's rows under both. The move against q3c
since the 0.26 run is the host's: on this machine, 0.26 itself takes 0.75–0.81
of q3c's time.
