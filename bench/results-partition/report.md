# Cross-matching a partitioned catalogue (0.44)

`../37_partition_xmatch.sql` on real36: 5M of the 10M real Gaia DR3 positions, once
in one table (`bp_flat`, clustered in cell order) and once in 16 partitions by
cell range (`bp_part`), the way an archive partitions by HEALPix index or by
Gaia's `source_id`. Probes on catalogue sources, moved 0.5″. Median ms of 5
warm rounds, the two tables in random order; both tables matched the same rows
in every case, in every run. PostgreSQL 18.6, 4 vCPU,
`max_parallel_workers_per_gather = 0`. Three libraries, each run on its own
(`versions.txt`):

- **0.43**: every partition's scan covers the cone and searches its index at
  every probe.
- **key test**: each scan tests the cone against its index's lowest and highest
  key before covering it (`run-keytest.out`).
- **pruning**: SQL 0.40 adds the cone's cell interval next to it, and
  PostgreSQL prunes the partitions it cannot reach at every probe
  (`run-pruning.out`).

Partitioned ÷ unpartitioned, each from its own run's paired medians:

| probes | radius | 0.43 | key test | pruning | unpartitioned ms | partitioned ms, pruning |
|---|---|---|---|---|---|---|
| 1,000 | 1″ | 3.64 | 1.94 | 1.17 | 10.2 | 11.9 |
| 1,000 | 5″ | 4.14 | 1.85 | 1.44 | 8.5 | 12.2 |
| 1,000 | 30″ | 3.36 | 1.78 | 1.42 | 9.8 | 13.8 |
| 10,000 | 1″ | 7.67 | 2.22 | 1.42 | 41.4 | 58.9 |
| 10,000 | 5″ | 6.34 | 2.39 | 1.74 | 47.0 | 81.7 |
| 10,000 | 30″ | 3.83 | 1.81 | 1.55 | 65.5 | 101.8 |
| 100,000 | 1″ | 8.11 | 2.37 | 1.51 | 387.7 | 585.0 |
| 100,000 | 5″ | 7.40 | 2.12 | 1.48 | 422.5 | 625.2 |
| 100,000 | 30″ | 5.06 | 1.77 | 1.21 | 759.1 | 921.5 |

With pruning only the partition holding the probe's cone runs (one, rarely
two). What is left, about 2 µs per probe, is PostgreSQL's own work at each
rescan of the Append: evaluating the two bounds and finding the partitions,
then restarting the one that runs. Turning off the key test, which pruning
makes redundant here, moved the times by less than this host's run-to-run
spread; it stays, for partitionings pruning cannot use (declination bands).
