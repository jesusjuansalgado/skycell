# Open problem: k-nearest-neighbour search on a clustered, unordered table

*A self-contained description for a second opinion: what the search does, where its time goes, what has been tried, and what is wanted.*

> **Status (0.52): resolved.** The best-first walk over the cell hierarchy suggested in the second opinion (the "better use of the index" question below) is implemented, together with two fixes the profile of it found: the projection no longer computes the distance a second time, and sorting finite distances skips the NULL/NaN checks. Against pgSphere on this table: 0.94 at k = 100 and 0.95 at k = 1000; on Gaia 0.53 and 0.61. See [`README.md`](README.md) ("Best-first, 0.52"). The description below is kept as it was written, for 0.51.

## Context

`skycell` is a PostgreSQL extension (C, PostgreSQL 16–18) that indexes sky positions with an ordinary **B-tree on a 64-bit HEALPix NESTED cell id at order 29**, computed from `(ra, dec)` in degrees:

- the index is an expression index, `CREATE INDEX ON t (skycell_ang2cell(ra, dec))`;
- an order-29 cell is about 0.4 milliarcseconds across;
- NESTED numbering means a cell at order k is a contiguous range of order-29 ids, so a region of sky is a set of id ranges.

A k-nearest-neighbour query is

```sql
SELECT ... FROM t ORDER BY skycell_dist(ra, dec, ra0, dec0) LIMIT k;
```

The answer must be exact: the same rows, in the same order, as PostgreSQL sorting the whole table by the distance expression, ties aside. `skycell_dist` is the great-circle distance in degrees:
- it converts both positions to unit vectors (`sincos` of RA and of Dec), with the centre's vector cached;
- it returns `atan2(|a × b|, a · b)`.

It is answered by a **custom scan node (`SkycellNearest`)** that searches in widening rings:

1. **First ring.** Its radius r is chosen so the ring should hold about k rows, from a density model. That model is the ANALYZE histogram of the cell index: the average density of the finest HEALPix cell round the centre that still holds at least 4k rows.
2. **Covering.** The ring's cone is covered by a cost-based set of cell-id ranges: cells inside, plus cells crossing the edge, at a mix of orders chosen to trade index ranges against false positives.
3. **Reading.** On a heap that is not in cell order, the ranges' TIDs go into a TID bitmap, which is read through PostgreSQL's bitmap heap access; on a heap in cell order, they're read by an ordered index scan.
4. **Filtering.** For every row read, the scan evaluates the exact distance and keeps the row if d ≤ r and d > (the previous ring's radius). Kept rows are copied.
5. **Ordering.** The kept rows are sorted by distance and returned in order. As of the latest change, only the rows the query is planned to read are sorted; the rest are sorted only if asked for.
6. **Growing.** If more rows are needed, the next ring is a wider cone that skips the cells wholly inside the previous ring, and is filtered on d > previous radius.

The comparison is **pgSphere's GiST index with KNN ordering** (`ORDER BY pos <-> spoint(...)`): a distance-ordered best-first walk of the GiST tree that fetches roughly k heap rows.

## Where skycell wins, and where it loses

The ratio is skycell's time over pgSphere's; under 1 means skycell is faster. These are medians over 30 centres, each query planned and run anew at a fresh centre, on a warm cache (shared_buffers 2 GB, so everything is in memory).

| table | k = 1 | k = 10 | k = 100 | k = 1000 |
|---|---|---|---|---|
| 10M real Gaia DR3 positions, heap in cell order | 0.56 | 0.48 | 0.57 | 0.67 |
| 0.5M ObsCore-like table, clustered, heap unordered | 1.05 | 1.00 | **1.16–1.20** | **1.43–1.48** |

**The problem:** on the second table, skycell is 16–20% slower than pgSphere at k = 100 and about 45% slower at k = 1000. In absolute terms, at k = 1000 skycell takes about 800 µs and pgSphere about 550 µs. Planning costs the same for both (about 35 µs); the difference is all execution.

### The table

- 499,968 rows, 40 MB heap; cell index 11 MB, GiST 30 MB.
- About 3,900 "fields": each field has 128 observations scattered uniformly within ±0.2° of the field centre in both RA and Dec.
- Fields are scattered over the sky. Every position is distinct.
- The heap is stored in collection-then-field order, not sky order: the correlation of the cell index with heap order is 0.009, so effectively random.
- Query centres are near field centres, so the k nearest rows sit in a dense patch, with empty sky around it.

### Where skycell's time goes at k = 1000

Measured by sampling backend stacks with gdb, about 280 samples:

| share | what |
|---|---|
| ~28% | exact distance per row read: `sincos` ×2 and `atan2`, through PostgreSQL's expression evaluator |
| ~20% | building the TID bitmap from the index ranges, and fetching heap tuples through it |
| ~18% | computing the ring's cell covering (cost-based cell classification) |
| ~10% | sorting (13% before the partial-sort change) |
| rest | tuple copies, executor overhead |

### How many rows are read

Per row returned, medians over 30 centres:

| k | rows read (in covering cells) | of which inside the ring | rings used |
|---|---|---|---|
| 10 | 12.8 | — | 1.0 |
| 100 | 3.6–3.9 | 1.7 | 1.0 |
| 1000 | 2.2–2.4 | 1.65 | 1.4 (a third need a second ring) |

So there are two kinds of waste:
- the ring is too wide, holding about 1.65–1.7 times the rows needed;
- the covering is coarse: only 55–70% of the rows read lie inside the ring at all.

Every row read pays the full exact-distance cost. pgSphere reads about k rows.

## What has been tried, and the result

1. **Testing each index entry's key (its order-29 cell) before the heap fetch**, and cutting the first ring to the nearest keys. Rows read fell to 1.2–1.6 per row returned, but it was 12–15% slower. Decoding a key into a position costs trigonometry too, and on an in-memory table a skipped heap fetch saves little.
2. **Sizing the first ring differently**, and **different ring-growth rules:** no net gain. A tighter first ring causes second rings, each costing a covering and an index pass.
3. **Finer ring coverings** (a lower price per index range, so fewer false positives): slower at every k, because the covering costs more than the rows it saves.
4. **Coarser ring coverings:** slower, because more rows are read.
5. **Shrinking the first ring before reading it**, using the count of index entries its covering's bitmap holds. When the count was more than 1.8–4 times k, the radius was shrunk in proportion to the area, then the covering was recomputed and the bitmap rebuilt. It cut rows read at k = 100 from 3.9 to 2.6 per row, but the discarded bitmaps and extra coverings (1.5 per query) cost as much as they saved: neutral to 10% slower.
6. **Kept:** sorting only the rows wanted (quickselect, then sort the prefix), and rejecting rows by declination before the exact distance (|dec − dec0| > r means the row is outside the ring). Together they gave 7% at k = 100 and about 2% at k = 1000 against the previous release.

## Constraints

- **Exact results:** the same rows and order as `ORDER BY skycell_dist(...)`. Approximations are only acceptable as filters that can never drop a row that belongs.
- **The index stays a plain B-tree on the cell id.** That is the extension's design: one index serves cones, polygons, cross-matches, partitions and MOCs. A second index type isn't an option for this. Extra columns in the B-tree (a covering index) are possible but would be an operational change for users.
- **Implementation:** a PostgreSQL custom scan in C. Available tools are index scans, TID bitmaps, heap fetches, and the extension's own HEALPix code (cell ↔ vector, cell classification against cones).
- **Workloads:** tables can be in cell order (fast already) or not (the problem case). Any change must not slow down the Gaia-like case, which is currently about twice as fast as pgSphere.

## The question

How could a ring-based (or other) kNN search over a B-tree of HEALPix cell ids close most of this gap at k = 100–1000 on a clustered, unordered heap, while staying exact? In particular:

- **A cheaper exact or bounding distance per row.** Is there an ordering-preserving surrogate cheaper than `sincos` ×2 + `atan2`, with an exact fallback near the cut? Or a way to compute exact distances only for rows near the k-th distance?
- **Better use of the index.** For example, a best-first walk over cells, with lower bounds from each cell's geometry, that reads heap rows only from cells that can still hold one of the k nearest. In effect this would emulate GiST's KNN with the B-tree's cell hierarchy.
- **Better first-ring sizing** on clustered data, from information available cheaply at query time.
- **Anything structural** that explains why a best-first index-ordered search beats ring-then-sort here, and how to get its behaviour within these constraints.

Concrete algorithms with their expected cost per query (rows read, distance evaluations, index probes) are most useful.
