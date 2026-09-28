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

## Round nine: a variable-length key for the region GiST opclass -- reverted

*(Documented here for the record; this round predates the SP-GiST work
below and was carried out and reverted in the same session, without ever
landing on `main`.)*

Prompted by the observation that pgSphere's own per-type GiST keys
(`scircle`/`spoly`, ~32-40 bytes) are far smaller than this opclass's fixed
~164-byte `GistMultiCap`, tried a variable-length key: a `bytea` encoding
only as many sub-caps as a region actually has (a cone: 1; a polygon: up to
`MAX_SUBCAPS`), tagged with a small header so `multicap_to_bytea`/
`bytea_to_multicap` could round-trip it. Measured index size dropped
~35-47% at both 5,000 and 50,000 rows, and cold-cache `EXPLAIN (ANALYZE,
BUFFERS)` confirmed ~35% fewer buffer reads per probe (1674-1691 vs
2604-2612 at 50,000 rows) -- the fanout win was real.

Query time got *worse* anyway, at every scale and cache state tried
(warm: ~1.5-2x slower at 5,000 rows, ~1.5-2.2x at 50,000; cold, at the
user's explicit request to re-check before deciding: still 35-45% slower
despite the real drop in disk reads). The decode cost of a variable-length
record on every `consistent()` call outweighed the fanout it bought --
tree descent was never the dominant cost this key redesign assumed it was;
per-candidate CPU was. Reverted (`git revert d0789b5`, commit `9f02a61`)
rather than kept as a documented-but-off option, since a reverted commit's
diff is already the full record of what was tried.

A narrower, lower-risk slice of the same idea -- a fast path in
`multicap_overlaps()`/`multicap_contains_point()` recognising when a key has
collapsed to exactly one sub-cap (always true for a leaf circle) and
skipping the O(MAX_SUBCAPS^2) loop in that case -- was tried right after
(commit `7e0c6a2`) as a safe, format-unchanged alternative. Correct (full
regression suite plus a 3,000-row/~4.5M-pair stress test), but measured as
statistically indistinguishable from baseline at both scales: the fast path
only fires for candidates that already pass the cheap overall-cap check,
and internal nodes (built by `merge_caps_greedy` across many children)
essentially never collapse to a single cap, so the optimisation rarely
fires where the time is actually spent. Kept (it can never make anything
slower), but closed out as a dead end: three independent attempts in this
area now (round two's original picksplit tuning, this variable-length key,
and this fast path) point at the same wall -- the multi-cap GiST's gap to
pgSphere is a structural tree-depth/fanout limit, not a per-comparison-cost
one, and not one `consistent()`-level tuning can close further.

## Round ten: a different index scheme entirely -- SP-GiST over the HEALPix
## hierarchy, indexing the *point* side

Round nine's dead end reframed the question: instead of tuning
`skyregion`'s own GiST key further, is there a *different* index structure
that fits this data better than an R-tree-shaped GiST at all? Two candidate
directions, both explored:

### PostGIS: reused, not reinvented -- and not a win

The obvious "why build this at all" question first: PostGIS ships its own
SP-GiST opclasses (`spgist_geography_ops_nd` for `geography`, plus 2D/3D
variants for `geometry`), an octree-style partition over 3D Cartesian
coordinates, not HEALPix -- a real, mature, independent implementation, not
a strawman. Installed (`postgresql-16-postgis-3`) and benchmarked directly
against the same corpus this file's other rounds use:

**Cone search** (`bench/03_cone.sql`'s harness, an added `postgis` arm using
`ST_DWithin(pos, center, r, use_spheroid=false)`, radii 1" to 3deg, 50,000-row
catalogue):

| method | avg query time | vs pgSphere | index size (50,000 rows) |
|---|---|---|---|
| pgSphere (native GiST) | ~0.04-0.08 ms | 1x | 3.1 MB |
| skycell (B-tree + planner rewrite) | ~0.05-0.14 ms | ~1.2-1.8x | 1.1 MB |
| PostGIS geography (GiST) | ~1.2-1.4 ms | ~20-30x | 3.7 MB |
| Q3C | ~2.8-3.5 ms | ~50-70x | 1.1 MB |

PostGIS beats Q3C here but loses badly to both pgSphere and skycell, and its
index is the largest of the four. For the region-contains-point workload
(many footprints, mixed circle/polygon, probed by a point) the picture is
more nuanced and workload-sensitive in an instructive way:

- `ST_DWithin` cannot use an index at all when the distance argument is a
  per-row column rather than a query-side constant (PostGIS's planner
  support function needs a fixed radius to precompute an expanded bounding
  box) -- exactly this project's own "many circles, one probe point, each
  circle its own radius" shape. Naively indexed, PostGIS's circle side falls
  back to a full sequential scan; the fix is to bake each circle's radius
  into an actual buffered polygon at build time (`ST_Buffer`), which does
  get indexed, at the cost of ~6.35s of one-time buffering for 5,000 rows and
  losing exactness (a finite-segment polygon approximates a true circle).
- With that fix, PostGIS's **GiST** on the buffered polygons (~17-24ms at
  5,000 footprints x 500 probes) is close to this project's own region GiST
  opclass (~14-15ms) -- genuinely competitive.
- PostGIS's **SP-GiST**, on the *same* buffered-polygon data, was
  consistently and substantially *slower* than its own GiST (~400-435ms vs
  ~17-24ms) -- a real, measured result, not a configuration mistake (same
  operator, same data, index type as the only variable): SP-GiST's
  octree partition does not cope as well with large, overlapping features
  as GiST's R-tree does, at least for this workload.

Conclusion reused directly into the design decision below: PostGIS's own
SP-GiST is not, on this evidence, a shortcut to beating pgSphere or Q3C --
worth knowing before spending effort on it, and worth recording so the next
person doesn't have to re-derive it. But it does confirm SP-GiST *can* be
competitive for spatial data given the right partition scheme; the question
became whether a HEALPix-native one (rather than PostGIS's generic
Cartesian octree) would do better for this specific, HEALPix-shaped problem.

### A HEALPix-native SP-GiST opclass for skypos

`paper/response-to-referee-2.md` and `bench/tap-ab/RESULTS.md` already
named "an SP-GiST opclass over the HEALPix cell hierarchy" as future work,
aimed at a specific, different bottleneck: the planner-visible cost of
covering computation and OR-ed B-tree ranges, 3% of query time at 10M rows
and 13% at 50M, one of the only costs in the whole design that *grows*
with catalogue size. This round builds a first version of exactly that,
aimed at both that bottleneck and this file's own repeated finding that
region-shape GiST is structurally capped against pgSphere.

**What it indexes, and how that differs from every other strategy in this
file.** Every strategy above indexes the *region* column (`skyregion_gist_
ops`, `skycell_region_moc`'s GIN alternative): many candidate regions, one
or few probe points. This is the mirror image: it indexes the *point*
column (`skypos`), for "one query region (or many, from a join), many
candidate points" -- cone search and cross-match against a large point
catalogue, skycell's actual headline workload, currently answered entirely
by the B-tree-rewrite planner machinery rather than a real index descent.
New file: `ext/src/spgist_region.c`; new opclass `skypos_spgist_ops`,
`DEFAULT FOR TYPE skypos USING spgist`, strategy 1 = the existing
`<@(skypos,skyregion)` operator (unchanged; already had a planner-rewrite
support function -- this opclass is a second, competing physical access
path the planner now also gets to cost against the rewrite).

**Tree shape.** A PATRICIA trie over HEALPix NESTED pixel ids: each inner
tuple carries an explicit `(order, pix)` prefix ("every leaf under here is
inside this HEALPix cell"), and its nodes decide the *next* order's digit --
up to 12 at the very first branch (order 0 has exactly 12 base pixels), up
to 4 below that (2 bits per order). `choose()`/`picksplit()` need only the
indexed point's own coordinates to compute a digit at any given order
(`sc_vec2pix()`, already in `healpix.h`); `inner_consistent()` reuses
`sc_region_classify()` unchanged -- the exact same function `cover.c`'s
cost-based covering and the region GiST opclass already use -- to prune a
child whose pixel provably doesn't meet the query region; `leaf_consistent()`
reuses `sc_region_contains()`, the same exact test `skycell_pos_in_region()`
uses, so this opclass is exact (`recheck` always false), unlike the region
opclass's intentionally-lossy multi-cap key.

**A real, non-obvious bug, found empirically before it was reasoned out.**
The first working version used one order per tree level with no prefix
compression at all (deliberately, to avoid `spgSplitTuple`). It passed a
6-million-pair correctness stress test (50,000 points, 120 mixed regions) --
and then missed 2 of 3 true matches on a single cone query, but *only* when
the indexed table was physically sorted by HEALPix cell before the index
was built; an unsorted build of the identical rows answered the same query
correctly. Root cause, found by bisecting the reproduction down to a
minimal case and instrumenting `choose()`/`picksplit()` directly: SP-GiST's
own core marks a freshly created inner tuple "all-the-same" whenever
`picksplit` reports only one node, and once marked, refuses `spgAddNode`
against it (a hard error) -- `choose()` is instead expected to dump
anything arriving there into that one node regardless of its real value, on
the documented assumption that "the eventual PickSplit call will re-sort
things out". That assumption only holds if "one node" truly means
"provably indistinguishable forever". A fixed one-order-per-level split
cannot promise that: a small overflow batch at some deep order is often
locally homogeneous by chance (especially under sorted insertion, where
long runs share a prefix) rather than because every point that could ever
land there shares that digit. A later point with a genuinely different
digit then has no legal route, is forced into the wrong subtree anyway, and
`inner_consistent`'s `classify()` calls downstream run against the WRONG
assumed pixel -- correctly, by its own lights, pruning a leaf that the
point's real position would not have been pruned from. An intermediate fix
(always declare the full 12-or-4-wide label set so `nNodes` is never 1)
turned out not to work either: SP-GiST's storage compacts an inner tuple
down to whichever labels actually got a leaf, regardless of how many were
declared, so the same trap re-opened one level deeper in testing.

The real fix is the standard PATRICIA-trie discipline the built-in
`inet_ops` spgist opclass already uses for address bits: `picksplit`
searches forward, order by order, for the batch's actual first point of
disagreement (a longest-common-prefix search, however many orders that
takes), so "one node" is only ever reported when a batch is truly identical
all the way to the deepest resolvable order (a genuine duplicate/near-
duplicate cluster -- the correct, intended use of "all-the-same").
`choose()` correspondingly reads a tuple's own `(order, pix)` prefix
directly (packed into one `int8`, not derived from the SP-GiST core's own
hop counter, which stops meaning "current order" once prefixes span more
than one order at a time) and, on a later point whose true ancestor
diverges from that prefix before reaching its decision order, issues
`spgSplitTuple` to insert a new, shorter-prefixed tuple at the true point of
divergence -- the case a no-compression design gets to skip entirely, and
the part that turned out not to be optional.

**Correctness, after the fix.** The regression suite (`skycell`, `adql`)
passes unchanged. A dedicated stress test rebuilt the catalogue physically
sorted by HEALPix cell -- the exact condition that exposed the bug -- and
joined 50,000 points against 160 mixed circle/polygon regions (deliberately
including near-pole clusters and RA=0/360 wraparound, sizes spanning three
orders of magnitude) against the same table's own unindexed
`skycell_pos_in_region`: 5,789 matches, 0 false positives, 0 false
negatives, in both directions. The original failing query (a 30-arcmin cone
that had returned 1 or 2 of 3 true matches at various points during
debugging) now returns exactly 3, matching brute force and every other
method.

**Performance** (`bench/03_cone.sql`'s own harness and radii, 50,000-row
catalogue, extended with a `skycell_spg` arm querying a `skypos`-indexed
table via plain `<@`):

| method | avg query time (1" to 3deg) | vs pgSphere |
|---|---|---|
| pgSphere (native GiST) | ~0.035-0.069 ms | 1x |
| skycell (B-tree + planner rewrite) | ~0.045-0.135 ms | ~1.3-2x |
| **this opclass (SP-GiST)** | **~0.14-0.27 ms** | **~3.5-4x** |
| Q3C | ~2.8-3.5 ms | ~50-80x |

Every radius bucket's row counts agreed exactly across all four methods.
So: this is not (yet) a win over pgSphere or over skycell's own existing
B-tree-rewrite path for the plain cone-search case at this scale -- but it
soundly beats Q3C (~15-20x), does so via a genuine single-index-qual descent
rather than a planner-time covering computation, and its index is
competitive to build: 0.088s/2.3MB for the SP-GiST structure alone at
50,000 rows (vs pgSphere's 0.2s/3.1MB GiST and skycell's 0.05s/1.1MB
B-tree) -- cheaper and smaller than pgSphere's own index, if not yet faster
to query. Where this direction was actually aimed -- removing the
planning-time covering cost that grows with catalogue density and size, the
paper's own flagged bottleneck at 10-50M rows -- has not yet been measured
at that scale; the numbers above are all at 50,000 rows, the scale where
that specific cost is smallest.

**Cross-match** (`bench/20_region_xmatch.sql`'s own fixture: 5,000 mixed
circle/polygon footprints x 50,000-row catalogue, `point <@ region`):

| method | query time | plan |
|---|---|---|
| pgSphere (native GiST, per-type circle/poly union) | ~267-292 ms | `Nested Loop -> Index Scan` per type |
| skycell, GIN-rewrite (round eight) | ~894-979 ms | `Nested Loop -> Bitmap Heap Scan on fpr` (GIN on `skycell_region_moc`) |
| **this opclass, GIN dropped (forces the native SP-GiST plan)** | **~793-863 ms** | `Nested Loop -> Index Scan using cat_spg_idx` |

A genuine, unplanned finding here: with *both* a GIN index on the footprint
side and this opclass's SP-GiST on the point side present, the planner
never even considers the SP-GiST plan -- `region_support_simplify`'s
existing rewrite (round eight) unconditionally rewrites `point <@ region`
into the GIN-array form the moment a matching GIN index exists, before
physical index selection ever gets a chance to cost the alternatives
against each other (documented, deliberate behaviour from round eight, not
new; this is simply the first time two competing physical answers to the
same predicate have coexisted on the same table). The two paths land within
~10% of each other in practice (793-863ms native SP-GiST vs 894-979ms via
the GIN rewrite), so it isn't costing much today, but it means this new
opclass cannot currently be *chosen* by the query optimizer's own cost
model on a table that also has the GIN index -- only on one that doesn't.
Both trail pgSphere's native per-type GiST by ~3x at this scale.

**Status: experimental, functionally complete for the point-in-region
predicate, not yet a performance win over any incumbent path.** Beats Q3C
soundly on cone search (~15-20x) and is a real, working, exact index
descent where none existed before, but has not (yet) closed the gap to
pgSphere on either cone search (~3.5-4x behind) or cross-match (~3x behind),
and currently loses the planner's attention entirely on a table that also
carries round eight's GIN index. Not yet measured: catalogue sizes at or
near the 10-50M rows where the B-tree-rewrite's planning cost is largest
relative to execution time (the regime this opclass should have its best
chance of winning, per the paper's own diagnosis, since it pays no
per-query covering-computation cost at all); and concurrent-write
behaviour, `spgSplitTuple` in particular, which no other opclass in this
file exercises. Prefix compression is real and load-bearing for correctness
here, not merely an efficiency add-on the way it would be for a plain radix
tree -- see `ext/src/spgist_region.c`'s file header for the full account.

## Round eleven: three more correctness bugs found at scale, and the
## planning-cost verdict at 10M rows

Round ten's SP-GiST opclass was correctness-verified at 50,000 rows (a
6-million-pair stress test, then a dedicated sorted-insertion reproduction
of the bug it found). Asked to test it at 10M rows -- the scale the
planning-cost argument for building it in the first place was actually
about -- `CREATE INDEX ... USING spgist (pos)` failed outright:
`ERROR: cannot add a node to an allTheSame inner tuple`. Three more real
bugs, found by bisecting the reproduction down from 10M rows to as few as
500,000 (dense clusters, not raw row count, are what triggers them -- the
scale that matters is "how many points end up sharing a very deep common
ancestor", not table size) and instrumenting `choose()`/`picksplit()`
directly. All three are now fixed, and `ext/src/spgist_region.c`'s file
header has the permanent account; summarised here for the record:

1. **`in->level` drift past `SC_MAX_ORDER`.** `picksplit`'s
   longest-common-prefix search started from `in->level`, trusting it as a
   lower bound on the batch's already-guaranteed-common order. `spgSplitTuple`
   breaks that trust: a split inserts an extra tree hop whose own decision
   can land at a much shallower order than the hop implies, so hop count and
   true order permanently diverge once enough splits accumulate -- in
   either direction. Un-clamped, this fed an order past 29 straight into
   `sc_vec2pix()`, out of its valid domain. Clamping the start to
   `SC_MAX_ORDER` stopped the crash but not the bug: the opposite drift
   (hop count *undershooting* the true order) made the search start already
   past a point the batch genuinely disagreed on, silently manufacturing a
   bogus "all-the-same" tuple. Fixed by not trusting `in->level` at all --
   the search now always starts fresh at order 0, which costs at most 30
   harmless re-checks of orders every ancestor already confirmed, negligible
   next to one page read.

2. **A split's upper tuple was created with only one node.** `spgSplitTuple`
   handed the new upper tuple the old content's single label, planning to
   `spgAddNode` the new point's (necessarily different) label on the
   re-invocation `choose()` gets right after a split. That re-invocation hit
   the exact wall this whole design exists to work around: the SP-GiST core
   marks *any* freshly created `nNodes==1` inner tuple all-the-same, whether
   `picksplit` or `spgSplitTuple` created it, and then refuses `spgAddNode`
   against it. Fixed by giving the new upper tuple both labels atomically
   (`prefixNNodes = 2`) from the start.

3. **The SP-GiST core synthesises its own all-the-same placeholders.** Even
   with (1) and (2) fixed, the crash persisted. `elog` tracing caught the
   actual cause directly: a node freshly created by `spgAddNode`, on
   receiving its first leaves, gets wrapped by the *core* in a placeholder
   inner tuple -- also marked all-the-same, also refusing `spgAddNode` --
   without ever calling this opclass's `picksplit`. Observed with 8 node
   slots, all labelled 0, a shape this opclass's own `picksplit` can never
   produce (every real split emits genuinely distinct labels only). Since
   neither this placeholder's prefix nor its labels reflect real content,
   the only universally safe response is the one the access method actually
   documents: whenever `in->allTheSame` is set, match the single existing
   node unconditionally, without inspecting its prefix or labels at all, and
   trust that the next real overflow on that bucket -- now driven by a true
   from-scratch search, not a per-level guess -- untangles whatever
   accumulated there. `choose()` and `inner_consistent()` both check
   `in->allTheSame` first, ahead of every other branch.

All three were invisible at 50,000 rows -- small enough that no bucket ever
accumulated the density needed to exercise them -- and reliably reproduced
between 500,000 and 1,000,000 rows once real clusters (this benchmark
corpus's own `\sigma \le 0.02\deg` cores) got dense enough. Re-verified after
the fix: full regression suite, the sorted-insertion stress test from round
ten (0 mismatches), and a fresh one at genuine 10M-row scale --
10,000,000 points x 80 mixed circle/polygon regions (deliberately
including near-pole clusters, RA=0/360 wraparound, sizes spanning three
orders of magnitude), 2,496 true matches, 0 false positives, 0 false
negatives in either direction against `skycell_pos_in_region` run directly.

### The planning-cost question, answered

Round ten's opclass exists to test one specific claim from the paper
(`response-to-referee-2.md`, `body.tex` \S\ref{sec:phase}): that
`skycell`'s planning cost -- computing a cost-based covering before every
query -- is "the whole of skycell's disadvantage at 6 arcmin" against
pgSphere, and that an SP-GiST index descent would remove it architecturally
rather than amortise it. Tested directly, at 10M rows, with
`EXPLAIN (ANALYZE, BUFFERS)` splitting planning from execution
(`bench/03_cone.sql`'s own harness, extended with a `skycell_spg` arm):

| radius | method | plan ms | exec ms | plan % | buffers |
|---|---|---|---|---|---|
| 1" | pgSphere | 0.019 | 0.037 | 33% | 5.9 |
| 1" | skycell (B-tree rewrite) | 0.032 | 0.017 | **66%** | 4.2 |
| 1" | **skycell_spg (this opclass)** | 0.022 | 0.175 | **11%** | 14.3 |
| 1' | skycell | 0.035 | 0.023 | 60% | 5.0 |
| 1' | skycell_spg | 0.021 | 0.218 | 9% | 14.7 |
| 30' | skycell | 0.084 | 0.251 | 25% | 32.3 |
| 30' | skycell_spg | 0.034 | 0.988 | 3% | 46.7 |
| 1\deg | skycell | 0.156 | 0.806 | 16% | 68.1 |
| 1\deg | skycell_spg | 0.037 | 2.343 | 2% | 98.7 |

**The architectural claim is confirmed exactly as stated**: this SP-GiST
opclass cuts skycell's planning share from 60-66% down to 9-11% at the
smallest radii, and its absolute planning time is consistently the lowest
of the three PostgreSQL-native methods measured (0.022ms vs pgSphere's
0.019-0.040ms and skycell's 0.032-0.245ms) -- a single index qual really
is cheaper to plan than either a GiST predicate or a cost-based covering.

**It does not translate into a win**, and the same table says why:
`skycell_spg`'s *execution* time is 5-10x its own planning saving, and 3-10x
pgSphere's or skycell's execution at the same radius -- 14-99 buffer touches
against skycell's 4-68 and pgSphere's 6-471 at the same radii. The wall-clock
comparison across all five tested radii (1", 1', 30', 1\deg, 3\deg):

| method | 1" | 1' | 30' | 1\deg | 3\deg |
|---|---|---|---|---|---|
| pgSphere | 0.070 ms | 0.070 ms | 0.363 ms | 1.026 ms | 6.314 ms |
| skycell (B-tree rewrite) | 0.061 ms | 0.064 ms | 0.293 ms | 0.661 ms | 9.976 ms |
| **skycell_spg** | 0.205 ms | 0.231 ms | 1.199 ms | 2.608 ms | 13.578 ms |
| Q3C | 3.271 ms | 3.085 ms | 3.270 ms | 3.673 ms | 14.829 ms |

skycell_spg is slower than both pgSphere and skycell's own existing path at
every radius tested (2.9-3.6x pgSphere, 3.2-3.9x skycell at the small end,
narrowing to ~1.4x and ~1.0x at 3\deg where execution dominates and
skycell's own B-tree-range approach is doing more scattered I/O than
pgSphere's compact GiST). It remains well ahead of Q3C throughout (2.7-16x).

**Why the saved planning time doesn't show up as a win**: skycell's
existing B-tree rewrite was never planning-bound in absolute terms -- 0.02-
0.16ms of planning bought a *very* cheap execution (4-68 buffers, mostly
sequential B-tree range scans against a pre-computed, density-tuned
covering). This opclass removes that 0.02-0.16ms, correctly, but pays for
it with a tree descent that costs more buffers than the covering it
replaced (14-99 against skycell's 4-68) -- a PATRICIA trie over 10M points'
worth of HEALPix structure is simply a taller, more scattered structure to
walk than a handful of pre-computed, contiguous B-tree ranges. The
architecture the paper proposed genuinely removes the cost it targeted;
what it does not do, at least in this first implementation, is replace it
with something cheaper than what a good *B-tree* covering already was.

**Index build, at the same 10M-row scale** (table already sorted along the
HEALPix curve for all four, so this isolates the index step alone):

| method | build time | index size |
|---|---|---|
| Q3C | 6.1 s | 214.2 MB |
| skycell (B-tree) | 4.4 s | 214.2 MB |
| **skycell_spg** | 28.6 s | 449.3 MB |
| pgSphere (GiST) | 90.9 s | 684.7 MB |

Consistent with round ten's 50,000-row numbers: smaller and roughly 3x
faster to build than pgSphere, but bigger and slower than the B-tree this
opclass was hoping to make unnecessary.

**Where this leaves the opclass**: correctness is now solid at the scale
that matters (10M rows, dense clusters, verified both by construction --
regression suite, two independent stress tests -- and by the specific
condition that broke it originally). The architectural hypothesis behind
building it is *confirmed*, not just plausible: planning cost really can be
removed this way, and the measurement proving that is now real, not
proposed. But the net result at 10M rows is unchanged from round ten's
50,000-row read: this is not (yet) a win over either incumbent path, and
the reason is now precisely located -- in the execution cost of the
descent itself, not in planning, which is exactly where round ten's own
50,000-row numbers already pointed (plan_pct was never the whole story
there either). Any further work on this opclass should target *that*: a
shallower or better-cached tree (real prefix compression already helps;
whether it helps enough, or whether the fundamental shape of a PATRICIA
trie over a skewed density field is the limit, as this file's rounds two,
five, nine and ten found for the GiST opclass's own fanout, is untested)
rather than the planning side, which is now a solved problem for this
predicate.

## Round twelve: wide-radix splitting, tried and measured -- reverted

Round eleven ended by locating the SP-GiST opclass's remaining problem
precisely: not planning (solved), but execution -- tree descent costs more
buffer touches (14-99 at 10M rows) than the B-tree covering it was meant to
replace (4-68 at the same radii). Its closing paragraph named a shallower
tree, via real prefix compression, as the obvious next thing to try.

**The idea**: `spg_healpix_choose`/`picksplit` decided one HEALPix order (a
2-bit, up to 4-way digit) per tree level. Combining `SPLIT_WIDTH`
consecutive orders into one combined digit -- up to 4^SPLIT_WIDTH-way
branching per inner tuple -- divides tree depth, and so descent buffer
touches, by roughly SPLIT_WIDTH, at the cost of more candidate children per
`inner_consistent()` call. That trade looked safe because rejecting the
extra candidates is pure in-memory trigonometry (`sc_region_classify()`),
not I/O, unlike round nine's variable-length GiST key where the added
per-page cost was itself on the critical path.

**A fourth correctness bug, found before any performance measurement was
trustworthy**: the first implementation derived a tuple's decision width
from its stored `order` alone (`split_width(order)`, -1 -> 1, else
`SPLIT_WIDTH`), on the claim that this was deterministic and needed no new
per-tuple storage. It wasn't deterministic. `choose()`'s `spgSplitTuple`
branch caps the new upper tuple's width by the *old* tuple's own
established order (`Min(common_order + width, order)` -- the old content's
digits beyond its own order live in its nodes, not in this prefix, so the
upper tuple genuinely cannot decide anything deeper), and nothing let a
later `choose()`/`inner_consistent()` call tell a capped tuple apart from a
normal one using `order` alone. A sorted-insertion stress test at 50,000
rows caught it directly: 2,869 false negatives, traced (the same
`elog`-tracing/point-filtering method rounds ten and eleven used) to a
point routed through an `order=1` tuple at insert time becoming unreachable
at search time, because `inner_consistent` recomputed that tuple's span as
the full `SPLIT_WIDTH` instead of the narrower width it was actually built
with.

**A second, latent bug found while designing the fix**: the existing
`(order, pix)` prefix packed into one `int8`, with `pix` capped at 58 bits.
A HEALPix NESTED id carries a face value (0-11) in bits *above* its
`2*order` interleaved position bits, so a full id at order 28-29 needs up
to 2*29+4 = 62 bits -- 4 more than the field had. This was never triggered
by any reproduction used before now (all stayed too shallow, order
0-15ish), but was always live: a large enough, dense enough cluster would
have silently truncated real pixel ids.

**The fix**: stop trying to fit `(order, width, pix)` into one `int8`'s
bits at all. The prefix is now a small `bytea` (`SpgPrefixData`: two
`int32`s and an `int64`, no packing), so `pix` stays the plain, full
`sc_vec2pix()` value with no ceiling, and `width` is stored explicitly and
read back verbatim by every later call -- never re-derived from `order`.
`spgSplitTuple`'s capped case now just stores whatever width the cap
actually produced; everything else (`choose()`'s normal-match branch,
`inner_consistent()`) trusts the stored value and needs no further
clamping. Re-verified after the fix: full regression suite, the specific
previously-failing case (id=7272 against a region whose true match wide-
radix had missed) now correct, the full 50,000-row x 160-region stress
test (9.28M pairs, 0 mismatches both directions), and a fresh 10M-row x
80-region correctness run identical in shape to round eleven's (2,496 true
matches, 0 false positives, 0 false negatives).

**The performance result, once correctness was no longer in question**:
negative. Measured with the same `EXPLAIN (ANALYZE, BUFFERS)` cone-search
harness round eleven used, at 10M rows, `SPLIT_WIDTH=3` (64-way) against
`SPLIT_WIDTH=1` (the pre-experiment, single-order behaviour, rebuilt with
the same bytea-prefix code to isolate the storage-format change from the
wide-radix change itself):

| radius | SPLIT_WIDTH=1 buffers | SPLIT_WIDTH=3 buffers | ratio |
|---|---|---|---|
| 1" | 14.5 | 15.6 | 1.08x |
| 1' | 14.7 | 15.9 | 1.08x |
| 30' | 47.1 | 99.5 | 2.11x |
| 1deg | 98.8 | 278.1 | 2.82x |
| 3deg | 472.3 | 1625.5 | 3.44x |

`SPLIT_WIDTH=1`'s numbers reproduce round eleven's int8-prefix baseline
almost exactly (14.3/14.7/46.7/98.7 there vs 14.5/14.7/47.1/98.8 here),
confirming the regression is wide-radix's own effect, not the switch to a
bigger prefix type. Execution time tracks the same pattern (e.g. 3.53ms vs
0.99ms at 1deg, 14.6ms vs 7.4ms at 3deg), and the regression *grows* with
query area rather than shrinking.

**Why, in hindsight**: a PATRICIA trie's branching already tracks exactly
where the data disagrees, at whatever order that happens to be -- that is
the entire point of the from-scratch longest-common-prefix search rounds
ten and eleven established. Forcing every decision to span `SPLIT_WIDTH`
orders regardless inflates picksplit's node count (and so the index's
total node/page count) past what the data's actual branch points need.
That extra breadth is cheap for a query that lands cleanly inside one
child, but a real cone or polygon query typically straddles a boundary at
several levels, and at each one it must now classify and potentially
descend into a wider set of siblings than a narrow split would have
offered at that same point -- and a larger query region crosses more such
boundaries, which is exactly the growing-with-radius shape measured above.

**Where this leaves the opclass**: `SPLIT_WIDTH` is left at 1 in
`ext/src/spgist_region.c` -- wide-radix splitting is implemented, correct,
and measured worse, not removed outright, because the fix underneath it
(explicit per-tuple width storage) is worth keeping independent of whether
wide splitting itself pays off: it is what makes revisiting the idea safe
to attempt again (e.g. a width chosen per-tuple from local density, rather
than one constant for the whole tree) without re-deriving the same
capped-width bug. Round eleven's own closing line ("real prefix compression
already helps") was written before this measurement and is superseded by
it: prefix compression, at least in this wide-radix form, does not help --
it was the single biggest lever this file could identify for the descent
cost, and it made descent cost worse. The opclass's standing verdict is
otherwise unchanged from round eleven: planning cost is solved
architecturally, execution cost is not, and after two attempts at the
tree's shape (variable-length GiST keys in round nine, wide-radix SP-GiST
here) that both made things worse, the more likely explanation is that a
PATRICIA trie over this kind of skewed density field is close to its
achievable shape already, the same conclusion this file's rounds two, five,
nine and ten reached for the GiST opclass's own fanout.

## Round thirteen: a coarse execution-time covering index, ruled out before
## being built

Round twelve closed off reshaping the SP-GiST tree itself. The next
candidate for the opclass's remaining execution-cost problem was a
different kind of change: leave the tree alone and instead attack
`skycell`'s own B-tree-rewrite path, which round eleven had shown still
pays a real, scaling planning cost (0.02-0.25ms at 10M rows) to compute its
covering fresh on every query via `cover.c`'s `sc_cover_compute()`.

**The idea**: replace that from-scratch, planning-time recursive
subdivision with a small, coarse index -- built over occupied HEALPix cells
or reusing the existing point-level SP-GiST tree's own inner structure --
walked once at *execution* time via a set-returning function, handing the
resulting ranges to the same cheap B-tree scan skycell already uses. The
premise: `cover_cone_direct()`'s recursive `sc_region_classify_cap()` walk
prunes only against the query's geometry, never against where data actually
lives, so for a large query region it should be re-examining (and
recursing into) plenty of cells that are empty of real data -- exactly the
waste a density-aware structure could skip.

**First, profiling the actual cost, before designing anything further**
(direct in-process timing, not `perf` -- unavailable in this container --
instrumented via the same temporary `elog`/`fprintf` technique used
throughout this file, at 2M rows with a warm per-backend density-map cache,
40 distinct queries per radius):

| radius | density lookup | `choose_order` | `cover_cone_direct` | `merge_gaps` | total covering compute | classify() steps | ranges |
|---|---|---|---|---|---|---|---|
| 1" | 0.0us | 0.5us | 2.3us | 0.0us | 2.8us | 4 | 1 |
| 1' | 0.0us | 0.5us | 2.6us | 0.0us | 3.2us | 4 | 1 |
| 30' | 0.0us | 0.7us | 61.2us | 0.4us | 62.3us | 260 | 5 |
| 1deg | 0.0us | 0.8us | 76.7us | 0.6us | 78.2us | 584 | 9 |
| 3deg | 1.0us | 1.0us | 141.3us | 1.6us | 144.3us | 825 | 19 |

This settled two things before any index work started. The per-backend
density-model cache (`skycell.cache_coverings`'s sibling caches for the
histogram and matching index, not the per-query covering memo itself, which
never hits for distinct query centers) is genuinely free once warm, so
"loading the density model costs more than computing the covering" (the
comment in `skycell.c` this describes) is a cold-start-only cost, not a
per-query one. And the covering computation itself is negligible at
sub-arcminute radii (~3us against round eleven's ~32-35us measured total
planning time -- the real cost there is generic planner/rewrite overhead no
index would touch) but genuinely dominant and scaling at 30'+ (60-140us,
60-75% of measured total planning time) -- exactly where an index-based
replacement would have to earn its keep.

**Then measuring the premise directly, rather than assuming it**: how much
of `cover_cone_direct`'s work at 30'-3deg lands on cells with zero real
rows. First attempt used the existing ANALYZE-histogram density map
(`sc_density_rows`) as the "is this empty" oracle in a modified copy of the
walk that short-circuits recursion into any cell the map reports as
empty -- 0.0% of steps avoided, at every radius. That number turned out to
be a false lead: reading `sc_density_rows()` shows it prorates an
equi-depth histogram bucket's count across every sub-cell inside it
("inside one map cell this is still an assumption of uniformity"), so it
structurally cannot report exactly zero for any cell that overlaps a
non-empty bucket, whatever the real occupancy inside that cell actually is.
Redone against ground truth instead -- logging every one of the walk's
2,116 distinct visited cells (16,318 total visits, orders 7-9) and checking
each with a real `EXISTS(...)` against the actual table -- came back the
same: **0.0% empty, exactly, weighted or unweighted.** Every cell the
production code's covering walk touches at these radii contains at least
one real row.

**Why zero, not just small**: `choose_order()`'s own cost model already
balances range count against expected false-positive rows, which pushes it
to stop getting finer once cells are reasonably populated rather than
continuing toward a resolution where emptiness would show up -- the
algorithm is not wasting effort on sparsity because it is not, in this
respect, looking in the wrong place. That, combined with this benchmark
corpus's substantial uniform background component (its density spec is
35% uniform sky, so no order-7-9 cell overlapping a plausible query is ever
genuinely unpopulated), leaves nothing for a sparsity-aware structure to
skip.

**Ruled out before writing an index at all.** The hybrid's entire case was
"the current walk wastes work a data-aware structure could avoid"; measured
directly, it does not, at least for this catalogue. Combined with round
twelve (reshaping the SP-GiST tree's own branching made large queries
worse, not better) and the profiling above (small-radius planning cost
isn't in the covering computation to begin with), there is no remaining
version of "reuse an index to make the covering cheaper" left to try
against this corpus. The one caveat worth keeping: this corpus has no true
coverage gaps (unobserved sky, not just low density), which a real survey
footprint might have and which this measurement cannot speak to -- but
absent a concrete reason to expect that, this line of attack on the
opclass's execution cost is closed.

## Round fourteen: a dedicated CONTAINS_REGION pruning test, tried and
## reverted

Round six's own header already named the gap: `@>(skyregion,skyregion)`
reuses OVERLAP's sub-cap-overlap test rather than a dedicated one, and
explicitly ruled out the obvious tightening (requiring every sub-cap of the
query to sit inside a single sub-cap of the candidate) as unsound -- a
SUFFICIENT condition for containment, not a NECESSARY one, so using it to
prune risks a false negative with no recheck to catch it. This round looked
for a *sound* tightening instead, on top of the existing test rather than
replacing it.

**The idea**: containment has a real, provable property the cap-overlap test
doesn't use at all -- if `A @> B` (B nonempty) then `area(A) >= area(B)`,
ordinary measure monotonicity. Unlike a tighter *shape* test, this needs no
approximation: `sc_region` already carries an exact `area` field for both
cones and polygons, and a subtree's largest reachable region's area is
exactly the max of its children's own, all the way down to true per-leaf
areas -- nothing lossy to propagate, unlike the cap geometry. Implemented as
one new field on `GistMultiCap` (`max_area`, max()-reduced through
`union()`/`picksplit()`, no changes needed to either's existing cap logic)
and one new check in `consistent()`, CONTAINS_REGION only: reject outright,
no recheck needed, whenever even the largest region reachable under a key is
smaller than the query region.

**Correctness held throughout**: full regression suite; round six/seven's
own fixtures agreed exactly with brute force at both 5,000 rows (206/206,
1250/1250) and 50,000 rows (788/788, 22,349/22,349); a third fixture built
specifically to give the new check something to reject (probe radii
0.08-0.13, overlapping the footprint corpus's own 0.02-0.32 range, unlike
round six's fixture where every probe is smaller than every footprint by
construction) also came back exact.

**Performance did not move.** Same corpus, same probes, A/B'd directly
against the pre-change code at 50,000 rows: 79.98/63.15ms before,
76.73/70.53ms after -- noise-level identical, nowhere near closing the
~4.9x gap to pgSphere round six measured. `EXPLAIN (ANALYZE, BUFFERS)` on
the size-mixed fixture (built to actually exercise the new check) showed
why: of ~2,135 buffer touches, the new check eliminated exactly *one* false
candidate that would otherwise have reached recheck. The overlap test was
already rejecting almost everything the area test could additionally
catch, because on realistic data the two failure modes are correlated, not
independent: a footprint too small to contain the query is usually also
positioned such that its cap doesn't overlap the query's cap in the first
place. The two tests are mostly redundant in practice, not complementary.

**Reverted rather than kept**, matching this file's own standard for a
verified-negative change (rounds nine and twelve did the same): sound,
correctness-clean code that adds 8 bytes/key and a comparison per
consistent() call for a measured performance effect indistinguishable from
noise is not worth carrying. Round two's own diagnosis of `@>`'s real
bottleneck stands unaddressed by this attempt: `EXPLAIN (ANALYZE, BUFFERS)`
already showed "too many internal and leaf pages have overlapping bounding
caps," an internal-node fan-out problem this round's leaf-level candidate
filter never touched. Closing `@>`'s gap for real would need to attack that
-- a sharper key (more than MAX_SUBCAPS=4 caps, or a HEALPix-cell-based key
closer to the MOC-ranges recipe this opclass exists to replace) -- which
round two's own text already flagged as "a materially bigger change than a
split-algorithm swap, and arguably undermines the 'simpler than the recipe'
case for building this opclass at all." Left as an open question, not
attempted here.

## Round fifteen: MAX_SUBCAPS=8, tried and reverted -- a real tradeoff, not
## a free lunch

Round three's 1->4 jump was framed as "does this pay for itself" and
answered yes, unambiguously, on both buffer count and wall-clock at once.
The natural next question: does continuing in the same direction (more
sub-caps) keep paying? Bumped `MAX_SUBCAPS` from 4 to 8 -- a one-line
change, since every array/loop in the file is already sized off the macro,
not hardcoded -- and re-ran round two/three's own `&&` benchmark and round
six/seven's `@>`/`<@` benchmark, same corpus, same probes, same 50,000-row/
500-probe scale, A/B'd directly against the committed MAX_SUBCAPS=4 code.

**Correctness held**: exact match counts against the known-correct values
for all three predicates (1282, 788, 22349).

**The two usual metrics disagreed, which is itself the finding.** Buffer
counts (deterministic) went down at 8 caps across all three: && 35,238 ->
28,988 (18% fewer), @> 31,819 -> 27,390 (14% fewer), <@ 65,998 -> 64,202
(3% fewer). But repeated wall-clock runs (4 passes each, to separate signal
from this machine's noise) went the other way, consistently: && ~121ms ->
~153ms, @> ~78ms -> ~89ms, both *worse* at 8 caps, every time measured. <@
was roughly flat.

**Why they disagree**: this whole corpus is cache-resident (every buffer
in `EXPLAIN (ANALYZE, BUFFERS)` is a `shared hit`, never a read), so "fewer
buffers" here means "fewer index tuples visited," not "less I/O" -- and
`multicap_overlaps()`/`multicap_overlap_amount()` are O(MAX_SUBCAPS^2) per
comparison. Doubling MAX_SUBCAPS quadruples that (16 -> 64 cap-pairs
checked per key comparison), and that CPU cost grows faster than the
tuple-visit count shrinks. Round three's own 1->4 jump was a rare case
where a sharper key both prunes more *and* costs little extra per check
(1 cap vs up to 4, not yet quadratic in a way that mattered at that
scale); by 8, the quadratic term has caught up and started costing more
than it saves. The index also grew 69% on disk (16MB -> 27MB) for this
negative result.

**Reverted**, same standard as rounds nine, twelve, and fourteen. Not
tried: MAX_SUBCAPS=16 -- with the loss already consistent and growing at
8, and the O(k^2) comparison cost about to quadruple again, there was no
reason to expect a reversal. The real implication is narrower than "more
sub-caps don't help": it's that round three's win was not simply "more
caps is better, indefinitely" -- the sweet spot is wherever the marginal
pruning gain crosses the marginal O(k^2) comparison cost, and for this
corpus and MAX_SUBCAPS's current linear-scan overlap test, that crossing
point is at or before 4. A sharper key that doesn't pay the full O(k^2)
cost of comparing every sub-cap against every other sub-cap (e.g. spatially
sorting/indexing each key's own sub-caps so overlap search is sub-quadratic,
or a fundamentally different key shape) is the more promising version of
"sharper key" left untried -- see the handoff summary at the top of this
file (SPATIAL_INDEXING_HANDOFF.md) for the full open-questions list.

## Round sixteen: the density-adaptive covering path, dug into and parked --
## a real bug found and fixed, but not a validated win, and a bigger
## question opened along the way

Prompted by an external proposal to build an "adaptive, density-aware
hierarchical HEALPix covering" for `cover.c`'s cone-search path. Before
writing anything new, checked whether the mechanism being proposed already
existed: it does. `sc_cover_compute()` has always had two paths --
`cover_cone_direct()`, the default for every cone (`p->direct=1`, covers
every radius tested anywhere in this file, including 3deg), which picks one
global target order and uniformly subdivides to it; and a heap-based
`refine:` loop (the fallback for polygons, or when `cover_cone_direct`
bails), which already does real per-cell adaptive KEEP-vs-SPLIT decisions
using the density map. The proposal's own Phase 1 ("expose reusable
`cover_cost`/`cover_split_cost` helpers") would have duplicated ~150 lines
that already exist. This alone was worth surfacing before any code got
written.

**Phase 0: does the existing adaptive path already fix the 3deg gap round
eleven found?** 10M rows, 40 real 3deg cone queries, `cover_cone_direct`
(shipped) vs. the `refine` loop (forced on via `p->direct=0`), `EXPLAIN
(ANALYZE, BUFFERS)`. On 39 of 40 queries: a wash (7.08ms vs 7.24ms
execution, refine's buffers slightly better) plus a real, consistent
planning-time tax for refine (~1.28ms vs ~0.45ms). **One query -- a 3deg
cone landing on an exceptional density overlap, 137,535 true matches --
was 5-7x worse under refine** (62-71ms -> 393-436ms), tracing back to 50
generated ranges against direct's 21 for a similar total candidate volume.

**Root cause, found by enabling `cover.c`'s own pre-existing `SC_COVER_TRACE`
facility** (unused elsewhere in this codebase until now) directly against
the failing query: of 362 split decisions, 85 (23%) had `removed == 0`
(nothing excluded, zero benefit) and were accepted purely through the
`delta <= 0 && P.pot > p->range_cost` fallback ("no extra ranges now, worth
looking deeper"). All 85 had no `SC_OUT` child at all -- `potential()`
scores a cell by expected row count alone, with no signal for whether
genuine boundary uncertainty remains, so deep inside a dense cluster's
interior (where density, and so `pot`, stays high at every order) the
fallback keeps recommending "worth looking deeper" for many consecutive
levels even once there is provably no false-positive area left nearby to
exclude -- cascading 5+ extra levels deep, purely fragmenting the covering
for no selectivity gain.

**The fix**: a bounded "dry streak" on each heap candidate -- at most
`SC_COVER_MAX_DRY_STREAK` (2) consecutive ancestor levels with
`removed == 0` before the density-only fallback stops firing and the cell
is just kept as one range instead. Not a removal of the fallback (a genuine
boundary can be a level or two away without any individual step yet
excluding area, so some speculative depth is worth keeping) -- a cap on how
far pure density alone is allowed to justify continued speculation.
Implemented, correctness-verified (full regression suite; the standalone
`cover_selftest` harness, 0 false negatives across ~900,000 polygon-sample
points and 30,000x200 cone samples; the exact failing query re-checked
against brute truth). Mechanism-level verification confirmed the diagnosis
exactly: zero-gain splits for the failing query dropped from 85 to 43
(49% fewer), ranges from 50 to 37 (26% fewer).

**But the net result did not validate as a win, and testing beyond the one
query that started this found something more consequential.** Two nearby
query centers (qids 64 and 74, all three sitting within a few degrees of
each other -- one exceptionally dense region of this synthetic corpus)
that were *fine* before the fix (25-26ms) got *dramatically worse* after it
(287-298ms). Splitting the cause with parallelism forced off
(`max_parallel_workers_per_gather=0`) for a clean, apples-to-apples read
found two separate effects tangled together:

1. **A genuine quality regression from the fix itself**, non-parallel:
   226ms (direct) vs 317ms (refine, fixed) for query 64 -- the dry-streak
   cap cuts off refinement too early in some cases, leaving a covering with
   more false-positive rows than either the original refine algorithm or
   direct produced there. `SC_COVER_MAX_DRY_STREAK=2` is not correctly
   calibrated against more than the single query that motivated it --
   exactly the mistake this file's own rounds two and fifteen already
   warned against (never trust a fix measured on one query or one metric).
2. **A separate, likely bigger effect, independent of the fix or the
   density-only-fallback bug entirely**: direct's covering shape
   parallelizes beautifully (226ms -> 55ms, a 4x speedup from
   `max_parallel_workers_per_gather`'s default) while refine's -- fixed or
   not -- barely parallelizes at all (317ms -> 298ms, ~6%). This is about
   how PostgreSQL's parallel bitmap heap scan divides work across workers
   relative to range *shape*, not about false-positive count or range
   count at all, and it was invisible in the Phase 0 numbers above because
   those never isolated parallel from non-parallel execution.

**Parked, not shipped.** The dry-streak fix is a real, correctly-targeted
fix for a real, precisely diagnosed bug (verified at the mechanism level:
it does exactly what it was built to do), but it is not a validated
improvement to net query time, and it was never checked against the
refine path's actual existing callers (polygon coverings) at all this
round -- shipping it as-is risked trading one under-tested regression for
another. Reverted rather than merged, matching this file's standing
practice for anything that does not clear its own bar (rounds nine, twelve,
fourteen, fifteen). What's worth keeping from this round is not the patch
but the two findings: the precise mechanism behind unbounded density-driven
fragmentation (documented here and in the reverted diff's own commit
history), and the previously-unknown parallel-bitmap-scan sensitivity to
covering shape, which looks like a bigger lever than anything in the
original 3deg-gap question and is completely unexplored. See
`SPATIAL_INDEXING_HANDOFF.md` for this round folded into the open-questions
list.

## Round seventeen: the parallel-scan sensitivity from round sixteen, run
## down to a precise, general cause -- PostgreSQL's own JIT, not covering
## quality

Round sixteen closed with a real, unexplained finding: direct's covering
shape parallelized 4x on a heavy query, the density-adaptive path's --
fixed or not -- barely parallelized at all (~6%), independent of
false-positive rate or absolute range count reading as "better" or
"worse." Dug into it directly rather than leaving it as an open question.

**A controlled experiment, isolating range count from any covering
algorithm entirely**: one contiguous ~200,000-row cell range, split into 2
equal OR'd sub-ranges vs. 50 equal OR'd sub-ranges -- byte-for-byte the
same rows scanned either way, only the number of `BETWEEN` arms in the
`WHERE` clause differs.

| | 2 ranges | 50 ranges |
|---|---|---|
| parallel execution, JIT on (default) | 57.6ms | **535.4ms (9.3x worse)** |
| parallel execution, JIT off | 27.2ms | 26.6ms (parity) |
| non-parallel, JIT on | -- | 320.0ms (264ms of it *is* JIT compilation) |

Turning JIT off made the 9.3x gap disappear entirely -- 2 and 50 ranges
landed within noise of each other. That isolates the cause completely:
**PostgreSQL JIT-compiles the `Recheck Cond`/`Filter` expression once the
plan cost crosses `jit_above_cost`, that expression is one OR-branch per
range, and JIT's inlining/optimization/emission cost scales steeply with
branch count** (21ms total JIT time for 2 branches, 264ms+ for 50, even in
a single process -- 83% of that query's entire execution time). Under
*parallel* execution specifically, every worker process independently
JIT-compiles its own copy of the same expression, so a cost that is already
real serially gets paid two or three times over, and that redundant
compilation tax outweighs whatever the extra workers save on the actual
scan. This is not a property of either covering algorithm, the density
fallback bug, or anything this file's own code produces -- it is a generic
PostgreSQL behavior that any sufficiently wide OR'd-range predicate would
hit, direct path included, once range count and parallel worker count are
both large enough.

**Why this matters beyond round sixteen**: `cover.c`'s own cost model
(`range_cost`, calibrated in `skycell.c`'s `auto_range_cost()` from B-tree
descent cost) has no term for this at all -- it prices a range by its
index-descent cost, never by what the *executor* pays to JIT-compile and
multiply across parallel workers. That is a real, previously-unknown gap
in the covering's own cost model, not specific to round sixteen's adaptive-
covering experiment: it would apply to the shipped `cover_cone_direct()`
path too, at whatever range count and parallelism combination crosses the
same threshold. Untested here: whether `p->max_ranges` (64, the existing
budget) is already routinely producing enough ranges at real production
radii to trigger meaningful JIT cost even on the shipped default path --
if so, this is not a hypothetical concern for a reverted experiment, it is
live in production today.

**Left open, not attempted**: two candidate directions, neither tried.
(1) Add a range-count-and-parallelism-aware term to the covering's own cost
model (something like `range_cost` scaling with expected parallel worker
count, not staying flat), so the covering computation itself prices in
what a wide OR expression will cost to execute, not just to descend.
(2) Investigate whether raising `jit_above_cost`/`jit_inline_above_cost`
for skycell's own wide-OR query shape, or disabling JIT for it specifically,
is a cheap win independent of any covering-algorithm change at all.

## Round eighteen: yes, it's already live on the shipped path -- not just a
## reverted experiment's edge case

Round seventeen's own closing question: does `cover_cone_direct()` (the
default, unmodified path, `p->direct=1` for every radius) already produce
enough ranges at real production radii/densities to pay the JIT tax today?
Measured directly, no code changes involved.

**Setup**: 10M-row corpus, 300 production-representative queries (60 each
at 1″, 1′, 30′, 1°, 3°, mixed real-data-cluster and uniform-sky centers),
default GUCs (`jit_above_cost=100000`, `jit_inline_above_cost=500000`,
`max_parallel_workers_per_gather=2`), unmodified extension.

| radius | avg ranges | max ranges | avg cost | max cost | JIT triggered |
|---|---|---|---|---|---|
| 1″ | 0.4 | 2 | 10 | 17 | 0/60 |
| 1′ | 0.3 | 3 | 20 | 171 | 0/60 |
| 30′ | 5.1 | 12 | 3,295 | 11,102 | 0/60 |
| 1° | 7.9 | 19 | 9,163 | 46,008 | 0/60 |
| 3° | 16.6 | 26 | 51,312 | 358,636 | **3/60** |

Two findings, both clean:

**`p->max_ranges=64` is nowhere close to binding.** The highest range count
seen across all 300 queries, at the largest radius tested, was 26 -- well
under the 64 ceiling. The covering algorithm is not running out of budget
anywhere in this distribution; `max_ranges` is not the lever this question
was really about.

**But `jit_above_cost` (100,000) gets crossed anyway, on the shipped path,
for real queries.** At 3° in dense regions, 3 of 300 queries (1%) crossed
it with 21-22 ranges each -- no adaptive path, no reverted fix, no
artificial range-splitting, just `cover_cone_direct()` doing what it always
does. Measured those 3 directly with `SET jit = on/off`:

| qid | ranges | JIT on | JIT off | slowdown | JIT compile time |
|---|---|---|---|---|---|
| 260 | 21 | 133.0ms | 22.2ms | 6.0x | 52.5ms |
| 264 | 22 | 58.2ms | 18.7ms | 3.1x | 45.0ms |
| 274 | 22 | 61.6ms | 19.1ms | 3.2x | 41.7ms |

`Workers Launched: 2` in every case, `Inlining: 0.000ms` throughout (cost
stays under `jit_inline_above_cost=500,000`, so only Generation+
Optimization+Emission run) -- and Emission alone (35-45ms) makes up
68-83% of total execution time. This matches round seventeen's mechanism
exactly: the parallel leader and each worker independently JIT-compile
their own copy of the same OR'd `Recheck Cond`, so the compile cost is
paid 2-3x over rather than amortized, and it dominates the query.

**Conclusion**: this is not a hypothetical concern scoped to a reverted
experiment. At realistic large-radius, high-density queries -- about 1% of
this benchmark's distribution, concentrated entirely at the largest radius
tested -- the already-shipped, non-experimental `cover_cone_direct()` path
produces enough ranges on its own to cross `jit_above_cost` and pay a
3-6x wall-clock tax under default parallelism, with zero code changes
anywhere in this investigation involved. `range_cost`'s calibration
(`auto_range_cost()` in `skycell.c`, from B-tree descent cost alone) still
has no term for this. Round seventeen's two untried directions -- a JIT-
aware cost term, or tuning/disabling JIT for skycell's own wide-OR shape --
are no longer speculative; this round establishes there is a real, live
cost for them to address. Not attempted here: either fix. Also untested:
whether a real (non-synthetic, non-uniform-sky) catalog would push this
1% figure higher, since dense real surveys plausibly have more large-
radius/high-density queries than this benchmark's mixed distribution.

## Round nineteen: disabling JIT for this query shape -- a cheap win, but
## not one skycell can apply automatically

Round eighteen's own closing question, option (b): is disabling JIT (or
raising its threshold) for skycell's wide-OR cone query shape a cheap win
with no covering-algorithm change needed? Tested directly, on a fresh
10M-row corpus and a new 300-query benchmark (same shape as round
eighteen's: 60 queries each at 1″/1′/30′/1°/3°, mixed real-data/uniform
centers, resampled so the "data" centers are drawn IID via `ORDER BY
random()` over the full row population, not block-sampled -- block
sampling on a HEALPix-clustered table undersamples the rare extreme-
density hits that matter here).

Three configurations run head-to-head for every query, same connection,
same corpus: (a) default GUCs (`jit_above_cost=100000`), (b) `SET jit =
off`, (c) `SET jit_above_cost = 400000` (raised comfortably above every
cost this benchmark produced, `jit` itself left on). This run reproduced
2 of 300 queries (0.67%, both 3° / real-data-cluster centers) crossing
the default threshold:

| qid | ranges | cost | (a) default | (b) jit off | (c) threshold=400k |
|---|---|---|---|---|---|
| 266 | 25 | 386,662 | 44.3ms | 25.2ms | **22.2ms** |
| 248 | 21 | 376,970 | 68.1ms | 30.7ms | **29.5ms** |

Raising the threshold performs *at least as well as* turning JIT off
entirely -- unsurprising, since it produces the identical plan-time
decision (`jit_present_highthresh = false` for both, confirmed from the
JSON) while leaving JIT available for anything actually costing more than
400,000 elsewhere in the same database. Across the full 300-query set,
zero regressions: every query flagged by a >10% slowdown check under (b)
or (c) had `jit_present_default = false` already -- i.e. it was already
below the default 100,000 threshold, so neither change could have altered
its plan; the flagged deltas are pure sub-millisecond measurement noise on
trivial queries, not real regressions.

**So yes, it is a cheap win -- confirmed, not just plausible.** But it is
an *operational* win, not a code change skycell itself can make. The
mechanism why: PostgreSQL's own docs are explicit that the JIT decision is
made once, at the end of planning, by comparing the finished plan's total
cost against `jit_above_cost`/`jit_inline_above_cost` -- before execution
starts, and therefore before any skycell C function (`skycell_cone`,
`sc_cover_compute`, the GiST/SP-GiST support functions) ever runs. Those
functions execute during the scan, strictly after the JIT go/no-go
decision is already baked into the `PlannedStmt`. Nothing skycell's
extension code does at execution time -- and nothing exposed through its
existing GUCs (`skycell.range_cost`, `skycell.max_ranges`, etc.) -- can
reach back and change a decision already made. The only hook that
*could* intervene at the right time is a `planner_hook` wrapping
`standard_planner()`, inspecting the finished plan for skycell's own
operators and clearing `jitFlags` selectively; skycell has no
`planner_hook` today, and implementing one is a materially bigger,
different kind of change than anything else in this file (a plan-tree
walk plus a new extension-wide hook, not a `cover.c`/`gist_region.c`
tweak) -- not attempted, and arguably out of proportion to a 0.67%-of-
queries problem unless real-catalog density (§round eighteen's other open
question) turns out to make it much more common.

**Practical recommendation, actionable today with zero code changes**:
applications running skycell cone/region queries at large radii (≳3° on
catalogs with dense clusters) should raise
`jit_above_cost` (tested clean at 400,000; needs recalibrating per corpus,
not a universal constant) or set `jit = off` for the session/connection
pool handling that workload -- confirmed strictly non-regressive and as
good as or better than a blanket JIT disable, on every query tested.

## Round twenty: farthest-point seeding for merge_caps_greedy -- tried,
## measured, a clear regression, reverted

Prompted by reading pgSphere's actual GiST source (`gist.c` from the
upstream `pgsphere/pgsphere` repo, not previously looked at directly in
this investigation): its bounding key isn't a cap at all -- it's a plain
3D axis-aligned box in Cartesian space (`Box3D { Point3D low, high; }`,
6 `int32` coordinates, 24 bytes, matching the installed extension's own
`spherekey` type's `internallength = 24`), unioned by exact per-axis
`min`/`max`. That union is lossless at every tree level. skycell's own
`merge_caps_greedy` -- the function `multicap_union_many()` and
`multicap_penalty()` both call to fold a node's children's sub-caps back
down to `MAX_SUBCAPS` -- is not: its own header comment already says so
("Not a globally optimal clustering"), and it seeds its k clusters by
just taking the first `MAX_SUBCAPS` input caps in whatever order the
caller's flattened list happens to be in -- an order that comes from tree
traversal, not spatial layout. That looked like a plausible, cheap,
narrowly-scoped fix: replace that arbitrary seeding with farthest-point
selection (each new seed is the unchosen cap whose *minimum* waste to
every already-chosen seed is largest -- the standard greedy k-center
heuristic), without touching the key format, `consistent()`, or anything
else -- exactly the kind of small, reversible experiment this file's own
discipline calls for before a bigger redesign.

**Measured with a true A/B**: same 50,000-row mixed circle/polygon
footprint corpus, same 500-probe sets, same seeds, index dropped and
rebuilt from scratch under each binary so the comparison isn't
contaminated by an index already shaped by the other code.

| direction | before (first-k seeding) | after (farthest-point seeding) | pgSphere |
|---|---|---|---|
| `@>`(region,region) | 80-83ms | **225-273ms (2.9x worse)** | ~20ms |
| `<@`(region,region) | 160-174ms (beats pgSphere 1.4x) | **281-286ms (now loses to pgSphere 1.2x)** | 232-252ms |

Not a wash, not noise (both passes in each configuration agree tightly) --
a clear regression on *both* strategies, and it erased round seven's own
headline result (`<@` beating pgSphere outright at this scale).
**Reverted** (`git stash`, working tree and installed extension both back
to the original `merge_caps_greedy`).

**Why, best explanation available, not independently confirmed**:
`merge_caps_greedy` isn't only invoked to polish an already-decided split's
final keys -- `multicap_penalty()` calls it on every candidate subtree for
every single row GiST inserts, to decide *where the row goes*. Farthest-
point/greedy-k-center seeding is a better one-shot clustering for a fixed
input set, but it's also the textbook-known-sensitive-to-outliers member
of the clustering-heuristic family: whichever cap happens to be most
extreme relative to whatever's currently in `out[]` dominates each seed
choice. Across thousands of incremental `penalty()` calls, each comparing
a different candidate subtree's current children, that sensitivity likely
makes the *waste* estimate less consistent from one call to the next than
the plain first-k seeding it replaced -- locally tighter for one fixed
merge, but a noisier placement signal repeated over the whole incremental
build, which is a property first-k seeding's very naivety doesn't have
(same deterministic order every time, at least self-consistent even if
arbitrary). Not verified further (would need direct tree-shape/buffer
inspection, not just wall-clock, to confirm this specific mechanism rather
than just the outcome) -- but the outcome itself is unambiguous enough
that this specific fix is closed, not just paused.

**What this doesn't close**: the box-vs-cap union insight that motivated
it is still real and still unexplained away -- an *exact* union composes
losslessly at every tree level; a *capped* cluster union, however seeded,
structurally cannot (there is some input for which any seeding strategy
loses information MAX_SUBCAPS+1 items in). Round twenty only tested one
narrow fix (reseed the same lossy algorithm) and found it backfires
specifically because of penalty()'s incremental-build role -- it says
nothing about whether a *different* kind of fix (below) would do better.
