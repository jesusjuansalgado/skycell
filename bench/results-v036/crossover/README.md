# ObsCore crossover with skycell 0.36 (tab:crossover)

`run.sh`: `../../results-pg18/crossover-stored/`'s protocol unchanged.

- `10_crossover.sql`, with the stored field and query centres.
- Sizes 0.5M, 2M and 10M rows, 30 centres, 3 repetitions.
- Methods: pgsphere, skycell (the custom scan) and skycell-rw (the range
  rewrite).
- Two complete runs that differ only in the seed ordering the trials (0.19,
  0.53).

Host: the cloud machine type of `../../results-pg18/` (x86-64, 4 vCPU), with
PostgreSQL 18.6 built from source, pgSphere 1.5.2 and skycell 0.36
(`versions.txt`). Server settings are REPRODUCING.md §2. Every query returned
the same rows across methods in both runs. `summary.md` is
`../../results-pg18/crossover-stored/summarize.py` on the two CSVs.

## skycell / pgSphere, total query time (run 0.19 / run 0.53)

| class | 0.5M | 2M | 10M | median buffers at 10M, pgSphere / skycell |
|---|---|---|---|---|
| Q05 (0.15 deg)       | 1.25 / 1.08 | 1.11 / 1.21 | 0.93 / 0.88 | 29–37 / 32 |
| Q06 (2 deg)          | 0.91 / 0.88 | 0.85 / 0.95 | 0.86 / 0.81 | 969–987 / 591 |
| Q07 (0.5 deg + cuts) | 1.02 / 1.02 | 0.91 / 0.94 | 0.84 / 0.82 | 108–115 / 87 |
| Q12 (empty 0.7")     | 0.76 / 0.74 | 0.75 / 0.71 | 0.50 / 0.53 | 7–8 / 3 |

## Against 0.26

The 0.26 run in `results-pg18/crossover-stored/` came from another container
of the same type. Q05 and Q07 agree with it within the spread between its own
two runs. At 10M, Q06 reads 0.86 / 0.81 here, against 0.74 / 0.76 there, and
Q12 reads 0.50 / 0.53, against 0.41 / 0.53.

To tell a change in skycell from a change of host, 0.26 and 0.36 were run on
this host with the same protocol at 10M (`same-host-026/fair.sh`):

- Each version used its own database and catalogue: skycell 0.26's library
  with `CREATE EXTENSION skycell VERSION '0.26'`, and 0.36.
- The 10M relation was rebuilt before every run, as `run.sh` does.
- The runs alternated: 0.26 then 0.36 with seed 0.19, 0.36 then 0.26 with
  seed 0.53.
- The heaps were identical: same row hash, same size.

| class | 0.26 (0.19 / 0.53) | 0.36 (0.19 / 0.53) |
|---|---|---|
| Q05 | 0.98 / 0.95 | 0.92 / 0.93 |
| Q06 | 0.85 / 0.78 | 0.87 / 0.86 |
| Q07 | 0.76 / 0.83 | 0.81 / 0.86 |
| Q12 | 0.51 / 0.47 | 0.49 / 0.48 |

On the same host the two versions are level, within the spread between runs.
pgSphere's own total varies by about ±10% between them, from 557 to 626 ms on
Q06. Both versions return the same rows, and skycell's median buffers are
equal (591 on Q06). So the higher Q06 ratio here comes from the host, not from
skycell: 0.26 itself gives 0.78–0.85 on this host.

One more check: a plain loop over the 30 Q06 centres, warm, each method's
queries back to back, on the identical heap. There 0.36's bitmap scan read
962 buffers on the first centre where 0.26 read 1,618, and it took 0.75 of
0.26's execution time over the 30. Table 8's protocol interleaves the methods
and warms each query just before timing it, and under that protocol the
difference does not show.
