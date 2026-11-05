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

## Choosing shards: a shard key (0.45)

Distributed by row id, every query goes to every shard. Distributed instead by
a coarse HEALPix cell, kept true by a CHECK constraint the planner reads,

```sql
CREATE TABLE cat (id int, ra float8, dec float8,
                  hpx3 bigint NOT NULL CHECK (hpx3 = skycell_ang2cell(ra, dec) >> 52));
SELECT create_distributed_table('cat', 'hpx3', shard_count := 32);
INSERT INTO cat SELECT id, ra, dec, skycell_ang2cell(ra, dec) >> 52 FROM ...;
CREATE INDEX ON cat (skycell_ang2cell(ra, dec));
```

a constant cone, polygon, box, composite or MOC on the table gets
`hpx3 = ANY('{the order-3 cells it meets}')`, and Citus sends the query only to
the shards holding those cells. The shift is `2 × (29 − order)`: 52 for order
3, 48 for order 5. Citus cannot distribute by a generated column, so the key
is filled when rows are loaded; the constraint rejects a wrong one, and only a
validated constraint counts. No key is added for a region meeting more than
1,024 cells, or for one that isn't constant (a cross-match's per-row cone).

[`shardkey.sql`](shardkey.sql): the same queries on the catalogue distributed
by id over 8 shards and by its order-5 or order-3 cell over 32. The shards
each query goes to:

| query | by id (of 8) | order-5 key (of 32) | order-3 key (of 32) |
|---|---|---|---|
| cone 0.1° | 8 | 1 | 1 |
| cone 1° | 8 | 4 | 1 |
| cone 5° | 8 | 21 | 6 |
| cone 20° | 8 | 32 | 22 |
| cone 3° at the pole | 8 | 19 | 4 |
| polygon (15° × 14°) | 8 | 26 | 5 |
| composite (annulus 3°–6°) | 8 | 21 | 6 |
| MOC (11 order-3 cells and more) | 8 | 32 | 8 |
| `distance(...) < 1.5` | 8 | 7 | 2 |

Every answer is the single node's. A key's cells should be larger than the
typical query region: each cell hashes to its own shard, so a region meeting
many cells meets many shards. Order 3 (768 cells of about 7°) suits cones up to
a few degrees; order 5 (cells of about 1.8°) suits arcminute queries, or a
table with many more shards than 32.

Building this found two bugs, both fixed in 0.45:
- since 0.44, a written `skycell_cone(...)` with another condition on a table
  range-partitioned by cell sent the planner round an endless loop, until the
  server ran out of memory;
- the shard key's cells were corrupted for `distance(ra, dec, ra0, dec0) < r`,
  which then found no rows.

## What is still to do

- **Aggregate pushdown.** A real state type for `skycell_moc_agg`, so that each
  shard aggregates its own rows.
- **Estimates.** `skycell_count_estimate` and its relatives should sum over the
  shards, as they already sum over a partitioned table's partitions.
- **Cross-match of two large catalogues.** Both sharded by the same coarse
  cell, with margin copies of the rows near each cell's edge.
