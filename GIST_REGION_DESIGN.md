# A GiST opclass for skyregion: design notes

Branch: `claude/zealous-cerf-mkda2j`. Status: a working, correctness-verified
opclass for one strategy (`&&`, region-region `INTERSECTS`), tuned once past
the initial spike (Quadratic split + a value-cached `consistent`) to close
most of the gap to pgSphere's native opclass. Still not exercised under
concurrent writes, and `<@`/`@>` aren't wired up. See "Spike scope" and
"Performance" below for exactly what is and isn't built and measured.

## Why this, and why now

PR #6 added a documented recipe (`skycell_region_moc_ranges`, see
`skycell--0.9.sql`) for region-region `INTERSECTS`: explode both sides of a
join into MOC-derived `[lo,hi]` cell-id ranges and join on PostgreSQL's
built-in `int8range` GiST opclass. That closed most of a real gap (skycell
had no indexed answer at all for `a.region && b.region`) but it's a manual
recipe -- a hand-maintained side table and a hand-written join, the same
status as the older point-in-footprint MOC recipe. The natural next question,
which that PR's own text flagged as follow-up: could `skyregion` carry a real
GiST opclass of its own, so `a.region && b.region` with a plain
`CREATE INDEX ON b USING gist (region)` is index-backed with no side table
and no rewritten query at all -- the way pgSphere's `scircle`/`spoly` already
work? This document and the code alongside it answer that empirically.

## The key structural fact: skycell already computes everything a bounding
## cap needs

A GiST opclass needs a cheap, fixed-size *key* to bound an arbitrarily large
or complex indexed value -- the same role `box2df` plays for PostGIS
geometries, or `spherekey` for pg_sphere's own region types. For a region on
a sphere, the natural choice is a **bounding spherical cap**: a centre
(unit vector) and an angular radius. Two caps overlap iff the angle between
their centres is at most the sum of their radii -- exactly
`sc_region_overlaps`'s own cone-cone formula in `cover.c`, lifted to bound
*any* region kind uniformly.

Building that cap needs no new geometry:

- **Cone**: it already *is* one. `center`/`radius` are already fields on
  `sc_region`.
- **Polygon**: `sc_region_centroid()` (already public, used for ADQL
  `CENTROID`) gives a centre; the existing (previously `static`) helper
  `region_farthest()` gives the farthest vertex from a point. Called with the
  centroid as that point, it's exactly the enclosing radius -- not the
  *minimal* enclosing cap (that would need real min-enclosing-circle
  computation), but a valid one, computed by recombining two functions that
  already existed for other reasons.

This is now `sc_region_bounding_cap()` in `cover.c` (12 lines), the only
addition to the pure-math, PostgreSQL-independent side of the codebase.
Everything else lives in a new file, `ext/src/gist_region.c`, which is
ordinary GiST glue: compress/decompress/union/penalty/picksplit/same/
consistent, storage type plain `bytea` (a packed `{cx,cy,cz,radius}` struct --
no custom SQL type needed, the same choice pg_sphere itself made for its
`spherekey`, which is `bytea`-shaped in its own union/picksplit argument
declarations even though its *storage* type is nominally distinct).

Only one strategy is registered: `&&(skyregion,skyregion)`, strategy number 1.
`consistent` computes the query's own bounding cap on the fly (via
`skycell_region_from_datum` + `sc_region_bounding_cap`, exactly what
`intersects()` itself does to parse a region) and tests cap-overlap;
`recheck` is always `true`, since the cap test is a real over-approximation
(both for the polygon case above, and inherently for `&&` itself, the same
way it is for the MOC-ranges recipe or any bounding-volume index) -- GiST's
own recheck mechanism then runs the actual `intersects()` predicate
automatically, with no manual join needed to do it.

## A real bug, and what it cost to find

The first working version of `picksplit` crashed the server (`SIGSEGV`)
building an index over as few as ~160 rows -- enough to force one page
split. The cause: **`GistEntryVector`'s indexing convention differs between
`union` and `picksplit`.** For `union`, `vector[0..n-1]` are all real entries
(standard C, 0-based). For `picksplit`, `vector[0]` is a reserved slot and
real entries start at `vector[FirstOffsetNumber]` (`FirstOffsetNumber == 1`)
through `vector[n-1]` -- because `GIST_SPLITVEC.spl_left`/`spl_right` store
`OffsetNumber`s referencing tuple positions on an actual page, and
`picksplit`'s input indexing is kept consistent with that 1-based addressing
throughout, not just in the output. The first version treated `vector[0]` as
real leaf data (it's uninitialised) and crashed dereferencing it as a bytea
pointer inside `bytea_to_cap`.

Diagnosed the hard way, since a crashed backend leaves no queryable state:
added `elog(LOG, ...)` tracing (NOTICE is not reliable once the connection
that would receive it dies mid-crash) at the top of every loop iteration in
`union` and `picksplit`, reduced the reproduction to a plain 3000-row circle
table, and read the server log for the last line printed before "terminated
by signal 11" -- which was the split-count log line itself, with *zero*
per-entry lines after it, pinning the fault to the very first
`vector[0]` access. Fixed by switching `picksplit`'s loop bounds and indices
to `OffsetNumber`/`FirstOffsetNumber`/`maxoff = entryvec->n - 1` throughout,
matching the convention `spl_left`/`spl_right` already required. (A second,
initially-plausible theory -- a 4-byte-header alignment fault in the `bytea`
packing -- was ruled out first, by switching to `memcpy` and `VARDATA_ANY`;
neither actually the cause, but both are still correct, defensive choices
worth keeping regardless.)

This is the single most important finding from the first pass, arguably more
than the performance numbers below: **a subtly wrong GiST opclass doesn't
fail loudly or return wrong rows -- it crashes the server**, and only past a
row count large enough to force a page split, which a small smoke test
easily misses (the initial 5-row and 300-row tests both built and queried
correctly; the bug only showed up at ~3000 rows). Any future work on this
opclass, or any new one, needs a correctness test large enough to force
multiple splits before it can be trusted at all.

## A second bug, from the performance pass, and a sharper lesson

Closing the performance gap to pgSphere (see "Performance" below) started
with an obvious win: `consistent()` is called once per index entry visited
during a scan, and for one probe row's scan that's easily dozens of calls,
all with the *same* query region -- yet the first version re-parsed that
region and recomputed its bounding cap from scratch on every single call.
Cached it in `fn_extra`, keyed on the query `Datum`'s own pointer, the same
shape `skycell.c`'s existing cone/poly/histogram caches already use.

The correctness suite caught this immediately: the self-join test that had
been 33,361/33,361 dropped to 3/64 matches on a smaller fixture used while
iterating -- 61 real matches silently missing, zero spurious ones. The
pointer-identity cache is unsound specifically *because* this opclass's main
use case is a join: PostgreSQL gives each outer row's evaluation a fresh
per-tuple memory context that gets reset and reused for the next row, so two
genuinely *different* rows' region values can land at the exact same
address. The cache took a matching pointer as "same value as last time, skip
recomputing" and silently kept serving a stale cap from an unrelated earlier
row. That is not a survivable kind of wrong: a false "no overlap" from
`consistent()` prunes a subtree outright, with no recheck downstream to
catch it -- unlike a false *positive*, which the real `intersects()` recheck
still filters out correctly. Zero false positives, en masse false negatives,
is exactly what a stale-cache-causing-false-pruning bug looks like, and nailed
down the cause immediately once framed that way.

Fixed by keying the cache on the query's actual *bytes* (a `memcmp` against a
copy held in `fn_extra`) instead of the Datum's pointer -- still a real win
within one scan (the same region's bytes genuinely repeat across all the
calls consistent() makes while walking one probe row's tree), but immune to
address reuse across different rows, since it compares content, not
identity. Re-ran the full 2923-row/33,361-pair correctness check clean
afterward. **The general lesson, sharper than "test at a scale that forces a
split": for a GiST opclass whose whole point is serving joins, `fn_extra`
caching keyed on Datum pointer identity is unsound by construction, not just
occasionally unlucky** -- the fresh-per-tuple-context/address-reuse pattern
this hits is exactly how PostgreSQL evaluates a join's inner index scan for
every outer row, not an edge case.

## Picksplit, round two: R*-tree split, and what it actually bought

The Quadratic split above closed most of the gap to pgSphere on a 200-probe
x 5000-footprint benchmark (~1.2-1.4x, from 2-3x). The natural next question
was whether PostgreSQL's own `box` opclass's actual split algorithm --
R*-tree-style margin/overlap search, not Quadratic -- would close the rest
of it. Implemented it (see "Spike scope" above for the algorithm), verified
correctness clean at the same 2923-row/33,361-pair scale, and re-ran the
200x5000 benchmark: **~30-32ms, statistically indistinguishable from
Quadratic split's ~31-34ms.** On this benchmark, a materially more
sophisticated, textbook-correct split algorithm bought nothing measurable
over the simpler one it replaced -- a real, honest negative result, not a
failed attempt to reproduce a known win.

That raised the obvious next question: is 5000 rows too small to see a split
algorithm's quality matter at all (a shallow tree has little for a good
split to fix), and does the picture change at real scale? Rebuilt `fpr` at
50,000 rows (10x) with 500 probes (2.5x) and re-measured all three
approaches on the identical query shape:

| approach | 200 x 5,000 | 500 x 50,000 (25x the pair-work) | scaling |
|---|---|---|---|
| pgSphere native `&&` | ~25-27 ms | ~88-89 ms | ~3.5x for 25x work |
| MOC-ranges recipe (#6) | ~74-75 ms | ~270-288 ms | ~3.8x for 25x work |
| **this opclass** | ~31-34 ms | **~466-477 ms** | **~14-15x for 25x work** |

Both scopes agree exactly with brute force (1282/1282 matches at the larger
scale, 0 false positives/negatives) -- this is a real performance finding,
not a correctness regression hiding as one. But it is not a good one: **the
gap to pgSphere widens sharply with table size, from ~1.3x at 5000 rows to
~5.3x at 50,000**, and the MOC-ranges recipe -- the "worse" interface this
opclass was meant to replace -- actually *scales better* than it does,
ending up faster in absolute terms at 50,000 rows despite starting more than
2x slower at 5000. `EXPLAIN (ANALYZE, BUFFERS)` on the 50,000-row case shows
why it isn't a recheck problem: `Rows Removed by Index Recheck: 0` -- every
candidate the index hands to the heap is a genuine match -- yet the scan
still touches ~89 buffers per probe to find ~2.6 matches on average. The
tree itself is being walked far more than it should be; a correctness-clean
index can still be a slow one if too many internal and leaf pages have
overlapping bounding caps for reasons no split algorithm fixes after the
fact.

**The likely real cause, and why another split algorithm won't fix it:**
a single bounding cap per region is a coarser key than either pgSphere's
native per-shape predicates or the MOC-ranges recipe's finer-grained cell
decomposition (several `[lo,hi]` ranges per region, not one blob). Once
enough regions of comparable, moderate size accumulate -- exactly what a
5000-to-50,000-row footprint table does -- their caps start overlapping each
other pervasively regardless of how cleverly they're grouped, because the
*key itself*, not the tree built over it, can no longer discriminate between
them. R*-tree split minimises overlap **given the caps as they are**; it has
no lever over the caps being an inherently blunter representation than the
alternatives at this scale. Closing this would need a sharper key (e.g. more
than one cap per region, or a HEALPix-cell-based key resembling the MOC
approach this opclass was meant to replace with something simpler) --
a materially bigger change than a split-algorithm swap, and arguably
undermines the "simpler than the recipe" case for building this opclass at
all.

`picksplit` was kept as the R*-tree-style version regardless of the flat
result on the small benchmark: it is not worse than Quadratic split on any
measurement here, it is a sounder, better-precedented algorithm (the same
shape PostgreSQL's own `box` opclass uses), and it costs less to compute
(O(n log n) vs O(n^2) per split, a real win for index *build* time even
where it didn't move query time).

## Spike scope

Built and measured:

- `compress`, `decompress`, `union`, `penalty`, `same`, `consistent` (`&&`
  only), and `picksplit` (see below, and "Picksplit, round two" further down
  for why it went through three versions).
- `picksplit` is now an **R*-tree-style split** (Beckmann et al. 1990),
  adapted from axis-aligned boxes to spherical caps: try sorting the entries
  by their cap centre's x, y, and z coordinate in turn (three candidate
  "axes" -- a cap has no natural per-axis bounding box the way a box does, so
  the centre's coordinate stands in for one), pick whichever axis has the
  smallest total margin (cap radius, as a size proxy) summed over every valid
  split point along it, then along that axis pick the split point that
  minimises *overlap* between the resulting two caps (not just their combined
  size) -- the same two-phase ChooseSplitAxis/ChooseSplitIndex structure
  PostgreSQL's own `box` opclass uses, with a cap-shaped overlap and margin
  proxy standing in for a box's exact ones. O(n log n): sort once per axis,
  then a single forward and backward cumulative-union sweep, versus the
  O(n^2) seed search the Quadratic split it replaced needed.
- `consistent()` caches the query region's bounding cap across repeated calls
  within one scan (see the bug writeup above for why this has to be a
  value-based cache, not a pointer-based one).
- `cap_union2`'s general-position formula (place the merged cap's centre
  along the geodesic between the two input centres, radius =
  `(d + r1 + r2) / 2`) has a documented, un-exercised weak spot: near-
  antipodal centres (`d` approaching `pi`). A single region's own cap cannot
  trigger this (`skyregion` already forbids a cone at or beyond a hemisphere,
  and a polygon spanning one), but repeated unions of far-apart small regions
  climbing an internal tree node in principle could. Not hit in testing at
  the scale tried here; flagged rather than fixed.

Not built: `<@`/`@>` strategies (point-in-region or full containment via the
same opclass -- likely a small addition once `&&` is trusted, since the cap
math is identical, only the strategy dispatch in `consistent` would grow),
and concurrent-insert/VACUUM stress testing (only single-threaded
`CREATE INDEX` and read-only querying were exercised).

## Correctness

A synthetic 2923-row table (mixed circles and polygons, deliberately
including clusters near both poles and straddling RA = 0/360, sizes spanning
~2 orders of magnitude) built cleanly after the picksplit fix. A full
self-join (`a.region && b.region`, ~4.3M candidate pairs after the count
filter) against the same table's own unindexed `intersects()` gave:

| indexed matches | brute-force matches | false positives | false negatives |
|---|---|---|---|
| 33,361 | 33,361 | 0 | 0 |

Separately, on this project's own 5000-row mixed footprint table (`fpr`,
2500 circle + 2500 polygon, from `bench/20_region_xmatch.sql`) against 200
probes, the GiST-indexed join and the unindexed exact predicate agreed
exactly (203 matches each) -- the same reference count `bench/21_region_
overlap.sql`'s MOC-ranges recipe and pgSphere's native `&&` already agree on.

## Performance

200-probe x 5000-footprint join (`bench/22_region_gist.sql` vs
`bench/21_region_overlap.sql`, same probe construction, same seed), across
every version built in this investigation:

| approach | first spike (Linear split) | + Quadratic split + cached consistent | + R*-tree-style split | setup needed | matches |
|---|---|---|---|---|---|
| unindexed (`intersects()`) | ~1.4-1.8 s | -- | -- | none | 203 |
| MOC-ranges recipe (#6) | ~74-75 ms | -- | -- | side table + hand-written join | 203 |
| **this opclass** | ~80-83 ms | ~31-34 ms | **~30-32 ms** | `CREATE INDEX ... USING gist (region)` | 203 |
| pgSphere native `&&` | ~27-42 ms | ~25-27 ms | ~25-27 ms | none (native types) | 203 |

At this scale: the Quadratic-split-plus-caching pass roughly **2.5x'd**
query speed and closed the gap to pgSphere from 2-3x down to ~1.2-1.4x; the
further R*-tree-style split moved nothing measurable on top of that. See
"Picksplit, round two" above for what happened when the same three
approaches were re-measured at 10x the table size and 2.5x the probes: the
gap to pgSphere *widens* to ~5.3x, and even the MOC-ranges recipe pulls
ahead of this opclass in absolute terms -- the honest headline result of
this investigation, not the flat 5000-row numbers above. `EXPLAIN` confirms
the join plans as `Nested Loop -> Index Scan using <idx> on fpr, Index Cond:
(s_region && p.s_region)` at both scales, a genuine per-row (non-constant)
indexed nested loop -- no special-casing was needed for "the query side
isn't a literal", which is precisely the point of building a real opclass
instead of a recipe, even where its performance doesn't yet earn that out.
Index build stayed cheap throughout: ~205ms for 5000 rows even with the
costlier Quadratic split.

## Where this leaves the decision

The core hypothesis -- `skyregion` can carry a real, standards-shaped GiST
opclass, built almost entirely from pieces the extension already had
(`sc_region_centroid`, the newly-exposed `region_farthest`, and
`sc_region_overlaps`'s own overlap formula) -- still holds, and the
correctness bar is solid: two real, well-understood bugs found and fixed
along the way, both re-verified against a 2923-row/33,361-pair stress test
each time. But the performance verdict has to lead with the larger-scale
result, not the smaller one this investigation started with: **at 5000
rows this opclass looks competitive with pgSphere; at 50,000 rows it is 5x
slower, and even the manual MOC-ranges recipe beats it.** Two split
algorithms (Quadratic, then R*-tree-style) gave one real win (2.5x, from
Quadratic) and one null result (R*-tree-style, on top of Quadratic) --
enough evidence that a *third* split algorithm is not the next thing worth
trying. The bottleneck this points to is the bounding-cap key itself being
too coarse once enough same-scale regions accumulate, which is a
representation question, not a tree-balancing one -- closing it for real
would mean a sharper per-region key (something closer to the MOC-ranges
recipe's multiple cell ranges, or an entirely different embedding), which
is a bigger redesign than this branch's remaining scope, not a tuning pass.
Recommendation: this opclass is a solid answer to "can `&&` be indexed
directly, with a correct, working implementation and two genuinely
instructive bugs to show for it" and not yet a good answer to "should this
replace the MOC-ranges recipe" -- that call should wait on whether closing
the at-scale gap is worth a real key redesign, not on further picksplit
tuning.
