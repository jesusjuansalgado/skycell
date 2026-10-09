# ObsCore crossover with the stored centres, PostgreSQL 18.6

`10_crossover.sql` after 0aeba0e: field centres from `bench/data/oc_fields.csv.gz`,
query centres from `bench/data/oc_queries.csv` (the first 30 of 40, the same at
every size), seeded offsets.  Sizes 0.5M, 2M, 10M rows; 3 repetitions;
methods pgsphere, skycell (the custom scan, skycell's default) and skycell-rw
(the range rewrite).  Two complete runs that differ only in the seed ordering
the trials (0.19, 0.53); each size rebuilds the same relation, and both runs
return the same 2,171,262 rows in total.  Same host and versions as `../`.

`summary.md` is `summarize.py` on the two CSVs.  Total query time (planning and
execution), skycell / pgSphere, as run 0.19 / run 0.53:

| class | 0.5M | 2M | 10M | rewrite at 10M | median rows at 0.5M / 2M / 10M | median buffers at 10M, pgSphere / skycell |
|---|---|---|---|---|---|---|
| Q05 (0.15 deg)       | 1.14 / 1.13 | 1.06 / 1.07 | 0.96 / 0.98 | 2.20 / 2.08 | 48 / 62 / 126   | 32 / 32   |
| Q06 (2 deg)          | 0.88 / 0.97 | 0.92 / 0.93 | 0.74 / 0.76 | 0.75 / 0.80 | 576 / 1651 / 8350 | 939 / 591 |
| Q07 (0.5 deg + cuts) | 1.05 / 0.86 | 0.94 / 1.02 | 0.80 / 0.81 | 1.06 / 1.05 | 128 / 208 / 502 | 116 / 89  |
| Q12 (empty 0.7")     | 0.66 / 0.73 | 0.65 / 0.71 | 0.41 / 0.53 | 0.73 / 0.86 | 0 / 0 / 0       | 8 / 3     |

The earlier PG18 run (`../bench_cross.csv`) drew its fields from the first rows
of the resampled-Gaia `src`, one patch of sky, and returned ~117k rows for a
median 2 deg cone at 10M; with all-sky fields it is 8350.
