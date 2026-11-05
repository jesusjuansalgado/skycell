# skycell on Citus

What works when a catalogue is a Citus distributed table, checked against the
same catalogue on one node. Citus 13.0.3 on PostgreSQL 16.15, a coordinator
and two workers on one machine; skycell 0.45. The catalogue has 200,000 rows
(a Fibonacci sphere), hash-distributed by `id` over 8 shards, with the usual
`skycell_ang2cell(ra, dec)` index, which Citus creates on every shard.

```bash
bench/citus/setup.sh                       # cluster and tables (see the script for building Citus)
psql -h /tmp -p 5601 -U postgres -f bench/citus/compare.sql   # the same rows as one node?
psql -h /tmp -p 5601 -U postgres -f bench/citus/more.sql      # plans, nearest, aggregate, estimates
psql -h /tmp -p 5601 -U postgres -f bench/citus/joins.sql     # cross-matches, density view, settings
```

## How a query runs

`CREATE EXTENSION skycell` on the coordinator creates it on the workers. For
each shard the coordinator sends the worker SQL, which the worker plans with
its own copy of skycell and its shard's own statistics. The coordinator
rewrites ADQL before sending it, so a shard receives skycell's own calls
(`skycell_cone`, `skycell_region_scan`, …), not `CONTAINS`.

## Results

| Query | On the distributed table |
|---|---|
| ADQL cone, `skycell_cone`, `distance(...) < r`, a cone at the pole, with another condition or under an `OR` | the single node's rows; each shard uses the custom scan on its index |
| Polygon, box, composite, MOC, MOC in a composite | the single node's rows; each shard uses the custom scan since 0.45 (before: a sequential scan of every shard, see below) |
| Nearest neighbours, `ORDER BY distance(...) LIMIT k` | the single node's rows; each shard runs the nearest-neighbour scan with its own `LIMIT`, the coordinator merges |
| Cross-match with a reference (replicated) probe table, or a local one | the single node's rows; every shard runs the custom scan once per probe |
| Cross-match of two distributed tables | refused by Citus: it joins distributed tables only on their distribution columns |
| `skycell_moc_agg`, `skycell_coverage_moc` | the single node's MOC, but the coordinator aggregates every row: Citus pushes down an aggregate only when its state is a real type, and this one's is `internal` |
| `skycell_count_estimate` (a region, or a cell) | 0: it reads statistics, and the coordinator holds none for a distributed table |
| `skycell_density_view` | works: the view is built on the coordinator from the shards |
| Prepared statement, centre as a parameter | the single node's rows |

All 11 single-table query forms in `compare.sql` give the single node's row
count and sum of ids.

**Fixed in 0.45:** a shard receives `skycell_region_scan(...)` already written
by the coordinator. Until 0.45 only the rewrite of `CONTAINS` on the same
server made that call the custom scan, so on a worker a polygon, composite or
MOC was answered by a sequential scan of the whole shard. Its support function
now makes the same decision when it meets the call written.

## What is still to do

- **Choosing shards.** Every query above goes to all 8 shards. Distributing by
  a coarse cell and adding that cell's `= ANY(...)` to a region's query would
  send a cone to the 1–4 shards that hold it.
- **Aggregate pushdown.** A real state type for `skycell_moc_agg`, so that each
  shard aggregates its own rows.
- **Estimates.** `skycell_count_estimate` and its relatives should sum over the
  shards, as they already sum over a partitioned table's partitions.
- **Cross-match of two large catalogues.** Both sharded by the same coarse
  cell, with margin copies of the rows near each cell's edge.
