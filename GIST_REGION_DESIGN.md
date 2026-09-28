# A GiST opclass for skyregion: design notes

Branch: `claude/zealous-cerf-mkda2j`. Status: a working, correctness-verified
opclass covering two strategies (`&&`, region-region `INTERSECTS`, and `@>`/
`<@`, region-contains-point), five design passes in: past the initial spike
(Quadratic split + a value-cached `consistent`), past a split-algorithm pass
that plateaued (R*-tree-style split), through a multi-cap key redesign that
closed `&&`'s at-scale gap to pgSphere and beat it outright at smaller
scale (round three), a second strategy built on that same key with no new
geometry needed (round four), a rejected tuning idea recorded rather than
silently dropped (`MAX_SUBCAPS=8`, worse at every scale), and a sub-cap-aware
picksplit that shaved a further ~10-15% off index pages visited per probe
(round five). Region-region full containment (`skyregion @> skyregion`, as
opposed to point containment) is still unbuilt, and it's still not exercised
under concurrent writes. See "Round three" through "Round five" for the
current design and numbers, and "Spike scope"/"Performance" for the full
history.

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

## Round three: a multi-cap key, and it actually closes the gap

Round two's diagnosis pointed at the key, not the split algorithm: a single
bounding cap per region can't discriminate between enough same-scale,
overlapping regions once a footprint table gets large, and no split
algorithm can fix that after the fact. The fix tried here: give each region
up to **four** bounding caps instead of one, reusing machinery the extension
already had rather than inventing new geometry.

**The key.** `GistMultiCap = { overall: GistCap; sub[4]: GistCap }`. A cone
still gets exactly one sub-cap (itself, exact -- no approximation loss for
the shape that was already exact). A polygon is decomposed via the
extension's own MOC builder (`moc_for_region`, previously `static` in
`skycell.c`, now exposed through `skycell_internal.h` for `gist_region.c` to
call directly) into up to four HEALPix cells at order <= 20, each converted
to a `(center, radius)` cap via the existing `sc_pix2vec`/`sc_pixrad`/
`sc_nuniq_decode` functions. `overall` is a plain union of the sub-caps,
computed and stored only as a cheap summary for `picksplit`'s axis-sort
heuristic and as a fast reject in `consistent()` -- every real overlap test
in `consistent()` walks the sub-cap lists (`multicap_overlaps`), never just
`overall` alone, since collapsing back to one cap there would throw away
exactly the precision this redesign is for.

**Merging many regions' sub-caps down to four.** `union()` (merging a whole
page's worth of entries into a parent key) and `penalty()` (merging two)
both need to reduce an arbitrary flat list of caps to at most four. Used a
greedy nearest-cluster-absorption pass (`merge_caps_greedy`): seed with the
first four caps, then fold each remaining cap into whichever of the four
running clusters it would waste the least area merging into. This is
O(n * 4), not a globally-optimal clustering -- deliberately cheap, since
both functions run on every index build/insert, not just at query time.

**`picksplit`** keeps the R*-tree-style axis-sort/margin/overlap structure
from round two. Entries are still *ordered* along a candidate axis by each
entry's `.overall` cap centre coordinate (sub-caps have no single natural
per-axis coordinate the way one cap's centre does, so this stays a cheap,
single-number sort key). As of round five, though, the split *cost itself*
-- both the per-axis margin sum used to choose an axis, and the per-point
overlap used to choose where to cut on it -- is computed from the *full*
multi-cap union of each side (`multicap_union2`, `multicap_overlap_amount`),
not just the overall caps; round three/four's version used the overall cap
for the cost too, reserving the full multi-cap union only for the two
output keys (`spl_ldatum`/`spl_rdatum`). Either way, the sharper sub-cap
structure propagates into internal nodes, not just leaves, which is
precisely what round two's `EXPLAIN (ANALYZE, BUFFERS)` finding (excess
internal-page traversal) called for -- round five's change is *how much*
of the split process gets to see that structure, not whether it does at
all. See "Round five" above for the measured effect (~10-15% fewer buffer
visits) of extending it to the cost functions too.

**Results, same two scales as round two, same query shape:**

| approach | 200 x 5,000 | 500 x 50,000 (25x the pair-work) |
|---|---|---|
| pgSphere native `&&` | ~25-27 ms | ~89-93 ms |
| MOC-ranges recipe (#6) | ~72-75 ms | ~252-284 ms |
| this opclass, round two (single cap) | ~30-34 ms | ~466-477 ms |
| **this opclass, round three (multi-cap)** | **~6.8-6.9 ms** | **~106-115 ms** |

Both scopes agree exactly with brute force at both scales (203/203 matches
at 5000 rows; 1282/1282 at 50,000 rows, 0 false positives/negatives).
Correctness was also re-checked against a fresh 2923-row mixed circle/
polygon self-join stress test (clusters near both poles and RA=0/360,
sizes spanning ~2 orders of magnitude, same shape as the earlier one but
regenerated): 104,215/104,215 matches, 0 false positives, 0 false
negatives.

This is the headline result of the whole investigation: the multi-cap key
doesn't just close the at-scale gap round two found, it makes this opclass
**faster than pgSphere's native operator** at the smaller scale (~3.7x) and
**close to it** at the larger one (~1.2x slower, down from ~5.3x slower with
the single-cap key), while beating the MOC-ranges recipe at both scales by
a wide margin. The scaling ratio itself (6.8ms -> ~110ms is still a steeper
25x-work-for-~16x-time curve than pgSphere's own ~3.5x) shows the multi-cap
key hasn't changed the fundamental shape of the scaling curve -- it has
just moved the whole curve down far enough that it no longer matters at
either scale tested. Whether it still matters at 500,000+ rows is untested;
the same MAX_SUBCAPS=4 cap could in principle need to grow for far larger
or far more oddly-shaped polygon-heavy tables, but there is no evidence yet
that it does.

## Round four: a second strategy, region @> point ("contains")

Round three closed the at-scale gap for `&&`. The natural next question:
does the same multi-cap key generalize to containment -- specifically
`skyregion @> skypos` (its commutator `skypos <@ skyregion` is the more
common spelling), the "which stored footprint(s) cover this sky position"
query? This is a genuinely different use case from `20_region_xmatch.sql`'s
existing point-in-footprint recipe, not a competitor to it: that recipe
needs the *point* side to be the large, cell-indexed table (its planner
rewrite turns the join into B-tree ranges over that table's own per-row
cell id, found from its indexes -- it has nothing to do with an index on
the region column, and only works in that one direction). Asking "which of
these thousands of stored regions contains this one point" is the opposite
direction, and nothing in skycell answered it efficiently before this: the
honest existing baseline is a plain sequential scan filtering with
`skycell_in_region()`.

**The design change is small, as expected.** A point can only be inside a
region if it's inside at least one of the region's sub-caps -- true by the
same covering guarantee `&&`'s pruning already relies on (`moc_for_region`'s
decomposition always covers the region, never leaves gaps) -- so the
pruning test is `multicap_contains_point()`: treat the query point as a
zero-radius cap and reuse the exact same `cap_overlaps()` check `&&` already
had, with the same overall-cap short-circuit reject first. No new geometry,
matching the "likely a small addition" prediction from round three's file
header. The one real wrinkle, caught immediately by testing rather than
found the hard way: `consistent()` unconditionally read the query argument
as a `bytea` (`DatumGetByteaP` + `VARSIZE`) *before* dispatching on strategy
number, left over from when `&&` was the only strategy. For the new
strategy the query is a `skypos` -- a fixed-size by-reference struct with no
varlena header at all -- so that unconditional read was misinterpreting raw
struct bytes as a (possibly toasted) varlena, which surfaced immediately as
`ERROR: compressed lz4 data is corrupt` the moment a correctness test
actually exercised strategy 2. Fixed by moving that read inside the `&&`
case, where it belongs. Caught before it ever reached a benchmark number,
unlike the two bugs in earlier rounds -- the value of writing the
correctness test *before* trusting any performance result held again here.

Registration mirrors exactly how pgSphere's own `scircle_ops` opclass
registers `scircle @> spoint` alongside `scircle && scircle` in one GiST
opclass with cross-type strategies -- `OPERATOR 2 @> (skyregion, skypos)`
added to `skyregion_gist_ops`, no new opclass needed. PostgreSQL's own
commutator resolution means both spellings, `f.region @> point(...)` and
`point(...) <@ f.region`, plan through the index automatically.

**Correctness.** A fresh 2923-row mixed circle/polygon table against 4000
query points: 59,342/59,342 matches against `skycell_in_region()`, 0 false
positives, 0 false negatives. Re-confirmed at both benchmark scales below
(202/202/202 at 5,000 rows/200 probes; 538/538/538 at 50,000/500).

**Performance**, same probe construction as `&&`'s benchmarks, brute force
(`skycell_in_region()`, seq scan) as the honest baseline since no indexed
alternative existed in this direction before:

| approach | 200 probes x 5,000 regions | 500 probes x 50,000 regions |
|---|---|---|
| brute force (`skycell_in_region`) | ~630-670 ms | ~16.2-17.0 s |
| pgSphere native `<@` | ~2.7-2.9 ms | ~17.6-19.1 ms |
| **this opclass, `<@`/`@>`** | **~5.7-5.9 ms** | **~99-104 ms** |

Against the only baseline that actually existed for this direction, this is
an unambiguous win: **~110x faster than brute force at 5,000 rows, ~160x at
50,000** -- exactly the "index vs. no index" gap indexing is supposed to
deliver, and the gap *widens* with scale, the right direction. Against
pgSphere, though, the relative gap widens too, from ~2x at 5,000 rows to
~5.5x at 50,000 -- worth being precise about why, because it is not a
repeat of round two's problem: `EXPLAIN (ANALYZE, BUFFERS)` on the 50,000-row
case shows ~33,600 buffers for 500 probes (~67 per probe to find ~1 match
each, `Rows Removed by Index Recheck: 0`), and this opclass's own *absolute*
time here (~100ms) is essentially the same as `&&`'s at the identical scale
(~110ms, from "Round three") -- the tree-traversal cost per probe hasn't
gotten worse for a point query, it's the same bottleneck round two already
diagnosed, still not fully closed by the multi-cap key at 50,000+ rows. What
changed is the *baseline*: pgSphere's own native point-in-shape test is
cheaper than its native shape-shape overlap test (exact containment is
simpler geometry than exact overlap), so pgSphere's `<@` benefits more from
being a native, exact primitive than its `&&` did -- while this opclass's
sub-cap test (an angle-and-radius comparison) costs about the same whether
the "other side" is a point or an extended shape, so it doesn't get the same
proportional speedup pgSphere's `<@` gets over pgSphere's `&&`. The
multi-cap key's remaining headroom (from "Round three": still not proven
past 50,000 rows) is the same headroom limiting this strategy too, not a
new, separate problem.

**Operational gotcha, not a design flaw:** verifying this against the
project's long-lived `skytest` benchmark database directly hit the exact
kind of catalog-versioning trap this file warns about elsewhere -- `skytest`
already had skycell installed at version 0.10 from *before* this change, so
editing `skycell--0.10.sql`/`skycell--0.9--0.10.sql` in place (correct,
since 0.10 is this whole PR's unreleased target version) did not reach that
already-created installation; `ALTER EXTENSION ... UPDATE` has nothing to
do since the version string didn't change, and `pg_amop` confirmed the new
operator was simply absent from the live catalog. The fix for an
already-open database is a real extension reinstall (drop and recreate,
which cascades through any `skyregion`/`skypos`-typed columns and needs
rebuilding), not something resolvable by re-running SQL files in place;
this benchmark's numbers above come from a database that ran a fresh
`CREATE EXTENSION` against the already-updated files instead, which needs
no such step. Noted in `bench/23_region_contains.sql`'s own header for
whoever hits this next.

**A faster alternative exists for this exact query, and -- at the time this
note was first written -- was deliberately not recommended.** The
MOC-in-a-B-tree recipe already documented in `skycell--0.11.sql`
(`skycell_region_moc` + `skycell_ancestors`, back then a manually built and
maintained side table of MOC cells per stored region) answers "which
regions contain this point" too. Run head-to-head against this opclass on
the identical `fpr` fixture and probe points: ~2.5-4.8ms against this
opclass's ~4.9-5.3ms at 5,000 rows/200 probes, and ~20-30ms against
~86-89ms at 50,000/500 -- roughly 2x faster at the smaller scale and 3-4x
at the larger one, closing most of the gap to pgSphere's native `<@`
(~2.5-2.6ms and ~12-14ms respectively) that this opclass does not.
Correctness matches exactly at both scales. At the time, this was not
written up as a benchmark script or mentioned in README.md: it needed a
hand-maintained side table and a hand-written join, exactly what the
`skyregion` GiST opclass exists to make unnecessary, so recommending it for
one specific query shape would have undercut the thing this whole file is
trying to build.

That objection is gone as of "Round eight" below: the same array this
recipe stored by hand in a side table is exactly what `skycell_region_moc()`
already returns, and PostgreSQL's own built-in GIN opclass for arrays
indexes it directly -- no side table, no custom opclass, and (round eight's
own numbers) faster still than this manual recipe's timings above. What was
a deliberately-unrecommended aside is now the documented, automatic,
README-recommended path; this note stays for the history (the manual
recipe was the thing that pointed at what to automate) rather than as
current guidance.

## Tried and rejected: MAX_SUBCAPS=8

Round four's `@>` numbers showed the same "too many pages visited" signature
as round two's original diagnosis (~65 buffers per probe at 50,000 rows,
`Rows Removed by Index Recheck: 0`), which raised the obvious question:
since more sub-caps per region is exactly the lever that fixed `&&`'s
scaling in round three, would doubling it again (4 -> 8) help further,
for both strategies at once since they share one index?

Tested directly, both strategies, both scales, same session and script so
the comparison is apples-to-apples:

| MAX_SUBCAPS | index size (50k rows) | `&&` @ 5k/50k | `@>` @ 5k/50k | buffers/probe (`@>`, 50k) |
|---|---|---|---|---|
| 4 (current) | 16 MB | ~6.8-7.0 / ~83-91 ms | ~5.4-5.9 / ~94 ms | ~65.5 |
| 8 | 28 MB | ~11.1-11.5 / ~147-151 ms | ~7.8 / ~130-134 ms | ~83.4 |

**Worse, not better, on every measurement, at both scales, for both
strategies** -- roughly 1.4-1.6x slower, with a ~1.75x bigger index and
*more* buffer visits per probe, not fewer. Correctness stayed clean at
MAX_SUBCAPS=8 (exact match against brute force at both scales), so this
isn't a bug -- it's a real cost/benefit finding: doubling the sub-cap count
roughly doubles the key's storage cost (fewer entries fit per index page,
so fanout drops and the tree gets taller/wider), and the extra pruning
precision doesn't pay that back at these scales -- if anything, the higher
buffer count suggests the fanout loss dominates outright rather than being
merely offset. Reverted; MAX_SUBCAPS stays 4. Left as a documented dead end
rather than silently not-tried, the same way "round two: R*-tree split
moved nothing" was recorded rather than omitted -- a negative result here
is exactly why round three's redesign (a coarser change: restructuring
*what* the key is, not just how many of the existing kind) was the right
lever and turning the existing dial further was not.

## Round five: a sub-cap-aware picksplit -- a real, modest win

MAX_SUBCAPS=8 was one answer to "picksplit's buffer trace still shows the
same too-many-pages-visited pattern round two diagnosed -- what else is
there to try": make the *existing* key sharper. The other answer, tried
here: picksplit already has the sharper multi-cap data sitting right there
in `mc[]` and only uses each entry's *overall* cap (a single summary cap) to
decide the split itself -- the full sub-cap sets are used only afterward, to
build the two output keys (`spl_ldatum`/`spl_rdatum`), a round-three design
choice made explicitly to keep the split search cheap. Does using the full
multi-cap data for the split *decision itself* -- both which axis to split
on, and where to cut on it -- do better than the overall-cap proxy?

**The change.** Two new helpers, both reusing existing machinery rather than
inventing new geometry: `multicap_union2()` (a thin wrapper over
`multicap_union_many()` for exactly two multi-caps, so the running
sweep can fold one more entry in at a time) and `multicap_overlap_amount()`
(the sum of `cap_overlap_amount()` over every pairwise sub-cap combination
between two multi-caps -- zero when no sub-cap pair overlaps at all).
`picksplit`'s forward/backward cumulative-union sweep, previously tracking a
single running `(centre, radius)` cap per side, now tracks a running full
`GistMultiCap` per side instead, folding in one more entry's *entire*
sub-cap set per step. Each fold is `O(MAX_SUBCAPS^2)` (a small constant, 16
comparisons at MAX_SUBCAPS=4), so the whole sweep stays `O(n)` per axis --
same complexity class as the overall-cap-only version it replaces, not the
`O(n^2)` the discarded Quadratic split needed. The per-axis margin sum (used
to *choose* an axis) switched from summed cap radius to summed multi-cap
area, since a multi-cap union has no single linear "extent" the way one
cap's radius did; the per-split-point cost (used to *choose where* to cut)
switched from single-cap overlap to `multicap_overlap_amount()`. A pleasant
side effect: since the winning split's `fwdMC[bestM-1]`/`bwdMC[bestM]` are
already the exact full multi-cap union of everything assigned to each side,
they're used directly as the output keys -- no separate final
`multicap_union_many()` pass needed, simplifying the code slightly on top of
whatever performance changed.

**Correctness**, same fresh stress test as every prior round: 104,215/104,215
for `&&`, 55,929/55,929 for `<@` (a different point count than round four's
59,342 -- a freshly regenerated probe set, not a regression), 0 false
positives/negatives on both.

**Performance.** Wall-clock timing at these query sizes (single-digit to
low-triple-digit milliseconds) turned out too noisy on this machine to trust
directly -- repeated runs of the *identical* query against the *identical,
unchanged* index varied by 20-30ms in isolation, larger than the effect
being measured. `EXPLAIN (ANALYZE, BUFFERS)`'s buffer count is not subject
to that noise (it is deterministic for a given index and query), so it is
the number trusted here; wall-clock medians over 6-8 repeated runs each are
reported alongside it as corroborating, not as the primary evidence:

| | buffers/probe, `&&` (50k) | buffers/probe, `<@` (50k) | buffers/probe, `&&` (5k) | buffers/probe, `<@` (5k) |
|---|---|---|---|---|
| overall-cap-only picksplit (round three/four) | 67.1 | 62.9 | 12.2 | 11.3 |
| **sub-cap-aware picksplit (round five)** | **58.5** | **53.9** | **10.9** | **10.0** |

**~11-15% fewer index pages visited per probe, at both scales, for both
strategies** -- a real, structural improvement, not noise, since it shows up
identically in a deterministic metric measured on unchanged data with only
the split algorithm's own code swapped out (verified with a controlled
A/B: same seed, same generated tables, only the installed `.so` differed
between runs). Wall-clock medians over 6-8 runs each moved the same
direction at a roughly similar magnitude (e.g. ~82ms -> ~76ms median for
`&&` at 50k rows, ~97ms -> ~84ms median for `<@`), consistent with, though
noisier than, the buffer-count finding.

**Kept.** Same complexity class as what it replaces, no correctness
regression, a genuine (if modest, ~10-15%, not the 2-3x round three's key
redesign delivered) reduction in the exact metric round two's original
diagnosis flagged as the bottleneck, and it happens to simplify picksplit's
own code besides. This is the "yes, and by how much" answer to the question
left open in round three/four's own file header about whether picksplit
could be made sub-cap aware too -- worth doing, not a game-changer on its
own, and not a substitute for whatever it would take to close the remaining
gap to pgSphere at 50,000+ rows (still open, per round three's and round
four's own honest scaling caveats).

## Round six: a third strategy, region @> region ("wholly contains"), and a
## real bug it found along the way

README.md and the paper both described `skyregion @> skyregion` as "not yet
indexed, evaluated by sequential scan". Round four indexed `region @> point`
by reusing `&&`'s own sub-cap-overlap pruning test unchanged (a point is
just a zero-radius region for that purpose); the same argument extends to
`region @> region` in principle -- "A contains B" (B nonempty) implies "A
and B overlap", and that implication survives replacing both sides with
their (superset) sub-cap covers, so a provable non-overlap between A's cover
and B's cover is still a sound proof that A does not contain B. **The
design change is again small**: `GIST_REGION_STRATEGY_CONTAINS_REGION` is a
second case label on the exact same `consistent()` branch `&&` already uses,
computing the identical `multicap_overlaps()` test, differing only in
strategy number and (implicitly) which exact operator recheck falls back
to. No new geometry, no new sub-cap decomposition, no new query cache.

This is a deliberate choice, not a placeholder for a tighter test written
later. A tighter one is temptingly easy to imagine -- "every sub-cap of B
sits inside some single sub-cap of A" -- but that is a *sufficient*
condition for containment, not a *necessary* one, so it cannot be used to
*prune*: a query region straddling two adjacent sub-caps of a row that
genuinely contains it would be wrongly rejected by that test, a false
negative with no recheck to catch it (unlike a spurious candidate, which
recheck always filters). The overlap-reuse test has no such failure mode.
What it costs is selectivity: `region @> region` prunes exactly as sharply
as `&&` does and no sharper, so a row merely touching the query region
reaches recheck alongside a row that actually contains it.

**A direction wrinkle, found while designing the benchmark, not a bug in
the opclass itself:** `@>(skyregion,skyregion)` has no registered
`COMMUTATOR` (there is no `<@(skyregion,skyregion)` operator at all), so
unlike round four's point strategy -- where PostgreSQL's own commutator
resolution lets *either* spelling, `f.region @> point(...)` or `point(...)
<@ f.region`, use the same index -- only the direction that puts the
indexed table's region on `@>`'s left argument can use this index. `probe
@> footprint` (indexed table on the right) plans as `Nested Loop -> Seq
Scan` on both sides; `footprint @> probe` (indexed table on the left) plans
as the intended `Nested Loop -> Index Scan using fpr_region_gist, Index
Cond: (s_region @> p.s_region)`. Both are real predicates asking different
questions ("which of my candidate containers contain this probe" vs "does
this one region contain each of these candidates"), not two spellings of
the same one, so this is not a bug to fix so much as a scope boundary to
document: a `<@(skyregion,skyregion)` operator plus a fourth strategy would
be needed to index the other direction, and nothing here builds one.

**A real, previously-undiscovered bug, found verifying the exact test this
strategy's recheck relies on:** `skycell_region_covers` (the function
behind `@>`) had its argument order backwards since the extension's very
first commit. It called the same primitive ADQL `CONTAINS(a, b)` correctly
uses ("is every point of a also in b"), so `a @> b` computed "a is inside
b" -- the reverse of what `@>` and its own documentation have always said.
A big circle genuinely containing a small one, `skycell_region_covers(big,
small)`, returned `false`; the reversed call returned `true`. The only
prior regression test for this function, `skycell_region_covers(p, p)`,
compares a region to itself, where argument order cannot matter -- exactly
why this survived undetected across every prior round of this file. Fixed
by swapping the arguments passed into `sc_region_contains_region` inside
`skycell_region_covers` specifically; `skycell_contains_region` (ADQL's own
`CONTAINS(a, b)`, which is supposed to use that "a inside b" convention) was
untouched. This opclass's own correctness is unaffected either way -- GiST
recheck always calls the real operator function, so indexed and sequential
results agreed before and after the fix -- but every containment answer
this database ever gave through `@>` before this fix was backwards for any
non-symmetric pair.

**Correctness.** A mixed circle/polygon table with deliberately nested
regions (a small circle exactly inside a larger one, and rows spanning
three orders of magnitude in size) against `skycell_region_covers` (post-
fix) directly: 0 mismatches, with genuine positive matches (not merely
agreement on an empty result -- `region_covers_bruteforce`'s own count was
539/539 nonzero in one such run). `ext/test/sql/skycell.sql`'s regression
suite gained both the asymmetric direction test and a self-join `@>`
correctness check requiring `gist_region_covers_join_has_positives`, so a
future change cannot silently regress back to a vacuous "always agrees on
zero" test.

**Performance** (`bench/24_region_contains_region.sql`; probes are tiny
regions nested near a sample of `fpr`'s own footprints, in the one
direction the index can serve -- see the direction wrinkle above),
`skycell_region_covers` (post-fix) as the honest sequential baseline:

| approach | 200 probes x 5,000 footprints | 500 probes x 50,000 footprints |
|---|---|---|
| brute force (`skycell_region_covers`) | ~1.14-1.16 s | ~28.4-28.9 s |
| pgSphere native `~` | ~3.0-3.1 ms | ~14.7-14.8 ms |
| **this opclass, `@>` (region)** | **~4.6 ms** | **~72.2-72.6 ms** |

Both scales agree exactly with the brute-force baseline (206/206 at 5,000
rows/200 probes; 888/888 at 50,000/500). Against the only baseline that
existed for this direction, this is another unambiguous win: **~250x faster
than sequential scan at 5,000 rows, ~390x at 50,000**. Against pgSphere the
pattern from round four repeats, only more so: ~1.5x slower at 5,000 rows,
widening to ~4.9x at 50,000 -- a *wider* gap than round four's point
strategy saw at the same scale (~2x -> ~5.5x there, starting from a lower
baseline), consistent with this strategy reusing `&&`'s own (less
selective, for this predicate) pruning test rather than a dedicated one:
more candidates reach recheck here than would for a tighter, purpose-built
containment test, so there is more room for pgSphere's native, exact
primitive to pull ahead as the table grows. The qualitative lesson from
round four holds again -- an unambiguous win over the only prior
alternative (sequential scan), a real but scale-dependent gap to a mature,
native implementation -- without needing to re-derive it: this strategy's
whole design is "reuse round three's test, pay whatever selectivity that
costs", so inheriting round four's scaling shape rather than improving on
it is exactly what should have been expected going in.

## Round seven: a fourth strategy, region <@ region ("contained by"), and
## the other direction round six's own comment flagged

Round six's own "direction wrinkle" left something on the table:
`@>(skyregion,skyregion)` had no `COMMUTATOR`, so only `indexed_col @>
probe` -- "which of my candidates contain this one" -- could use the
index. The mirror question, "which of my candidates are wholly contained
*within* this one" (`indexed_col <@ probe`), is a different, real predicate
(not just a different spelling of the same one), and round six explicitly
deferred building it.

**The design change needs no new geometry at all.** The necessary condition
round six established -- "A contains B" (B nonempty) implies "A and B
overlap", surviving the swap to sub-cap covers -- holds regardless of which
side plays container and which plays contained, since "overlap" is
symmetric. So `GIST_REGION_STRATEGY_CONTAINED_BY_REGION` shares
`CONTAINS_REGION`'s entire `consistent()` case verbatim; the two differ only
in strategy number, which only matters for GiST recheck to know which exact
operator function to fall back to (`skycell_region_covers` vs the new
`skycell_region_covered_by`) -- machinery GiST already owns, not something
this file's code has to manage.

**The exact test was almost free too.** `skycell_region_covered_by(a, b)` --
"a is contained by b" -- is precisely `sc_region_contains_region(a, b)`'s own
convention ("every point of a is also in b"), the same one ADQL
`CONTAINS(a, b)` and `skycell_contains_region` already use correctly. Unlike
`skycell_region_covers` (round six's `@>`, which needed its arguments
swapped because `@>`'s convention is the opposite one), this new function
just calls the existing `region_region(fcinfo, true)` helper unchanged.

**Wiring in the commutator closes the loop properly**, not just for the new
operator: `CREATE OPERATOR <@ (... COMMUTATOR = @> ...)` triggers
PostgreSQL's documented forward-reference fixup and backfills the
*pre-existing* `@>(skyregion,skyregion)` operator's own missing commutator
link too (it has had none since skycell 0.1). Verified directly against
`pg_operator`: both operators' `oprcom` now point at each other. The
practical effect, mirroring how round four's point strategy already let
both `f.region @> point(...)` and `point(...) <@ f.region` use the same
index: `t.region <@ :probe` and the commutator spelling `:probe @>
t.region` now both plan through `fpr_region_gist`, confirmed by `EXPLAIN`.

**Correctness.** A self-join on the same nested/multi-scale fixture round
six's own stress test used: 0 mismatches against `skycell_region_covered_by`
directly, 1346/1346 genuine matches in one run (not a vacuous
always-empty agreement). `ext/test/sql/skycell.sql` gained a matching
`gist_region_covered_by_join_*` block, including a check that the
commutator spelling (`b.region @> a.region`) returns exactly the same count
as the direct one (`a.region <@ b.region`).

**Performance** (`bench/24_region_contains_region.sql`, extended with a
`covered_by` scope: probes are large regions nested around a sample of
`fpr`'s own footprints -- the mirror of round six's tiny probes, since this
direction wants candidates *smaller* than the query, not larger),
`skycell_region_covered_by` as the honest sequential baseline, pgSphere's
native `<@` as the mature-implementation comparison:

| approach | 200 probes x 5,000 footprints | 500 probes x 50,000 footprints |
|---|---|---|
| brute force (`skycell_region_covered_by`) | ~1.17-1.22 s | ~29.4 s |
| pgSphere native `<@` | ~12.4-12.9 ms | ~210-215 ms |
| **this opclass, `<@` (region)** | **~7.6-8.3 ms** | **~134-139 ms** |

Both scales agree exactly across all three methods (1250/1250/1250 at 5,000
rows/200 probes; 26,768/26,768/26,768 at 50,000/500 -- a much higher match
rate than `@>`'s scope, expected: the probes here are large enough to
contain many small footprints each, not just the one they're centred on).
Against sequential scan this is the same order of win as round six's:
**~150-160x faster at 5,000 rows, ~215-220x at 50,000**. Against pgSphere,
though, this strategy does something round six's `@>` scope never managed:
**it wins outright, at both scales** -- about 1.6x faster at 5,000 rows and
1.5x faster at 50,000, the gap *not* widening with scale the way `@>`'s
did. The likely reason is the baseline, not this opclass: pgSphere's own
`<@` for two extended shapes is exact shape-in-shape containment, not the
cheap point-in-shape test round four's comparison benefited from, so it
carries more of pgSphere's own overlap-test cost -- meanwhile this
strategy's sub-cap test costs the same as `@>`'s did, and it's already
competitive there. This is the first case in this file where the
"reuse round three's test" strategy doesn't just win against sequential
scan but against the native implementation too, without any tuning
specific to this direction.

## Round eight: an automatic GIN alternative for region @> point, and why
## it isn't a GiST opclass strategy at all

Round four's own gap to pgSphere (~2x at 5,000 rows, ~5.5x at 50,000) was
never closed by tuning `consistent()` further -- `EXPLAIN (ANALYZE,
BUFFERS)` there already showed the strategy's own absolute cost matching
`&&`'s at the same scale, so the ceiling is the multi-cap tree traversal
itself (round two's diagnosis, round five's picksplit only closing ~10-15%
of it), not this strategy's pruning precision. Separately, an aside added
to round six's own section after the fact noted a *manual* MOC-in-a-B-tree
recipe already beats this opclass at the same query by 2-4x -- but flagged
as a side-table recipe, deliberately not recommended, exactly the kind of
manual join `skyregion`'s own GiST opclass exists to make unnecessary.

The two findings point at the same fix: automate that recipe instead of
tuning the tree it was built to route around. `skycell_region_moc(region,
max_cells, max_order)` already gives every row a small `int8[]` of covering
cells (skycell--0.11.sql's "MOC-in-a-B-tree" comment block); PostgreSQL's
own built-in GIN opclass for arrays already indexes `&&`/`@>`/`<@` on any
array type, `int8[]` included, with no custom opclass needed at all. A
plain `CREATE INDEX ... USING gin (skycell_region_moc(region))` is
therefore already a real index over exactly the array the manual recipe's
side table stored one row at a time -- what was missing was the automatic
rewrite that lets `region_col @> point`/`point <@ region_col` reach it
without the user spelling out the array-overlap form themselves.

**The rewrite.** `region_support_simplify` (`ext/src/adql.c`) already had
exactly the right shape of gap to extend: its existing rewrite needs an
index on the *point* side (`density_for_expr`) and gives up entirely
(`return NULL`) when none exists. That gap is precisely "many regions, one
point, no point-side index" -- the case this round is for. Added: when
`density_for_expr` fails and the region argument is a column (not a
constant -- a literal region has no column to carry an index on),
`gin_moc_index_for_region()` (`ext/src/skycell.c`) checks for a GIN index
on `skycell_region_moc(that column, ...)`, mirroring `density_for_expr`'s
own var-extraction and `RelationGetIndexList` walk. On a match it returns
the index's actual stored expression (so the rewritten query cites it
verbatim, syntactically indexable) and the `max_order` that expression's
`skycell_region_moc()` call was built with. The rewrite itself is then
`moc_expr && skycell_ancestors(point_cell, 0, max_order) AND
skycell_in_region(point, region)` -- `array_overlap_expr()` resolves the
`&&` operator via `oper()`, not a plain OID lookup, since the catalog entry
is the polymorphic `anyarray && anyarray`, not one specific to `int8[]`
(an exact-match lookup silently finds nothing and the rewrite would never
fire; caught immediately by testing before trusting any EXPLAIN output).

**Why the order range is capped, not narrowed, and why that boundary was
drawn deliberately.** The obvious further win -- narrow `skycell_ancestors`'
own range to whatever *coarsest* order the data actually uses, not just
capping the finest -- was tried by hand first (see below) and rejected as
an automatic default: the coarse end depends on which regions exist in the
table, knowable only from a real scan or a statistics sample, and a sample
is not guaranteed to include a rare, unusually large region's actual
order. Guessing that bound wrong risks a false negative -- a genuine match
silently dropped, with no recheck able to catch it, unlike a spurious
candidate. So `gin_moc_index_for_region` only ever reads the `max_order`
argument the index's own `skycell_region_moc()` call was built with --
exact, not sampled, since no row's covering can use a finer cell than that
argument allows -- and leaves the coarse end at 0. A user who knows their
own corpus's size distribution can still get the tighter range by hand, by
passing a smaller `max_order` when creating the index; the rewrite reads
back whatever was actually asked for rather than assuming a default.

**Correctness.** 0 mismatches against `skycell_in_region` on 800 random
query points (single-table form) and on the join form both spellings
(`region @> point`, `point <@ region`) use, at 5,000 and 50,000 rows;
`ext/test/sql/skycell.sql` gained a dedicated block confirming the rewrite
fires for both spellings and that a user-supplied narrower `max_order` is
read back and used verbatim (checked via the plan text, not just
inferred). On the regression suite's own small fixture (~140 rows) the
rewritten form still computes the right answer but a sequential scan
legitimately beats either index there, the same scale caveat already noted
for other tests in that file -- proving the rewrite fires and is correct
does not require proving it wins at every scale.

**Performance** (`bench/23_region_contains.sql`'s own fixture and probes,
so directly comparable to round four's own numbers above), using the plain
`@>`/`<@` spelling -- no manual query rewriting, this is what the automatic
path actually produces:

| approach | 200 probes x 5,000 footprints | 500 probes x 50,000 footprints |
|---|---|---|
| brute force (`skycell_in_region`) | ~600-643 ms | ~15.2-15.7 s |
| this opclass, round four's `@>`/`<@` | ~5.2-6.3 ms | ~90-126 ms |
| pgSphere native `<@` | ~2.7-2.9 ms | ~12.8-12.9 ms |
| **this rewrite, default (`max_order` = index's own, here 29)** | **~4.2-4.8 ms** | **~15.1-17.2 ms** |
| **this rewrite, user-narrowed `max_order` (= 14, matching the data)** | **~2.4-3.8 ms** | **~10.9-12.9 ms** |

Even at the fully-automatic default (no tuning, whatever `max_order` the
index was created with) this beats round four's own GiST strategy by
**~1.3-1.5x at 5,000 rows and ~5.7-8x at 50,000** -- closing almost all of
round four's own gap to pgSphere at the smaller scale and most of it at
the larger one. With the one optional, user-supplied lever documented
above, it goes further and **matches or slightly beats pgSphere outright at
both scales** -- the second case in this file (after round seven) where an
alternative to the GiST opclass beats the native implementation, not just
sequential scan. Build cost and index size are comparable to the region
GiST index at the same scale (measured at 50,000 rows: ~3.7s/18MB for this
GIN index against ~2.25s/17MB for the GiST one) -- not the deciding factor
either way.

**Why this is not a fifth GiST strategy.** Every other round in this file
extended `skyregion_gist_ops` itself -- one opclass, one index, every
strategy sharing its consistent()/picksplit machinery. This one is a
different *kind* of index (GIN, not GiST) reached through a planner
rewrite (`SupportRequestSimplify`), the same mechanism the B-tree cell
rewrite has always used, applied here to a case that mechanism's existing
logic already gave up on. It coexists with the GiST opclass rather than
replacing it: a table with only a GiST index still gets round four's own
strategy exactly as before (this rewrite requires its own GIN index to
apply at all), and a table with both gets this rewrite instead, since it
unconditionally replaces the `@>`/`<@` clause once its own index exists --
deliberate, given the numbers above, not an oversight to fix later.

## Spike scope

Built and measured:

- `compress`, `decompress`, `union`, `penalty`, `same`, `consistent` (`&&`
  only), and `picksplit` (see below, "Picksplit, round two", and "Round
  three" further up for why the key and split went through three versions).
- The key is now `GistMultiCap` (up to 4 sub-caps per region plus a summary
  `overall` cap), not a single `GistCap` -- see "Round three" above for the
  full design. Everything below this point describes pieces that predate
  that redesign but are otherwise still accurate (the R*-tree split
  structure, the value-based query cache, the `cap_union2` weak spot, etc.)
  now operating on multi-cap keys instead of single caps.
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
(`sc_region_centroid`, the newly-exposed `region_farthest`, the existing MOC
builder, and `sc_region_overlaps`'s own overlap formula) -- holds, and now
the performance verdict is a genuinely good one, not a qualified one. Three
rounds of tuning: Quadratic split (2.5x win), R*-tree-style split (null
result on top of Quadratic, but confirmed the bottleneck was the key, not
the split algorithm), and a multi-cap key redesign (the real win) -- took
this opclass from "2-3x slower than pgSphere, competitive only at small
scale" to **faster than pgSphere at 5000 rows and within ~1.2x of it at
50,000**, while beating the MOC-ranges recipe -- the manual recipe this
opclass was meant to replace -- at both scales by 2.5-10x. The correctness
bar stayed solid throughout: two real, well-understood bugs found and fixed
in earlier rounds (the `picksplit` offset-convention crash, the pointer-
identity cache), a third design pass that introduced no new ones, and a
fresh 2923-row/104,215-pair stress test plus both benchmark scales agreeing
exactly with brute force after the multi-cap rewrite.

Recommendation: this opclass is now a good answer to both of the questions
this investigation opened with -- "can `&&` be indexed directly, correctly"
and "should this replace the MOC-ranges recipe" -- yes to both, on the
evidence gathered here. What's left before calling it non-experimental is
scope, not performance: concurrent-write behavior is untested, and the two
scales measured (5000, 50,000 rows) don't prove the multi-cap key's
advantage holds at 500,000+ rows or under a very different footprint-size
distribution than the ones benchmarked here.

(Written as of round three, before `@>` existed in either direction; rounds
four and six since added it for point and region respectively -- see those
sections for what that did and didn't close. The one direction still
genuinely unbuilt is `<@(skyregion,skyregion)`, i.e. indexing "which of my
candidate footprints are contained *within* this region" rather than
"contains it" -- round six's own "direction wrinkle" explains why that
needs its own operator and strategy, not just a different query spelling.)
