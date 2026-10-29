# The memory test with skycell 0.36, scaled to 10M rows (Sect. 5.3's last sentence)

The original (`../../results-pressure/iso_results.txt`, the laptop, skycell 0.6)
ran a 20.5M-row, 22 GB ObsCore-shaped relation in a 3 GiB container with
`shared_buffers = 768MB`, where pgSphere's GiST (1162 MB) was 1.5 times the
buffer pool and skycell's B-tree (439 MB) 0.57 times. This repeats it at 10M
rows on the cloud machine, keeping those ratios:

- `setup.sql` builds the relation: 10M rows of the original's width (1,170
  bytes, seven to a page; 11 GB), 3,498 2-degree cones drawn uniformly on the
  sphere. The original's build script was never committed, so this is a
  rebuild, not a copy: observations come in groups of 128 per field from the
  stored DR3-weighted field centres (`data/oc_fields.csv.gz`, as Table 8's
  observation table), ordered by collection and field. REPRODUCING.md §3
  describes the original's positions only as resampled from `gaia_map`.
- `run.sh` scales the memory with the row count: `shared_buffers` 394 MB
  (pgSphere's 591 MB index / 1.5; skycell's 214 MB index is then 0.54 of it),
  a cgroup memory limit of 1,498 MB on the server (3 GiB × 10/20.5; the cgroup
  charges the page cache, as the container limit did) and
  `effective_cache_size` 999 MB.
- `13_pressure.sql` runs unchanged, four phases (pgSphere, skycell, skycell,
  pgSphere), each after a restart with the page cache dropped.

Raw output: `iso_results.txt`; `run.log`, `setup.out`. The first line of
`run.log` with a 149 MB limit is from the build-only pass, before a factor in
`run.sh` was corrected (10/205 for 100/205); no phase ran with it.

## Result

| phase | method | median ms | pages cached | pages from disk | from disk |
|---|---|---|---|---|---|
| A1 | pgSphere | 6.1 | 1,807 | 983 | 35% |
| A2 | skycell  | 2.6 |    63 | 936 | 94% |
| B1 | skycell  | 2.6 |    63 | 936 | 94% |
| B2 | pgSphere | 6.3 | 1,807 | 983 | 35% |

skycell answers the stream 2.3–2.4 times faster, touching 999 pages per
query against pgSphere's 2,790. The server reached its memory limit in every
phase (`memory.max_usage_in_bytes`).

The laptop measured 4.2–4.8 times (pgSphere 10.4 / 13.4 ms, skycell 2.5 /
2.8 ms). skycell's median is the same; pgSphere's is lower here. The plans
(one 2-degree cone, `EXPLAIN (ANALYZE, BUFFERS)`): pgSphere an Index Scan on
its GiST, 10,182 buffers and 155 ms; skycell its custom scan in bitmap mode,
16 ranges, 3,408 buffers and 19 ms. The two runs differ in relation layout,
host and memory scale as well as version, so the change cannot be put on any
one of them; the plans and page counts above are what this run measured.

## The same relation, 0.26 against 0.36 (`same-host-026/`)

`ab.sh` swaps the extension inside `press` itself, so the heap and pgSphere's
GiST are the very same files throughout (`check_0*.txt`: filenodes 17843 and
17873 in every run). Per version: the library swapped, `DROP EXTENSION
skycell CASCADE` (which drops only `oc20_cell`), `CREATE EXTENSION skycell
VERSION`, `oc20_cell` rebuilt (214 MB both times) with its statistics target,
`ANALYZE`, then `run.sh`'s four phases. Order: the 0.36 run above, then 0.26
(`iso_results_026.txt`), then 0.36 again (`iso_results_036.txt`).

| run | pgSphere ms (A1 / B2) | skycell ms (A2 / B1) | pgSphere / skycell |
|---|---|---|---|
| 0.36, first  | 6.1 / 6.3 | 2.6 / 2.6 | 2.3–2.4 |
| 0.26         | 5.9 / 5.9 | 2.7 / 2.6 | 2.2–2.3 |
| 0.36, repeat | 5.6 / 5.5 | 2.4 / 2.4 | 2.3 |

Every run touched the same pages (skycell 63 cached and 936 from disk per
query, pgSphere 1,807 and 983). The two versions are level; the change from the
laptop's 4.2–4.8 is not 0.36's.
