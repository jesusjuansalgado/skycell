# Performance check

A guard against slowdowns, run before a release: the library just built
against the last release, on the same server, the two installed in turn over
alternating rounds, so that the host's drift falls on both. Not a benchmark
for the paper: short queries, 30 centres, 5 timed executions each.

```bash
cd ext && make perfcheck                      # against the latest v* tag
make perfcheck PERF_BASE=v0.42 PERF_ROUNDS=6
```

It was added after 0.44 shipped a nearest-neighbour search a third slower at
k = 1 (two index descents added to every query), which no test caught.

- **Server:** a development one. The check copies each library into the
  server's `pkglibdir` in turn, and restores the candidate at the end. Each
  workload runs in a new session, which loads the library installed then.
  The baseline must be able to run against the installed SQL version.
- **Database** (`PERF_DB`, default `real36`): `gaia_realc`, the 10M real Gaia
  DR3 positions with the cell index (`../19_gaia_real.sh`). `setup.sql` adds,
  once: 30 centres on sources, 30 probe sets of 300 sources, and 2M of the
  positions in 16 partitions by cell range.
- **Workloads** (`workloads.sql`): ADQL cones of 1″ and 1°, a native 6′ cone,
  a 0.5° polygon, an annulus, a cross-match of 300 probes at 1″, nearest
  neighbours k = 1 and 100, and on the partitioned table a 0.5° cone and a
  cross-match of 300 probes at 5″. Per centre the median of five executions,
  each planned anew at a centre 1e-9° away, after an untimed one.
- **Verdict** (`report.py`): the round is the unit. Within one, the two
  libraries run back to back, so the host's drift between sessions, which
  moves every centre of a session together, falls on both alike. Per round
  and workload, the median over centres of the candidate's time over the
  baseline's at the same centre; over the rounds, their geometric mean with a
  95% t interval. An interval wholly above `PERF_LIMIT` (1.10) is a REGRESSION
  and the check exits 1; wholly below 1/1.10, faster. Both libraries must
  return the same counts at every centre. (A first version bootstrapped over
  centres; an A/A run gave it intervals that excluded 1, 0.87 [0.83, 0.91] for
  a 1″ cone.)
- **Time:** about 4 s a round on the 4-vCPU host, the tables built; 20 rounds
  by default, under three minutes.

## Validation (`validation/`)

- **A/A**, one library as both (`aa.csv`): every interval holds 1; none is
  flagged. Their half-widths show what the check can see: about ±5% on the
  cross-matches and the partitioned workloads, ±10–20% on the sub-millisecond
  ones, so a short query is caught once it slows by about a fifth, a long one
  by a tenth.
- **v0.42 against v0.44** (`v042-v044.csv`): nearest k = 1 is flagged, 1.21
  [1.13, 1.30], the regression 0.44 shipped; the polygon (0.60) and the
  partitioned cross-match (0.35) come out faster, as 0.43 and 0.44 made them.
