# Cross-matching a partitioned catalogue (0.44)

`../37_partition_xmatch.sql` on real36: 5M of the 10M real Gaia DR3 positions, once
in one table (`bp_flat`, clustered in cell order) and once in 16 partitions by
cell range (`bp_part`), the way an archive partitions by HEALPix index or by
Gaia's `source_id`. Probes on catalogue sources, moved 0.5″. Median ms of 5
warm rounds, the two tables in random order; both tables matched the same rows
in every case, under both libraries. Library v0.43 (tag) and the 0.44
development build, PostgreSQL 18.6, 4 vCPU, `max_parallel_workers_per_gather = 0`.

| probes | radius | flat, 0.43 | partitioned, 0.43 | ÷ flat | flat, 0.44 | partitioned, 0.44 | ÷ flat | partitioned 0.44 ÷ 0.43 |
|---|---|---|---|---|---|---|---|---|
| 1,000 | 1″ | 8.7 | 31.9 | 3.64 | 7.4 | 14.3 | 1.94 | 0.45 |
| 1,000 | 5″ | 8.7 | 36.1 | 4.14 | 8.4 | 15.5 | 1.85 | 0.43 |
| 1,000 | 30″ | 11.9 | 39.9 | 3.36 | 9.5 | 17.0 | 1.78 | 0.43 |
| 10,000 | 1″ | 45.9 | 352.1 | 7.67 | 39.2 | 87.2 | 2.22 | 0.25 |
| 10,000 | 5″ | 46.5 | 294.7 | 6.34 | 43.4 | 103.5 | 2.39 | 0.35 |
| 10,000 | 30″ | 80.8 | 309.5 | 3.83 | 62.6 | 113.3 | 1.81 | 0.37 |
| 100,000 | 1″ | 377.2 | 3057.6 | 8.11 | 374.1 | 888.2 | 2.37 | 0.29 |
| 100,000 | 5″ | 447.4 | 3309.9 | 7.40 | 436.7 | 925.6 | 2.12 | 0.28 |
| 100,000 | 30″ | 640.4 | 3240.8 | 5.06 | 632.1 | 1117.1 | 1.77 | 0.34 |

Each partition's scan is rescanned for every probe (PostgreSQL's Append under
the nested loop). Up to 0.43 each rescan covered the cone against its own
partition's density and searched its index, though a cone lies in one partition
or two. Since 0.44 a scan reads its index's lowest and highest key once, and
tests each cone against them with the covering's seed before covering it: the
other partitions' rescans cost about 0.3 µs, and the remaining gap to the
unpartitioned table is those rescans, 15 per probe here.
