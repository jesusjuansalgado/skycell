# Performance

*The measurements behind the README's figures, with how each was made and, where a change moved them, how they moved. The benchmark scripts and raw results are under [`bench/`](../bench/).*


Against Q3C 2.0.5 and pgSphere 1.5.2 on PostgreSQL 18.6, with skycell's
defaults throughout (ratio below 1 = skycell faster):

| workload | skycell ÷ pgSphere | skycell ÷ Q3C |
|---|---|---|
| cone searches, 1″–3°, warm | 0.66–0.95 | 0.02–0.58 |
| cone searches, 1″–3°, cold | 0.40–0.98; level below 6′ on real positions | |
| cross-matching, 1″–10″ | 0.26–0.33 | 0.73–0.84 |
| convex polygons, 0.05°–2° | 0.21–0.23 | |
| non-convex polygons (U, L, comb, star), 0.05°–2°, real Gaia DR3 positions | 0.13–0.29 | |
| nearest neighbours, k = 10–1000, real Gaia DR3 positions | 0.48–0.67 | |
| nearest neighbours beyond a distance (10 beyond 2°), real Gaia DR3 positions | 0.25 (since 0.45) | |
| stored regions: overlap and region-in-region (`&&`, `@>`, `<@` between regions) | 0.12–0.70 | |
| stored regions: footprints that contain a position (`@>` a point) | 0.57 on x86; 0.75–0.93 on an Apple M3 with 0.6 | |

The stored-region rows were re-measured on the x86 cloud host with skycell 0.34,
0.35 and 0.36, 8 runs each ([`bench/results-regions/`](../bench/results-regions/); 0.36 as
`main` at b3bb7be, the same library code). Overlap and containment between regions kept their
margins there (0.11, 0.55, 0.22), and the v0.35 GiST key changed none of them.
Point-in-footprint was within 5% of pgSphere with 0.34 and 0.35 (0.98 and
0.95): the index converted the query point to a unit vector at every entry it
visited. Since 0.36 the point is converted once per index scan, and it takes
0.57 (interval 0.55–0.60) of pgSphere's time.

Its index is a third the size of pgSphere's (214 MB against 687 MB at 10M
rows, the size of Q3C's) and builds about 23× faster (3.0 s against 70.2 s),
and its row estimates stay within 1.4× of the truth where pgSphere's are off
by up to 7.1× and Q3C's by up to 2.6×. At Gaia
DR3 scale that is one 38 GiB index where an archive running pgSphere for cones
and Q3C for cross-matching keeps 160 GiB.

**Partitioned catalogues** (0.44, SQL 0.40, [`bench/results-partition/`](../bench/results-partition/)).
5M real Gaia DR3 positions, once in one table and once in 16 partitions by cell
range (as by HEALPix index or Gaia `source_id`), cross-matched with 10³–10⁵
probes at 1″–30″. A cross-match is a nested loop that rescans the partitioned
table, and so every partition, for each probe. Two changes cut that down:

- Each partition's scan tests a cone against its index's lowest and highest
  key before covering it, so a partition that cannot hold the cone answers in
  about 0.3 µs.
- On a catalogue partitioned by its cell, the planner adds the cone's cell
  interval next to it (`skycell_cone_cell_lo/hi`), and PostgreSQL prunes at
  every probe the partitions it cannot reach. Only one partition runs.

Partitioned time over unpartitioned, both tables matching the same rows:

| probes, radius | 0.43 | key test | with pruning | unpartitioned ms |
|---|---|---|---|---|
| 10³, 5″ | 4.14 | 1.85 | 1.44 | 8.5 |
| 10⁴, 5″ | 6.34 | 2.39 | 1.74 | 47.0 |
| 10⁵, 1″ | 8.11 | 2.37 | 1.51 | 387.7 |
| 10⁵, 30″ | 5.06 | 1.77 | 1.21 | 759.1 |

The 1.2–1.7 left is PostgreSQL's own work at each rescan: evaluating the bounds,
finding the partition, restarting its scan, about 2 µs a probe. A constant cone
on such a catalogue is pruned at plan time to the partitions it touches.
Since 0.45 constant polygons and composites are pruned at plan time too, by
the cells of their bounding circle; a `NOT` region, which bounds nothing, is
not.

Partitioning brings no parallel gain of its own. A parallel cross-match
shares out its probes, each worker rescanning the partitioned table as above:
100k probes at 5″ took 175 ms with three workers against 146 ms unpartitioned.
A wide cone gets a Parallel Append over the partitions it reaches, but the
unpartitioned table's parallel cone scan (0.42) is faster still: a 10° cone
took 49 ms against 36, a 30° cone 89 against 73. Partition a sky catalogue to
load, index, vacuum or archive it by region, not for speed: cones cost the
same, cross-matches 1.2–1.7 times as much.

**Citus distributed tables** (0.45, [`bench/citus/`](../bench/citus/)). On Citus
13 with a catalogue hash-distributed over 8 shards, every cone, polygon,
composite and MOC form tried gives the single node's rows. So do nearest
neighbours and cross-matches against a reference or local probe table. Each
shard plans its part with skycell and uses the custom scan on its own index.
Distributed by a coarse cell column kept true by a CHECK constraint, e.g.
`hpx3 bigint CHECK (hpx3 = skycell_ang2cell(ra, dec) >> 52)`, a constant
region gets `hpx3 = ANY('{its cells}')`, and Citus sends it only to the shards
holding them: a 1° cone to 1 shard of 32, a 5° cone to 6.
`skycell_moc_agg` (and `skycell_coverage_moc`) aggregates on the shards. The
estimate functions sum over the shards. skycell's settings must be the same
on every node: a plain `SET` on the coordinator does not reach them. Not yet:
two distributed catalogues cannot be cross-matched.

**Polygons through the custom scan** (0.43, [`bench/results-polyscan/`](../bench/results-polyscan/)).
Since 0.43 a constant polygon or box takes the custom scan that cones have.
Before that it was rewritten into an `OR` of index ranges plus a per-row test.
The benchmark ran 60 centres on the 10M real Gaia DR3 positions, 0.05°–2°
across, with three shapes: a convex quadrilateral, a box and a non-convex U.
Each was timed five ways in random order, planning included:

- `skycell_poly` through the custom scan;
- the same query with `skycell.custom_scan` off (the 0.42 rewrite);
- ADQL `CONTAINS(POINT, POLYGON)`;
- pgSphere's `<@ spoly`;
- Q3C's `q3c_poly_query` (convex shapes only).

All counted the same rows. Summed time over the 0.42 rewrite's, with a 95%
bootstrap interval:

| shape | scan ÷ rewrite | scan ÷ pgSphere | scan ÷ Q3C |
|---|---|---|---|
| quadrilateral | 0.93 [0.89, 0.97] | 0.05 | 0.11 |
| box | 0.90 [0.87, 0.92] | 0.09 | 0.07 |
| U | 0.93 [0.89, 0.96] | 0.03 | — |

Per query the gain is modest, because a polygon's covering was already tight.
It grows with size: a 10° × 10° box (376k rows) takes 58 ms against the
rewrite's 113 ms. A large polygon is also split among workers like a cone
(0.42): with two workers, 22.8 ms against the rewrite's parallel bitmap scan at
43.7 ms ([`parallel.md`](../bench/results-polyscan/parallel.md)). The two
spellings cost the same. In this benchmark, which runs five methods in turn on
each polygon, ADQL came out about 10% ahead below 0.5°; timed on their own, on
the same centres and sizes, `skycell_poly` was the faster by 5%.

**Composite regions** (0.39, [`bench/results-composite/`](../bench/results-composite/)).
Five shapes on 60 centres on the 10M real Gaia DR3 positions, 0.05°–2° across.
Each is written three ways: one `REGION`, the same pieces as a boolean of ADQL
`CONTAINS` predicates in skycell, and pgSphere's boolean of `<@`. pgSphere has
no composite region type. Its MOC type, `smoc`, has union and intersection, but
it is approximate to its cells and no index serves `spoint <@ smoc`, so the
boolean is how a query gets exact answers there. All three spellings count the
same rows. Median ms per query, and the `REGION`'s summed time over the others':

| shape | `REGION` | skycell boolean | pgSphere boolean | `REGION` ÷ skycell boolean | `REGION` ÷ pgSphere |
|---|---|---|---|---|---|
| annulus (circle less circle) | 0.11 | 0.13 | 0.11 | 0.84 | 0.89 |
| two circles, union | 0.10 | 0.42 | 0.11 | 0.23 | 0.92 |
| lens (intersection of two circles) | 0.07 | 0.10 | 0.10 | 0.71 | 0.45 |
| square less a circle | 0.13 | 0.21 | 0.36 | 0.61 | 0.14 |
| five circles, union | 0.10 | 0.67 | 0.20 | 0.17 | 0.62 |

A composite is covered from its operands' own coverings: the union of theirs
for a `UNION`, their intersection for an `INTERSECTION`, the first operand's
for a `DIFFERENCE` that takes away at most half of it. A circle operand then
takes the cone's direct path. A `NOT`, and a difference with a large hole, are
covered by classifying cells against the whole composite, which keeps a thin
ring from reading its hole.

Since 0.39 a constant composite written as a top-level condition is answered
by the custom scan, as a circle is: the planner keeps one call,
`skycell_region_scan(cell, ra, dec, region)`, covers the composite once (cached
for its estimate and its path), and the scan reads the ranges in cell order,
testing each row from its `ra` and `dec`.
In 0.38 the covering became an `OR` of range conditions that the planner costed
one bitmap index scan at a time, and each row built a `point` for its test.
That took the annulus from 1.33 to 0.89 of pgSphere's time and the union of two
circles from 1.39 to 0.92; every shape is now faster than both boolean
spellings. Under an `OR`, or with `skycell.custom_scan` off, a composite keeps
the range rewrite.

Before 0.38, an ADQL `OR` of `CONTAINS` predicates was a sequential scan in
skycell: 2.4 s for two circles and 4.9 s for five on these 10M rows. It is now
indexed like `<@ OR <@`, which is the skycell boolean column above.

**Stored composite footprints** ([`bench/results-footprint/`](../bench/results-footprint/)).
An ObsCore-like table of 50,000 observations whose `s_region` holds each
footprint as it is: NIRCam-like (two modules of 2×2 detectors with gaps),
UVIS-like (two CCDs either side of a gap), coordinated parallels (two fields 7′
apart) and coronagraphs (a field less its mask), half over the whole sky and
half in twenty deep fields. skycell keeps one `skyregion` per observation
under its default GiST. pgSphere, with no composite type, keeps one `spoly` per
detector and one `scircle` per mask, each under its GiST, and a query returns
the distinct observations less those whose mask holds the whole field (or the
position), which for these shapes gives the same rows. Both answered the same
observations for all 900 queries. Median ms per query, and skycell's summed
time over pgSphere's:

| workload | skycell | pgSphere | skycell ÷ pgSphere |
|---|---|---|---|
| observations meeting a search polygon (1′–20′) | 0.20 | 1.40 | 0.04 |
| observations containing a position | 0.13 | 0.27 | 0.70 |

The overlap margin is widest in the deep fields, where pgSphere tests every
detector polygon of every candidate observation (1,500 of them for one 20′
field) and skycell one footprint each. skycell's index is 2.9 MB against
pgSphere's 10.1 MB for the detectors and 0.7 MB for the masks.

**Parallel cross-matching.** The comparisons above, like the paper's, are
serial. They run with `max_parallel_workers_per_gather = 0` for every method.
With parallelism on, a cross-match with enough probes runs in parallel
workers. The setup:

- PostgreSQL 18.6 with its default settings, 4 cores.
- 5M catalogue rows, the heap stored once in cell order and once at random.
- 200k and 1M probes on sources.
- Median of 3 warm runs, against v0.35, which cannot run a cross-match in
  parallel. Every result is identical to the serial one.

| probes | cone | serial | default workers | 3 workers |
|---|---|---|---|---|
| 200k (10 MB) | 1″–30″ | 1.4–18.2 s | 1 worker: 1.8–2.2× faster | — |
| 1M (50 MB) | 1″, cell order | 8.4 s | 2 workers: 2.9× | 3.7× |
| 1M | 30″, cell order | 20.9 s | 3.3× | 3.9× |
| 1M | 1″, random order | 6.9 s | 2.7× | 3.6× |
| 1M | 30″, random order | 92.0 s | 3.4× | 4.6× |

"3 workers" sets the probe table's `parallel_workers`, so the leader and three
workers use the four cores. Q3C's and pgSphere's cross-matches can run under a
parallel nested loop too. They were not measured that way, so these figures
compare skycell with itself only.

**Parallel cone searches.** A single cone with many rows behind it is split
among processes since 0.42. On `gaia_realc` (10M Gaia DR3 positions, heap in
cell order, 4 cores), cones centred on the Galactic centre, best of 5 warm runs,
counts identical to the serial ones:

| search | rows | serial | 2 workers allowed | 4 workers allowed |
|---|---|---|---|---|
| 1° cone | 2.8k | 0.6 ms | planned serial | planned serial |
| 3° | 90k | 14.2 ms | 1 worker: 11.0 ms | 1 worker: 13.1 ms |
| 5° | 294k | 45.5 ms | 2 workers: 20.9 ms | 2 workers: 20.7 ms |
| 10° | 1.29M | 189.5 ms | 2 workers: 71.2 ms | 3 workers: 71.9 ms |
| 15° | 2.20M | 286.8 ms | 2 workers: 136.8 ms | 3 workers: 123.9 ms |
| annulus 5°–10° (`DIFFERENCE`) | 1.00M | 209.9 ms | 2 workers: 95.9 ms | 3 workers: 124.8 ms |

PostgreSQL picks the workers by the heap pages the search is expected to read,
as it would for a parallel scan of that many pages, so small cones, which are
the common case, keep their serial plan and their planning cost. Away from the
Galactic plane the same 15° cone holds 33k rows and stays serial.

The benchmark scripts behind the paper describing skycell, and the
documentation of every measurement it quotes, are in [`bench/`](../bench/):
[`bench/README.md`](../bench/README.md) maps each table to its directory, and
[`bench/REPRODUCING.md`](../bench/REPRODUCING.md) gives the hosts, settings and run
order. The paper itself, the raw per-query results, the runs of earlier versions
and the design notebook (`GIST_REGION_DESIGN.md`) that the "Round …" references
in the source comments point to are in
[skycell-paper](https://github.com/jesusjuansalgado/skycell-paper).

---

