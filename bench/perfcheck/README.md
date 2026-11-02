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
- **Verdict** (`report.py`): per workload, the candidate's summed per-centre
  medians over the baseline's, each centre's median taken over the rounds,
  with a 95% bootstrap interval over the centres. An interval wholly above
  `PERF_LIMIT` (1.10) is a REGRESSION and the check exits 1; wholly below
  1/1.10, faster. Both libraries must return the same counts at every centre.
- **Time:** about a minute a round on the 4-vCPU host, with the tables built.
