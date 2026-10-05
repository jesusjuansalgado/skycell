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

## Round twenty-one: decoupling penalty() from merge_caps_greedy -- also a
## regression, and it falsifies round twenty's own hypothesis

Round twenty's best explanation for its regression was that `penalty()`
calling the same (reseeded) `merge_caps_greedy` union() uses made the
*placement* signal noisy across thousands of incremental inserts. The
natural next experiment: stop `penalty()` from calling `merge_caps_greedy`
at all. `GistMultiCap` already carries an `overall` cap -- a plain,
already-computed two-cap union, exact and O(1), currently used only for
`picksplit`'s axis-sort heuristic. Swapped `multicap_penalty()` to score by
overall-cap area growth (`cap_area_proxy(union(orig, new).radius) -
cap_area_proxy(orig.radius)`) instead of flattening both sides' sub-caps
and running a full greedy merge every call. No seeding involved at all
this time -- a single deterministic cap union, cheaper than what it
replaced, `consistent()` untouched.

**Measured with the same true-A/B discipline** (same database, same
50,000-row corpus/500-probe set, index dropped and rebuilt under each
binary, back-to-back in the same session to control for machine-load
drift after round twenty's cross-session numbers turned out noisier than
expected -- see below):

| direction | before (original `penalty()`) | after (overall-cap growth) | pgSphere |
|---|---|---|---|
| `@>`(region,region) | 87-88ms | **243-256ms (2.9x worse)** | 15-16ms |
| `<@`(region,region) | 172-197ms (beats pgSphere 1.3-1.5x) | **435-446ms (now loses 1.08-1.11x)** | 257-262ms |

**Worse than round twenty's own regression, not better.** This falsifies
the hypothesis that motivated it: it wasn't specifically farthest-point
seeding's noise -- a fully deterministic, non-clustering, cheaper metric
hurt *more*. The real explanation, visible in hindsight: `overall` is a single
bounding cap -- exactly round one's original key design, which round two's
own history already found too coarse to scale as a *pruning* key ("the key
itself, not the split algorithm, is the bottleneck"). Using it for
`penalty()`'s placement decisions throws away the same fine-grained
sub-cap shape information round three's multi-cap redesign exists to
capture in the first place. `consistent()` still prunes against the full
stored multi-cap regardless of how the tree was built, but if placement
decisions during the incremental build could only see one coarse cap per
candidate subtree, the resulting tree shape is worse no matter how sharp
the final pruning test is once you're at a (badly-placed) leaf. The full
sub-cap merge, imperfect as its own comment admits, is apparently
load-bearing for placement quality -- impoverishing that signal, whether
by bad reseeding (round twenty) or by outright simplifying it away (round
twenty-one), hurts either way. **Reverted** (stashed, working tree and
installed extension back to the original `multicap_penalty()`).

**A methodological note worth recording**: this round's baseline pass
(same database, same code, rerun immediately after the "after" pass) 
measured `@>`/`<@` gist/pgsphere times both meaningfully different from
round twenty's own separately-run baseline on a *different* freshly-built
database with the same seeds and row counts (round twenty: `@>` 80-83ms,
`<@` gist 160-174ms/pgsphere 232-252ms; this round's baseline: `@>`
87-88ms, `<@` gist 172-197ms/pgsphere 257-262ms -- consistent in relative
terms, both confirming `<@` beats pgSphere, but not identical in absolute
terms). Cross-database absolute wall-clock numbers in this file should be
read as directionally consistent, not bit-for-bit reproducible; every A/B
comparison in this file that matters is a same-database, same-session,
back-to-back comparison for exactly this reason, and this round is a
concrete example of why that discipline matters, not just a stated
principle.

**Where this leaves the placement/union question**: two different attempts
to change how `merge_caps_greedy` (or what replaces it) feeds `penalty()`
have both regressed, for what looks like the same underlying reason --
placement quality needs the same rich sub-cap information pruning does,
so anything that simplifies or perturbs that signal for `penalty()`
specifically has hurt so far. Untried: leaving `penalty()`'s use of
`merge_caps_greedy` completely alone (proven, by omission, not to be the
problem on its own) and instead improving *only* the key actually written
to disk at split time -- i.e. scoping any future seeding/clustering
improvement to `multicap_union_many()` alone, via a separate function, so
it never touches `penalty()`'s signal at all. Neither of this round's two
experiments tested that -- round twenty's reseeding change hit both call
sites at once (so its regression can't be blamed on the union-time path
in isolation), and this round's change touched only `penalty()`. See §6
for how this is written up as an open direction.

## Round twenty-two: scoping the seeding fix to union() alone -- a real win,
## kept

The untried direction rounds twenty and twenty-one both pointed at:
farthest-point seeding for the greedy sub-cap merge, but applied *only* to
`multicap_union_many()` (the path that builds the key actually stored on
disk and read by `consistent()`), never to `multicap_penalty()` (the
placement signal both prior rounds found sensitive to any change). Added a
second function, `merge_caps_greedy_fp()` -- the same farthest-point
seeding logic round twenty tried -- called only from
`multicap_union_many()`; `multicap_penalty()` keeps calling the original,
untouched `merge_caps_greedy()`.

**Measured with the same true-A/B discipline, same-session/same-database
throughout this time** (learning directly from round twenty-one's own
methodological note):

50,000 rows / 500 probes:

| direction | before (original) | after (union()-only farthest-point) | pgSphere |
|---|---|---|---|
| `@>`(region,region) | 116-117ms | **51-52ms (2.2-2.3x faster)** | 19-24ms |
| `<@`(region,region) | 163-172ms (beats pgSphere 1.4-1.5x) | **134-136ms (beats pgSphere 1.76-1.91x)** | 236-257ms |

5,000 rows / 200 probes (`&&` included):

| strategy | before | after |
|---|---|---|
| `&&` | 7.25-7.30ms | 6.56-6.74ms (modestly faster, ~7-8%) |
| `@>`(region,region) | 5.83-6.32ms | 5.98-6.99ms (flat, within noise) |
| `<@`(region,region) | 10.29-10.88ms | 9.74-10.67ms (flat, within noise) |

**A genuine win at the scale that matters, no regression at the smaller
one.** `@>`'s persistent, widening gap to pgSphere -- unclosed since round
two, untouched by three prior targeted fixes (rounds fourteen, fifteen,
twenty) and actively worsened by two of them (twenty, twenty-one) --
narrows from ~4.9-5.2x to ~2.3-2.9x at 50,000 rows. `<@`'s existing win
over pgSphere widens too. The neutral (not negative) result at 5,000 rows
makes directional sense: fewer rows means a shallower tree and fewer
internal-node union operations for a tighter merge to compound across,
the same scale-dependence this file has seen before (round two's single-
cap gap itself "widened with scale" in the other direction).

**Correctness verified at both scales, all four strategies** (`&&`,
`@>`(region,point), `@>`(region,region), `<@`(region,region)): brute-force
count matches both the GiST-indexed and pgSphere-native count exactly in
every comparison run. `@>`(region,point) (round four's strategy, sharing
the same union path but not independently A/B'd this round) measured
67.72-69.94ms at 50,000 rows under the fix -- correctness-clean, no
"before" comparison taken since this round didn't target that strategy
specifically.

**Kept, not reverted** -- the first proven win in this specific
investigative thread (rounds fourteen, fifteen, twenty, twenty-one were
all reverted; this one measures as a clear net improvement with no
observed downside). `merge_caps_greedy()` (first-k seeding) remains
exactly as shipped, used only by `multicap_penalty()`; `merge_caps_greedy_
fp()` (farthest-point seeding) is new, used only by
`multicap_union_many()`. The `@>`(region,region) gap to pgSphere is
narrowed, not closed -- round two's original diagnosis (internal/leaf
pages still have overlapping bounding caps, just less so now) still
applies to whatever the farthest-point-seeded merge doesn't catch;
§6's remaining ideas (a sub-quadratic overlap test, the pgSphere-style
exact-box summary) are still open for whoever wants to keep closing it
further.

## Round twenty-three: a Lloyd's-style refinement pass on top of round
## twenty-two -- small real gain, not worth its cost, reverted

Round twenty-two's own flagged follow-up: `merge_caps_greedy_fp()`'s
assignment is a single greedy sweep, order-sensitive by construction (a
cap's cluster is locked in the moment it's absorbed, however the caps
later assigned would have changed the picture). Added a second,
"second-look" pass, safe here specifically because this function only
feeds `multicap_union_many()`'s stored key, never `multicap_penalty()`'s
placement decision (rounds twenty/twenty-one's whole reason for caution
doesn't apply to a change scoped this way). A true Lloyd's/k-means
reassignment step needs each cluster's union with a candidate cap
*removed*, which a cap union can't do cheaply (unlike a centroid, nothing
subtracts back out) -- approximated instead by comparing each cap's
*recorded* marginal cost at the moment it first joined against the
marginal cost of joining a different, now-fully-formed cluster, using the
`out[]` snapshot frozen at the end of the first pass; all reassignments
decided against that frozen snapshot and applied in one batch, then every
cluster rebuilt from its final membership. Same O(n * MAX_SUBCAPS) order
as the pass it follows.

**Correctness held** (brute-force count matches GiST and pgSphere exactly,
both strategies, 50,000 rows). **Wall-clock was contradictory enough to
distrust on its own**: the "before"/"after" same-session pass showed `@>`
apparently ~1.8x faster but `<@` apparently ~1.6x *slower* -- yet pgSphere's
own unmodified numbers moved by a similar magnitude between the two passes
(225-228ms -> 277-313ms for the same unchanged code), pointing at ambient
system noise contaminating the comparison rather than a real effect either
way (echoing, and now a second concrete instance of, round twenty-one's
same methodological caution).

**Switched to `EXPLAIN (ANALYZE, BUFFERS)` on the combined queries**, this
file's usual fallback when wall-clock is too noisy to trust (index
freshly rebuilt under each binary, same database, same corpus):

| strategy | buffers before | buffers after | change |
|---|---|---|---|
| `@>`(region,region) | 26,202 | 24,870 | -5.1% |
| `<@`(region,region) | 68,976 | 67,954 | -1.5% |

**A real, if modest, tightening -- but not obviously worth what it costs.**
Unlike round fifteen's `MAX_SUBCAPS` increase (which *also* improved
buffers 14-18% while regressing wall-clock 20-25%, purely from added
O(MAX_SUBCAPS²) `consistent()` cost paid on every query), this round's
extra cost lands somewhere different: a whole second O(n * MAX_SUBCAPS)
evaluation pass plus an O(n) rebuild, paid once per `union()`/split call
during index *build*, not per query. That should in principle be a much
cheaper place to spend extra cycles than round fifteen's per-query
tax -- but a 1.5-5% buffer reduction is also a much smaller win than round
fifteen's 14-18%, and the noisy wall-clock numbers gave no confident signal
that the tighter tree actually pays for the extra build-time work. Round
twenty-two's own gain (a real 2x+) came from fixing an actual bug in the
signal (`penalty()` contaminating the key-building path); round
twenty-three's gain is a genuine but marginal refinement of an
already-reasonable greedy heuristic, and marginal wins built on noisy
measurement aren't this file's bar. **Reverted** (stashed, working tree
and installed extension back to round twenty-two's shipped code).

**Where this leaves it**: `merge_caps_greedy_fp()` stays a single greedy
sweep, as round twenty-two shipped it. If someone wants to revisit
refinement passes specifically, the buffer-count numbers above are real
and worth taking as a starting point -- but this round's own reading is
that the returns from tuning `merge_caps_greedy_fp()`'s clustering quality
further are getting small, and §6's other two directions (a sub-quadratic
`consistent()` test, the pgSphere-style exact-box summary) look more
likely to move the `@>` gap further than another pass over the same
greedy-merge family.

## Round twenty-four: the pgSphere-style exact box -- theoretically sound,
## measured worse, reverted

The remaining untried idea from reading pgSphere's own `gist.c` (§ round
twenty-two/twenty-three): pgSphere's `spherekey` is a plain 3D
axis-aligned box, unioned by exact per-axis min/max -- lossless at every
tree level, unlike skycell's own capped sub-cap merge, bounded to
`MAX_SUBCAPS` entries and forced to approximate once more accumulate.
Added a `GistBox3D` (`lo[3]`, `hi[3]`) field to `GistMultiCap`, populated
via a closed-form exact bounding box of a spherical cap (not an
approximation -- for a cap at angular distance `theta` from an axis's
pole with radius `r`, the extreme coordinate on that axis is the pole's
own coordinate if the cap contains it outright, else `cos(theta -+ r)`,
a standard spherical-cap identity), unioned exactly (plain min/max,
`box_union_inplace()`) in `multicap_union_many()` alongside the existing
sub-cap merge.

**Scoped narrowly, informed directly by rounds twenty/twenty-one's
accumulated evidence**: the box feeds only `skyregion_gist_picksplit()`'s
cost metric (`box_area_proxy()`, Euclidean volume, replacing
`multicap_total_area()`'s sub-cap-area sum for both axis selection and
split-point choice) -- never `multicap_penalty()` (round twenty-one
showed a single-shape summary can actively hurt placement specifically)
and never `consistent()` (pruning stays on the sub-cap list exactly as
shipped). The idea: `multicap_total_area()` reflects `merge_caps_greedy_
fp()`'s own approximation, which can distort the very cost estimate
picksplit uses to choose a *good* split; an exact, never-approximated
volume should be a more reliable signal, mirroring how pgSphere's own
penalty function is described (in its `gist.c`) as scoring by box volume
growth.

**Correctness held.** But `EXPLAIN (ANALYZE, BUFFERS)` on the same
same-database, index-rebuilt-under-each-binary comparison this file has
used since round twenty-one's lesson about noisy wall-clock:

| strategy | buffers before (sub-cap area) | buffers after (box volume) | change |
|---|---|---|---|
| `@>`(region,region) | 25,874 | 29,725 | **+14.9% worse** |
| `<@`(region,region) | 68,365 | 74,304 | **+8.7% worse** |

**A clear regression on both strategies, not a wash and not noise --
reverted** (stashed; working tree and installed extension back to round
twenty-three's shipped state, which is round twenty-two's code). The
theoretical premise (exact composition beats approximate composition)
didn't translate into a better *picksplit cost function* in practice, and
the likely reason is the proxy itself: `box_area_proxy()`'s Euclidean
volume `(hi.x-lo.x)(hi.y-lo.y)(hi.z-lo.z)` is not a good stand-in for
"how much of the sphere this region actually covers" the way
`cap_area_proxy()`'s `1-cos(radius)` is a true monotonic spherical-area
proxy -- a box near a pole and a box of similar true angular coverage
near the equator can have very different Euclidean volumes purely from
where their coordinates sit in `[-1,1]`, and a thin band-shaped region's
box can have near-zero volume along one axis while covering a large
angular extent in the other two. `multicap_total_area()`, despite
reflecting an approximated sub-cap merge, is at least measuring something
proportional to real spherical area throughout; `box_area_proxy()` is
exact about the wrong quantity. **This isolates the actual gap, useful
for anyone revisiting this idea**: pgSphere's box union being exact isn't
by itself what makes it work for pgSphere -- pgSphere's own types
(`spoint`, `scircle`, `spoly`) are relatively compact/simple shapes where
Euclidean box volume tracks true coverage reasonably well across its
whole corpus; skycell's regions (in particular polygons decomposed into
several HEALPix-cell sub-caps scattered across a boundary) are a
different, more scattered shape family where that correlation breaks
down. A real spherical-area proxy for the box (something like the box's
own solid angle, more expensive to compute exactly, or a cheaper
monotonic approximation of it) is the natural next thing to try if this
idea is revisited -- not attempted here.

## Round twenty-five: closing out the pgSphere comparison for `&&` and
## `@>`(region,point) under round twenty-two's shipped code

Round twenty-two measured `@>`/`<@`(region,region) directly, since those
were what rounds twenty/twenty-one's regressions were about, but never
re-measured `&&` or `@>`(region,point) against pgSphere -- even though
both strategies share the exact same `multicap_union_many()` path that
changed. Filled that gap directly: fresh corpus, both scales, current
shipped code (round twenty-two's `merge_caps_greedy_fp()`, no later
round's reverted changes).

`&&` needs pgSphere's own number pulled from `21_region_overlap.sql`
(the MOC-ranges-recipe-vs-pgSphere script; its `pgsphere` row is
apples-to-apples with `22_region_gist.sql`'s `gist` row since both use
the same seed/probe construction -- same match count, `226`/`1349` at
5,000/50,000 rows respectively, confirming the two are comparable):

| strategy | 5,000 rows | 50,000 rows |
|---|---|---|
| `&&` | 6.54-6.89ms vs pgSphere 26.81-26.82ms (**~3.9-4.1x faster**, matches round three) | 63.70-65.07ms vs pgSphere 83.23-98.76ms (**~1.28-1.55x faster**) |
| `@>`(region,point) | 5.39-7.14ms vs pgSphere 2.95-3.06ms (~1.8-2.3x slower, matches round four) | 73.64-75.38ms vs pgSphere 16.30-18.24ms (~4.0-4.6x slower) |

Correctness verified at both scales for both strategies (brute-force
count matches GiST and pgSphere exactly: `226`/`1349` for `&&`,
`209`/`765` for `@>`(region,point)).

**A genuinely new finding, not something round twenty-two claimed**:
`&&` at 50,000 rows was skycell's one lingering loss to pgSphere since
round three (~1.2x slower, never closed by any of rounds four through
twenty-one). Round twenty-two's `merge_caps_greedy_fp()` fix -- measured
and justified purely by its effect on `@>`/`<@`(region,region) -- flipped
this too, apparently as a side effect of tightening the same shared
key-building path every strategy's `consistent()` prunes against.
`@>`(region,point) shows no comparable improvement, consistent with round
four's own diagnosis being a different mechanism entirely (pgSphere's
native point-in-shape test is inherently cheaper than a sub-cap loop,
not a key-tightness problem round twenty-two's fix could touch).

**Where this leaves the region GiST opclass vs. pgSphere, complete
picture as of round twenty-two's shipped code**: `&&` and `<@` are now
solid wins at both scales; `@>`(region,region) has a narrowed but real
gap (round twenty-two); `@>`(region,point) has a wide, structurally
explained, untouched gap (round four) that no round since has addressed
and that round twenty-two's fix does not reach.

## Round twenty-six: a sub-quadratic overlap test -- mathematically sound,
## measured worse on all four strategies, and why

The remaining untried item from round fifteen's own conclusion: the
O(MAX_SUBCAPS^2) all-pairs check in `multicap_overlaps()`/`multicap_
overlap_amount()` is what capped the profitable sub-cap count at 4 --
doubling it to 8 (round fifteen) quadrupled comparison cost and lost on
wall-clock despite better buffers. The proposed fix: sort each key's
valid sub-cap prefix by centre x coordinate at build time, then skip
pairs that provably cannot overlap at query time, using a real
(not approximate) geometric bound: for unit-vector centres, one
coordinate's difference is always <= the true chord (3D Euclidean)
distance between centres, and chord distance is monotonic in angular
distance -- so if `|a.sub[i].cx - b.sub[j].cx|` exceeds the chord of
`(a.sub[i].radius + max radius among b's sub-caps)`, that pair cannot
overlap, full stop, and (with `b` sorted ascending by `cx`) every `j`
past that point can't either.

Implemented: `cap_radius_chord()` (the chord identity), `sort_subcaps_
by_cx()` (insertion sort on the valid prefix, called from `region_to_
multicap()`'s polygon branch and `merge_caps_greedy_fp()`'s output --
the two places that finalize a *stored* `sub[]`), and rewrote both
overlap functions to walk the sorted lists with a skip-low/break-high
window instead of the naive double loop.

**Correctness held at 50,000 rows, all four strategies** (brute-force
count matches GiST and pgSphere exactly: `&&` 1349, `@>`(point) 765,
`@>`(region,region) 882, `<@`(region,region) 28186) -- the pruning bound
itself is exact, not approximate, so this isn't surprising.

**But buffer counts got consistently worse, not better, across all four**
(same-database comparison, index rebuilt under each binary):

| strategy | buffers before | buffers after | change |
|---|---|---|---|
| `&&` | 29,905 | 31,244 | +4.5% worse |
| `@>`(region,point) | 25,449 | 26,625 | +4.6% worse |
| `@>`(region,region) | 26,594 | 27,399 | +3.0% worse |
| `<@`(region,region) | 69,090 | 70,461 | +2.0% worse |

**Reverted** (stashed; working tree and installed extension back to
round twenty-five's shipped code).

**Why, and it's not the overlap test itself**: the pruning bound is
mathematically airtight (proven above, not just measured), so a skipped
pair genuinely never contributes to the boolean answer or the sum --
`multicap_overlaps()`/`multicap_overlap_amount()` return *exactly* the
same values sorted or not. The regression comes from an unintended
coupling with round twenty-two's `merge_caps_greedy_fp()`: its farthest-
point seeding is deliberately order-sensitive (`out[0] = caps[0]` --
the *first* input cap becomes the first seed, by design, and every
subsequent seed choice builds on that). `multicap_union_many()` builds
`merge_caps_greedy_fp()`'s input by flattening `entries[i]->sub[j]` in
entry order, so `flat[0]` is `entries[0]->sub[0]` -- and sorting
`entries[0]`'s own `sub[]` by `cx` (this round's whole change) changes
*which cap that is*, compared to round twenty-two's original order
(whatever the earlier seeding/assignment pass happened to produce).
Since a stored key's `sub[]` becomes some *future* call's flattened
input the next time it's read back during an insert or split, sorting
for query-time benefit silently reseeds every downstream clustering
decision -- not incorrectly (the resulting multi-cap is still a valid
summary either way), just *differently*, and this round's specific
different tree shape happened to be worse on all four metrics tested.

**What this rules out, and what it doesn't**: this isn't evidence the
sub-quadratic *technique* is unsound -- it's evidence that ordering
changes to `sub[]` are not "free" the way they'd naively look, given
round twenty-two's order-sensitive seeding is now load-bearing shipped
behavior. A version that kept clustering's input order untouched (e.g.
sorting only a serialization-time copy that never feeds back into a
future `merge_caps_greedy_fp()` call, or making the seeding itself
order-independent) could still work -- neither was attempted here, and
the first would need a second, unsorted copy of `sub[]` carried
alongside the sorted one (doubling that part of the key, undermining
the cheapness that made this idea attractive), while the second means
touching round twenty-two's proven seeding logic again, which this
file's own track record (four reverted attempts, rounds twenty through
twenty-four, all centered on this exact function family) suggests
doing carefully, not as a quick follow-up.

## Round twenty-seven: a trig-free point-in-cap test -- a real, kept win
## on the one strategy nothing had touched

Every round from fourteen through twenty-six targeted `@>`/`<@`(region,
region) -- tree/key quality for the CONTAINS_REGION/CONTAINED_BY_REGION
strategies. `@>`(region,point) (CONTAINS_POINT) had sat untouched since
round four's own diagnosis: a wide, scale-independent gap to pgSphere
("pgSphere's cheap native point-in-shape test pulls further ahead...
than pgSphere's own `&&` does of its own"), confirmed still wide and
unmoved by round twenty-two's key-tightening fix (round twenty-five).
That diagnosis pointed at *per-comparison cost*, not tree shape -- a
genuinely different kind of lever from anything tried in rounds
fourteen through twenty-six.

`multicap_contains_point()` tested each sub-cap via `cap_overlaps(cap,
cap_make(p, 0.0))`, which calls `sc_angle()` -- a cross product, a
`sqrt`, and an `atan2`, computing the true angular distance so it can be
compared against a *sum* of two radii (needed for genuine cap-vs-cap
tests). A point is a zero-radius cap, so that sum collapses to just the
cap's own radius, and `angle(centre, point) <= radius` is exactly
`dot(centre, point) >= cos(radius)` (`cos` monotonic decreasing on
`[0, pi]`) -- one dot product and one cosine, no cross product, no
`sqrt`, no `atan2`. Added `cap_contains_point()` implementing exactly
that, used only inside `multicap_contains_point()`; `cap_overlaps()`
itself, the stored key format, and every seeding/union/penalty function
are untouched.

**Correctness held at both scales** (brute-force count matches GiST and
pgSphere exactly: 209 at 5,000 rows, 765 at 50,000). **Buffer counts
came back nearly identical** (25,050 vs 25,557 at 50,000 rows, +2.0%,
almost certainly GiST build non-determinism rather than a real
algorithmic difference -- expected, since this change can't alter any
`consistent()` boolean answer, hence can't alter tree-traversal
decisions). **Wall-clock, same-database same-session, both scales**:

| scale | before | after | speedup |
|---|---|---|---|
| 5,000 rows | 5.10-6.21ms | 3.06-3.27ms | ~1.6-1.9x |
| 50,000 rows | 83-85ms | 47-50ms | ~1.7-1.8x |

**A real, consistent, mechanism-precise win -- kept.** The gap to
pgSphere narrows sharply: at 5,000 rows, from ~1.7-2.1x slower to
~1.1-1.2x slower, nearly closing it entirely; at 50,000 rows, from
~4.0-4.6x (round twenty-five) to roughly ~3.4-3.9x (pgSphere's own
13-15ms unchanged, skycell's gist time dropped from 73-95ms to 47-50ms).
Not fully closed -- pgSphere's own native point test is presumably doing
something comparably cheap already, so this narrows the gap rather than
flipping it -- but the largest single-round improvement to this specific
strategy since it was first measured in round four, achieved without
touching anything in the fragile seeding/merge family six of the last
seven rounds have been about.

**What's left for `@>`(region,point)**: the remaining gap is most likely
still about comparison *count* (how many sub-caps/nodes get visited),
not comparison *cost* anymore -- i.e. back to a tree-tightness question,
same family as `@>`(region,region)'s remaining gap. Untried: checking
whether round twenty-two's `merge_caps_greedy_fp()` fix, measured only
for `@>`/`<@`(region,region) and (round twenty-five) `&&`, also
meaningfully tightened the tree `@>`(region,point) walks -- if the
comparison-count side is already about as good as it gets, the
remaining multiplier is likely structural (pgSphere's index descent
shape vs. skycell's), which would make this gap's floor close to
already reached.

## Round twenty-eight: round twenty-two also tightened @>(region,point)'s
## tree -- confirmed, but purely explanatory, not a new lever

Round twenty-seven's own closing question: does round twenty-two's
`merge_caps_greedy_fp()` fix -- measured only for `@>`/`<@`(region,region)
and (round twenty-five) `&&` -- also meaningfully tighten `@>`(region,
point)'s tree, separately from round twenty-seven's own per-comparison
speedup? Pure measurement, no code change: temporarily reverted
`multicap_union_many()`'s call from `merge_caps_greedy_fp()` back to the
original `merge_caps_greedy()` (round twenty-two's own change, in
isolation), rebuilt the index under each binary, same database.

| scale | buffers before round 22 | buffers after round 22 | change |
|---|---|---|---|
| 5,000 rows | 2,619 | 1,946 | **-25.7%** |
| 50,000 rows | 34,117 | 26,036 | **-23.7%** |

Correctness held at both scales throughout (already established;
re-confirmed here). **Yes, confirmed -- round twenty-two's fix was never
scoped to the region-region strategies specifically; it tightened
`@>`(region,point)'s tree by roughly a quarter at both scales, a real
effect never separately measured until now.** That this barely showed up
in round twenty-five's wall-clock comparison to pgSphere makes sense in
hindsight: round twenty-seven's per-comparison cost (`sc_angle()`'s cross
product/`sqrt`/`atan2`, paid once per sub-cap at *every* visited page) was
the dominant term, so a ~24-26% reduction in *how many* pages get visited
was a much smaller lever on total wall-clock than removing the
per-comparison cost itself turned out to be.

**Not a new performance change -- purely explanatory.** Both rounds
twenty-two and twenty-seven were already shipped before this measurement;
this doesn't alter any code or add any new speedup on top of what's
already measured. Its value is in closing round twenty-seven's own open
question cleanly: `@>`(region,point)'s remaining gap to pgSphere is not
"one lever pulled, one still sitting on the table" -- both the tree-
tightness lever (round twenty-two, confirmed here) and the per-comparison-
cost lever (round twenty-seven) have already been pulled for this
strategy. Whatever gap remains (~3.4-3.9x at 50,000 rows, round
twenty-seven) is more likely to be structural -- something about
pgSphere's own index descent shape for point-in-shape queries -- than a
further-untapped fix inside this file's own code, though that's an
inference from elimination, not independently verified.

## Round twenty-nine: caching cos(radius) for cap_overlaps()/cap_area_
## proxy() -- exact, correct, and still a net loss, via key bloat

Round twenty-seven's trig-free point test doesn't apply to genuine
cap-vs-cap comparisons (`cap_overlaps()`, used by `&&`, `@>`(region,
region), `<@`(region,region) via `multicap_overlaps()`), since those
compare against a *sum* of two radii, not one. But the angle-sum identity
`cos(ra+rb) = cos(ra)cos(rb) - sin(ra)sin(rb)` gives the same kind of
shortcut IF `cos(radius)` is available cheaply for each cap -- so added
`cos_radius` as a new field on `GistCap`, computed once in `cap_make()`
(the same clamped `cos(min(radius, pi))` value `cap_area_proxy()` used to
recompute fresh every call, so that function got the same optimization
for free). `cap_overlaps()` rewritten to use `dot(a,b) >= cos(ra+rb)`
via the identity, with `sin` derived from the cached `cos` via
`sqrt(1-cos^2)` rather than a second trig call.

**A real correctness subtlety caught before shipping, not after**: the
angle-sum identity only stays valid while `ra+rb` remains in cos's
monotonic range `[0, pi]` -- past `pi`, `cos(ra+rb)` starts increasing
again, which would silently produce false negatives for near-antipodal
caps. This isn't a theoretical edge case here: `cover.c` clamps a single
CONE region's own radius up to `M_PI` (not `M_PI/2` -- only *polygons*
are restricted to smaller-than-a-hemisphere), so two large circular
regions alone, no unioning required, can have `ra+rb` exceed `pi`.
Guarded explicitly (`if (a.radius + b.radius >= M_PI) return true;` --
correct, since every angle is trivially `<= pi <= ra+rb` once that
holds) rather than trusted to the formula.

**Correctness verified two ways**: the standard 50,000-row suite (all
four strategies, brute-force counts matching GiST and pgSphere exactly),
and a dedicated stress test built specifically to exercise the guard --
400 circular regions with radii 30-179 degrees (well past the `pi/2`
range where the old and new formulas start being able to diverge if the
guard were wrong), full self-join for `&&`/`@>`(region,region) and a
2,000-point cross-check for `@>`(region,point): brute force matched GiST
exactly in every case (76,169 `&&` pairs, 18,728 `@>` pairs, 487,742
point-containment pairs).

**But the key grew** -- one more `double` per cap, 5 caps per
`GistMultiCap` (`overall` + `sub[4]`), 40 bytes more per stored key,
confirmed directly: index size 17MB -> 21MB, ~24% bigger, matching the
~24% key-size growth almost exactly. Same-database, index rebuilt under
each binary:

| strategy | buffers before | buffers after | change |
|---|---|---|---|
| `&&` | 29,353 | 33,432 | +13.9% worse |
| `@>`(region,region) | 25,937 | 29,286 | +12.9% worse |
| `<@`(region,region) | 68,355 | 73,685 | +7.8% worse |

Wall-clock was mixed, not a clean win either way (`&&` worse, `@>`/`<@`
each modestly better) -- consistent with a real per-comparison speedup
being mostly offset, and for `&&` outweighed, by visiting more pages.
**Reverted** (stashed; working tree and installed extension back to
round twenty-eight's shipped state).

**Exactly the risk flagged before implementing, now confirmed a fourth
time**: rounds two, fifteen, and twenty-four all found that growing the
stored key costs real fanout, and this round's own regression is the
same mechanism precisely (not a bad proxy this time -- the computed
*values* are provably identical to the original, byte-for-byte-equivalent
math, confirmed by every correctness check -- just a bigger key). Round
twenty-seven's point-test win avoided this entirely by deriving
everything from the *already-stored* radius with zero new storage; this
round's version needed to cache a value to get the same class of
speedup, and that caching is what cost it. Left as an open question, not
attempted: whether a NON-cached version (recomputing `cos`/`sin` fresh
per comparison, no format change at all) could still win on pure
instruction count against `sc_angle()`'s cross-product/sqrt/atan2 chain
-- unlikely on a rough operation-count basis (four trig calls instead of
one atan2 plus one sqrt), and not measured here since the whole point of
caching was to avoid paying for `cos`/`sin` more than once per cap.

## Round thirty: trying to calibrate `cover.c`'s `split_cost` -- no net
## win found, and two measurement confounds caught along the way

This round is about `cover.c`, not `gist_region.c` -- `skycell.probe_orders`
(default 0, off) is a mechanism that, when enabled, probes 1-N covering
orders finer than the closed-form starting order and scores each via
`rlist_score()`: `l->n * range_cost + rows + steps * split_cost`.
`split_cost` defaults to `1.0`, and the code's own comment says this is
wrong -- it treats "examining one cell while building the covering" as
costing the same as "fetching one false-positive row", and prescribes
calibrating it against measurement the way `range_cost` already is
(`auto_range_cost()`, derived from PostgreSQL's own planner constants).
Unlike `range_cost`, there's no PostgreSQL-native cost to borrow from for
`split_cost` -- covering computation is a pure skycell-internal,
in-memory, planning-time operation -- so this had to be measured directly.

Both `skycell.split_cost` and `skycell.probe_orders` are live GUCs
(settable via `SET`, no rebuild needed), which made this look like a
cheap experiment: build a query-center corpus at the docstring's own
cited "6'-30'" radii on a 10M-row corpus, sweep `split_cost` with
`probe_orders=3`, and see which value wins on wall-clock.

**First attempt -- sequential blocks, invalidated by a ~40% time drift.**
Running each `split_cost` value as a full block of 90 queries, one block
after another, repeating the whole grid twice as a sanity check: the
*second* pass of every identical configuration came in ~40% faster than
the first, with no relationship to the actual `split_cost` value --
purely a function of position in the run. At sub-millisecond
per-query times, some combination of cache warming and system-level
drift dominates any real signal in a sequential A/B block design.

**Second attempt -- interleaved by center, invalidated by a
within-cycle position bias.** Redesigned to cycle through all 6 variants
for each query center before moving to the next center (so no single
config is systematically "later" in the overall run). This looked like
it worked -- and produced an eye-catching result: `probe_orders=3` at
the *shipped* `split_cost=1.0` beat `default` (`probe_orders=0`) by 18%
at 6' radius, with the win shrinking to ~6% at 15'/30' and lower
`split_cost` values trailing off worse than `default`. Paired by query
center (t ~ 5.3), this looked like a real, significant effect -- until
checked against `skycell_cover_info()`: for the 6' sample, the chosen
order changed in only 3 of 30 queries, with `nranges` essentially
unchanged (2.07 -> 2.07 average). An 18% wall-clock difference with a
90%-of-the-time-identical covering decision isn't plausible as a real
effect of the GUC. The actual cause: `default` was *always first* in
each 6-variant cycle for a given center, so it alone paid the cold-buffer
cost of that center's first touch in the cycle, while the other five
variants inherited warm buffers from `default`'s own touch moments
earlier -- a pure position artifact, not a `split_cost` effect.

**Third attempt -- order shuffled independently per (repetition, query
center), the fix that held up.** With the 6-variant visit order
re-randomized on every cycle so no variant is systematically
first-in-cycle, the effect mostly vanished: `default` vs `probe_orders=3`
at `split_cost=1.0` or `0.3` came back statistically indistinguishable
(paired difference 0.0028ms and -0.0017ms respectively, both well under
one standard error over 90 paired centers x 8 repetitions). Lower
`split_cost` values (0.1, 0.05, 0.0) came back measurably *worse* than
`default` (paired difference -0.0095 to -0.0173ms, `default` vs `0.0`
significant at t ~ -6.4).

**Buffer counts -- immune to timing noise, and telling a real,
consistent story that wall-clock doesn't fully reflect.** Independent of
all three wall-clock attempts, `EXPLAIN (ANALYZE, BUFFERS)` (second run
per query, so buffers reflect warm state for every variant equally) shows
a genuine, monotonic, noise-free reduction as `split_cost` drops from
`1.0` to `0.0`, holding at every radius from 6' to 30':

| radius | default | sc=1.0 | sc=0.3 | sc=0.1 | sc=0.05 | sc=0.0 |
|---|---|---|---|---|---|---|
| 6' | 29.80 | 29.60 | 27.93 | 27.73 | 27.73 | 27.60 |
| 15' | 42.80 | 42.47 | 41.47 | 40.53 | 39.80 | 39.73 |
| 30' | 109.07 | 106.47 | 104.40 | 103.20 | 103.13 | 103.13 |
| 60' | 262.10 | 260.60 | 260.20 | 260.20 | 260.20 | 260.20 |
| 120' | 355.30 | 355.30 | 355.30 | 355.30 | 355.30 | 355.30 |

(90 centers at 6'/15'/30', 20 at 60'/120', avg buffers/query.) So the
docstring's underlying premise is correct as far as it goes: `split_cost`
lower than `1.0` really does make the mechanism pick finer, tighter
coverings more often, and that really does touch fewer buffers -- a
genuine 6-8% reduction at 6'-30' radii, saturating to nothing by 60' and
vanishing entirely by 120' (matching the docstring's own cited "6'-30'"
window almost exactly -- past that, the closed-form order is apparently
already about as good as probing can find).

**But the buffer win doesn't show up as a wall-clock win, because
`probe_orders` itself has a fixed planning-time cost that a `split_cost`
tweak alone can't pay for.** `skycell_cover_info()`'s `steps` field
(the covering algorithm's own step counter) jumped from 18 to 79 for one
sample query when `probe_orders` went from 0 to 3 -- with the *chosen
order unchanged* -- confirming that probing 3 extra candidate orders
costs real, fixed C-level work independent of whether it changes the
final decision or of what `split_cost` is set to (`probe_orders=3` was
held constant across the whole buffer sweep above; only `split_cost`
varied, and the buffer curve moved smoothly while this fixed cost did
not). At this corpus/query scale -- sub-millisecond queries touching
tens to low hundreds of buffers -- that fixed planning tax outweighs the
handful of buffers saved by a better covering choice. A real net win
would need either a cheaper probing loop (fewer or smarter candidate
orders, not a fixed 3) or a context where the row-fetch savings are
large enough to dominate the fixed probing cost (a much larger `radius`
x `rows-per-cell` product than this corpus's 6'-30' sweet spot has, and
by 60'/120' the buffer savings themselves have already vanished, so
there's no wider-radius escape hatch either).

**No code change made.** `skycell.split_cost` stays at its shipped
default (`1.0`), `skycell.probe_orders` stays off (`0`) -- calibrating
`split_cost` doesn't unlock the win the docstring anticipates, because
the mechanism's own fixed overhead, not the cost-model imbalance, is
what's actually blocking it. This is a negative result, but a real one:
worth recording so a future attempt doesn't re-derive the same dead end,
and doesn't repeat either of the two measurement confounds this round
had to find and rule out first (sequential-block time drift, and
within-cycle first-position cache bias) before trusting any A/B number
at this query scale.

## Round thirty-one: does a shallower probe fix it? No -- but it does
## prove the deeper probe is pure waste

Round thirty found `probe_orders`'s own fixed planning cost, not
`split_cost`'s value, is what blocks a net win. The natural follow-up:
`probe_orders=3` lets `sc_cover_compute()` try up to three orders finer
than the closed-form starting point (`k0..k0+3`), each a full independent
re-enumeration (`cover_cone_direct()` called fresh per candidate, "about
four times the cells of the one just done" per the code's own comment).
The loop already breaks the moment a candidate stops improving
(`rlist_score()` not beating the running best), so in practice most
queries never reach `k0+2` or `k0+3` -- but every query that tries even
one losing candidate before breaking pays for a full wasted enumeration.
Does capping the probe at depth 1 (`k0..k0+1`, at most one extra
enumeration ever) recover any of that waste as a net win?

**Buffers: `probe_orders=1` and `probe_orders=3` are byte-identical at
every radius tested**, 6' through 120', at both `split_cost=1.0` and
`0.3` (five radii, both split_cost values, same 130-center corpus as
round thirty). Not "close" -- exactly equal, to the buffer. This proves
directly, not just by code inspection, that for every query in this
corpus the loop never successfully improves twice in a row: either
`k0+1` doesn't beat `k0` (breaks immediately, `probe_orders=3` couldn't
have gone further anyway), or `k0+1` beats `k0` but `k0+2` doesn't beat
`k0+1` (so `probe_orders=3` tries one more full enumeration than
`probe_orders=1` and gets nothing for it). Either way, depth beyond 1 is
pure planning-time waste here, with zero quality difference.

**Wall-clock (same randomized-per-repetition methodology round thirty
had to adopt to trust the result): the direction matches but the margin
doesn't clear noise, and the core conclusion is unchanged.**
`probe_orders=3` came in slower than `probe_orders=1` at the same
`split_cost` (paired diff 0.0087ms at `split_cost=1.0`, `t ~ 0.9` over
130 paired centers x 10 repetitions) -- consistent with the wasted
enumeration, but not significant at this scale. More importantly,
neither `probe_orders=1` nor `probe_orders=3` beat plain `default`
(probing off entirely) by a margin distinguishable from noise (`t ~ 0.5`
for `default` vs `probe_orders=1` at `split_cost=1.0`). Cutting the probe
to its cheapest possible depth removes the *proven* waste but still
doesn't clear the bar against not probing at all.

**No code change.** If `skycell.probe_orders` is ever turned on, there's
no reason to set it above `1` -- proven zero quality cost either way,
some planning time back, directionally consistent even where not
individually significant. But that's operator guidance for a GUC that
ships off, not a case for changing the default: round thirty's
conclusion holds at the cheapest depth too. `skycell.probe_orders` stays
at `0`, `skycell.split_cost` stays at `1.0`.

## Round thirty-two: could BRIN replace the B-tree for cone search?
## No -- lossy block-level summaries can't match row-exact ranges

Went looking directly in pgSphere's source (`gist.c`, `gist_support.c`,
`gq_cache.c`, `key.c`, `brin.c`) for whatever else might explain its
performance, beyond the Box3D/R-tree architecture already characterised.
Two things confirmed dead ends immediately: pgSphere's `gq_cache.c`
caches the query's derived box key across a whole index scan to avoid
re-running `gen_key()`'s trig on every node -- `gist_region.c`'s own
`region_gist_query_cache` (in `fn_extra`, keyed on the query's *bytes*,
not pointer identity -- more robust than pgSphere's own version) already
does exactly this, since round one. And pgSphere's per-node pruning test
being a plain axis-aligned-box overlap (`spherekey_inter_two()`, six
comparisons, no trig at all) rather than a cap comparison is precisely
what round twenty-four's `GistBox3D` field tried adding to the region
GiST key, and reverted for the same key-bloat/fanout cost every
size-growing attempt in this file has hit.

The one genuinely new thing pgSphere's source surfaced: it also ships a
BRIN opclass (`brin.c`), built on the same box/union primitives but
summarizing per *block range* rather than per row. This is architecturally
unrelated to anything compared in this whole investigation (GiST, GIN,
SP-GiST, B-tree) and specifically suited to naturally spatially-clustered
tables -- worth checking directly against skycell's own B-tree covering
path (`cover.c`), which has no BRIN counterpart at all.

**Tested under BRIN's own best case, not a strawman.** A BRIN index only
has a chance when the table's physical row order correlates with the
indexed column, so the 10M-row corpus was explicitly `CLUSTER`ed on
`cell` first (`CLUSTER cat_cell USING cat_cell_idx`) before building
`brin(cell)` -- the synthetic catalogue's natural load order (population
group, then insertion order within each) has no such correlation
otherwise. Compared the existing B-tree against BRIN on that *same*
physically-clustered table (the B-tree dropped and rebuilt around the
BRIN test so only one index services the query at a time), across the
same 130-center, 6'-120' corpus rounds thirty/thirty-one used.

**BRIN loses decisively, at every radius, even here:**

| radius | btree avg buffers | brin avg buffers (ppr=32) | brin avg ms | btree avg ms |
|---|---|---|---|---|
| 6' | 31.87 | 185.83 | 1.30 | 0.22 |
| 15' | 44.33 | 225.40 | 1.61 | 0.24 |
| 30' | 114.33 | 443.60 | 3.51 | 0.54 |
| 60' | 323.00 | 1036.60 | 7.36 | 2.28 |
| 120' | 433.25 | 1143.45 | 7.81 | 3.22 |

~3.3x more buffers and ~3.5x slower overall (130 centers), consistently
across radii -- not close at any scale tested. A finer `pages_per_range`
(4 instead of the default-ish 32) made it markedly *worse* (2561 avg
buffers, ~8x worse than the B-tree), not better: more, smaller ranges
means more separate BRIN entries to visit for the same physical span,
without shrinking the per-range false-positive rate enough to pay for it.

**Mechanism, seen directly in `EXPLAIN (ANALYZE, BUFFERS)`:** `Heap
Blocks: lossy=128` and `Rows Removed by Index Recheck: 15308` for a
single 15' query. BRIN's bitmap is *lossy* -- it can only say "this whole
page might contain a match," not name the matching rows the way a
B-tree's exact TID bitmap does, so every flagged page's rows all get
pulled and rechecked against the *range* condition itself (before the
geometric filter even runs). skycell's own covering computation already
produces tight, near-minimal cell ranges (the whole point of
`cover.c`'s cost model); collapsing that per-row precision down to
whatever a 32-or-4-page block happens to contain throws away exactly the
precision the covering algorithm worked to produce, and a real match
region essentially never aligns with block-range boundaries, so some
fraction of every flagged block is always pure waste. No page-range size
fixes this -- it's structural, not a tuning problem.

**No code change; nothing to ship.** `cat_cell_idx` (B-tree) remains the
only index skycell needs for the cone-search path. BRIN isn't a
dead-simple free alternative for this workload even under its own best
conditions (deliberately clustered data) -- it would only be worth a
second look for a use case BRIN is actually built for (a table too large
for a B-tree to be worth maintaining at all, trading row-level precision
for near-zero index size/maintenance cost), which isn't what any of this
investigation's benchmarks are testing for.

## Round thirty-three: does BRIN cross over at lower selectivity? Yes --
## on buffers, cleanly; on wall-clock, only past a point

Round thirty-two only tested 6'-120' radii -- all under 0.03% of the sky,
squarely B-tree territory. Reasonable pushback: BRIN's real proposition
is beating a *sequential scan* at low selectivity, not beating a
highly-selective B-tree lookup, so swept radius up into the range where
that's actually the comparison (1 degree to 90 degrees, ~0.008% to 50%
of the sky), on the same `CLUSTER`ed-by-`cell` table, planner's own
choice both times (not forced), plus a GUC-forced sequential scan as a
reference floor.

**Buffers cross over cleanly around 15-20 degrees (~2-3% of the sky) and
BRIN's lead grows from there:**

| radius | selectivity | btree buffers | brin buffers | btree ms | brin ms |
|---|---|---|---|---|---|
| 3d | ~0.07% | 913.5 | 2016.8 | 3.56 | 18.63 |
| 8d | ~0.49% | 7407.0 | 7447.5 | 36.67 | 42.26 |
| 12d | ~1.1% | 16457.5 | 18400.0 | 362.91 | 416.77 |
| **20d** | **~3.0%** | **27718.0** | **25728.0** | 425.66 | 396.89 |
| 45d | ~14.6% | 93760.5 | 71174.0 | 573.29 | 581.15 |
| 90d | 50% | 266046.5 | 189234.0 | 907.05 | 975.09 |

29% fewer buffers for BRIN by 90 degrees. The mechanism: `skycell.
max_ranges` (64) caps the covering's OR'd range count from ~12 degrees
on, so past that point the number of ranges is fixed but each range
covers more rows as radius grows. A B-tree's bitmap-build cost scales
with how many leaf pages it has to visit to name every matching row in
each range (proportional to result size); BRIN's 96KB index costs
almost the same tiny, fixed amount to scan regardless of how many rows
a range matches. The crossover is exactly where per-range result size
gets big enough for that difference to dominate -- the textbook
BRIN-vs-B-tree condition, not a `cell`-covering quirk.

**But wall-clock doesn't track the buffer win as cleanly, and sometimes
reverses (90 degrees: B-tree 907ms vs BRIN 975ms despite 29% fewer
buffers).** `EXPLAIN` shows why: BRIN's bitmap goes lossy at these
scales (`Heap Blocks: lossy=5688` at 45 degrees), forcing ~39,000 extra
rows through a CPU-bound *index recheck* (re-testing the raw cell-range
condition itself, on top of the real geometric filter) that the
B-tree's exact-TID bitmap never pays -- confirmed directly: the B-tree's
own plan at the same 45-degree query shows `Heap Blocks: exact=4339`,
not lossy at all, so its bitmap-build genuinely stays precise even at
this scale. BRIN trades I/O for CPU: fewer buffer touches, real extra
per-row recheck work. Whether that nets out ahead depends on whether
the workload is I/O-bound (cold cache, disk-bound) or CPU-bound (warm
cache, like this measurement) -- a disk-bound production workload would
plausibly see a cleaner win than this warm-cache test shows.

**Still no code change** (this is a physical-design/DBA choice, not
something `cover.c` can decide at plan time -- it would need to pick
between two *different indexes*, not tune a cost constant), but the
conclusion is now sharper than round thirty-two's: BRIN is a real,
usable alternative for skycell's own cone-search path specifically for
wide, low-selectivity queries (roughly >2-3% of the sky) on a table
that's physically clustered by `cell` -- worth an operator's
consideration for that specific query shape, not a blanket "no."

## Round thirty-four: pgSphere's actual BRIN use case isn't cone search
## at all -- it's rectangular RA/Dec box queries, and there it wins clean

Checked pgSphere's own BRIN test suite (`sql/spoint_brin.sql`,
`sql/sbox_brin.sql`) rather than assuming its `brin.c` generalizes to
every shape. It doesn't: pgSphere's BRIN opclass is registered only for
`spoint` and `sbox`, and both test files exercise exactly one query
shape -- `spoint <@ sbox` / `sbox <@ sbox`, a rectangular RA/Dec box
range (`sbox '((10d,10d),(20d,20d))'`), never a circle or polygon. This
makes structural sense: an axis-aligned rectangle in coordinate space is
already what BRIN's per-block min/max summary represents natively --
zero lossy conversion, unlike a circle needing Euler-rotated boundary
vertices to even approximate a box (round thirty-two/three's whole
`Heap Blocks: lossy=...` story). So the fair analogue to what pgSphere
actually ships isn't cone search on `cell` at all -- it's a rectangular
`ra`/`dec` range query directly against skycell's plain columns, not the
HEALPix covering machinery.

**Tested it: composite B-tree(ra, dec) vs multi-column BRIN(ra, dec)**,
same 10M-row corpus, box half-widths from 0.1 to 30 degrees (48 centers,
8 sizes), on the table as already `CLUSTER`ed by `cell` from round
thirty-two/three (not re-clustered for this -- see below for why that
matters):

| half-width | btree buffers | brin buffers | btree ms | brin ms |
|---|---|---|---|---|
| 0.1d | 54.0 | 794.0 | 0.38 | 4.57 |
| 0.5d | 1153.5 | 964.7 | 1.77 | 6.14 |
| 3d | 3533.5 | 1882.0 | 15.03 | 10.75 |
| 6d | 5620.8 | 3947.0 | 24.78 | 17.85 |
| 10d | 9682.0 | 4245.7 | 38.57 | 24.41 |
| 20d | 179518.7 | 19044.0 | 146.10 | 42.87 |
| 30d | 333336.0 | 44000.0 | 262.03 | 103.08 |

**Decisively cleaner than the cone-search case, on both axes at once.**
BRIN wins buffers from 0.5 degrees on and wall-clock from ~3 degrees on
-- no CPU-bound reversal at the wide end this time (9.4x fewer buffers
and 2.5x faster wall-clock at 30 degrees, where the composite B-tree's
plan has degenerated to a full sequential scan, `333336` buffers
matching round thirty-three's forced-seqscan reference exactly). The
reason wall-clock tracks buffers cleanly here, unlike the cone case: a
box query's "recheck" *is* the query's own predicate (`ra BETWEEN ...
AND dec BETWEEN ...`) -- there's no separate, more expensive geometric
filter riding on top the way `skycell_in_cone()`'s trig-based distance
test does for a circle. BRIN's lossy per-block recheck here costs
almost nothing extra per row; for a circle it costs a real trig call.

**One more thing worth knowing, found while checking whether
re-clustering for BRIN's textbook best case would do even better**: it
doesn't, cleanly -- re-clustering the table by `CLUSTER ... USING
(ra, dec)` (rather than leaving it clustered by `cell`) made BRIN(ra,
dec)'s wall-clock *worse* in the 0.5-10 degree range (still fewer
buffers, but slower: e.g. 1.5 degrees, 1754.0 buffers / 9.26ms vs the
`cell`-clustered table's 954.0 buffers / 4.60ms for the same box size),
only overtaking the `cell`-clustered result again at 20+ degrees. The
likely reason: clustering by a *composite* B-tree order effectively
sorts by its first column only (`ra`, since two rows essentially never
tie on a float), leaving `dec` uncorrelated with physical position
within any given block -- so a BRIN block's `dec` summary ends up close
to the full possible range regardless of how tight the `ra` summary is,
starving exactly one of the two dimensions a box query needs pruned.
`cell`'s HEALPix ordering, imperfect as any single linearization of a
2D surface must be, still preserves some joint locality in *both*
axes at once, which a naive multi-column composite-index `CLUSTER`
doesn't. Not chased further to a precise mechanism (the corpus's sharp
density contrast and RA's coordinate degeneracy near the poles make a
clean per-block locality metric hard to construct honestly), but the
headline is solid: skycell's existing `cell`-based physical clustering
-- already the natural choice for its own cone-search path -- turns out
to serve a BRIN(ra, dec) box index reasonably well too, with no separate
clustering scheme needed.

**No code change** -- this confirms a real, structural pgSphere
advantage for a query shape skycell has no dedicated support for at all
(a plain coordinate-rectangle region type), not a gap in the cone-search
or region-GiST machinery this file is about. Worth a future look if
rectangular sky-cutout queries are a real workload: skycell has no `sbox`
equivalent, and a plain `ra`/`dec` B-tree or BRIN on the existing columns
already works today without any extension change, exactly as measured
here -- adding a dedicated box region type would only be worth it if the
plain-column approach's lack of HEALPix integration (no single covering
codepath shared with the cone/polygon operators) becomes a real
maintenance or composability problem, which hasn't been shown yet.

## Round thirty-five: one more pgSphere check -- the exact per-row
## distance math itself -- and this one goes skycell's way already

Three rounds of mining pgSphere's source (architecture/caching in the
original investigation, BRIN in rounds thirty-two through thirty-four)
raised one more candidate worth checking directly rather than assuming:
the actual exact-recheck math pgSphere runs on a surviving candidate row
-- the per-row cost that mattered so much in round thirty-four's finding
(a box query's recheck is free, a circle's costs a real trig call).

pgSphere stores points as `(lat, lng)` (`SPoint`), and its distance
function is Vincenty's formula, computed fresh on every call:

```c
float8
spoint_dist(const SPoint *p1, const SPoint *p2)
{
	float8	dl = p1->lng - p2->lng;
	float8	f = atan2(norm2(cos(p2->lat) * sin(dl),
						cos(p1->lat) * sin(p2->lat)
							- sin(p1->lat) * cos(p2->lat) * cos(dl)),
					sin(p1->lat) * sin(p2->lat)
						+ cos(p1->lat) * cos(p2->lat) * cos(dl));
	...
}
```

Six trigonometric calls (four `cos`, two `sin`) plus a `sqrt` (inside
`norm2`) and an `atan2`, every single time two points are compared --
because `SPoint` stores angles, not vectors, so `sin`/`cos` of both
coordinates get re-derived on every comparison.

skycell stores points as Cartesian unit vectors from the moment they're
parsed (`sc_radec2vec()`, once), so `sin`/`cos` of RA/Dec happen exactly
once per value, not once per *comparison*. A point-in-cap test (the
direct analogue of what `spoint_dist()` would be used for) is round
twenty-seven's `cap_contains_point()`: `dot(center, point) >=
cos(radius)` -- one dot product, zero trig calls, since `cos(radius)`
was already cached at cap-construction time. Where pgSphere pays six
trig calls and a sqrt per comparison, skycell pays none.

**No action needed -- this is confirmation, not a gap.** This is the
third source-level pgSphere investigation this file has done (index
architecture and per-scan query-key caching in the original rounds;
BRIN's actual scope in rounds thirty-two through thirty-four; this
round's exact-math check), and the pattern holds throughout: pgSphere
sometimes wins on the *indexing-structure* axis (a cheaper aggregate
index for specific low-selectivity or box-shaped query regimes,
per round thirty-three/four), but on the *per-comparison math* axis --
query-key caching, trig-free point tests, storing vectors instead of
re-deriving them -- skycell's existing design is already ahead or
matched everywhere checked. Nothing further to mine here without
diminishing returns; the remaining unexamined pgSphere source
(`epochprop.c`'s proper-motion propagation, `gnomo.c`'s projection math,
the sparse MOC storage) is either unrelated to spatial-indexing
performance or already covered in substance by what `skyregion`'s own
multi-cap representation does.

## Round thirty-six: `min_area` for `<@` -- round fourteen's mirror,
## and the same outcome

This file's own open-questions list carried a `min_area` field for
`CONTAINED_BY_REGION` (`<@`) as the mirror of round fourteen's reverted
`max_area` idea for `CONTAINS_REGION` (`@>`), flagged explicitly as low
priority -- `<@` already wins against pgSphere at both scales tested
(round seven), so there was never a known gap for this to close, only a
"worth checking if a future benchmark finds `<@` losing somewhere round
seven didn't test," which none has. Implemented and measured anyway.

**The idea, precisely mirrored**: containment gives a sound area bound
in the other direction too -- if `A <@ Q` (A nonempty) then `area(A) <=
area(Q)`, the same ordinary measure monotonicity round fourteen used,
min()-reduced through `union()` instead of max()-reduced. Added one field
to `GistMultiCap` (`min_area`, set to `r->area` at leaf construction in
`region_to_multicap()`, min-reduced in the one place `union()` and
`picksplit()` both already route through, `multicap_union_many()`), and
one check in `consistent()`, `CONTAINED_BY_REGION` only: reject outright,
no recheck needed, whenever even the *smallest* region reachable under a
key already exceeds the query's own area -- checked before the existing
`O(MAX_SUBCAPS^2)` overlap test, cheaper besides.

**Correctness held**: `<@` match counts identical to the unmodified
code at both 5,000 rows (471) and 50,000 rows (4,900), matching round
seven/twenty-five's own established reference counts exactly.

**Performance didn't move, and needed care to see that clearly.** First
buffer-count pass looked promising -- 50,000 rows via `REINDEX` on an
already-built corpus showed 16179 -> 16130 (0.3% fewer) -- but a
*second*, independently fresh build (`DROP`/recreate `fpr`, not just
`REINDEX`) of the exact same corpus and query came back 16183, and
reverting the code and rebuilding again came back 16186 -- all three
numbers within a handful of buffers of each other, no reproducible
direction. The first "improvement" was `REINDEX`-order page-layout
noise, not a real effect, caught by rebuilding from scratch rather than
trusting a single before/after pair (the same discipline this file has
needed repeatedly at small scales -- rounds thirty/thirty-one's
sequential-block and cache-position confounds are the same lesson
again, this time from index build order rather than query timing). At
5,000 rows the direction was at least consistent, but the wrong way:
2099 -> 2197 buffers, a real 4.7% *regression*.

**Why, matching round fourteen's own diagnosis exactly**: on realistic
data, being too large to be contained by a query region and having a
cap that doesn't overlap the query's cap are correlated failure modes,
not independent ones -- the existing overlap test already rejects
almost everything the area test would additionally catch. Round
fourteen found this for `@>` ("too small... usually also positioned
such that its cap doesn't overlap"); this round finds the exact mirror
holds for `<@` too. The two tests are mostly redundant in practice for
either direction.

**Reverted**, same standard as rounds nine, twelve, fourteen, and
twenty-nine: correct, sound code that costs 8 bytes/key (one `double` on
`GistMultiCap`, a real if small ~5% key-size increase, well short of
round twenty-nine's 24% but the same mechanism) for a measured effect
that is either negative or indistinguishable from rebuild noise is not
worth carrying. The open-questions list's own instinct -- that this
mirror wasn't expected to close a gap that doesn't exist -- held up
under actual measurement, not just prediction; worth having tried
rather than left as a guess, but nothing to ship.

## Round thirty-seven: a JIT-aware term in `cover.c`'s cost model --
## structurally impossible, not just unmeasured

The last open item from rounds seventeen through nineteen's own list:
add a term to `cover.c`'s covering-choice cost model that prices in the
execution-side JIT tax (§ round eighteen: the shipped, non-experimental
`cover_cone_direct()` path already crosses `jit_above_cost` for ~1% of
realistic large-radius/dense queries, paying a 3-6x parallel-JIT
compile tax as a result), so the covering computation could route
around the cliff itself rather than needing an operational GUC (round
nineteen's confirmed but code-side-unreachable fix). Before writing any
code, tested the load-bearing assumption directly: for an actual
crossing query, does *any* alternative covering choice reduce the real,
Postgres-reported plan cost below the threshold?

**Setup**: 10M-row corpus, a genuine dense real-data-cluster centre
(the kind round eighteen's own benchmark found crossing the threshold),
radius pushed to 3.5 degrees to land just over `jit_above_cost`
(default order 9: plan cost 99,165, `Finalize Aggregate`/`Gather` in the
plan -- parallel, exactly the shape that pays the tax N times over).
Swept `skycell.force_order` from 5 through 15 and read both `skycell_
cover_info()`'s own numbers and the real `EXPLAIN` cost at each:

| force_order | nranges | exp_rows | area_ratio | EXPLAIN cost |
|---|---|---|---|---|
| 5 | 8 | 176,905 | 1.83 | (not measured directly, strictly worse than 6) |
| 6 | 12 | 135,142 | 1.46 | **262,125** |
| 7-15 | 30 | 107,478 | 1.19 | 99,165 (order 9, the default) |

**Two findings, both clean.** The covering already converges to its
best available shape at the default choice: orders 7 through 15 all
produce the *identical* 30-range, 107,478-row covering -- there is no
finer order left to try that would reduce false positives further, the
geometry's own boundary-cell count is already the limit. And every
coarser alternative makes the real cost *worse*, sharply: order 6 (12
ranges instead of 30) more than doubles the reported plan cost, to
262,125 -- moving further over the threshold, not under it.

**Why this isn't specific to this one query.** `rlist_score()`'s own
`merge_gaps()` (called on every candidate before it is scored) already
greedily merges every gap cheaper than `range_cost` -- by construction,
whatever covering `choose_order()`/`merge_gaps()` settles on has already
absorbed every merge that was a net win. Forcing *more* merging past
that point (a coarser order, or a tighter `max_ranges`) necessarily
merges gaps whose row-cost *exceeds* `range_cost` -- each such forced
merge changes the total score by exactly `(gap's own rows) -
(range_cost)`, which is positive by definition of it not already having
been merged for free. This holds for any query, not just the one
measured: past the point `merge_gaps()` already reaches, there is no
direction left in which coarsening the covering reduces cost. A JIT-aware
term added to `rlist_score()` would have nothing to select *for* -- the
"duck under the JIT cliff" alternative it would need to prefer doesn't
exist in the space of coverings `cover.c` can produce.

**What this actually means for round eighteen's finding.** The queries
that cross `jit_above_cost` aren't crossing it because of a covering
*quality* problem skycell's cost model failed to price -- they cross it
because they are genuinely expensive queries, matching genuinely large
numbers of real rows in a dense region, which is exactly the situation
where JIT compilation is normally *supposed* to help (amortizing a
large scan's per-tuple evaluation cost). The defect is specifically that
PostgreSQL's parallel workers each independently pay the compile cost
with no amortization across them (round eighteen's own `Workers
Launched: 2`, `Inlining: 0.000ms`, Emission alone at 68-83% of total
time) -- an execution-side interaction between JIT and parallelism that
happens entirely after any covering choice is finalized, in a part of
PostgreSQL's own execution machinery `cover.c` has no visibility into or
influence over. No covering-time cost term, however designed, can
substitute for fixing (or working around) that interaction directly.

**No code written -- this is a negative result established before
implementation, not after.** Round nineteen's operational fix (raise
`jit_above_cost` or disable JIT for the session/connection pool running
this workload) remains the only actionable mitigation from this whole
line of investigation; a `planner_hook` intervening after PostgreSQL's
own cost is computed (not attempted, a materially bigger and different
kind of change than anything else in this file) is the only remaining
code-level lever, and this round's finding rules out the smaller,
`cover.c`-scoped alternative that rounds seventeen through nineteen had
left open as the more approachable option.

## Round thirty-eight: a bulk-loaded point SP-GiST structure -- the cheap
## approximation works dramatically, and precisely diagnoses why the
## expensive version wouldn't finish the job

The last open item from the point SP-GiST opclass's own closing verdict
(rounds nine through thirteen, §4): a bulk-loaded, statically-packed
structure instead of the incremental `choose()`/`picksplit()` construction
every `CREATE INDEX`/`INSERT` currently goes through. A genuine version of
this -- a custom `ambuild` writing SP-GiST inner/leaf tuples directly,
bypassing the per-row insertion API entirely -- means hand-rolling page
layout and WAL-safety at the level PostgreSQL's own storage code operates
at, a materially bigger and riskier undertaking than anything else in
this file. Before committing to that, tested the load-bearing hypothesis
cheaply: does *insertion order* alone, with the existing, unmodified
opclass code, change tree quality enough to matter? If order doesn't
matter, a full bulk-loader (which mainly buys a better-ordered
construction sequence) has nothing to fix. If it does, the cheap version
(sort the input, don't touch the opclass) already captures most of the
same value a custom `ambuild` would work much harder for.

**This wasn't a hypothetical risk to check casually.** The opclass's own
header comment (top of `spgist_region.c`) documents a *real* correctness
bug found the first time a HEALPix-sorted build was tried, pre-fix: "a
physically HEALPix-sorted table missed 2 of 3 true matches on a cone
query that an unsorted build of the identical rows answered correctly,"
because a *fixed* one-order-per-level split can report a false
"all-the-same" for a batch that's only locally homogeneous by chance of
insertion order, not genuinely indistinguishable -- and once marked,
SP-GiST's own core refuses to ever add a node to it again. The documented
fix (rounds ten/eleven: always search for the true longest common prefix
from scratch, never trust accumulated hop count) is specifically what's
supposed to make sorted-order construction safe now. This round is the
first time that exact scenario -- building against a fully HEALPix-sorted
table -- has actually been tried against the current, fixed code.

**Setup**: the same 10M-row corpus and radii round eleven/twelve
established (1", 1', 30', 1deg, 3deg), two physical copies of the same
`skypos` data -- `spg_pts_random` (natural/generation order, matching how
every prior round in this opclass's history built it) and
`spg_pts_sorted` (`ORDER BY skycell_ang2cell(ra, dec)` before the
`CREATE INDEX`, the cheapest possible proxy for "the build saw
HEALPix-local batches together" without touching a line of C). 60 query
centers, a mix of real-data and uniform-sky (both kinds, all five radii,
matching round nineteen's own resampling fix), `EXPLAIN (ANALYZE,
BUFFERS)`.

**Correctness first, given the specific documented risk**: brute-force
count from both tables agreed exactly on the initial spot checks
(including a near-pole query, `dec=89`), and a full sweep across all 60
corpus queries came back **0 mismatches, 244,779 total matches identical
both ways**. The from-scratch-prefix-search fix holds up under the exact
stress case its own comment flags -- sorted-order construction is safe
with the current code.

**Buffer touches: a large, clean, and robust win.**

| radius | random avg buffers | sorted avg buffers | ratio |
|---|---|---|---|
| 1" | 37.8 | 40.5 | 0.93x (noise) |
| 1' | 42.6 | 42.2 | 1.01x (noise) |
| 30' | 956.9 | 102.9 | **9.3x fewer** |
| 1deg | 5,139.8 | 363.6 | **14.1x fewer** |
| 3deg | 19,330.8 | 1,226.3 | **15.8x fewer** |

Checked separately by centre kind (real-data-cluster vs. uniform-sky) to
rule out the win being an artefact of one or the other: both show the
same 4-17x reduction at every radius from 30' up, nothing close to it
below. Index build time was *also* 2.3x faster for the sorted table
(28.3s vs 64.8s) -- fewer, cheaper page reorganisations during
construction, consistent with the same mechanism. This is the single
largest buffer-count improvement measured anywhere in this whole
investigation, region-GiST or SP-GiST.

**Against skycell's own B-tree path -- the thing this opclass exists to
replace -- the sorted build closes the buffer gap almost entirely, and at
the largest radius, reverses it:**

| radius | B-tree buffers | SP-GiST-sorted buffers | B-tree ms | SP-GiST-sorted ms |
|---|---|---|---|---|
| 1" | 14.0 | 40.5 | 0.31 | 0.21 |
| 1' | 16.8 | 42.2 | 0.34 | 0.23 |
| 30' | 87.0 | 102.9 | 0.75 | 0.87 |
| 1deg | 265.7 | 363.6 | 1.84 | 3.42 |
| 3deg | 1,314.5 | **1,226.3** | 5.78 | 13.45 |

Round eleven's own verdict ("tree descent costs more buffer touches...
at every radius tested [than the B-tree]") no longer holds at 3 degrees:
SP-GiST-sorted touches *fewer* buffers there. Wall-clock, though, does
not follow -- SP-GiST-sorted is faster at 1"/1' (lower fixed
per-query overhead than the bitmap-scan machinery for a handful of
matches) but 1.2-2.3x *slower* at 30'-3deg, widening with radius, despite
comparable or better buffer counts.

**Isolated why directly, rather than guessing parallelism or cache
state.** First suspect: the B-tree path's plan is parallel (`Gather`,
2 workers) while SP-GiST's Bitmap Index Scan plan has no `Gather` node at
all -- PostgreSQL's SP-GiST access method doesn't implement parallel
index scan the way B-tree does, so this opclass is structurally
single-threaded regardless of tree quality. Tested by forcing the B-tree
path serial too (`SET max_parallel_workers_per_gather = 0`) and comparing
both, warm-cache, on the identical query: B-tree serial was *faster* than
B-tree parallel here (8.8ms vs 10.9ms -- parallel overhead exceeded
benefit at this row count), ruling out parallelism as the dominant
factor. Warm-cache, serial-vs-serial, same query: B-tree 488 buffers/
8.8ms vs SP-GiST-sorted 729 buffers/20.8ms -- SP-GiST touches 1.5x more
buffers *and* costs about 1.6x more per buffer touched. The remaining gap
is a genuinely higher **per-node CPU cost**: SP-GiST's `inner_consistent
()`/`leaf_consistent()` run real geometric classification
(`sc_region_classify()`, vector/trig math) at every node visited, where
the B-tree's bitmap scan does plain integer range comparisons. Tree shape
improved dramatically; the cost of evaluating each node the (now much
smaller) tree still visits did not, because it was never the thing
sorted-order construction could touch.

**What this means for the original question.** The core hypothesis behind
"try a bulk-loaded structure" is emphatically confirmed: insertion order,
not some inherent ceiling on a PATRICIA trie's achievable shape (round
twelve's own closing guess), was the dominant reason the unsorted-build
tree performed as poorly as it did on buffer touches. A full custom
`ambuild` would very plausibly refine tree shape further still (a global,
one-pass balanced construction should beat "insert in sorted order and
let incremental picksplit do its best," even if the gap between them is
smaller than the gap this round already closed) -- but the *remaining*
wall-clock deficit against the B-tree is now precisely diagnosed as a
separate, orthogonal cost (per-node classification work, not tree
topology), which no amount of *better shape* can fix on its own. That
makes the expensive version of this idea a poor bet on its own: it would
need to be paired with cheapening `inner_consistent()`/`leaf_consistent
()` itself (a different, unexplored lever -- e.g. a cheaper coarse
pre-filter before the full geometric test, mirroring what round
twenty-seven did for the region GiST opclass's point predicate) to have
a real chance at closing the remaining 1.2-2.3x gap, not tree
construction on its own.

**Shipped**: nothing -- this remains a research-only opclass, not
recommended for production use, per round thirteen's own standing
verdict. But the verdict itself is now sharper: if anyone does pick this
opclass up again, building against a table already `CLUSTER`ed (or
otherwise physically sorted) by HEALPix cell is a real, free, zero-code
improvement over an unsorted build -- and the honest next step to close
the remaining gap is cheapening the per-node consistent-function cost,
not further chasing tree shape via a full custom bulk-loader, which this
round's diagnosis suggests would be a much larger engineering bet for a
smaller remaining return than the sorted-build experiment alone already
captured.

## Round thirty-nine: caching the per-node consistent-function cost --
## the lever round thirty-eight identified, and it reverses the verdict

Round thirty-eight's own diagnosis named the next lever precisely: the
remaining wall-clock gap against the B-tree wasn't tree shape (already
fixed by sorted-order construction) but a genuinely higher per-node CPU
cost in `inner_consistent()`/`leaf_consistent()`. Reading those two
functions found exactly why, and it's the same wasted-work shape
`gist_region.c`'s own `consistent()` found and fixed for the region GiST
opclass at round one: both functions call `skycell_region_from_datum()`
-- re-parsing the query region *from scratch* -- on **every single node
visited**, dozens of times per query, all for the same unchanging query
argument. Worse than a plain parse: for a cone, `sc_region_cone()` fills
`out_c2[]`/`in_c2[]` for every order 0 through `SC_MAX_ORDER` (30
entries, up to four `sin()`/`pow()` calls each), plus the initial
`sc_radec2vec()`/`sc_ang2pix()` -- on the order of 120+ transcendental
calls, repeated at every node instead of once per scan.

**The fix**: the exact same `fn_extra` query cache `gist_region.c`
already uses, keyed the same way (the query Datum's *bytes*, not
pointer identity -- a pointer-identity cache would silently reuse a
stale region across different join rows, the precise bug class round
one of that opclass found and fixed; this opclass's own header comment
already documents an analogous sorted-insertion risk, so the same
discipline applied here without needing to rediscover it the hard way).
`inner_consistent()` and `leaf_consistent()` are separate catalog
functions with their own `fn_extra` slots, so each gets its own cache
instance via a shared `spg_cached_region()` helper. A polygon query's
heap-allocated `v[]`/`n[]` arrays get allocated into `fn_mcxt` (a
`MemoryContextSwitchTo` around the parse) so they survive past the
per-call context, and get freed via `sc_region_free()` before being
overwritten on a genuine cache miss.

**Correctness verified two ways, given the exact stale-cache risk this
pattern carries.** First, the same style of check round thirty-eight
used: 60 queries, index result vs. brute-force sequential scan, 0
mismatches, 244,779 matches identical both ways. Second, and more to the
point -- a loop of independent top-level statements doesn't actually
exercise the risk this cache introduces (each gets its own fresh
`fcinfo`), so built a dedicated stress case: 200 distinct query regions
joined against the point table in *one* statement (`JOIN ... ON p.p <@
circle(q.ra, q.dec, q.r)`, hash/merge join disabled to force a nested
loop probing the index once per outer row) -- the scenario that forces
the *same* `fn_extra` slot to be reused across 200 different region
values within a single execution, exactly the shape that broke a
pointer-identity cache before. 0 mismatches against an independent
per-region reference, 22,207 matches identical both ways, run against
both the sorted and random-order tables.

**Performance measured as a true same-session A/B**, not against
round thirty-eight's previously-recorded numbers (this session has
repeatedly found that invalid -- system state drifts between sessions;
see round thirty's own sequential-block confound for the general
lesson). Reverted the change, rebuilt, measured; reapplied, rebuilt,
measured again, both in the same sitting:

| radius | sorted before (ms) | sorted after (ms) | speedup | random before (ms) | random after (ms) | speedup |
|---|---|---|---|---|---|---|
| 1" | 0.340 | 0.283 | 1.2x | 0.519 | 0.284 | 1.8x |
| 1' | 0.377 | 0.269 | 1.4x | 0.558 | 0.249 | 2.2x |
| 30' | 1.219 | 0.478 | 2.6x | 2.655 | 1.143 | 2.3x |
| 1deg | 5.580 | 1.621 | 3.4x | 8.391 | 6.308 | 1.3x |
| 3deg | 26.906 | 7.415 | 3.6x | 56.061 | 44.614 | 1.3x |

Buffer counts are exactly unchanged (confirmed identical to round
thirty-eight's own numbers throughout) -- exactly as the mechanism
predicts: this touches nothing about which nodes get visited, only what
each visit costs. A clean, universal win, 1.2x-3.6x, every radius, both
physical orders.

**Against skycell's own B-tree path, this changes the standing
verdict.** Round thirty-eight found SP-GiST-sorted trailing the B-tree
by 1.2-2.3x at 30'-3 degrees despite comparable buffer counts. Re-ran
that exact comparison three times (same corpus, same script) to check
reproducibility before trusting it:

| radius | btree ms (3 runs) | spg-sorted ms (3 runs) | spg wins? |
|---|---|---|---|
| 1" | 0.560 / 0.228 / 0.286 | 0.265 / 0.229 / 0.310 | a wash, noise-level either way |
| 1' | 0.603 / 0.228 / 0.297 | 0.241 / 0.273 / 0.331 | a wash, noise-level either way |
| 30' | 0.957 / 0.655 / 0.971 | 0.576 / 0.438 / 0.449 | **spg, 1.7-2.2x, every run** |
| 1deg | 3.892 / 2.894 / 3.458 | 1.559 / 1.861 / 1.609 | **spg, 1.8-2.5x, every run** |
| 3deg | 9.368 / 8.433 / 9.639 | 5.332 / 6.289 / 7.773 | **spg, 1.2-1.8x, every run** |

At the smallest radii (1", 1') the two are now statistically
indistinguishable -- fixed per-query overhead dominates a handful of
buffer touches either way, and this round's fix has nothing left to
save there. At 30' through 3 degrees -- exactly the radii round eleven
originally found the B-tree winning, and round thirty-eight still found
it winning after the sorted-build fix alone -- SP-GiST-sorted-plus-
cached now wins outright, consistently, across three independent
measurement passes, by a solid 1.2-2.5x margin. Buffer counts already
favoured SP-GiST-sorted at 3 degrees (round thirty-eight); wall-clock
now agrees with buffers everywhere it was measured separately from them
before.

**This reverses, not just narrows, the standing verdict from round
eleven onward.** The combination of round thirty-eight's sorted-order
build and this round's query caching -- both zero-risk, both verified
correct under dedicated stress tests, neither one alone sufficient
(round thirty-eight's own sorted build still lost on wall-clock; this
round's caching alone, without sorted order, would still carry the
unsorted build's 4-17x worse buffer counts) -- together beat the
already-shipped, non-experimental B-tree covering path at every radius
from 30' up, on a real 10M-row corpus, both real-data and uniform-sky
query centres. This does not by itself make the point SP-GiST opclass
production-ready (still marked EXPERIMENTAL in the extension's own SQL,
still needs the sorted-build discipline documented and probably
enforced rather than left to the caller to remember, and still untested
beyond the radii and corpus this whole investigation has used
throughout), but "not recommended for use, a net loss at every radius"
is no longer an accurate description of where this opclass stands.

## Round forty: back to pgSphere, the original reference point -- and a
## real methodology bug caught and fixed before trusting the result

Rounds thirty-eight/nine measured against skycell's own B-tree, but
round eleven's original question was about pgSphere specifically (2.9-
3.9x slower at small radii, narrowing to ~1.4x at 3 degrees, back when
this opclass had neither the sorted build nor the query cache). Worth
re-measuring directly now, on the same corpus, rather than assuming the
B-tree comparison implies anything about pgSphere.

**First attempt produced an implausible result, and it was right to be
suspicious of it rather than write it up.** Built an `spoint` table via
`SELECT ... FROM spg_pts_sorted r JOIN src s ON s.id = r.id` (intending
to inherit `spg_pts_sorted`'s HEALPix-cell-sorted physical order for a
fair comparison) and got pgSphere losing by 4.5-12.5x at 1-3 degrees --
an order of magnitude worse than round eleven's own recorded number for
the same operator on (presumably) the same shape of corpus. That gap was
too large to just accept. `EXPLAIN (ANALYZE, BUFFERS)` on one query
showed why it was worth checking further: `Heap Blocks: exact=3407` for
~7,450 candidate rows -- barely better than one heap page per row, and a
`written=1283` in the buffer line (a spill, not a hint-bit fluke).
Checked directly whether the table actually inherited the sort order
intended: `(ctid, id)` on the first few physical rows of `spg_pts_sorted`
vs. the new `spg_pts_pgsphere` table showed completely different
orderings -- the `JOIN` had not preserved `spg_pts_sorted`'s scan order
at all (unsurprising in hindsight: nothing about a join's output order is
guaranteed to follow either input's physical order without an explicit
`ORDER BY`). The new table was build in something close to `id` order,
unrelated to sky position, so *any* index's matching rows for a given
query were scattered across the whole table's physical extent --
crippling heap-fetch locality for pgSphere's GiST regardless of how good
its own tree is, and nothing to do with pgSphere's actual index quality.

**Rebuilt with `ORDER BY r.ctid`** (verified this time: first-five
`(ctid, id)` pairs identical to `spg_pts_sorted`'s own), re-indexed, and
the same single query dropped from `Heap Blocks: exact=3407`/35ms to
`exact=67`/3.2ms -- an order of magnitude from fixing physical layout
alone, nothing about pgSphere's own GiST changed at all.

**With that fixed, the three-way comparison is coherent and closely
matched, run twice for reproducibility:**

| radius | btree ms | pgsphere ms | spg-sorted-cached ms |
|---|---|---|---|
| 1" | 0.19-0.22 | 0.15-0.20 | 0.22-0.25 |
| 1' | 0.20-0.21 | 0.15 | 0.24 |
| 30' | 0.63-0.66 | 0.40-0.41 | 0.42-0.44 |
| 1deg | 1.94-2.54 | 1.75-1.90 | 1.45-1.55 |
| 3deg | 9.53-9.69 | 7.70-8.19 | 5.11-7.09 |

pgSphere has a modest edge at 1"-30' (where skycell's B-tree also does
relatively worse). From 1 degree on, skycell's cached-and-sorted
SP-GiST takes over as the fastest of the three, by a real if not huge
margin over pgSphere (about 1.1-1.6x), with the B-tree trailing both at
every radius from 30' up. All three are within a tight band of each
other throughout -- nothing like the original 2.9-3.9x gap round eleven
measured, or the false 4.5-12.5x gap the row-order bug produced here
before it was caught.

**What this means, put together with rounds thirty-eight/nine**: the
combination of a sorted-order build and query caching doesn't just beat
skycell's own B-tree path (already established) -- it also closes round
eleven's original gap to pgSphere essentially completely, and edges
ahead of it at the radii skycell's own B-tree was already weakest at (1
degree and up). Both zero-risk, both independently verified correct
under dedicated stress tests (rounds thirty-eight/nine), neither
sufficient alone. This is now a genuinely competitive index, not a
research curiosity that happens to have an interesting planning-cost
story -- though see rounds thirty-eight/nine's own caveats (still marked
EXPERIMENTAL, sorted-build discipline needs documenting or enforcing,
untested beyond this investigation's corpus/radii) before treating it as
production-ready.

**One more thing worth recording**: `CREATE INDEX ... USING gist` on
`spoint` took 118-141s for pgSphere against this SP-GiST opclass's 28s
sorted build (round thirty-eight) -- a data point on build cost, not
just query cost, though not chased further here.

## Round forty-one: order-independent seeding, to unblock round twenty-
## six's sub-quadratic overlap test -- regresses on its own, not attempted

Round twenty-six's own postmortem named the precise reason its
mathematically sound sub-quadratic overlap test regressed: `out[0] =
caps[0]` in `merge_caps_greedy_fp()` hardcodes the first seed to
whichever cap happens to land at array index 0, so sorting a stored
`sub[]` for query-time benefit silently reseeds every future re-merge of
that same key -- a real, diagnosed coupling, not a vague guess. Two
fixes were named but neither attempted: carry a second, sorted-only copy
of `sub[]` (doubling that part of the key, the exact key-bloat tax this
file has paid for and regretted every time it's been tried), or make the
farthest-point seeding itself order-independent. Took the second path.

**The idea**: replace the hardcoded `caps[0]` first seed with a
deterministic, array-position-independent choice -- the cap whose centre
has the smallest dot product with the centroid direction of the whole
batch (i.e. the outlier farthest from the set's own "middle"). Summing
vectors and taking a minimum don't depend on scan order, so two calls
given the same caps in different array orders now provably pick the same
first seed, and since every later seed is chosen purely from the seeds
picked so far, the same full seed sequence and final clustering
throughout -- exactly what round twenty-six's fix needed, with no new
storage (the centroid is computed and discarded within the call, never
persisted). Everything after seed selection -- the greedy
farthest-point loop for the remaining seeds, the final absorption pass
-- untouched.

**Correctness held**: `@>`(region,region) (217 matches) and
`<@`(region,region) (4,900 matches) both matched their established
reference counts exactly, same 50,000-row corpus round twenty-two/
twenty-five/twenty-eight all used.

**But the seeding change alone -- before even re-adding round twenty-
six's overlap test -- already regresses the two strategies it was meant
to help, not neutral as hoped:**

| strategy | buffers before | buffers after | change |
|---|---|---|---|
| `@>`(region,region) | 7,721 | 8,294 | +7.4% worse |
| `<@`(region,region) | 15,625 | 16,433 | +5.2% worse |

`&&` and `@>`(region,point) came back a wash either way (109-113ms vs
110-111ms, and 24-27ms vs 26-27ms -- both within this file's usual
wall-clock noise band, no clear direction). Buffer counts are
deterministic, so the `@>`/`<@`(region,region) regression is real, not
noise.

**Reverted before attempting round twenty-six's overlap test on top**,
matching this file's own standing practice: no reason to compound a
second change onto an already-regressed baseline hoping the two effects
net out favourably rather than measuring each independently. `caps[0]`
remains the first seed, exactly as shipped since round twenty-two.

**Why, in hindsight**: `caps[0]` isn't as arbitrary in practice as its
name suggests. `multicap_union_many()` flattens `entries[i]->sub[j]` in
entry order, and entries arrive from GiST's own page-level grouping --
which, by construction (R-tree-style splits group spatially nearby
items onto the same page), tends to make `caps[0]` a locally
representative starting point already, an emergent property of how data
flows through the tree rather than a genuinely uninformative default.
"Farthest from the batch's centroid," while cleanly order-independent,
is a *different* seeding heuristic outright -- deliberately picking a
geometric outlier rather than a locally-typical point -- and this round
measures that it clusters this data's actual sub-cap distributions
worse, not just differently. This doesn't rule out every order-
independent seeding choice (a different deterministic criterion closer
in spirit to what `caps[0]` already achieves in practice might do
better), but it does rule out this specific one, and confirms round
twenty-six's own warning that this function family "needs real care,
not a quick follow-up" -- a fourth attempt now, after rounds twenty
through twenty-two and this one, three of which regressed.

## Round forty-two: a separate box-only opclass, and it wins everything

Round forty-one closed `@>`(region,region) and `@>`(region,point) as an
accepted structural floor: pgSphere's win there came entirely from its
box-shaped index key giving cheap, coarse pruning that the multi-cap
key's richer per-sub-cap test doesn't reproduce as cheaply, and every
attempt to cut the multi-cap key's own redundancy (`max_area`/`min_area`
in rounds fourteen/thirty-six, order-independent seeding in round
forty-one) either washed or regressed. The next question, not about
fixing the multi-cap key but about replacing it: "is there a way to
generate a cheaper index (forget, if needed, our own region indexes)."

**The idea**: build a second, genuinely separate, non-default GiST
opclass for `skyregion` -- `skyregion_box_gist_ops` in a new file,
`gist_region_box.c` -- using a plain axis-aligned 3D box key (six
doubles: `xmin, ymin, zmin, xmax, ymax, zmax`) instead of the multi-cap
key's spherical caps. This directly mirrors pgSphere's own key shape
(the thing round thirty-two through thirty-five kept confirming is the
*only* place pgSphere has a genuine structural edge), so the experiment
asks the question as directly as possible: strip the multi-cap key's
richness down to the same shape pgSphere uses, on the *same* unified
`skyregion` type, and measure what's actually gained and lost.

**Deriving the box, exactly, not by sampling**: a spherical cap with
unit-vector centre `c` and angular radius `r` has, on each Cartesian
axis `k`, a provable closed-form extent rather than needing corner
sampling. Parameterise the cap's boundary circle in an orthonormal frame
built from `c`: boundary points are `c*cos(r) + (u*cos(t) +
v*sin(t))*sin(r)` for `t` in `[0, 2*pi)`, where `u, v` are any two unit
vectors completing an orthonormal basis with `c`. Axis `k`'s component
of this is `c_k*cos(r) + (u_k*cos(t) + v_k*sin(t))*sin(r)`, and
`u_k*cos(t) + v_k*sin(t)` ranges over exactly `[-sqrt(u_k^2+v_k^2),
sqrt(u_k^2+v_k^2)] = [-sqrt(1-c_k^2), sqrt(1-c_k^2)]` (since `u, v, c`
are orthonormal, `u_k^2 + v_k^2 = 1 - c_k^2`). So axis `k`'s extent is
exactly `c_k*cos(r) +/- sqrt(1-c_k^2)*sin(r)` -- with one correction:
when the cap fully swallows the axis's positive or negative pole
(`c_k >= cos(r)` or `-c_k >= cos(r)`), the true extent on that side is
the pole itself (`+1` or `-1`), wider than the boundary-circle formula
alone would give, since the cap's *interior* then crosses the pole.
`region_to_box3d()` gets a region's box by calling the existing,
already-tested `sc_region_bounding_cap()` (safe for both CONE and
POLYGON, no new per-kind bounding logic) and running this conversion on
the result -- deliberately reusing the existing bounding-cap step rather
than computing a tighter polygon-specific box, so the box is a safe
over-approximation of the bounding cap, not necessarily the tightest
box obtainable from the raw region.

**Kept deliberately simple everywhere else**, to keep the comparison
honest about what a plain box key costs rather than measuring some
other change bundled in: `box3d_union/volume/overlaps/contains_point()`
are pure interval arithmetic, no trig at all. `picksplit` is a
widest-spread-axis sort-and-median-split -- not `gist_region.c`'s own
R*-tree-style sweep -- and `penalty` is volume-increase, the textbook
R-tree choice. `consistent()` dispatches all three containment-shaped
strategies (`&&`, `@>`(region,region), `<@`(region,region)) through
`box3d_overlaps()`, same as pgSphere's own box-level pruning and the
multi-cap opclass's own reuse of overlap as a containment proxy;
`@>`(region,point) uses `box3d_contains_point()`. `*recheck = true`
always -- this key is lossy on every strategy, unlike the multi-cap
key's exact `&&`/`<@` tests. A query-side `fn_extra` cache
(`box_cached_query()`) reuses round thirty-nine's pattern, just over a
much smaller cached value (a 48-byte box instead of a full `sc_region`).

Registered ad hoc against `splitcost_test` (not touching the extension's
own versioned SQL -- this stays fully disposable pending a verdict):
`CREATE INDEX fpr_box_gist ON fpr USING gist (s_region
skyregion_box_gist_ops)`, alongside the existing default `fpr_region_gist`
multi-cap index, same 50,000-row `fpr` corpus rounds twenty-two through
forty-one all used.

**Correctness held exactly**, recheck and all: all four strategies,
run against the box-only index (the multi-cap index dropped so the
planner had no other choice), reproduced the brute-force reference
counts on this corpus/probe combination precisely --

| strategy | brute-force | box-indexed |
|---|---|---|
| `&&` | 286 | 286 |
| `@>`(region,point) | 222 | 222 |
| `@>`(region,region) | 217 | 217 |
| `<@`(region,region) | 4,900 | 4,900 |

**Then the size and performance numbers, all three opclasses measured
in the same session against the identical probe tables** (`rox_probe`,
`rox_probe_pts`, `rox_probe_tiny`, `rox_probe_big`, unchanged since round
twenty-four), each index freshly rebuilt this round so none carries
stale bloat from earlier rounds' rebuilds, buffers confirmed
reproducible by rerunning the full box-opclass pass a second time after
a full index drop/rebuild (1,172 -> 1,163; 1,034 -> 1,039; 1,056 ->
1,061; 8,293 -> 8,301 -- noise-level, buffers deterministic):

| strategy | box (buffers / ms) | multi-cap (buffers / ms) | pgSphere (buffers / ms) |
|---|---|---|---|
| `&&` | 1,172 / 8.2 | 8,375 / 34.3 | 1,193 / 44.5 |
| `@>`(region,point) | 1,034 / 7.0 | 7,505 / 22.2 | 1,082 / 16.2 |
| `@>`(region,region) | 1,056 / 6.1 | 7,588 / 24.6 | 1,083 / 12.6 |
| `<@`(region,region) | 8,293 / 25.4 | 15,614 / 49.4 | 9,520 / 65.7 |

The box key **wins every strategy against both competitors, on both
buffers and wall-clock** -- including the two strategies (`@>`
region/point and region/region) round forty-one had just closed out as
an accepted structural loss to pgSphere. Buffers against pgSphere are
essentially tied (box is marginally lower on all four, as expected: two
box-shaped keys pruning the same way should cost about the same
buffers), but wall-clock isn't tied at all -- box beats pgSphere
2.1-8.4x, because `box3d_overlaps()`/`box3d_contains_point()` are
interval comparisons while pgSphere's own exact recheck still calls
`spoint_dist()`'s Vincenty formula per candidate (the same per-
comparison cost gap rounds thirty-two through thirty-five kept finding,
now showing up downstream of an equivalently-shaped index rather than
being masked by it). Against the multi-cap opclass the win is larger
still and entirely expected in direction (fewer, larger caps merged
away all the precision a box never had to begin with) -- 1.9-7.3x fewer
buffers, 1.9-4.2x faster -- including on `&&` and `<@`, the two
strategies the multi-cap key was already winning against pgSphere; the
box key simply wins them by more.

**Size**, same corpus, all freshly built: box-only index 5,472 kB, vs.
the multi-cap index's 17 MB (box is ~0.32x the size) and pgSphere's
combined `rox_fpr_circ_gist` + `rox_fpr_poly_gist` at 3,024 kB (box is
~1.8x pgSphere's combined footprint, but is one index over one unified
type where pgSphere needs two indexes split by shape).

**Why this reverses round forty-one's "structural floor" framing**: that
framing was correct about the multi-cap *key* -- no amount of picking
which caps to merge or how to seed the merge closes the gap, because
the key's expressiveness is exactly what makes its `consistent()` test
more expensive per candidate than a box overlap check, however the caps
are chosen. What round forty-one didn't test is dropping that
expressiveness altogether. It turns out skycell doesn't need the multi-
cap key's tighter pruning to *beat* pgSphere on `@>`; it needs a key
*at least as cheap to test* as pgSphere's, and a plain box is that key,
available on the exact same unified `skyregion` type pgSphere needs two
types to cover. The multi-cap key remains real and useful on its own
terms (tighter index-level pruning, exact `&&`/`<@` without recheck) --
but as a pruning-plus-cost-reduction package for these four strategies
specifically, the box key dominates it, not just pgSphere.

**Status**: `gist_region_box.c` is new, measured, and correct, but not
yet the shipped default for anything -- it exists as a second,
explicitly-named opclass (`skyregion_box_gist_ops`) a user can opt into,
the same non-default posture `skyregion_gist_ops` and `skypos_spgist_ops`
already have. Whether it's worth promoting over the multi-cap key as
*the* region GiST default, keeping both for different use cases, or some
other disposition is a decision for whoever ships this, not something
this round's measurement alone settles -- but the measurement itself is
unambiguous: on this corpus, at this scale, across all four strategies,
the box key is smaller and faster than both the opclass it sits next to
and the competitor it was built to match.

**Addendum: does the box key hold up near the poles?** Worth asking
directly, since pole-proximity is a real, well-known failure mode for
spherical indexing -- round thirty-four's own BRIN(ra,dec) recipe is
exactly that class of bug, RA becoming meaningless as lines of constant
RA converge to a point. The `fpr` corpus every number above used never
actually tests this: `least(greatest(dec, -85), 85)`-style clamping
(visible in several `bench/*.sql` scripts) keeps every row at
`|dec| <= 85`, so round forty-two's headline numbers say nothing about
what happens closer in.

Built a dedicated declination-stratified corpus to check: 6,000 rows,
2,000 each at `|dec| < 10` (equator), `40 <= |dec| <= 50` (mid-
latitude), and `85 <= |dec| <= 89.9` (near-pole), same 0.02-0.3 degree
radius range as `fpr`, 300 stratified probes (~100/band), both opclasses
built fresh.

**Correctness held exactly in all three bands, near-pole included**:
brute force, box-indexed, and multicap-indexed all agree precisely --
105/105/105 matches (equator), 115/115/115 (mid-lat), 1,012/1,012/1,012
(near-pole; the much higher near-pole count is itself expected and a
good sanity check -- fixed-angular-radius circles overlap far more
easily as RA lines converge).

**And the box key's relative advantage over multi-cap doesn't erode near
the pole**:

| band | box buffers | multi-cap buffers | box's advantage |
|---|---|---|---|
| equator | 336 | 454 | 1.35x fewer |
| mid-latitude | 329 | 464 | 1.41x fewer |
| near-pole | 1,361 | 1,767 | 1.30x fewer |

If anything the margin narrows very slightly at the pole (1.30x vs.
1.35-1.41x elsewhere) -- noise-level, not a breakdown.

**Why this design doesn't have the classic pole bug**: the key is built
in Cartesian (x, y, z) unit-vector space, never RA/Dec, so there's no
coordinate singularity to hit in the first place -- the same reason this
extension's own multi-cap key and pgSphere's native Box3D don't have it
either; it's specifically schemes built directly on RA/Dec (like round
thirty-four's BRIN recipe) that do. `cap_to_box3d()`'s own pole-
swallowing special case ("BUILDING THE BOX" in `gist_region_box.c`'s
header) makes this exact, not just incidentally fine: when a cap's
radius is large enough to fully contain a pole, that axis's extent is
set to precisely +-1 rather than computed from the boundary-circle
formula, so a cap centred at or straddling a pole is bounded exactly on
that axis, not approximated. The box's real, generic source of slack
(any cap whose centre direction doesn't line up with a coordinate axis
gets some circle-in-a-square looseness, and a polygon's box comes from
its *bounding cap*, round forty-two's own caveat, not its own vertices)
is present everywhere on the sphere depending on shape and orientation,
not specifically worse at the poles.

**Second addendum: does the box key hold up at very large radii?** The
natural next question, and this one *does* find a real limit. Every
number in this round so far, `fpr` included, sits in the realistic
catalog-footprint range (0.02-0.3 degrees) -- nothing tests what happens
as a cap's radius grows toward a hemisphere, where a circle-in-a-square
argument says the box's slack should get worse, not better.

Built a third dedicated corpus to check directly: 6,000 circle regions,
centres sampled uniformly on the sphere (`dec = degrees(asin(2*U-1))`,
not `dec = degrees(180*U-90)`, to avoid the classic pole-clustering bug
in naive uniform-sphere sampling), radius stratified into four bands --
`small` 1-3 degrees (matching `fpr`'s own scale), `medium` 20-40,
`large` 60-80, `huge` 85-89 -- 400 stratified probes (nearby, 1.1x-
scaled variants of sampled rows, same construction as `fpr`'s own
probes), both opclasses built fresh.

**Correctness held exactly in every band**, huge included -- brute
force, box-indexed, and multicap-indexed all agree precisely (e.g.
495,873 matches in the huge band out of 1,500 x 99 probe-row pairs; the
enormous match count is itself expected and not a red flag -- at these
radii most circles genuinely do overlap most others on a uniformly-
populated sphere).

**But the box key's advantage over multi-cap doesn't just shrink here --
it reverses**, and the crossover is not subtle:

| band | box buffers | multi-cap buffers | winner |
|---|---|---|---|
| small (1-3 deg) | 17,359 | 25,154 | box, 1.45x fewer |
| medium (20-40 deg) | 286,587 | 244,633 | **multi-cap, 1.17x fewer** |
| large (60-80 deg) | 485,932 | 452,201 | **multi-cap, 1.07x fewer** |
| huge (85-89 deg) | 520,287 | 490,576 | **multi-cap, 1.06x fewer** |

Reproduced on a full index rebuild for the medium and huge bands
(286,587 -> 288,680; 520,287 -> 521,177 -- noise-level, confirms this is
real, not a one-off).

**Why, and why the pole addendum's reasoning doesn't rescue this case**:
a circle region's multi-cap key *is* the circle -- a single exact cap,
no decomposition, no approximation loss, at any radius, since round
three's multi-cap design only decomposes *polygons* into sub-caps; a
circle already fits in one. The box, by contrast, squeezes that same
circle into an axis-aligned box at every radius, and how much that costs
depends on the circle's size and orientation relative to the coordinate
axes -- worst in roughly this round's medium-to-large range, not
monotonically worsening toward 90 degrees the way a naive "bigger is
always looser" guess would predict (the pole addendum's own hemisphere
case, a 90-degree cap centred exactly on an axis, works out to an
*exact* box: x/y extents of +-1 and z extent of [0,1], matching the true
hemisphere precisely -- it's off-axis, medium-to-large caps that pay the
real cost, not the extreme-radius ones specifically). The multi-cap
key's per-circle exactness doesn't degrade with radius at all, so once
the box's slack-driven false-positive rate outgrows the box's per-
comparison cost advantage, multi-cap wins back the tradeoff -- and does,
starting well before a hemisphere.

**Net effect on round forty-two's conclusion**: the "box wins
everything" framing holds for the regime this whole investigation has
actually benchmarked -- realistic catalog footprints, arcsec to a few
degrees, `fpr`'s own 0.02-0.3 degree range included -- but does not
generalize to wide-area queries (all-sky cross-matches, large cone
searches, hemisphere-scale footprints). For that regime the shipped
multi-cap opclass remains the better choice, not just the safer default.
This is a real, radius-dependent tradeoff between the two opclasses, not
a flaw in either -- and a concrete argument, on top of round forty-two's
own closing note, for keeping both available rather than picking one
over the other.

**Shipped**: the decision landed on keep-both, not promote-and-replace.
`skyregion_box_gist_ops` is now registered in the extension's own
versioned SQL (`ext/sql/skycell--0.12--0.13.sql`, folded into
`ext/sql/skycell--0.13.sql`, `default_version` bumped to `0.13`) rather
than the ad-hoc `splitcost_test`-only registration this round started
with -- both the `ALTER EXTENSION ... UPDATE TO '0.13'` upgrade path and
a fresh `CREATE EXTENSION` land on it, verified directly against a real
database rather than assumed. Still not `DEFAULT` (`skyregion_gist_ops`
keeps that): select it explicitly with `USING gist (col
skyregion_box_gist_ops)`. Documented in `README.md`'s "Operators -- the
indexable spelling" section as the opt-in choice for columns known to
hold small, catalogue-scale footprints (arcsec-few degrees) specifically
-- not a general recommendation, given the large-radius reversal just
above.

## Round forty-three: a Spherical-Cap GiST for *points* -- the mirror
## image of round forty-two, a genuine win in part of its range, and a
## bigger fix to the B-tree rewrite path found along the way

Round forty-two's box opclass is for `skyregion`; everything in this round
is instead about `skypos` -- a new, separate opclass (`gist_point_cap.c`,
`skypos_cap_gist_ops`) competing directly with pgSphere's native `spoint`
GiST and skycell's own SP-GiST (`skypos_spgist_ops`), not with the region
opclasses above. First, pgSphere's own key was confirmed empirically
rather than assumed: a one-row `spoint`-GiST index inspected with
`pageinspect`'s `gist_page_items()` showed a leaf tuple's key as two
identical 3-tuples (a degenerate min=max box around the single point),
`itemlen=32` -- 24 bytes of payload, i.e. six *float* (not double)
coordinates. So pgSphere's key is a box, lossy even at the leaf, at half
the per-coordinate precision skycell's own double-precision opclasses use.

**The idea** (the user's): leaves store the exact point as a zero-radius
cap; internal nodes store a bounding `GistCap` (centre + angular radius);
pruning is `cap_overlaps()` against the query region's own bounding cap;
`picksplit()` minimises cap overlap, not bounding-box overlap. Structural
motivation: for an isotropic point cluster (a globular cluster, a density
peak, a HEALPix-sorted run of nearby catalogue sources -- the common case
in a real catalogue), a bounding cap is tighter than pgSphere's bounding
box, the same circle-in-a-square argument as round forty-two's own file
header, just run in the opposite direction: there, a box beats a multi-cap
at bounding an odd-shaped *region*; here, a cap beats a box at bounding an
isotropic *cluster of points*.

**Round one -- double precision, exact leaf test, no recheck at the
leaf**: `GistCap` (four doubles, 32 bytes) reused from `gist_region.c`
(duplicated, not exported, same reasoning as every prior round -- minimise
blast radius on proven code). `picksplit()` is the single-cap analogue of
`skyregion_gist_picksplit()`: the same R*-tree axis-sort/margin/overlap
sweep, simpler throughout since each entry holds exactly one cap, not a
multicap union of up to four. `consistent()` exploits the point being
stored losslessly at the leaf: `sc_region_contains()` against the exact
point, `recheck=false` -- a real structural edge pgSphere's own leaf
lacks, which is always a lossy box even for one point.

Correctness: 0 mismatches, 210 brute-force probes (1" to 3 degrees) against
a 10M-row synthetic corpus (`cat_pos`, the same `src_designed` table used
throughout the paper's own benchmarks). Measured against pgSphere's native
GiST (`cat_sphere_idx`) and skycell's own SP-GiST (`cat_pos_spgist`),
buffer counts (`EXPLAIN (ANALYZE, BUFFERS)`, warmed, 20 probes/radius):

| radius | cap-GiST | pgSphere | skycell SP-GiST |
|---|---|---|---|
| 1" | 11.8 | **5.6** | 16.2 |
| 10" | 11.9 | **5.6** | 13.4 |
| 1' | 14.2 | **6.6** | 16.5 |
| 6' | 14.1 | **6.6** | 16.3 |
| 30' | **30.8** | 40.6 | 34.8 |
| 1 deg | **74.2** | 161.4 | 83.6 |
| 3 deg | **537.7** | 575.9 | 579.6 |

Two findings, both clean: the new opclass beats skycell's own existing
SP-GiST opclass at *every* radius tested (a strict improvement over what
skycell already ships for this strategy). Against pgSphere, there's a
sharp, monotonic crossover around 10'-30': pgSphere wins decisively below
it (its smaller, lossy-anyway box key gives better fanout when few
candidates matter regardless of pruning precision), the cap-GiST wins
decisively above it (2.2x fewer buffers at 1 degree) -- the isotropic-
cluster argument, actually paying off, in the regime it predicts. Costs:
build time ~7-8.5 minutes (generic buffered GiST build, no bulk-load fast
path) vs pgSphere's ~2 minutes and SP-GiST's ~28 seconds; index size 1031
MB vs pgSphere's 683 MB and SP-GiST's 451 MB -- both traceable to the
32-byte double-precision key against pgSphere's 24-byte float one.

**A critical correction, found by checking median against mean**: buffer
counts favoured the cap-GiST from 30' on, and an early wall-clock mean
comparison seemed to agree (e.g. 4.3ms vs pgSphere's 7.2ms at 3 degrees).
That mean was an artifact. `src_designed`'s corpus is 60% clustered
(six clusters including both poles and RA=0); a handful of probes landing
near real structure produced extreme per-probe outliers (one 3-degree
probe: 28.8ms for the cap-GiST, dwarfing its other ~0.7-2ms probes) that
dominated the average for *every* tag, pgSphere's own mean included
(pgSphere's 3-degree mean of 7.2ms was itself outlier-inflated; its
*median* was 1.346ms). Recomputing with medians throughout reverses the
wall-clock verdict entirely: **pgSphere is faster at every single radius
tested**, cap-GiST's buffer-count win notwithstanding. Mean vs median
is not a stylistic choice here -- it changes which opclass "wins."

**Round two -- float precision (reverted)**: shrinking `GistCap` to
pgSphere's own 16-byte float layout shrank the index (1031 MB -> 711 MB)
and *improved* buffer counts further at every radius from 1' on (-16% at
3 degrees) -- the smaller-key hypothesis, confirmed. But it had to give up
the exact, `recheck=false` leaf test: a float-rounded point has no slop
margin `sc_region_contains()` can safely use (a true match rounded just
outside the query boundary would be silently, unrecoverably dropped), so
every leaf candidate needed `recheck=true` instead, re-invoking the real
`<@` operator on every matching row. That cost scales with *result row
count*, not buffer count, and at the radii where the pruning edge over
pgSphere shows up, result counts are large enough that every query got
3-5x slower in wall-clock despite touching fewer pages -- confirmed
per-probe, not just in the average (a ~100-buffer probe: 0.7ms -> 2.5ms;
the single densest probe measured: 28.8ms -> 141ms, the same ~5x ratio at
both scales). Reverted; the double-precision, exact-leaf design is kept.

**Round three -- a safer shrink (kept)**: the radius field, not the
centre, was the real opportunity -- it is only ever used in the already-
lossy, already-`recheck=true` internal-node test, and at a leaf it is
always exactly 0 regardless of what precision could represent it in.
Leaf tuples now store only `(cx, cy, cz)` as doubles (24 bytes, radius
implicit 0 -- centre precision, and the exact leaf test, fully intact);
internal tuples store double centres plus a *packed* float radius (28
bytes; a naive `{double,double,double,float}` struct rounds up to 32
under default alignment, silently erasing the saving -- caught by
checking `sizeof()` directly before trusting it, not assumed). The format
is self-describing via `VARSIZE`, since one `compress()`/`union()`/
`same()`/`consistent()` must handle whichever shape a given page holds.
Correctness: 0/210 again. Buffers improved 2.8%-11% across every radius
(smaller internal keys, better fanout) with no recheck cost anywhere,
widening the already-existing margin over pgSphere at 30'+ (e.g. 1.48x
fewer buffers at 30' vs round one's 1.32x).

**The trig-free `cap_overlaps()` fix**: with buffers improving, median
wall-clock still showed pgSphere faster at *every* radius, including 30'
(pgSphere 4.5x faster despite the cap-GiST's 33% fewer buffers) --
confirming buffer counts, even accurate ones, don't capture everything.
`cap_overlaps()` (every internal node visited, every query) used
`sc_angle()`: a cross product, a `sqrt`, and an `atan2`. pgSphere's own
internal box-vs-box test is six plain comparisons, no trig at all. Round
twenty-seven's own precedent (`gist_region.c`'s `cap_contains_point`)
already established the fix for this shape of test: `angle(a,b) <= sum`
is exactly `dot(a,b) >= cos(sum)` for `sum` in `[0, pi]`, one dot product
and one `cos()` instead of a cross product plus `atan2` (with `sum >= pi`
handled as an unconditional true, since any two points are within pi of
each other). Applied, correctness-verified again (0/210), measured: a
modest, somewhat radius-dependent improvement (roughly -15% to -30% at
small-to-medium radii, flat at 1 degree and 3 degrees) -- real, but far
short of closing the gap to pgSphere, which remained faster at every
radius by median even after this fix.

**Selectivity: the actual dominant effect, and a real bug on the way
there.** `EXPLAIN` on the cap-GiST plan showed the planner estimating
10,000 rows for a query that actually matched 58 -- a 172x overestimate --
which pushed it onto a `Bitmap Index Scan` + `Bitmap Heap Scan` (built for
an estimated-large result) where pgSphere's own, well-estimated `<@` got a
cheap plain `Index Scan` for the same 58 rows. This was *not* specific to
the new opclass: skycell's existing, already-shipped SP-GiST opclass got
the identical flat 10,000-row estimate and the identical plan shape, since
both share the same `<@(skypos,skyregion)` operator, declared `RESTRICT =
contsel` -- PostgreSQL's generic, statistics-free containment-operator
default. `skycell_pos_in_region` (the operator's backing function) already
carries a `SUPPORT skycell_region_support` clause, but per PostgreSQL's
own documented rule (`nodes/supportnodes.h`): *"If the target function is
being used as the implementation of an operator, the support function
will not be used [for selectivity]; the operator's restriction or join
estimator is consulted instead."* A `SupportRequestSelectivity` handler on
that support function would be dead code for this operator's actual query
shape -- confirmed, not assumed, by checking the header before writing
any of it. The fix had to be the classic `oprrest`-shaped mechanism
instead: new functions `skycell_pos_region_sel`/`skycell_region_pos_sel`
(signature `(internal,oid,internal,int4) returns float8`, exactly
`contsel`'s own shape), assigned via `ALTER OPERATOR ... SET (RESTRICT =
...)`, reusing the `area(region)/4*pi` "uniform sky" estimate
`skycell_region_sel_support` already computes for the post-rewrite exact
test. Measured: row estimate corrected from 10,000 to 190 (actual: 58,
matching pgSphere's own estimate almost exactly), plan correctly switches
to a plain `Index Scan`. Clear win for ordinary (sparse-sky) queries at
every radius.

**But a real regression for queries landing in or near a genuine density
cluster**, found directly, not hypothesised: per-probe buffer counts at
30' showed a handful of extreme outliers (627, 446, 371, 249 buffers
against a typical ~20-60) after the fix, where before the flat estimate
had accidentally kept the (correct, needed) `Bitmap Scan` for exactly
these probes. A uniform-sky estimate understates a cluster-hit query's
true row count, flipping the plan to a plain `Index Scan` where the
overshoot actually needed a bitmap's heap-page consolidation. Not a bug --
the known, accepted limitation of ignoring real density -- but real on
this 60%-clustered corpus. A density-aware refinement was built to fix it
(`region_pos_density_sel()`, reusing `region_support_simplify()`'s own
`cell_expr_for_point()`/`density_for_expr()`/`cover_cached()` machinery to
get `cov.exp_rows` -- a histogram- or multi-order-count-map-based
estimate, not a uniformity assumption) and confirmed working when a
`skycell_cell(pos)` expression index exists. But it is **architecturally
inert for the cap-GiST's own deployment shape**: the very same expression-
index lookup also gates `skycell_region_support`'s unconditional
`SupportRequestSimplify` rewrite, so the moment an index exists to make
density-aware selectivity possible, the clause gets rewritten into B-tree
range conditions before the GiST opclass's own selectivity -- or its
index -- is ever consulted. Confirmed directly: creating the expression
index changed the `EXPLAIN` plan from the GiST index to the B-tree rewrite
outright, for both the old and new selectivity code. The density-aware
function is correct and kept (harmless, falls back to the uniform-sky
estimate when no such index exists, and it does narrow the B-tree
rewrite's own cost model's blind spot -- see below), but closing this
specific gap for real means deciding which strategy wins when both a
cell index and a GiST index exist on the same table, which is the
subject of the gate below, not this selectivity fix alone.

**A direct B-tree-vs-pgSphere comparison, prompted by the discovery
above, and an apparent inversion of an earlier finding, explained.**
With a freshly-built `skycell_cell(pos)` expression index (plain
`ANALYZE`, no `skycell_density_build()` multi-order map) enabling the
classic rewrite, same corpus, same session:

| radius | B-tree buffers | pgSphere buffers | B-tree median ms | pgSphere median ms |
|---|---|---|---|---|
| 1" | **4.0** | 5.6 | **0.009** | 0.016 |
| 10" | **4.2** | 5.6 | **0.010** | 0.051 |
| 1' | **7.0** | 6.6 | **0.028** | 0.037 |
| 6' | 6.9 | 6.6 | 0.050 | **0.040** |
| 30' | **23.1** | 40.6 | 0.269 | **0.056** |
| 1 deg | **57.5** | 161.4 | 0.618 | **0.207** |
| 3 deg | **357.8** | 575.9 | 6.285 | **1.346** |

B-tree wins on buffers at *every* radius, and on wall-clock too below
1' -- but loses badly from 30' on (4.8x slower at 30', 4.7x slower at 3
degrees) despite fewer pages touched. `EXPLAIN (ANALYZE, BUFFERS)` at 3
degrees showed why directly: `Rows Removed by Filter: 1696` out of 4092
candidates -- the covering's overshoot means 41% of fetched rows are
rejected by the exact `skycell_in_region` test, a real per-row CPU cost
(plus six separate `BitmapOr`-combined ranges' own overhead) that scales
with candidate count, not with pages touched. This looks like close to
the *inverse* of a much earlier bootstrap-significance finding in this
same investigation (tied at 1"/1 degree, confidently slower 10"-30',
confidently faster at 3 degrees) -- flagged honestly as a likely artifact
of comparing a quickly-built expression index with only a plain `ANALYZE`
histogram against that earlier setup's probable use of
`skycell_density_build()`'s finer multi-order density map (a cruder
density picture picks a looser covering, which overshoots more at large
radii) rather than a proven reversal of the underlying architecture --
not confirmed by rebuilding the finer map and re-testing.

**The gate: `skycell.rewrite_max_waste`, a conditional B-tree rewrite.**
The B-tree-vs-pgSphere table's own mechanism is the fix: the covering's
*ratio* (`sel = reg.area/cov.area`) doesn't predict the problem (sel was
*lower* at 1" than at 3 degrees, yet 1" was the B-tree's best case) --
what predicts it is the *absolute* expected waste, `cov.exp_rows * (1 -
sel)`, measured directly across all seven standard radii: ~4, 4, 26, 87
(all B-tree wins) vs ~223, 668, 2104 (all B-tree losses). `region_support_
simplify()` already computes exactly this value for its own exact-test
selectivity hint; it just never used it to gate whether to rewrite at
all. New GUC `skycell.rewrite_max_waste` (default 100.0, a `DefineCustom
RealVariable` alongside skycell's existing cost-model knobs): past this
many expected wasted rows, `region_support_simplify()` returns `NULL`
instead of rewriting, leaving the original `<@`/`@>` clause in place for
the planner's own cost-based index selection -- which now has a real,
non-default selectivity estimate for that clause either way, from the
fix above.

**A real bug, found by the crash it caused, not by inspection.** The
first version of this gate called `sc_cover_free(&cov)` before returning
`NULL`, matching the instinct that a function receiving a struct should
clean it up. The existing `region_support_simplify()` success path has
never freed `cov` -- for a reason this change didn't account for:
`cover_cached()` (used for the cone branch) returns `entry->r`, a pointer
*directly into its own long-lived cache entry* (`skycell_cache_cxt`), not
a caller-owned copy. `sc_cover_free()` on it `pfree`s the cache's own
backing array out from under every future hit on that key, corrupting the
memory context. The failure mode was exactly as nasty as that implies:
not an immediate crash, but a segfault several *unrelated* queries later
(`terminated by signal 11`, PostgreSQL's own log), whichever later
allocation from the corrupted context happened to hit the damage first.
Found by reproducing a crash during a routine 20-probe benchmark loop,
bisected to the exact probe (the 11th of 20) by replaying the identical
loop with `RAISE NOTICE` markers between each statement, and root-caused
by reading `cover_cached()`'s own source rather than guessing. Fixed by
removing the `sc_cover_free()` call (both the new gate and a second,
identical copy in the new `region_pos_density_sel()` selectivity
function had the same bug) -- matching, not fighting, the existing code's
own established convention of never freeing `cov`. Reproduced the exact
crash before the fix and confirmed it gone after, on the same input, not
just re-run-and-hope.

**The gate, measured, with both an index and a GiST alternative present
on the same table** (`cat_pos_cellexpr` B-tree + `cat_pos_capgist`, the
default threshold of 100):

| radius | adaptive ms | pure B-tree ms | pgSphere ms | adaptive buffers | chose |
|---|---|---|---|---|---|
| 1" | 0.015 | 0.009 | 0.016 | 4.0 | B-tree |
| 10" | 0.020 | 0.010 | 0.051 | 4.2 | B-tree |
| 1' | 0.027 | 0.028 | 0.037 | 7.5 | B-tree |
| 6' | 0.071 | 0.050 | 0.040 | 8.9 | B-tree |
| 30' | **0.211** | 0.269 | 0.056 | 125.6 | GiST |
| 1 deg | **0.492** | 0.618 | 0.207 | 190.4 | GiST |
| 3 deg | **1.581** | 6.285 | 1.346 | 496.9 | GiST |

(median ms, 20 probes/radius.) At 3 degrees the gate is **4x faster than
always-rewriting** (1.581ms vs 6.285ms) and lands within 17% of pgSphere,
instead of 4.7x behind it -- automatically, per query, with no manual
index choice. Small radii track pure B-tree closely (the gate correctly
keeps the rewrite) and beat pgSphere as before. One soft spot: 6' came
out slightly worse than both alternatives (0.071ms vs 0.050/0.040ms) --
its estimated waste (~87 rows) sits just under the 100-row default, so
the gate still rewrites there; a single borderline data point, plausibly
tunable with a lower threshold, not investigated further this round. The
gate does not fully close the 30'-1 degree gap to pgSphere (the available
fallback, cap-GiST, is not as fast as pgSphere specifically in that
window -- see the median-vs-mean correction above), but it does exactly
what it was built for: removes the B-tree rewrite's large-radius blind
spot automatically, while keeping its small-radius strength, without
forcing a static per-table choice between the two strategies.

**STATUS**: `gist_point_cap.c` (round three's double-precision/safer-
shrink/trig-free design) is correctness-verified (0/210 at every stage)
and measured as a strict improvement over skycell's shipped SP-GiST
opclass, with a real, structurally-explained win over pgSphere from
roughly 30' on and a real, structurally-explained loss below it -- not
yet wired into any versioned SQL file; register ad hoc (`CREATE FUNCTION
... AS 'MODULE_PATHNAME'; CREATE OPERATOR CLASS ...`) against a scratch
database until a shipping decision is made. The selectivity fix
(`skycell_pos_region_sel`/`skycell_region_pos_sel`, replacing `contsel`
on the raw `<@`/`@>` operators) and the rewrite gate (`skycell.rewrite_
max_waste`, defaulting to 100.0) touch skycell's existing, already-
shipped B-tree rewrite path directly and are a more consequential change
than anything else in this round -- correctness-verified via `make
installcheck` and the same 210-probe brute-force suite, but the
threshold is empirically chosen from this one corpus's measured
crossover, not derived from a general cost model, and is a GUC
specifically so it can be retuned (or set arbitrarily high to recover
every prior version's always-rewrite behaviour) per deployment.

## Round forty-four: chasing an old "26% faster" cold-cache number --
## not reproducible, and why, plus what a real per-radius-group cold
## run actually shows

A much earlier part of this same investigation (this whole document's
branch is one long-running session, just repeatedly summarised) had
quoted a comparison table with a "26% faster" cold-cache figure for
skycell vs pgSphere at 3 degrees. Asked to explain the method behind it,
nothing in-context reproduced it, which is itself worth recording rather
than quietly re-deriving a number and presenting it as settled.

**Finding the actual historical methodology.** This entire investigation
turns out to be one continuous Claude Code session going back to the
project's early history, not a series of separate ones -- its own
transcript (`~103 MB` of JSONL) is searchable, and grepping it for the
leftover `cold_probe` table's name in `paper_bench` found the exact
commands that built it: a single `sudo service postgresql restart`,
followed by looping over *all seven* radius labels and both methods (2
loops, 1,456 query pairs) in one uninterrupted pass, never restarting or
dropping caches again in between. That means only the earliest-run label
(1") was ever genuinely cold; by the time the loop reached 3 degrees --
last in the ordering, and the smallest group at only 16 pairs -- roughly
1,440 other queries had already run, substantially rewarming the cache
the "cold" label was nominally measuring against.

**Two hypotheses tested directly, neither explained it.** The surviving
`cold_probe` rows still exist in `paper_bench`; recomputing from them
with different aggregations gave 36.3% (ratio of means) and 5.1% (ratio
of medians) -- neither close to 26%, and no principled reason to prefer
one over the other without knowing which the original number used.
Separately, asked whether disabling `skycell.use_stats` (no density
correction, pure uniform-sky covering) might explain the gap: tested
directly at 3 degrees, same corpus, same session -- it made the B-tree
path *faster in absolute terms* (3.291ms median vs 6.285ms with stats)
but it was still ~2.4x slower than pgSphere's 1.346ms, moving in the
wrong direction to explain a 26%-faster result.

**The aggregation that came closest, and why it still isn't trustworthy.**
The mean of *per-trial paired ratios* `(1 - sky_ms/pg_ms)`, matching
REPRODUCING.md's own stated protocol ("every comparison is... analysed
paired"), gave 28.2% at 3 degrees for the historical data -- close to 26%
-- but applying the identical formula to the smaller radii in the same
table produced nonsense: -326%, -1,116%, -1,863%. A ratio denominator
that happens to be small on a single noisy trial makes `1 - x/small`
blow up, and cold-cache timing is exactly the kind of noisy, heavy-tailed
data where that happens. So this statistic is not a generally trustworthy
aggregation; its apparent 28.2%-vs-26% closeness at 3 degrees specifically
is more likely a coincidence of scale (large radius, larger, more stable
per-query times) than a hint at the real underlying number.

**The deeper problem: `/proc/sys/vm/drop_caches` is a no-op here.**
Tested directly rather than assumed. A query was run cold (1211ms, 76
disk reads), repeated immediately (0.87ms, all cache hits -- expected),
then `echo 3 > /proc/sys/vm/drop_caches` (exit 0, no error) was followed
by a full `postgres` restart and the *same* query a third time: 10ms,
with the identical 76 "read" (not "hit") count PostgreSQL's own
`shared_buffers` correctly reported as cleared -- but 120x faster than
the original cold run. The pages were still being served from a page
cache that never actually got dropped; the write to `drop_caches`
silently succeeds at the file-permission level without the host-level
privilege to do anything, a known container limitation. A second test
with a brand-new, never-before-touched region confirmed the other half
of this: first touch after a fresh restart+drop was genuinely slow
(903ms, real reads), and the immediate repeat was fast (2ms) -- so a
first-ever cold touch is real and measurable here, it is *re-cooling
already-touched data* that silently fails. A more disciplined version of
the *same* restart+drop_caches technique (e.g. per radius group instead
of once) would therefore inherit the identical flaw -- the fix has to
route around the broken primitive, not use it more carefully.

**The fix: fresh, never-queried coordinates per radius group, restarted
before each.** `shared_buffers`-clearing via a full `service postgresql
restart` works reliably (confirmed above); what doesn't is reusing
previously-touched coordinates and expecting a cache drop to re-cool
them. So: 1,456 brand-new probe centres were generated (`bench_centers`,
`qid` 10001-11456, matching `bench/03_cone.sql`'s own generation formula,
sample sizes per label, and `kind` mix exactly, just reseeded), and a
driver script restarts PostgreSQL before *each* of the seven labels in
turn, running only that label's (fresh) pairs before moving to the next
-- reusing the project's own existing `cone_sql()`/`cat_cell`/
`cat_sphere` methodology unchanged, so this is directly comparable to
`bench/03_cone.sql`'s own headline numbers. One bug on the way, the exact
one this investigation's own history already named once before (a label
containing an apostrophe, `1'`, breaking a hand-built SQL string literal)
-- recurred because this script was written fresh rather than reusing the
earlier fix, fixed the same way (double the embedded quote before
interpolating), and recovered without re-running (and so re-warming) the
two labels that had already completed successfully before the crash.

**The result, and it is clean: under genuine cold-cache conditions,
skycell beats pgSphere at every single radius**, on both buffers and
wall-clock:

| radius | skycell buffers | pgSphere buffers | skycell mean ms | pgSphere mean ms | skycell faster by |
|---|---|---|---|---|---|
| 1" | **4.2** | 5.7 | **88.6** | 118.7 | 25% |
| 10" | **4.7** | 5.8 | **25.8** | 29.7 | 13% |
| 1' | **6.4** | 7.7 | **20.5** | 25.8 | 20% |
| 6' | **9.1** | 12.8 | **9.1** | 22.2 | 59% |
| 30' | **28.6** | 72.9 | **13.9** | 26.1 | 47% |
| 1 deg | **57.9** | 193.1 | **18.0** | 32.9 | 45% |
| 3 deg | **196.5** | 327.8 | **34.5** | 72.4 | 52% |

skycell touches fewer pages than pgSphere at every radius tested here
(confirmed, not assumed -- these buffer counts are deterministic and
reproduce exactly across repeated runs), and when each uncached page
costs real disk latency, that page-count advantage is the whole story:
it wins uniformly. This is a genuine reversal of the warm-cache picture
measured earlier in round forty-three, where pgSphere often won at
medium-large radii because of skycell's own exact-filter CPU cost on
false-positive candidates -- a cost that is irrelevant when I/O, not CPU,
is the bottleneck.

**"What happens without a cold buffer?"** -- the identical, freshly-
generated query set was run again immediately afterward (shared_buffers
now warm, double-pass with the first discarded to avoid measuring
PostgreSQL's own cold start), giving a direct cold-vs-warm comparison on
the same queries rather than a different historical sample:

| radius | warm pgSphere ms | warm skycell ms | cold pgSphere ms | cold skycell ms | pgSphere cold/warm | skycell cold/warm |
|---|---|---|---|---|---|---|
| 1" | 0.031 | 0.018 | 118.70 | 88.60 | 3851x | 4888x |
| 10" | 0.028 | 0.023 | 29.70 | 25.75 | 1079x | 1116x |
| 1' | 0.031 | 0.035 | 25.78 | 20.52 | 823x | 593x |
| 6' | 0.051 | 0.064 | 22.16 | 9.07 | 431x | 142x |
| 30' | 0.305 | 0.294 | 26.09 | 13.90 | 85x | 47x |
| 1 deg | 0.948 | 0.729 | 32.90 | 17.97 | 35x | 25x |
| 3 deg | 3.327 | 2.391 | 72.41 | 34.46 | 22x | 14x |

Two things stand out. First, the cold penalty is enormous at small radii
(nearly 4,000-4,900x at 1 arcsecond) and shrinks steadily with radius
(down to 14-22x at 3 degrees) -- not because cold reads get cheaper, but
because warm per-query cost grows with radius too (more rows touched even
when every page is a cache hit), while the absolute cold penalty per page
stays roughly constant, so the *ratio* compresses as the warm baseline
rises. Second, and more telling: **the warm-cache verdict is not
uniform the way cold was.** pgSphere wins at 1' and 6' when warm (0.031
vs 0.035, 0.051 vs 0.064) -- skycell loses there by the same mechanism
round forty-three's `Rows Removed by Filter` diagnosis already
identified (false-positive candidates from the covering's overshoot cost
real per-row CPU once I/O is free) -- while skycell wins everywhere else,
cold or warm. Buffers were identical between the cold and warm runs for
every radius (deterministic, as expected), which is exactly what makes
the contrast informative: the *only* thing that changed between these
two tables is whether those buffer reads cost real I/O latency or a
cache hit, and that alone decides whether skycell's fewer-pages advantage
or its exact-filter CPU tax is the one that matters.

**Synthesis, tying this round to round forty-three's own lesson.** Buffer
counts predict performance cleanly in an I/O-bound (cold) regime, where
this round found skycell wins everywhere because it touches fewer pages
everywhere. They predict nothing reliable in a CPU-bound (warm) regime,
where round forty-three already found the cap-GiST opclass could win on
buffers and still lose on wall-clock, and where this round finds
skycell's own B-tree rewrite can do the same -- both traceable to the
same root cause, a real per-candidate CPU cost (an exact geometric test
for the B-tree rewrite, `cap_overlaps()`'s trig for the cap-GiST opclass)
that a page count does not capture. Neither "buffers" nor "wall-clock"
alone tells the whole story in general; which one dominates depends on
whether the pages in question are likely to be resident, which depends
on the deployment's actual cache-to-data-size ratio -- not something
either number alone can report.

**STATUS**: a measurement round, no production code changed. The old
"26% faster" figure is retired -- not reproducible under controlled
conditions, traced to an uncontrolled, partially-rewarmed cache state
from a single historical restart plus a numerically fragile aggregation
method that happened to land close by coincidence of scale. In its
place: a real, reproducible cold-cache methodology (fresh coordinates
restarted per radius group, sidestepping a confirmed-broken `drop_caches`
rather than trusting it) and a clean result from it -- skycell beats
pgSphere at every radius when genuinely cold, loses at 1'-6' when warm,
for the same mechanistic reason round forty-three already diagnosed for
a different opclass. `skycell.rewrite_max_waste` (round forty-three) was
tuned from warm-cache measurements; this round's warm re-measurement
reproduces that same crossover independently, which is some corroboration
that the threshold is measuring a real effect and not an artifact of the
corpus it was first tuned on -- though a deployment that is reliably
I/O-bound (a cold-cache-dominated workload, or a dataset much larger than
available cache) would want a much more permissive threshold than one
tuned warm, since this round shows the two regimes can disagree about
which strategy wins at the same radius.

**Follow-up, same round: made the threshold cache-state-aware instead of
asking for a manual choice between the two.** The first instinct this
round's own closing note raised -- "a cold-cache-dominated deployment
would want a much more permissive threshold" -- was nearly implemented
backwards (lowering the threshold, which would have made the gate
abandon the B-tree rewrite even more readily, exactly wrong given cold
cache is where it wins most broadly here). Caught before writing any
code by walking through this round's own table again: at 30'/1 degree/3
degrees, the radii with the *most* estimated waste, skycell still won
under cold cache by 45-52%, meaning a cold-dominated deployment wants the
gate to tolerate *more* waste there, not less.

PostgreSQL's own planner already has a GUC for exactly the signal this
needed -- `effective_cache_size`, the planner's own estimate of how much
of a relation's data a backend can expect to find resident, used
internally the same way (`index_pages_fetched()` in `costsize.c`'s
Mackert-Lohman-style heuristic) to decide how many of a scan's pages will
be cache hits versus real I/O. `rewrite_waste_threshold()` (`skycell.c`)
reuses it directly: `cache_frac = min(1, effective_cache_size / relpages)`,
`threshold = skycell.rewrite_max_waste / cache_frac`. When the relation
comfortably fits in `effective_cache_size` (`cache_frac` = 1), the
threshold is exactly the tuned default, unchanged from round forty-three;
as the relation grows past it, the threshold scales up without bound,
the same direction this round's cold-cache table demands. Deliberately
the same level of approximation as `auto_range_cost()`'s own existing
heuristic -- a simple ratio, not a reproduction of PostgreSQL's actual,
more elaborate formula -- for the same reason: legibility over precision
for a GUC whose own threshold was already an empirical choice, not a
derived one.

Verified directly, not just by inspection: with `cat_pos_capgist` valid
and `effective_cache_size` left at its default (5GB, comfortably above
`cat_pos`'s own size), a 3-degree query still chose the cap-GiST index,
identical to round forty-three's own finding -- no behavioural change at
realistic settings. Setting `effective_cache_size = '1MB'` (simulating a
relation far larger than available cache) on the *same* query flipped the
plan straight to the B-tree rewrite (`BitmapOr` over `cat_pos_cellexpr`,
the `skycell_in_region` exact-test filter) -- the threshold scaling
working exactly as designed, confirmed by watching the actual plan change
rather than trusting the arithmetic alone. `make installcheck` passes
unchanged.

**Second follow-up, same round: `effective_cache_size` swapped for
`NBuffers`, an actual measurement instead of an admin guess.**
`effective_cache_size` is not tied to anything the server actually holds --
it is a standalone GUC an admin sets (often left at build-in defaults, or
sized for a machine the server no longer runs on), with no mechanism
keeping it truthful. `NBuffers` (`miscadmin.h`, already included) is the
real, already-allocated size of the shared buffer pool: a measurement, not
a configured guess. `rewrite_waste_threshold()` now reads `cache_frac =
min(1, NBuffers / relpages)` in its place -- same formula, same shape,
grounded in memory PostgreSQL actually has rather than memory an admin
said it should assume.

The tradeoff: `NBuffers` only counts `shared_buffers`, not the OS page
cache behind it, so a relation can be fully OS-cached and still read as
"past cache" here, scaling the threshold up more readily than
`effective_cache_size` (conventionally sized at 50-75% of system RAM,
covering the OS cache too) would have. That is the same direction round
forty-four's own table already argued for, not a new risk: the regime
this makes the gate more tolerant of waste in (big-relative-to-
shared_buffers) is exactly the regime this round measured skycell's
rewrite winning most broadly in, and the one case this scaling leaves
alone -- `cache_frac` = 1, relation fits in `shared_buffers` -- is the
fully-warm case the original tuned default was already measured against.

Verified the same way as the first follow-up, but driving a real
`shared_buffers` change rather than a session-local GUC override (`NBuffers`
is fixed at postmaster start, so this needs an actual restart):
`shared_buffers` at its working default (2GB, far above `cat_pos`'s 93504
pages) left the 3-degree query choosing the cap-GiST index at the
unmodified default `skycell.rewrite_max_waste` (100) -- `effective_cache_size`
sitting untouched at 5GB the whole time, confirming the old GUC is no
longer read. Setting `shared_buffers = '16MB'` (2048 pages, far below
`cat_pos`'s size) in `postgresql.auto.conf` and restarting flipped the
*same* query to the B-tree rewrite with that *same* unmodified default
threshold -- no manual GUC override at all, the scaling now driven purely
by the server's real memory. `shared_buffers` was restored to 2GB and the
server restarted again before re-running `make installcheck`, which still
passes.

**Checked, not adopted: a `pg_buffercache`-based variant.** `NBuffers`
answers "how big is the pool," not "are *this relation's* pages actually
in it" -- the latter is what `pg_buffercache` exists to answer, and it is
available in this environment (`pg_buffercache--1.4`, contrib, already
installed alongside core). Tried it directly against `paper_bench` rather
than reasoning about it in the abstract:

```
CREATE EXTENSION pg_buffercache;
SELECT count(*) FROM pg_buffercache;                                    -- 262144 rows, 55 ms
SELECT count(*) FROM pg_buffercache WHERE relfilenode = ...('cat_pos');  -- 185 ms
SELECT * FROM pg_buffercache_summary();                                  -- 2-4 ms, global only
```

Two real options, both rejected:

- `pg_buffercache_pages()` (what the view is built on) gives true
  per-relation residency, but it is an `O(NBuffers)` scan that
  materializes one row per buffer -- 55-185ms here at a 2GB
  (262144-buffer) pool that is small by production standards; it scales
  linearly with `shared_buffers`, so a 32GB pool would cost on the order
  of a full second. `rewrite_waste_threshold()` runs inside
  `SupportRequestSimplify`, i.e. during planning, for every query whose
  plan reaches this gate -- paying a scan that dwarfs the rest of
  planning combined to decide a heuristic threshold is backwards, the
  cost this gate exists to budget would be the smaller number.
- `pg_buffercache_summary()` is cheap (2-4ms, no per-row materialization)
  but only global (`buffers_used` out of `NBuffers`, pool-wide) -- it
  cannot say whether `cat_pos` specifically is resident, which is the
  only question that matters here. Not usable for this purpose at any
  price.

Either path also adds a hard dependency skycell does not otherwise have
(`CREATE EXTENSION pg_buffercache` in every database that wants the
scaling) plus an SPI call from inside a planner support function, a
combination with its own reentrancy risk skycell does not take on
anywhere else in this codebase. `NBuffers` stays: free (`O(1)`, already in
memory, zero dependencies), and per this round's own data erring toward
"assume less cache than there really is" is the direction that was
already safe to err in. `pg_buffercache` extension dropped from
`paper_bench` again after the measurement; no code changed.

## Round forty-five: re-running the per-radius-group cold benchmark against
the NBuffers change -- wrong table re-run would have proven nothing, the
right one found a real boundary case

Asked to re-run "the full per-radius-group benchmark" against the
`NBuffers` change. That benchmark -- `cold_probe_v2`/`warm_probe_v2`, round
forty-four's `cat_cell` (`skycell_cone()`) vs `cat_sphere` (pgSphere)
comparison -- turns out not to exercise this change at all: `skycell_cone()`
is a plain function with no `SupportRequestSimplify` hook, so it never
calls `region_support_simplify()` or `rewrite_waste_threshold()`. Only
`<@`/`@>` on `cat_pos` go through the gate this round changed. Re-running
the literal benchmark would have reproduced round forty-four's numbers
byte-for-byte and told us nothing about this change -- so the right
re-run is the one that actually exercises it: `cat_pos`'s adaptive gate
(`cat_pos_cellexpr` B-tree + `cat_pos_capgist`, `cat_pos_spgist` disabled
for a clean two-way choice) against a forced-rewrite control
(`skycell.rewrite_max_waste = 1e9`), using the same fresh-coordinates-plus-
restart-per-label methodology as round forty-four, on two new disjoint
2912-row-total center batches (`qid` 20001+ for 'adaptive', 30001+ for
'purebtree' -- never queried against `cat_pos` before, and disjoint from
each other so probing one doesn't warm the other's reads) so neither
mode contaminates the other's coldness. 14 restarts (one per mode per
radius label), `shared_buffers` left at its working 2GB default (`NBuffers`
= 262144 pages, comfortably above `cat_pos`'s 93504).

| label | n | adaptive median ms | pure-rewrite median ms | adaptive median buffers | pure-rewrite median buffers | adaptive's actual pick |
|---|---|---|---|---|---|---|
| 1" | 400 | 0.687 | 0.440 | 4.0 | 4.0 | rewrite (gate agrees) |
| 10" | 400 | 0.450 | 0.346 | 4.0 | 4.0 | rewrite |
| 1' | 300 | 0.559 | 0.748 | 4.0 | 4.0 | rewrite |
| 6' | 200 | 1.672 | 1.041 | 10.0 | 8.0 | rewrite (mostly) |
| 30' | 100 | 15.926 | 1.944 | 60.5 | 19.0 | cap-GiST (96/100) |
| 1 deg | 40 | 9.068 | 3.857 | 171.0 | 43.5 | cap-GiST (30/40) |
| 3 deg | 16 | 2.696 | 68.190 | 106.0 | 438.5 | cap-GiST (16/16) |

(median ms and median buffers; mean ms is not shown -- it is wildly
skewed by rare, very slow first-ever-touch outliers here, the same
instability already flagged in round forty-four.)

**First result: this specific run can't show the `NBuffers` change doing
anything, and that is itself informative, not a null result.**
`cat_pos` (93504 pages) comfortably fits under both the old
`effective_cache_size` (5GB) and the new `NBuffers` (2GB here) at this
box's actual settings -- `cache_frac = 1` either way, so the gate's
decision above is identical to what the pre-`NBuffers` code would have
produced. The only way to see the two implementations disagree is to
make `shared_buffers` itself smaller than the relation, which is exactly
what the direct `EXPLAIN` check two sections up already did (`shared_buffers
= '16MB'`) -- this per-radius-group run was never going to add evidence on
top of that one, and didn't.

**Second, unplanned result: cold cache does not uniformly favour the
rewrite on `cat_pos`, unlike round forty-four's `cat_cell` table -- and the
reason is physical layout, not algorithm.** At 30' and 1deg, the forced
rewrite wins decisively even cold (8x and 2.3x faster, touching a third to
a quarter the buffers) -- the gate's cap-GiST pick there is wrong under
cold cache, the same direction round forty-four already found for
`cat_cell`. But at 3 degrees the result inverts: the forced rewrite is
*25x slower* and touches *4x more buffers* than cap-GiST, which is what
the gate actually picks there (correctly, by luck of `cache_frac = 1`
leaving the default threshold's existing verdict untouched). Checked why:
`pg_stats.correlation` for `cat_cell.cell` is exactly `1` -- that table's
physical row order already matches cell order, the ideal case for a
B-tree range scan, because its rows happen to have been loaded that way.
`cat_pos` was never laid out that way; its rows matching a given cell
range are scattered across the heap, so the rewrite's `BitmapOr` arms at 3
degrees (the radius with the most and widest ranges) pull far more
scattered heap pages than cap-GiST's own structured descent touches. This
is a real difference between the two tables' on-disk layout, not a flaw
in either query strategy -- round forty-four's "skycell wins everywhere
cold" was true for a favourably-clustered table and does not automatically
generalize to an ordinary, unclustered one, which is what `cat_pos`
deliberately is (round forty-three built the gate against `cat_pos`
specifically because it is the realistic case, not `cat_cell`).

**Why this matters for the `NBuffers` scaling specifically, not just as a
general caveat.** `rewrite_waste_threshold()` scales *without bound* as
`cache_frac` shrinks, on the sole justification that wasted CPU rows are
cheap once I/O dominates -- true of the waste term itself, but silent
about the rewrite's *own* I/O footprint, which this round's 3-degree row
shows can grow *faster* than cap-GiST's on an unclustered table. A
deployment with `shared_buffers` genuinely far below `cat_pos`'s size
(the exact regime this scaling targets) would scale the threshold well
past the ~2104-row waste estimate round forty-three measured at 3 degrees,
forcing the rewrite there too -- and, per this round's measurement, that
would be the *wrong* call on an unclustered table, for a reason the
waste-based model does not see at all. Not reachable on `paper_bench` at
its current settings (`cache_frac = 1` here throughout), but reachable on
a real deployment whose working set exceeds `shared_buffers`, which is
precisely the case the scaling exists for. Flagged, not fixed: a bound on
how far the threshold is allowed to scale, or a second term accounting
for the rewrite's own expected page count (not just its wasted rows),
would close this, but deciding which is a design choice worth its own
round rather than a reflexive patch on top of this one.

**STATUS**: a measurement round, no code changed. Confirms `NBuffers` is
wired correctly (by showing where it can and can't matter) and surfaces a
genuine, reachable boundary case in the unbounded-scaling design: correct
at small-to-mid radii even cold, silent about the rewrite's own I/O
footprint at the largest radius on an unclustered table, where this
round's measurement shows that footprint -- not cache state -- is what
decides the winner.

**Follow-up, same round: bounded the scaling instead of leaving it
unbounded.** Picked the simpler of the two options this round's own
STATUS left open (a cap on the scaling, not a second cost term for the
rewrite's own page count -- that would need `region_support_simplify()`'s
own `cov`/`sel` numbers threaded into `rewrite_waste_threshold()`, a
bigger interface change for a problem a cap already closes). New GUC,
`skycell.rewrite_waste_scale_cap` (`DefineCustomRealVariable`, default
10.0): `rewrite_waste_threshold()` now floors `cache_frac` at `1 / cap`
instead of at a value close to zero, so the threshold can scale up to at
most `cap` times `skycell.rewrite_max_waste` (1000 at both defaults), not
without bound.

The default (10x) is chosen the same way `skycell.rewrite_max_waste`
itself was (this corpus's own measured crossover, a GUC specifically so
it can be retuned per deployment, not a derived constant): it sits
between round forty-three's measured waste at 1 degree (~668, so a
cache-constrained deployment can still correctly reach the rewrite
there) and at 3 degrees (~2104, so the same deployment can no longer
reach past 3 degrees's own waste estimate and force the rewrite this
round measured losing there). A cap any looser than roughly 21x would
let 3 degrees's own waste back in reach; any tighter than roughly 7x
would start giving up 1 degree's correct cold-cache win too -- 10x
leaves comfortable margin on both sides without being read as a precise
derivation.

Verified directly, reproducing round forty-five's own 16MB-`shared_buffers`
setup (`cat_pos_spgist` disabled, `cat_pos_cellexpr` + `cat_pos_capgist`
valid): at `shared_buffers = 16MB` (`cache_frac` = 0.0219, below the new
0.1 floor), the 3-degree query now stays on `cat_pos_capgist` -- the
regression this round measured as reachable is closed -- while the
1-degree and 30-arcminute queries still flip to the B-tree rewrite
exactly as before the cap, confirming the cap sits where intended rather
than clamping away the scaling's actual benefit. `shared_buffers` and
`cat_pos_spgist` restored afterward; `make installcheck` passes.

## Round forty-six: the same selectivity bug, on the region-region
operators -- "Tier 1" of letting skyregion_box_gist_ops and
skyregion_gist_ops compete automatically

Asked whether `skyregion_box_gist_ops` (round forty-two's box opclass)
could fall back to `skyregion_gist_ops` (the default multi-cap opclass)
the same way the B-tree rewrite falls back to a GiST-family index for
points. Unlike that case, no custom gate is needed here at all: both are
ordinary GiST opclasses on the same operators, so creating both indexes
on the same `skyregion` column and letting PostgreSQL's own cost-based
planner choose per query should already work -- *if* the operators'
selectivity estimates are accurate enough to drive that choice. They
were not: `&&`, `@>`, `<@` between two `skyregion` values were still
declared `RESTRICT = areasel`/`contsel`, PostgreSQL's generic, radius-
blind defaults -- the exact same bug round forty-three found and fixed
for `<@(skypos,skyregion)`, just never ported to this family of
operators.

**The fix**, reusing round forty-three's own machinery rather than
inventing a new mechanism: three new `oprrest`-shaped C functions
(`skycell_region_overlap_sel`, `skycell_region_covers_sel`,
`skycell_region_covered_by_sel`, `ext/src/adql.c`, next to
`skycell_pos_region_sel`/`skycell_region_pos_sel`), assigned via
`ALTER OPERATOR ... SET (RESTRICT = ...)`. Confirmed first, the same way
as before: none of `skycell_region_overlap`/`_covers`/`_covered_by` has a
`SUPPORT` clause, so there was no dead-code risk to rule out, just a
straight `RESTRICT` swap. Each reuses `skycell_pos_region_sel`'s own
"round one" formula -- `area(region)/4pi`, a uniform-sky estimate -- via
a shared helper, `region_area_sel()`, applied to whichever operand is a
compile-time `Const`.

`&&` is symmetric (overlap doesn't care which side is "the query"), so
either operand being constant gives a usable estimate; `region_area_sel()`
is tried on the left first, falling back to the right. `@>`/`<@` are not
symmetric: the ratio only answers the right question when the
*container* side is the constant one (`@>`'s LEFTARG, `<@`'s RIGHTARG) --
a constant on the *contained* side instead asks "how many of the table's
regions contain this one," a different question the same ratio does not
answer, so that direction keeps the flat default (1e-4, this file's
existing "no better guess" convention) rather than apply it backwards.

Verified directly against a 200,000-row synthetic `footprints` table
(uniform random circles, radius 0.1-2.1 degrees, GiST on `region`),
checking the planner's own row estimate against the hand-computed
`area/4pi` value, not just that *a* number changed:

| query | constant region area | expected rows (area/4pi x 200000) | `EXPLAIN` estimate |
|---|---|---|---|
| `region && circle(0.5deg)` | 2.4e-4 sr | 4 | **4** |
| `region && circle(60deg)` | pi sr | 50000 | **50000** |
| `circle(90deg) @> region` (container constant, correct direction) | 2pi sr | 100000 | **100000** |
| `region <@ circle(60deg)` (container constant, correct direction) | pi sr | 50000 | **50000** |
| `region @> circle(0.5deg)` (contained constant, wrong direction) | -- | flat default (20) | **20** |
| `circle(0.5deg) <@ region` (contained constant, wrong direction) | -- | flat default (20) | **20** |

The last two confirm the asymmetry handling: the wrong-direction cases
correctly decline to apply `area/4pi` (which would have given 4, not 20,
had it been misapplied) and fall back to the same flat default as before
this round -- not a regression, a deliberate "don't guess" choice. The
very last row also confirms, incidentally, that PostgreSQL's own
commutator rewrite collapses `circle <@ region` into `region @> circle`
before selectivity is ever consulted, landing on the identical
fallback path as the direct `@>` wrong-direction case.

Shipped as `skycell` 0.16 (`ext/sql/skycell--0.16.sql`,
`skycell--0.15--0.16.sql`) -- pure selectivity functions, no new opclass,
no catalog structure change. Verified the full `0.15 -> 0.16` upgrade
chain on a fresh database and `make installcheck` on the fresh-install
path; both clean.

**What this does not do, by design (the Tier-1/Tier-2 split).** This has
no analogue to `region_pos_density_sel()`'s own later round, which reads
a real point-density histogram when a `skycell_cell(pos)` expression
index exists instead of assuming uniformity. There is no equivalent
statistics source for regions -- a histogram of the sizes and sky
positions of the regions actually *stored* in a column -- so this stays
a uniform-sky estimate throughout, same as round forty-three's own first
round was before its density-aware refinement. The natural follow-up
would reuse `density_for_expr()`'s own trick (reading whatever `ANALYZE`
histogram already exists on a plain expression index -- `area(region_col)`,
say -- the same way the point case piggybacks on a `skycell_cell(pos)`
expression index) rather than a new `typanalyze` for `skyregion` from
scratch, but that is a separate, bigger round, not bundled into this one.

**STATUS**: shipped, correctness-verified by hand-computed row estimates
against four real query shapes and two deliberate-fallback shapes, not
just "the planner did something different." Enables, but does not by
itself demonstrate, the originally-asked-about goal: creating both
`skyregion_gist_ops` and `skyregion_box_gist_ops` on the same column and
letting the ordinary cost-based planner choose between them per query
now has an accurate selectivity signal to choose from -- that combined
choice has not yet been measured end-to-end against real footprint data
at the scale round forty-two's own box-vs-multi-cap crossover was
measured at.

## Round forty-seven: running both region opclasses together, measured --
a mixed result, not the clean crossover round forty-six predicted

Round forty-six's closing note said the combined choice "has not yet
been measured end-to-end." Measured it directly: both
`skyregion_box_gist_ops` and `skyregion_gist_ops` built on the same
column of a fresh 12,000-row corpus (3,000 rows/band, radius-stratified
the same way round forty-two's own large-radius addendum was: small
1-3deg, medium 20-40, large 60-80, huge 85-89, uniform-on-sphere
centres), 60 independently-sampled probe circles per band. Correctness
first, as always: `intersects()` brute force vs the indexed `&&` agreed
exactly in every band (204 / 46,985 / 158,934 / 179,465 matches for
small/medium/large/huge) before trusting any buffer number.

Then, per band, three conditions measured for all 60 probes each
(`box` only, `multicap` only -- each via `UPDATE pg_index SET indisvalid`,
not a drop/rebuild -- and `both` valid, the planner's free choice),
comparing the free choice's buffers against whichever forced condition
was actually cheaper for that exact query:

| band | box median buffers | multicap median buffers | planner's free choice | correct? |
|---|---|---|---|---|
| small | 3267.5 | **2795.0** | box (matches the `box`-only column exactly, every probe) | **no** -- picks the ~17% more expensive one, consistently |
| medium | **312.0** | 535.5 | box | yes |
| large | 113 (median; mean 2316 -- see below) | 113 | box/multicap tied at seq scan for most probes | mostly yes, with a sharp exception |
| huge | 113 | 113 | seq scan (index not used in any condition) | yes |

**Small: not a subtle miss.** Every one of 60 probes, under `both`,
reproduced the `box`-only column's number exactly -- the planner always
picks box at this radius in this corpus, even on the probes (the
majority) where multicap was measurably cheaper (e.g. 2890 vs 3737,
2800 vs 3629, consistent across probes, not noise: these are warm
buffer counts against GiST indexes neither toggle nor query touches
physically, so they're deterministic by construction). Round forty-two's
own "small: box wins" finding doesn't straightforwardly reproduce here
reversed -- what reproduces instead is that the planner has no way to
prefer the one that's actually cheaper either way, because both
indexes' cost estimates are built from the *same* externally-supplied
selectivity (round forty-six's own fix) and PostgreSQL's generic
`gistcostestimate`, which has no notion of "opclass A's recheck rejects
more false positives than opclass B's at this radius" -- the one real
difference round forty-two's own numbers are actually about. Giving the
planner an accurate *radius* signal was never going to give it an
accurate *recheck-rate* signal; those are different things, and only
the first was in scope for round forty-six's "Tier 1."

**Large: a real, separate planner edge case, exposed, not caused, by
having two closely-costed options.** The band-level median (113) hides
a sharp split: roughly 40% of the 60 probes, under `box` or `both`,
chose a *plain* `Index Scan` on `rc_corpus_box` touching **9,293 buffers**
-- not a typo, confirmed directly on probe 11's exact circle -- against
a plain sequential scan's 113-128. Diagnosed, not guessed: `SET
enable_indexscan = off` on the identical query collapses that same probe
to a `Bitmap Heap Scan` over the *same* `rc_corpus_box` index at 319
buffers, 29x fewer. The planner's own cost estimate for the two paths was
a near-tie (259.46 for the plain Index Scan vs 267.51 for the Bitmap
Heap Scan) -- a generic PostgreSQL blind spot, not a bug in this
extension or its new selectivity functions: a plain Index Scan's cost
model doesn't account for revisiting the same heap page many times when
a large number of matches are scattered across an unclustered table
(exactly what this synthetic corpus is -- rows in generation order, not
sorted by region). Multicap's own cost estimate at this same radius
happened to land safely on the seq-scan side of that knife-edge instead,
which is *why* the band-level numbers read as "mostly fine" rather than
"uniformly broken" -- not because multicap's cost model is better
calibrated, but because of where its estimate happens to fall relative
to a threshold neither opclass's cost function is actually reasoning
about. This specific failure mode predates this round and isn't
specific to the box opclass -- any two GiST options whose cost estimates
land close together near this boundary could trigger it -- but having
*two* indexable options on the same clause is what gives the planner
the opportunity to pick the worse side of that knife-edge at all; with
only one opclass present, there's no choice to get wrong here, only the
option that exists.

**What this means for the originally-asked question.** Creating both
opclasses on the same column and letting the planner choose is not a
clean drop-in replacement for the documented per-column choice yet: it
gets the headline direction right at medium and the no-index-needed
case right at huge, but picks the measurably worse opclass consistently
at small (a mild, ~17% cost, not correctness-threatening) and is exposed
to a sharp, severe regression at large through a generic planner edge
case unrelated to either opclass's own correctness. `SET enable_indexscan
= off` is a workable, low-cost mitigation for the second problem (confirmed
directly above) if someone wants to run both opclasses together today;
there is no equivalently cheap mitigation yet for the first (small-band)
one, since it would need the planner to know something about relative
recheck rates that round forty-six's fix was never meant to supply.

**STATUS**: measured, documented, nothing shipped this round. README's
"both opclasses can also coexist... and the planner picks between them
per query" (added alongside round forty-six) is accurate as far as it
goes -- it does pick, automatically, in response to real selectivity --
but should not be read as "and it always picks correctly": this round's
own numbers are the honest caveat that sentence was missing. Keeping
both opclasses registered but choosing one explicitly per column, per
the existing README guidance, remains the safer default until either the
small-band gap or the large-band knife-edge has an actual fix, not just
a measured workaround.

## Round forty-eight: fixing the large-radius regression -- the real
root cause wasn't where round forty-seven's first pass said it was

Asked to fix round forty-seven's large-radius plain-Index-Scan
regression. Before building anything: fed the planner, by hand, a
literal query region sized so its *own* `area/4pi` matched the *true*
observed match fraction for the pathological probe (0.674, not the
0.307 the formula actually produced) and watched it correctly choose a
sequential scan instead. That settled it directly -- the regression
traces to round forty-six's own selectivity estimate being wrong by
2.2x (3685 estimated rows, 8085 actual), not to an inherent, opclass-
independent PostgreSQL cost-model blind spot as round forty-seven's own
first-pass diagnosis concluded. The generic plain-Index-Scan-vs-Bitmap-
Heap-Scan cost tie that diagnosis described is real (confirmed again
below), but it is the *mechanism* the wrong estimate tripped, not an
independent cause.

**Why the estimate was wrong, precisely**: `area(const)/4pi` (round
forty-six's "Tier 1") implicitly assumes the *other* operand -- the
column being matched against -- is point-like, zero-area. True for a
small-catalogue-footprint column (what Tier 1 was scoped for), false for
round forty-two's own large/huge radius bands, where the *stored* rows
are themselves 60-89 degree circles. Two circles of comparable size
overlap whenever their centres are within the *sum* of their radii, not
within the query radius alone -- Tier 1 was computing `area(query)/4pi`
when the real question needed something closer to
`area(query_radius + stored_radius)/4pi`, and assumed `stored_radius = 0`
because it had no way to know otherwise.

**The fix**: a new function, `typical_region_area()` (`skycell.c`, next
to `density_for_expr()`), looks for a plain `CREATE INDEX ...
(area(region_col))` expression index and, when ANALYZE has run on it,
reads a representative value from its histogram -- the region-size
analogue of `density_for_expr()`'s own point-density lookup, deliberately
a separate, simpler function rather than a generalisation of it (that
one's statistics are a point-density model, cell-id buckets; this one
needs nothing beyond what ANALYZE already builds for any ordinary
float8 expression). `region_angle_est()` (`adql.c`) converts whichever
information is available for an operand -- exact, from `.area`, for a
constant; `typical_region_area()`'s estimate for a plain column with
such an index; otherwise "unknown" (angle 0, exactly Tier 1's own
assumption) -- into an angular radius (`area = 2*pi*(1-cos(theta))`,
inverted) and reports whether it found anything at all. `&&` sums both
operands' angles (two circles of radius `theta_a`, `theta_b` overlap
roughly whenever their centres are within `theta_a+theta_b`);
`@>`/`<@` subtract the contained side's angle from the container's,
clamped at 0. This is a strict generalisation of Tier 1, not a
replacement: identical output when no `area()` index exists anywhere
(confirmed directly below), strictly more informed when one does --
including, as a bonus nobody asked for yet, the "wrong direction"
containment case Tier 1 could only shrug at (a constant on the
*contained* side with the *container* column itself having an `area()`
index now gets a real estimate instead of the flat default).

**Two false starts on the way, both caught by verifying rather than
assuming the fix worked**:

1. First version averaged all of the histogram's bound values as the
   "typical" area. On the round forty-seven corpus (four sharply
   separated radius bands, 3,000 rows each, no single characteristic
   scale by construction) this collapsed *every* band to a sequential
   scan, including "small," where an index was clearly better -- the
   huge/large bands' extreme area values dominate a plain average,
   pulling the "typical" estimate far above what most rows actually
   look like. Switched to the histogram's *median* bound (ANALYZE's own
   bounds are equal-frequency quantiles, so the middle one is a real,
   skew-resistant median) -- which changed nothing, because:
2. The real bug was a unit mismatch, not a statistics-robustness
   problem: `sc_region.area` (what the `Const` branch reads directly) is
   steradians; the SQL-visible `area()` function -- what
   `typical_region_area()` necessarily probes, since that is the
   expression a user would actually index -- returns *square degrees*
   (`skycell_area()` multiplies by `RAD2DEG` twice). Confirmed directly:
   `area(circle(..., 180))` (the full sky) returns 41252.96, exactly
   4pi steradians in square degrees, not 4pi. Feeding a square-degree
   number into a formula written for steradians inflates it by a factor
   of ~3283, clamping every angle to pi regardless of band -- the same
   "collapse to seq scan everywhere" symptom the averaging false start
   produced, for a completely different reason, which is exactly why
   switching to the median alone didn't fix it. Fixed by converting
   `typical_region_area()`'s result back to steradians in
   `region_angle_est()` (the caller), not inside `typical_region_area()`
   itself -- that function's contract stays "whatever `area()` would
   return for this expression," matching the index a user would
   actually write, and the unit conversion lives at the one place that
   needs it.

**Measured after both fixes, same corpus and methodology as round
forty-seven** (`rc_corpus`/`rc_probe`, 60 probes/band, `box`/`multicap`/
`both` conditions, plus a new `rc_corpus_area` expression index):

| band | before (both/box median buffers) | after | plain-Index-Scan pathology |
|---|---|---|---|
| small | 3267.5 | 275.5 | gone -- both forced conditions now agree on Bitmap Heap Scan |
| medium | 312.0 | 113 (median), mean 1435.3, max 6274 | reduced, not gone: 13/60 probes (22%) still hit it |
| large | 113 (median) / mean 2316.2 (knife-edge present) | 113 (median and mean) | **gone: 0/60 probes** |
| huge | 113 | 113 | none, before or after |

**Large and huge: the regression round forty-seven measured is fully
closed.** Every one of 60 probes in each band now lands on the correct
(cheapest) plan with no exceptions -- a clean before/after on the exact
corpus that found the problem, not a different, friendlier one.

**Small: fixed, and it explains an unresolved thread from round
forty-seven too.** Buffers dropped across the board, and -- unexpectedly
-- re-checking the same probe round forty-seven used to show "the
planner picks the measurably worse opclass" found that both forced
conditions now use a `Bitmap Heap Scan` (before: box used a plain
`Index Scan`, multicap a `Bitmap Heap Scan` -- different scan *shapes*,
not just different opclasses, making the old "box vs multicap" buffer
comparison an apples-to-oranges one). With both now on the same footing,
box turns out to be the genuinely cheaper opclass at this radius (290 vs
413 buffers) -- matching round forty-two's own original small-radius
finding. Round forty-seven's "the planner picks the wrong one at small"
was itself downstream of the same selectivity inaccuracy as the large-
band regression, not an unrelated, separate miscalibration.

**Medium: improved, not eliminated -- and the residual is a real,
separate, generic limitation, not a loose end in this fix.** 13 of 60
medium-band probes still hit the identical pathology (a plain `Index
Scan` touching 5,000-6,300 buffers against a `Bitmap Heap Scan`'s 113-
319 on the same index, same query), down from roughly 40% of probes at
large radii before this round. Diagnosed directly, not assumed: on one
such probe, the planner's own cost estimate was 251.46 for the plain
scan against 258.42-259.72 for the bitmap scan -- a near-tie PostgreSQL's
generic GiST cost model produces regardless of how accurate the
selectivity feeding it is, because that model has no term for a plain
Index Scan revisiting the same heap page many times on an unclustered
table. `SET enable_indexscan = off` on the identical query collapses it
to the cheap Bitmap Heap Scan every time, confirming the mechanism is
unchanged from round forty-seven's own diagnosis of it -- what changed
is only how *often* an estimate lands close enough to that knife-edge to
trip it (the residual ~24% remaining selectivity error at medium radii,
from using a column-wide median rather than a genuinely per-query
estimate, is still occasionally enough). This is not fixable from
within skycell: GiST's cost estimator is fixed at the access-method
level in PostgreSQL itself, not something an opclass can override.
`SET enable_indexscan = off` remains the complete mitigation for
whoever wants zero exposure to this specific residual, exactly as round
forty-seven already found.

Correctness re-verified after both fixes, not just once at the start:
`intersects()` brute force against the indexed `&&` agreed exactly in
every band (same 204 / 46,985 / 158,934 / 179,465 counts as round
forty-seven), and a separate check on a fresh `footprints` table
confirmed the previously-flat-default "wrong direction" containment
case now returns a real, hand-verified estimate (5 rows, matching the
formula's own prediction from that table's actual median footprint
size) instead of the old flat 20. `make installcheck` passes
throughout.

**Shipped as skycell 0.17** (`ext/sql/skycell--0.17.sql`,
`skycell--0.16--0.17.sql`) -- same function names and signatures as
0.16, corrected implementation; no new SQL objects. Verified the full
`0.16 -> 0.17` upgrade chain on a fresh database.

**STATUS**: the large-radius regression round forty-seven found and this
round was asked to fix is fully closed, verified on the same corpus that
found it. The combined-opclass picture from round forty-seven is now
better than "not yet reliable" but still short of "always correct": small
is fixed outright, large/huge are fully fixed, and medium carries a
reduced but real residual risk from a PostgreSQL-core limitation outside
this extension's reach. README's existing caveat (from round forty-seven)
still correctly describes the overall posture -- picking one opclass
explicitly remains the safer default -- but the magnitude of the known gap
is now much smaller than it was.

## Round forty-nine: closing the medium-band residual properly, not
leaving it as a documented limitation

Asked to address round forty-eight's own residual rather than leave it
as "a real, generic PostgreSQL limitation this extension cannot
override." That framing was correct about the *mechanism* (the plain-
Index-Scan-vs-Bitmap-Heap-Scan cost tie really is outside this
extension's reach -- confirmed again this round) but wrong to treat the
residual as therefore unfixable: the actual lever still available is
making the selectivity estimate feeding that cost model accurate enough
that fewer queries land close enough to the tie to trip it at all, and
round forty-eight's own fix left real room there.

**The gap round forty-eight left**: `typical_region_area()` collapsed
a whole column's area histogram down to one number (its median) before
handing it to the geometry. On `round forty-seven`'s own test corpus --
four radius bands mixed in one column *by construction*, with no single
characteristic scale -- any one number is a poor stand-in for rows that
don't look like it, and the column-wide median for a mixed small/medium/
large/huge distribution sits well above what an actual "medium" row
looks like, which is exactly why 13/60 medium probes still tripped the
knife-edge: the single-number estimate for *those* queries specifically
was still off by enough.

**The fix**: stop collapsing the histogram at all. `region_area_
histogram()` (`skycell.c`, replacing `typical_region_area()`) now
returns the *whole* histogram instead of one representative value.
`node_angles()` (`adql.c`, replacing `region_angle_est()`) converts
every one of its bounds to an angle, not just one. `combine_angles_avg()`
averages the selectivity formula -- `cap_frac(angle_a +/- angle_b)` --
over the full cross product of both operands' angle sets, rather than
evaluating it once on two single numbers. This is exact under an
equal-frequency histogram's own implicit model (each bound represents
an equal share of the rows), not a best guess at which single bound
speaks for all of them -- the natural conclusion of "don't assume the
other side has zero area," taken all the way rather than half way. A
constant operand is just a one-element set, so this still reduces
exactly to round forty-eight's own formula whenever the non-constant
side has no matching `area()` index (folded in as a single angle of 0),
and the "unknown operand" fallback is preserved by treating it as one
angle of 0 rather than zero pairs -- an entirely-unknown operand still
degrades to exactly the other operand's own `area(.)/4pi`, never to no
estimate at all.

**Measured on the identical corpus and methodology as rounds forty-
seven and forty-eight** (`rc_corpus`/`rc_probe`, 60 probes/band, the
same `rc_corpus_area` expression index, `box`/`multicap`/`both`
conditions):

| band | before (round forty-eight) | after | outliers (buffers > 1000) |
|---|---|---|---|
| small | 275.5 median, 0 outliers | 275.5 median, 0 outliers | 0 (unchanged -- already clean) |
| medium | 113 median, mean 1435.3, max 6274, **13/60 outliers** | 113 median and mean, max 113 | **0/60** |
| large | 113, 0 outliers | 113, 0 outliers | 0 (unchanged -- already clean) |
| huge | 113, 0 outliers | 113, 0 outliers | 0 (unchanged -- already clean) |

**The medium-band residual is gone**: all 60 probes in every band, under
every condition, now land on exactly the plan their own buffers say is
cheapest, with zero exceptions -- not a reduction in frequency this
time, a clean sweep across all four bands simultaneously. Re-verified
correctness against the *exact* query shape the benchmark runs (whole-
table `&&`, not the band-restricted join an earlier check in round
forty-seven had used by construction): brute force and indexed `&&`
agree exactly (571,006 / 500,727 / 300,850 / 164,327 for huge/large/
medium/small). The large totals here, incidentally, explain why even
"small"-band queries now correctly favour a sequential scan on this
particular corpus: with a quarter of the table's rows covering up to 89
degrees each, even a 1-3 degree query genuinely matches a large fraction
of the table, which the new estimate -- unlike round forty-eight's own
single-number one -- now correctly reflects.

Also re-verified the `@>`/`<@` containment path and the ordinary,
single-characteristic-scale case (not this round's adversarial four-band
corpus) still produce sensible, non-degenerate estimates on a realistic
200,000-row footprints table (44 rows for `&&`, 10 for `@>`, both
consistent with that table's actual 0.1-2.1 degree footprint range) --
this round's fix is a strict generalisation, not a special case tuned to
the one corpus that motivated it.

**Why this was the right lever, not a workaround**: `SET enable_indexscan
= off` (round forty-seven's own mitigation, still valid, still the
complete answer for anyone who wants zero exposure to this class of risk
regardless of estimate quality) sidesteps the cost-tie mechanism
entirely. This round instead shrank how often an estimate lands close
enough to that tie to matter, by using more of the real statistics
already available rather than compressing them into one number first.
Both are legitimate; this one was available, used data already being
read for a different purpose (round forty-eight's own index), and left
no case worse off than before (the cross-product average is a strict
generalisation, confirmed by the small/large/huge bands' numbers not
moving at all).

**Shipped as skycell 0.18** (`ext/sql/skycell--0.18.sql`,
`skycell--0.17--0.18.sql`) -- same operator signatures as 0.17, the
backing `oprrest` functions' implementation changed; no new SQL
objects. Verified the full `0.17 -> 0.18` upgrade chain on a fresh
database. `make installcheck` passes.

**STATUS**: the medium-band residual round forty-eight left as a
documented limitation is closed, not just mitigated. Combined with
round forty-eight's own large/huge fix and small's side-effect fix, all
four radius bands in round forty-seven's original corpus now land on
the cheapest available plan with zero exceptions across 240 probes.
`SET enable_indexscan = off` remains available for anyone who wants a
guarantee independent of estimate quality, but is no longer needed to
avoid the specific regression these three rounds chased.

## Round fifty: reproducing rounds forty-four and forty-five's cold-cache
numbers after `paper_bench` was rebuilt from scratch

`paper_bench` was wiped down to 36MB by an (explicitly confirmed, not
accidental) `make installcheck` run pointed at it instead of the
disposable `contrib_regression` database. `src`, `cat_q3c`/`cat_sphere`/
`cat_cell` (10M rows each, `bench/01_data.sql`/`02_build.sql`), `cat_pos`
with its three point indexes (`bench/25_cat_pos.sql`, new this round),
`bench_centers`, and the region-crossover corpus (`bench/26_region_
crossover.sql`) were all rebuilt and correctness-reverified before this
round started -- see the session notes for that recovery. What hadn't
been re-measured yet was the cold-cache numbers themselves: rounds
forty-four and forty-five's own `cold_probe_v2`/`cold_catpos_v1` tables,
and the fresh probe coordinates they need (`bench/27_cold_probes.sql`,
also new this round, generating the same three reseeded batches --
qid 10001-11456, 20001-21456, 30001-31456 -- those rounds used).

**Didn't assume the cold-cache premise still held on a freshly-rebuilt
corpus -- checked it, the same way round forty-four insisted on checking
`drop_caches` rather than trusting it.** A real concern going in: table
and index *construction* itself does a lot of sequential I/O (building
`cat_pos`'s three indexes alone moved multiple GB through the OS page
cache minutes before this round), so "freshly rebuilt" is not obviously
the same thing as "genuinely cold." Checked directly rather than
assumed: after a real restart, a coordinate touched earlier in *this*
session (not this round) came back with a real mix of `hit`/`read`
buffers and an 855ms execution time -- evidence enough intervening I/O
(rebuilding `bench_centers`, the region corpus, writing this round's own
new scripts) had already evicted it from OS cache, despite having been
queried before. Then the actual test: a brand-new, qid-10001 coordinate
read cold the first time (529ms, real `read` buffers) and instantly on
an immediate repeat (0.049ms, all `hit`, zero `read`) -- better than a
10,000x gap, confirming both halves directly: genuinely fresh
coordinates are still genuinely cold here, and a touched page does get
cached. Proceeded only after seeing this.

**Round forty-four, reproduced** (`cat_cell`/`cat_sphere`, all seven
labels run fresh this time, not resuming a partial prior run):

| radius | skycell buffers | pgSphere buffers | skycell mean ms | pgSphere mean ms |
|---|---|---|---|---|
| 1" | **4.2** | 5.7 | **62.76** | 85.99 |
| 10" | **4.6** | 5.8 | **18.68** | 26.35 |
| 1' | **5.6** | 6.5 | 18.25 | **16.12** |
| 6' | **9.9** | 15.4 | **5.78** | 12.03 |
| 30' | **29.3** | 73.3 | **14.03** | 22.19 |
| 1 deg | **59.1** | 195.7 | **13.71** | 23.23 |
| 3 deg | **284.9** | 495.2 | **69.24** | 155.32 |

Buffers favour skycell at every radius, same as the original round;
wall-clock favours skycell at every radius except 1' (16.12ms vs
18.25ms, a near-tie reversed by a small margin) -- a fresh random seed
and genuinely different probe coordinates were never going to reproduce
the original numbers exactly, and weren't expected to; the qualitative
story (skycell wins broadly under real cold-cache conditions) reproduces
cleanly regardless.

**Round forty-five, reproduced** (`cat_pos` adaptive gate vs forced
rewrite, `cat_pos_spgist` disabled for the clean two-way comparison, the
same as that round's own setup):

| radius | adaptive median ms | forced-rewrite median ms | adaptive's actual pick |
|---|---|---|---|
| 1" | 1.072 | **0.373** | rewrite |
| 10" | 0.709 | **0.358** | rewrite |
| 1' | 0.820 | **0.365** | rewrite |
| 6' | 2.254 | **0.654** | rewrite (mostly) |
| 30' | 29.651 | **1.425** | cap-GiST (96/100) |
| 1 deg | 14.349 | **2.821** | cap-GiST (30/40) |
| 3 deg | **15.345** | 57.114 | cap-GiST (16/16) |

Reproduces the exact qualitative structure the fix in rounds forty-six
through forty-nine was built around: the fixed-threshold gate still
declines the rewrite at 30'/1deg (losing to a forced rewrite there under
genuine cold cache, same direction round forty-four already found,
unchanged here because `cache_frac = 1` on this box's actual
`shared_buffers` -- the `NBuffers`-based scaling from round forty-eight
has nothing to engage on this specific machine, exactly as round
forty-five's own first measurement found), but correctly keeps the
cap-GiST pick at 3 degrees, where this round's own numbers confirm the
forced rewrite is still the *wrong* choice (15.345ms / cap-GiST vs
57.114ms / forced rewrite) -- the unclustered-table scattered-I/O effect
round forty-five discovered and round forty-eight's cap (`skycell.
rewrite_waste_scale_cap`) was built to stay clear of.

**STATUS**: a reproduction round, no new code or findings -- both prior
rounds' qualitative conclusions hold on a fresh build of the corpus with
fresh random coordinates, confirming they describe something real about
this environment and these opclasses, not an artifact of one particular
corpus instance. `cat_pos_spgist` restored to valid afterward;
`make installcheck` passes.

**Follow-up, same round: the region-crossover corpus (rounds forty-seven
through forty-nine), under genuine cold cache for the first time -- every
prior measurement of it was warm.** Checked the premise first, the same
way as above, because `rc_corpus` plus all three of its indexes together
are under 7MB -- tiny next to the 10M-row catalogues, so "cold matters
here too" was a real question, not an assumption: a fresh circle's first
touch after a restart read 268 buffers, all `read`, in 23.9ms; an
immediate repeat read the same 268 buffers, all `hit`, in 5.05ms. A real,
measurable gap (4.7x), just a smaller one than the 10,000x seen on the
10M-row tables -- expected, since the total bytes at stake here are
themselves far smaller. Worth measuring properly, not worth skipping.

New fresh probe circles (`rc_probe_cold`, `bench/28_region_cold_probes.sql`,
reseeded, same four-band structure as `rc_probe`, not reusing its rows --
those have been queried repeatedly across rounds forty-seven through
forty-nine already) against the existing `rc_corpus`, 12 restarts (four
bands x three conditions -- `box`-only, `multicap`-only, `both`, matching
round forty-seven/forty-nine's own setup), 60 probes run per restart:

| band | box median buf | multicap median buf | both median buf | pathological plan anywhere? |
|---|---|---|---|---|
| small | 273 | 113 (seq scan) | 273 | no |
| medium | 113 | 113 | 113 | no |
| large | 113 | 113 | 113 | no |
| huge | 113 | 113 | 113 | no |

Every band and condition reproduces the exact plan-choice pattern round
forty-nine's warm measurement already found: `box` winning over
`multicap`'s own seq-scan fallback at `small` (matching round forty-two's
original small-radius finding, same as round forty-nine's warm run), and
all three conditions agreeing on a plain sequential scan at medium/large/
huge. Checked directly across all 720 cold measurements (4 bands x 3
conditions x 60 probes) for the one thing that actually mattered here --
whether the pathological plain-Index-Scan plan rounds forty-seven/
forty-eight chased could reappear under genuine cold-cache conditions
specifically, not just warm: it did not, anywhere; buffers stayed bounded
(max 300, `small`'s own `box` ceiling) throughout.

One honest methodological limitation, not swept past: because `rc_corpus`
is so small, a sequential scan (the plan every medium/large/huge
probe gets) touches the *entire* table, so the first probe in each
60-probe restart batch warms the whole table for the remaining 59 -- only
genuinely cold for roughly the first probe or two per batch, not all 60.
This does not undermine the conclusion above (the planner picks its plan
*before* execution, from cost estimates, not from whether this particular
run happens to be warm or cold, so the plan-choice results are unaffected),
but it does mean the *wall-clock* numbers for those three bands are a mix
of a few genuinely cold reads and many already-rewarmed ones within each
batch, not a clean 60-probe cold timing distribution the way `cat_pos`/
`cat_sphere`/`cat_cell`'s much larger corpus allowed in round fifty's main
measurement above. Getting a strictly cold wall-clock reading for every
single probe at these bands would need a restart before each of the 720
individual queries rather than each of the 12 batches -- not attempted
this round, since the question that actually mattered (does the fix hold,
does the pathology reappear) is already answered cleanly by the plan-
choice and buffer-count data, which carries no such caveat.

Indexes restored to valid afterward; `make installcheck` passes.

**Second follow-up, same round: the two warm comparison tables the cold
regenerations above were always meant to sit next to.** `warm_probe_v2`
(round forty-four's "what happens without a cold buffer" direct
comparison -- the identical qid 10001-11456 query set as `cold_probe_v2`,
run with no restart) and `rc_results` (rounds forty-seven through
forty-nine's own warm `box`/`multicap`/`both` table) hadn't been
regenerated after the rebuild; both now are. Checked against their cold
counterparts rather than just assumed correct: `warm_probe_v2` reproduces
round forty-four's own cold/warm structure exactly -- warm is faster at
every radius, by a wide margin at small radii (85.99ms cold vs 0.28ms
warm at 1", ~307x) narrowing but never closing at large ones (155.32ms
cold vs 25.21ms warm at 3 degrees, ~6.2x, since a larger radius touches
proportionally more pages that are cold regardless of overall cache
state); `rc_results` reproduces round forty-nine's exact warm outcome,
zero outliers in every band, `box` winning at `small`, sequential scans
elsewhere, matching `rc_cold_results` in everything but raw timing.
`make installcheck` passes.

**Third follow-up, same round: correctness of `&&`/`@>`/`<@` on the
region-crossover corpus, against the brute-force oracle -- never checked
before.** Every prior round here measured plan shape and timing for these
operators; none had confirmed the operators actually return the right
rows. Checked directly, on both `rc_probe` (warm, queried repeatedly
across rounds forty-seven through forty-nine) and `rc_probe_cold`
(round fifty's own fresh batch, above), against `rc_corpus`, for every
radius band.

`&&` (overlap) against `intersects(a, b)`: exact match, every band, both
probe batches. On `rc_probe_cold`: small 165,177; medium 292,028; large
493,751; huge 571,131 -- identical whether counted via `&&` or via
`intersects()`.

`@>`/`<@` (containment) needed a brute-force oracle, `contains(a
skyregion, b skyregion)`, which hadn't been exercised this way before
either. The first attempt at wiring it up gave a wild mismatch --
`contains(p.region, c.region) = 1` counted only 15 rows against `p.region
@> c.region`'s 128,018 on the `huge` band. Didn't shrug this off as a bug
in the operator; built the smallest possible sanity check instead:
`contains(circle('ICRS', 0, 0, 10), circle('ICRS', 0, 0, 1))` returns 0,
and the reverse, `contains(circle('ICRS', 0, 0, 1), circle('ICRS', 0, 0,
10))`, returns 1. So `contains(a, b)` means **a is contained within
b** -- the reverse of what the plain-English function name suggests, but
exactly the IVOA ADQL standard's own `CONTAINS(s1, s2)` convention (s1
enclosed in s2), which this project deliberately mirrors throughout (see
the `ivo_epoch_prop` naming elsewhere). `@>`/`<@` themselves were never
wrong -- the brute-force query had the oracle's argument order backwards.
Corrected mapping: `p.region @> c.region` (p contains c) and `c.region <@
p.region` (c is inside p) are the same fact, both verified against
`contains(c.region, p.region) = 1`.

With the corrected oracle, exact matches everywhere:

| band | `rc_probe` (warm) | `rc_probe_cold` (fresh) |
|---|---|---|
| small | 9 | 6 |
| medium | 12,071 | 10,824 |
| large | 80,180 | 75,773 |
| huge | 128,018 | 127,939 |

**STATUS**: `&&`, `@>`, and `<@` all return exactly the rows the
brute-force oracle says they should, in every radius band, on both the
long-queried warm probe set and a probe set that had never been touched
before this round. The one real finding is about `contains()`'s argument
convention, not about any of skycell's own operators: `contains(a, b)`
reads as "a contained within b" (ADQL/IVOA order), not "a contains b" --
worth remembering before reaching for it as an oracle again.

## Round fifty-one: pgSphere beat skycell 3-5x on the region crossover --
chased it to a wasted computation, not an inherent cost, and fixed it

Asked a question round fifty never had a baseline for: on the identical
`rc_corpus`/`rc_probe` crossover, how does skycell's `skyregion` compare
to pgSphere's `scircle` -- the same circles, mirrored into a second table
(`rc_corpus_pg`/`rc_probe_pg`, parsed straight out of `skyregion`'s own
STC-S text so it is the exact same geometry, not a re-sampled one) with
its own GiST index. Correctness first, not assumed: `&&`/`@>`/`<@`
against the `scircle` equivalents, forced through both systems' GiST
indexes, matched exactly on every one of ~4.3M row-pairs checked (both
probe batches, all four bands). One genuinely interesting aside, not a
bug: pgSphere's own `@>`/`<@`-backing functions (`scircle_contains_circle`/
`scircle_contained_by_circle`) are *not* reversed the way skycell's
`contains()` is -- they read in plain English, matching their operators
1:1. The reversal round fifty's own follow-up tripped on is specific to
skycell choosing to name its oracle after the ADQL spec; it is not
something inherent to spherical-region libraries.

**Performance was not close.** Measured with `EXPLAIN (ANALYZE, BUFFERS,
TIMING OFF)` per probe, median over 60 probes/band, warm cache, both
systems' indexes left to the planner's own natural choice:

| band | op | skycell ms | skycell buf | pgSphere ms | pgSphere buf |
|---|---|---|---|---|---|
| small | overlap | 5.29 | 277 | 1.46 | 169 |
| small | contained_by | 5.58 | 3594 | 1.34 | 169 |
| medium | overlap | 13.29 | 113 | 2.15 | 193 |
| medium | contained_by | 8.91 | 312 | 1.93 | 193 |
| large | overlap | 13.63 | 113 | 2.84 | 209 |
| large | contained_by | 12.71 | 320 | 2.64 | 209 |
| huge | overlap | 13.94 | 113 | 3.14 | 213 |
| huge | contained_by | 14.40 | 320 | 2.93 | 213 |

3-5x slower almost everywhere, and not an I/O story: at medium/large/huge,
skycell's `overlap` seq-scans the *whole* 113-page table (fewer buffers
than pgSphere's 193-213) and still loses on wall clock by 4-6x -- CPU-bound,
not I/O-bound. The `small`/`contained_by` row is its own, narrower problem:
confirmed via `EXPLAIN` that skycell picks a plain (non-bitmap) `Index Scan
using rc_corpus_box`, re-visiting the same ~113 heap pages thousands of
times (`Rows Removed by Index Recheck: 3704`, 3594 buffer hits on average,
consistent across all 60 probes in the band). Forcing a Bitmap Heap Scan by
hand (`enable_indexscan = off`) cut buffers 13x (3747 -> 293) but barely
moved wall time (6.39ms -> 6.11ms) -- proof the buffer count was not the
real cost either.

**Chased the CPU cost to its root, not just its symptom.** `region_region()`
(adql.c -- the exact test behind `&&`/`@>`/`<@`/`contains()`/`intersects()`)
calls `skycell_region_from_datum()` on *both* arguments, every row, which
for a circle calls `sc_region_cone()` (cover.c). Reading `cover.c` before
changing anything: for a cone-cone pair, `sc_region_overlaps()` reduces to
one `sc_angle()` call and `sc_region_contains_region()` to one
`region_farthest()` call -- neither ever reads `out_c2[]`/`in_c2[]`, the
30-entry (one per HEALPix order), up-to-4-`sin()`/`pow()`-calls-each table
`sc_region_cone()` fills on *every* construction. Confirmed by grep: those
arrays are read in exactly one place in the whole codebase,
`sc_region_classify_cap()` (cover.c:338/343/354), the pixel classifier a
GiST/SP-GiST descent or a planner covering runs -- never an exact test.

Quantified with a standalone microbenchmark (`cover.c`/`healpix.c` build
outside Postgres by design -- confirmed in Round thirty-eight's own
`make selftest`):

| call | ns/call | what it does |
|---|---|---|
| `sc_region_cone()` (as shipped in 0.18) | ~375-411 | center + center_pix + area + the 30-level table |
| same, table skipped | ~31-32 | everything the exact test actually reads |
| `sc_region_overlaps()` alone | ~24-26 | the actual geometry test |
| `sc_region_contains_region()` alone | ~26 | the actual geometry test |

~90% of every construction was spent on data the exact-test path never
looks at, and `region_region()` built two per row -- one for the corpus
row (unavoidable, it varies) and one for the probe (avoidable: the same
value on all 12,000 rows, rebuilt from scratch every time anyway). Back
of envelope, 12,000 rows x (~400ns x 2 + ~25ns) ~= 9.9ms, close enough to
the observed ~13-14ms (plus normal per-row call overhead) to call this the
dominant cost, not a guess.

**The fix, two parts, both measured before shipping:**

1. `sc_region_cone()`/`sc_region_poly()` (cover.h/cover.c) take a new
   `need_covering` argument gating the 30-level table; every index/
   covering call site (gist_region.c, gist_region_box.c, spgist_region.c,
   gist_point_cap.c, skycell.c's cone/MOC/ranges functions, cover_selftest.c)
   keeps passing `true`, unchanged. A new `skycell_region_from_datum_lite()`
   (adql.c, declared in skycell_internal.h) passes `false`, used only by
   the exact-test call sites.
2. `region_region()` and `skycell_region_covers()` (adql.c, the `@>`
   function, which built its own regions directly rather than going
   through `region_region()`) now cache each argument's built `sc_region`
   in `fn_extra`, one slot per argument position, keyed on the argument
   datum's *bytes* -- the same cache-key discipline `spg_cached_region`
   (spgist_region.c, an earlier round's identical fix for the SP-GiST
   support functions) already established, for the documented reason: a
   short-lived per-tuple memory context is reused across calls, so
   pointer identity alone cannot distinguish two different rows' regions.
   Two slots, not one keyed on a fixed "query" side, because a plain
   two-argument predicate (unlike a GiST support function) has no fixed
   constant argument position -- either side, or neither, can repeat
   across a run of calls, and whichever one does stops paying to rebuild.

Correctness re-checked after the fix, not assumed preserved: `make
installcheck` (both `skycell` and `adql` regression suites) passes, and
the exact `&&`/`@>`/`<@` vs. brute-force-oracle check from round fifty's
own follow-up was re-run against both `rc_probe` and `rc_probe_cold` --
still an exact match in every band.

**Measured again, same methodology, after the fix:**

| band | op | before (ms) | after (ms) | speedup | pgSphere (ms) | remaining gap |
|---|---|---|---|---|---|---|
| small | overlap | 5.29 | 2.26 | 2.3x | 1.46 | 1.5x |
| small | contained_by | 5.58 | 2.48 | 2.3x | 1.34 | 1.8x |
| medium | overlap | 13.29 | 3.52 | 3.8x | 2.15 | 1.6x |
| medium | contained_by | 8.91 | 3.51 | 2.5x | 1.93 | 1.8x |
| large | overlap | 13.63 | 3.52 | 3.9x | 2.84 | 1.2x |
| large | contained_by | 12.71 | 4.38 | 2.9x | 2.64 | 1.7x |
| huge | overlap | 13.94 | 3.58 | 3.9x | 3.14 | 1.1x |
| huge | contained_by | 14.40 | 4.78 | 3.0x | 2.93 | 1.6x |

**STATUS**: shipped as skycell 0.19 (`ext/sql/skycell--0.18--0.19.sql`).
No SQL-visible change -- same function names and signatures -- a pure
C-level fix, same changelog-via-version-bump convention as 0.17/0.18. The
`small`/`contained_by` plain-Index-Scan plan-choice question from the
earlier measurement is untouched by this fix (same plan, same buffer
count, just a cheaper per-row test underneath it) and is left for a
separate round if it is worth chasing. One related, adjacent finding not
yet fixed: `pos_in_region()` (adql.c, backing `skycell_contains`/
`skycell_pos_in_region`/`skycell_intersects_pos` -- the point-in-region
ADQL `CONTAINS`, skycell's actual headline cone-search workload) has the
identical shape, a full `skycell_region_from_datum()` rebuild of the
region argument on every point tested, for exactly the same reason this
round just fixed. Not touched here because it was outside what was asked
this round; flagged for a follow-up.

**Follow-up, same round: fixed `pos_in_region()` too.** It backs
`skycell_contains`/`skycell_pos_in_region`/`skycell_region_has_pos`/
`skycell_pos_in_region_sel`/`skycell_intersects_pos` -- the ADQL `CONTAINS`
point form, `<@`/`@>` on `(skypos, skyregion)`, and `INTERSECTS(point,
region)` -- and had the identical bug: a full `skycell_region_from_datum()`
rebuild of the region argument on *every point tested*, typically the
*same* region across a whole scan (one constant circle tested against
many points, the normal shape of a cone search).

Same two-part fix, reusing the exact machinery just built for
`region_region()` rather than duplicating it: `pos_in_region()` now takes
the calling `FunctionCallInfo` and fetches its region argument through
`cached_region_arg()` (adql.c), the same `fn_extra` cache keyed on the
argument's bytes; the five wrapper functions above now pass `fcinfo`
through. `cached_region_arg()` and its backing structs moved earlier in
adql.c, above `pos_in_region()`, purely so both call sites could share one
implementation -- no behaviour change from the move itself.

Measured the same way as the standalone numbers above, since this shape
(N points against one constant region) is exactly what a standalone
microbenchmark can isolate cleanly, unlike a real query against a 10M-row
table on this container (whose OS page cache would not stay warm between
runs, adding I/O noise that swamped the signal when tried directly):
500,000 points against one constant region, 413.9ns/point before (full
build every row) vs 30.8ns/point after (lite build once, reused) -- a
~13.4x reduction in the raw construction-and-test cost. The end-to-end
per-row win inside a real query is smaller once normal executor/
function-call overhead is added back in, but it is the same mechanism
already measured end-to-end on the region-region crossover above.

Correctness re-checked, not assumed: a fresh brute-force angular-distance
check (`2*asin(sqrt(haversine(...)))`  <=  radius, never run against
`contains()` before) against a synthetic 500,000-point table, exact match
(890 = 890); the `<@`, `@>` and `intersects()` operator spellings checked
the same way, all three also exact (890 each). `make installcheck` (both
`skycell` and `adql`) passes.

**STATUS**: shipped as skycell 0.20 (`ext/sql/skycell--0.19--0.20.sql`).
No SQL-visible change, same convention as 0.19. Between 0.19 and 0.20,
every region-region and point-region exact-test call site in adql.c now
goes through the same lite-build-plus-cache path; nothing in that family
is known to still pay the full covering-table cost for a plain predicate.

## Round fifty-two: why the planner never picks `skyregion_gist_ops` over
the box opclass -- a real cost-estimate miscalibration, and a reverted fix

Round fifty-one's own remaining 1.2-1.9x gap prompted a direct question:
is the multi-cap (cap-GiST) opclass actually slower than the box opclass,
since the planner never picks it naturally on `rc_corpus`? Measured
directly rather than assumed from the planner's own choice: forced each
opclass in turn (`UPDATE pg_index SET indisvalid = ...`), warm cache,
repeated runs, all four bands. **The planner's choice is backwards from
reality.** In every band, `skyregion_box_gist_ops`'s own *estimated* cost
is lower (226 vs 307 at `small`, 316 vs 461 at `medium`, 477 vs 745 at
`large`, 495 vs 778 at `huge`) -- which is the entire reason box always
wins the comparison -- but the *measured* execution time is the reverse,
multicap faster by 10-28% everywhere, and at `medium` multicap ties the
planner's own natural Seq Scan choice rather than losing to it as an
earlier same-session estimate (from a single, not-repeated run) had
claimed.

Chased the estimate itself rather than stopping at "the planner is
wrong." PostgreSQL's GiST cost estimator (`genericcostestimate`, no
per-opclass override -- the same limitation round forty-eight already
hit) scales its page-visit estimate off `pg_class.relpages`, fed the
*same* selectivity number for both opclasses (skycell's selectivity
functions are operator-level, not opclass-level). The only thing left
that can make multicap look pricier is index size, and it is: `rc_corpus_
box` is 207 pages / 57.97 tuples-per-page, `rc_corpus_multicap` 531 pages
/ 22.60 tuples-per-page -- a 2.56x size ratio matching the tuples-per-page
ratio almost exactly, confirming it is pure per-entry size, not
fragmentation. Confirmed in the source: `GistBox3D` is 6 doubles (48
bytes); `GistMultiCap` is an overall cap plus `sub[MAX_SUBCAPS]` with
`MAX_SUBCAPS = 4` (5 caps x 32 bytes = 160 bytes) -- ~3.3x bigger per
entry, which is what inflates the page count the cost estimator penalizes.

**Checked the project's own history before proposing a fix**, since
shrinking this exact key had already been tried: Round nine built a
variable-length encoding storing only as many sub-caps as a region
actually has (a real, measured 35-47% smaller index, ~35% fewer cold
buffer reads) and found query time got *worse* anyway at every scale
and cache state (1.5-2.2x slower warm, 35-45% slower cold) -- the CPU
cost of decoding a variable-length key on every `consistent()` call
during index traversal outweighed the fanout it bought. Reverted, with
the project's own conclusion on record: "the multi-cap GiST's gap to
pgSphere is a structural tree-depth/fanout limit... not one
`consistent()`-level tuning can close further." That round predates this
session's 0.19/0.20 exact-test fixes, but it tests a different layer
entirely (`consistent()`/`union()`/`picksplit()`'s own in-memory key
handling during traversal, never touched by the exact-test caching
fix), so its conclusion still applies unchanged.

**Tried the one angle Round nine didn't: not a format change, just a
smaller fixed constant.** `MAX_SUBCAPS` from 4 to 2 shrinks every entry
to an overall cap plus 2 subs (96 bytes) with no decode branching at
all -- still a fixed-size struct, same code, smaller arrays. Correctness
held: `make installcheck` passed, and the project's own existing mixed-
circle/polygon GiST benchmark (`bench/20_region_xmatch.sql` + `bench/
22_region_gist.sql`, 50,000 footprints half circle/half polygon, the
same scale "Picksplit, round two" used when it first found a single-cap
key scaling badly) still matched brute force exactly (720 = 720) after
`REINDEX`.

Performance was the opposite story, and workload-dependent in exactly
the way the file header already warned it would be. On `rc_corpus`
(all-circle -- every leaf is already a single cap regardless of
`MAX_SUBCAPS`, so only internal-node unions could be affected): index
shrank 531 -> 339 pages, and real execution time was unchanged within
noise (3.69-3.98ms repeated vs 3.26-3.92ms before -- no regression, but
also, checked directly, not enough of a cost-estimate change to flip the
planner's own natural Seq Scan choice at `medium` either). On `fpr`
(half polygon, the same 50,000-row scale Round two's original single-cap
key regression was measured at): index shrank 2118 -> 1394 pages (~34%,
tracking the ~40% smaller fixed struct), but query time went from
48.7-49.1ms to **816-926ms -- a 17-19x regression**, not a smaller one.
Fewer sub-caps per node starves exactly the polygon decomposition this
design exists for (`region_to_multicap`'s MOC-based split into up to
`MAX_SUBCAPS` HEALPix cells), reproducing the "tree walked too much"
failure mode Round two's own single-cap key first hit, now from the
other direction (4 -> 2 instead of 1 -> 4).

Reverted (`MAX_SUBCAPS` back to 4, `REINDEX` both indexes, `make
installcheck` passes, `bench/22_region_gist.sql` back to 51.5-57.6ms,
consistent with the pre-change baseline). Not kept as a documented-but-
off option, same reasoning Round nine gave for its own revert: the diff
is the record of what was tried.

**STATUS**: the cost-estimate miscalibration is real and confirmed (box
is not actually faster; the planner just thinks it is), but it is not
fixable by shrinking the opclass's own key -- that trades a planner-
visible number this extension cannot directly influence (GiST has no
per-opclass cost hook) for a real, severe regression on exactly the
workload (polygon regions) the multi-cap design exists to serve. This is
the second independent confirmation of Round nine's structural
conclusion, this time via a fixed-size constant rather than a format
change, closing off that entire direction rather than narrowing it: the
remaining gap to pgSphere on `rc_corpus` is not something the region
GiST opclass's own key size can close, in either a variable-length or a
smaller-fixed-size form.

## Round fifty-three: promoting `skyregion_box_gist_ops` to DEFAULT --
re-measuring a stale crossover figure first, then shipping it

Round fifty-two's own finding (box's cost *estimate* is wrong, not its
real performance) raised the obvious next question: since the planner
already effectively never reaches `skyregion_gist_ops` (multi-cap) on
`rc_corpus` anyway, why is multi-cap still the DEFAULT opclass for
`skyregion` -- the one a plain, opclass-unqualified `CREATE INDEX ...
USING gist (region)` gets?

**Checked the existing shipped guidance before touching anything, and
it directly contradicted this round's own `rc_corpus` measurements.**
`skycell--0.20.sql`'s own comments (from round forty-two) say the box
opclass wins at "catalogue-scale footprints (arcsec-few degrees)" and
multi-cap "wins back from roughly 20 degrees radius on." But round
fifty-one/fifty-two's own repeated, median-of-60-probes measurement on
`rc_corpus` found multi-cap winning *every* band tested, `small` (1-3
degrees) included -- squarely inside the old comment's "box wins" range.
Not a contradiction to shrug off: re-measured cleanly, both buffers and
wall-clock, repeated runs, all four bands, forcing each opclass in turn:

| band | box avg buf | box median ms | multicap avg buf | multicap median ms |
|---|---|---|---|---|
| small | 276.6 | 2.293 | 417.4 | 1.637 |
| medium | 6254.1 | 4.102 | 5109.6 | 3.419 |
| large | 9106.8 | 5.482 | 8212.1 | 4.846 |
| huge | 10233.3 | 5.941 | 9261.2 | 5.390 |

Multi-cap wins wall-clock at every band, box only wins on raw buffers at
`small` (276.6 vs 417.4) -- and even there, fewer buffers doesn't
translate to less wall-clock time anymore. That's the resolution, not a
genuine contradiction: round forty-two's own "20 degrees" figure
predates 0.19/0.20's per-row exact-test fix. Before that fix, both
opclasses paid the same inflated per-candidate recheck cost (region_
region() rebuilding a full covering-capable `sc_region` from scratch
every row), so buffers and wall-clock tracked each other closely and the
~20-degree buffers-based crossover was also roughly the wall-clock
crossover. After the fix, per-candidate cost dropped enough that it no
longer swamps whichever opclass returns fewer buffer-touching
candidates -- except now *multi-cap* is the one with fewer effectively-
costly false positives at `rc_corpus`'s scale, not box, so the wall-
clock crossover moved independently of the still-roughly-unchanged
buffers crossover.

**But `rc_corpus`'s "small" band (1-3 degrees) isn't the same scale as
the realistic catalogue footprints the old comment actually meant.**
Round forty-two's own `fpr` corpus uses circle radii of 0.02-0.3 degrees
(`power(10, -1.7 + 1.2*random())`), an order of magnitude smaller than
`rc_corpus`'s "small" band. Re-ran that comparison fresh, under current
(0.19/0.20-fixed) code, isolating circle-only and polygon-only subsets
of `fpr` separately to find out whether box's win there was a polygon
effect or a small-circle effect:

| subset | box ms | multicap ms |
|---|---|---|
| circle-only | 13.554 | 72.449 |
| polygon-only | 12.783 | 50.544 |

Box wins **both** subsets, by a wide margin, including the pure-circle
one -- so box's advantage on `fpr` isn't a polygon effect at all; it's
a radius-scale effect, and `fpr`'s circles (0.02-0.3 degrees) sit well
below wherever the true crossover now is. Tried to pin that crossover
precisely with a dedicated 0.2-1.6-degree stratified corpus
(`xover_corpus`/`xover_probe`, 3,000 rows/band, 40 probes/band, same
construction as `rc_corpus`) and found it too sparse at these radii to
produce a measurable per-probe signal (6-16 buffers, 0.025-0.032ms,
indistinguishable from fixed overhead) -- would need a much denser
corpus to resolve further, not attempted since the two existing
bracketing points (`fpr`: 0.02-0.3 degrees, box wins; `rc_corpus`: 1-3
degrees, multi-cap wins) already answer the question this round needed
answered. The crossover sits somewhere between roughly 0.3 and 1 degree
now -- down from the old ~20-degree figure, not reversed, just moved by
0.19/0.20's own fix.

**Promoted `skyregion_box_gist_ops` to DEFAULT on this evidence.**
Realistic catalogue footprints (arcsec to a few degrees -- source
apertures, instrument footprints, single-object cones) are squarely
inside box's now-larger domain, box has full feature parity with
multi-cap (all four strategies: `&&`, `@>`(region,point),
`@>`(region,region), `<@`(region,region)), and multi-cap's one
remaining real edge (circles above roughly a degree) is exactly the
case still available by naming `skyregion_gist_ops` explicitly.

**Implementation note, since this was a genuine SQL/catalog change, not
a C-level one**: PostgreSQL has no `ALTER OPERATOR CLASS ... SET
DEFAULT`, and only one default opclass is allowed per (type, access
method) pair, so swapping which one is default means dropping both and
recreating them with `DEFAULT` moved. First attempt used `DROP OPERATOR
CLASS ... CASCADE`, which failed twice before landing:

1. A stray `\set ON_ERROR_STOP 1` line, copied from habit out of this
   project's own `bench/*.sql` scripts (which *are* run through psql),
   is a psql-only meta-command -- invalid when the line isn't the
   file's very first line. Only a single leading `\echo ... \quit` is
   specially tolerated when an extension script is loaded by `ALTER
   EXTENSION ... UPDATE` (the backend loads the file directly, not
   through psql); every other backslash command in this file family
   has to be real SQL. Caught immediately (`ERROR: syntax error at or
   near "\"`) and removed.
2. `DROP OPERATOR CLASS ... CASCADE` alone hit `duplicate key value
   violates unique constraint "pg_amop_fam_strat_index"` on the
   subsequent `CREATE OPERATOR CLASS`: both opclasses were originally
   created without an explicit `FAMILY` clause, which implicitly
   creates a same-named operator family holding the real `pg_amop`/
   `pg_amproc` rows (keyed on the family, not the class) -- `DROP
   OPERATOR CLASS` removes the class but leaves that family and its
   rows behind, so recreating the class re-registered the same
   strategies into an already-populated family. Fixed by dropping the
   *family* (`DROP OPERATOR FAMILY ... USING gist CASCADE`) instead,
   which cascades through the class to any dependent index in one step.

Both failures rolled back cleanly (`ALTER EXTENSION UPDATE` runs as one
transaction) and were caught before shipping, not after -- tested
against both paths this project's own convention requires: a fresh
`CREATE EXTENSION` (via `make installcheck`, exercising `skycell--
0.21.sql`) and a real incremental upgrade (`ALTER EXTENSION skycell
UPDATE TO '0.21'` against `paper_bench`, exercising `skycell--0.20--
0.21.sql`, including the `CASCADE`'s documented side effect of dropping
this session's own `rc_corpus_box`/`rc_corpus_multicap`/`fpr_box_gist`/
`fpr_region_gist` benchmark indexes, which were then rebuilt and
re-verified). Both pass `make installcheck`; a post-upgrade correctness
spot-check (indexed `&&` vs brute-force `intersects()`) matched exactly
(4,915 = 4,915).

**STATUS**: shipped as skycell 0.21. `skyregion_box_gist_ops` is now
`DEFAULT FOR TYPE skyregion USING gist`; `skyregion_gist_ops` remains
available, non-default, for wide-area or very large regions. Both
opclasses' `COMMENT ON OPERATOR CLASS` text updated to state the
re-measured crossover and point at this round. No C code changed --
pure catalog/SQL. One open question this round didn't chase: the exact
crossover radius between 0.3 and 1 degree, which would need a denser
dedicated corpus than the one tried here; left as a bracket, not a
point estimate, since the two existing bracketing measurements were
enough to decide the default.

## Round fifty-four: pinning the crossover precisely -- it wasn't 0.3-1
degree, and it isn't one number at all

Asked directly to pin down round fifty-three's own bracket (0.3-1
degree). Built a dense, isolated-scale corpus (`pin_corpus`/`pin_probe`,
25,000 rows/band -- dense enough to avoid the earlier `xover_corpus`
attempt's "too sparse to measure" failure) at 0.3, 0.4, 0.5, 0.6, 0.7,
0.85, 1.0 degrees, each band's corpus and probes drawn at that one
fixed radius, measured with the exact same per-probe `EXPLAIN (ANALYZE,
BUFFERS)` methodology round fifty-one's own numbers used.

**First measurement attempt used a batched nested-loop query (`rox_
probe`-style, one query joining all 60 probes against the corpus) and
got numbers flatly contradicting round fifty-three's own `rc_corpus`
finding -- box winning at *every* radius tried, 0.3 through 2.5
degrees, not just below ~1.** Didn't trust either result on its own;
chased the discrepancy instead of picking a side. Root cause: a batched
nested-loop's inner index scan is parameterized by the outer row
(a Param, not a per-query Const), and the planner chose a plain,
non-bitmap `Index Scan` for that shape -- re-visiting the same heap
pages redundantly, exactly the pathology rounds forty-seven/forty-eight
chased for a different reason. A single-probe query with the region as
a literal gets a `Bitmap Heap Scan` instead and a very different,
smaller buffer count. Confirmed directly: the exact single-literal
query from round fifty-one's own methodology, re-run against the
current `rc_corpus`, reproduced its original numbers precisely (293/413
buffers, multicap winning) -- the batched form was measuring a
different, non-representative execution shape, not a different
reality. Redid the whole sweep with the validated per-probe/literal
form.

**With the corrected methodology, box won every band from 0.3 through
1.0 degrees, and -- extending the sweep further -- through 2.5 degrees
too**, directly contradicting round fifty-three's `rc_corpus`-based
claim that multicap already wins by 1-3 degrees. Not a measurement
artifact this time (reproduced, same methodology, same validated shape)
-- a real, different result from testing an *isolated* single-scale
corpus instead of `rc_corpus`'s own deliberately mixed one.

**That's the actual resolution: `rc_corpus` mixes all four of its own
radius bands (1-3, 20-40, 60-80, 85-89 degrees) into one shared GiST
index, by design, to stress a wide scale range in one benchmark. Round
fifty-three's own "small-band" measurement was never testing "a small
probe against a small-only corpus" -- it was testing a small probe
against a corpus that also contains near-hemisphere-sized circles
sharing the exact same physical tree.** Confirmed this is the deciding
variable, not probe radius, with a direct, controlled test: built a
corpus half small (1-3 degree) rows and half huge (85-89 degree) rows,
sharing one index (`pin5_corpus`), and ran the *same* small-radius
probes against it two ways --

| corpus | box ms | multicap ms |
|---|---|---|
| isolated small-only (`pin_corpus`, same probes) | ~0.07-0.10 | ~0.17-0.21 |
| small+huge mixed, one index (`pin5_corpus`) | 8.601 | **5.796** |

Identical probes, identical small radius -- box wins by ~1.5-3x in
isolation, multicap wins by ~1.5x once a huge-radius population shares
the index. An extreme outlier region distorts the box key's own
bounding boxes for everything in its subtree; the multi-cap key's
per-sub-cap structure tolerates that distortion better. This is a
second, genuinely separate effect from the pure radius crossover, not
an alternative explanation for the same one.

**Having isolated that confound, re-ran the pure, single-scale radius
sweep properly** -- `pin_corpus` (0.3-1.0 degrees) still box throughout;
extended with `pin6_corpus` (10, 25, 45, 60 degrees, same isolated-
single-scale design): multicap won every one of those, decisively (e.g.
10.627ms vs 13.960ms at 10 degrees). So the true, isolated-scale
crossover sits somewhere between 2.5 and 10 degrees -- narrowed further
with `pin7_corpus` (3, 4, 5, 6, 7, 8 degrees, same design): box still
clearly wins at 3-4 degrees, multicap clearly wins by 10, and 5-7
degrees is a genuine, reproducibly close contest -- re-ran that specific
range a second time and the ranking flipped band to band within normal
run-to-run noise (buffers identical both runs, confirming the data and
index didn't change, only wall-clock ordering at the margin). Reported
as a bracket, honestly, rather than forcing a single number the data
doesn't support: **the uniform-scale crossover is roughly 5-8 degrees**,
not round fifty-three's 0.3-1.

**Net effect on round fifty-three's decision**: the promotion of
`skyregion_box_gist_ops` to DEFAULT stands, and is better supported now,
not worse -- its real domain for a realistically uniform-scale catalogue
column (sub-degree through several degrees) is wider than first
measured. What was wrong was the stated number and its single-cause
explanation; the actual picture needs two separate statements (a ~5-8
degree uniform-scale crossover; a separate scale-mixing effect that can
favour multicap at any radius if the column mixes very different region
sizes in one index), not one. Both opclasses' `COMMENT ON OPERATOR
CLASS` text and their preceding comment blocks corrected to say this,
shipped as skycell 0.22 (`ext/sql/skycell--0.21--0.22.sql`) -- comment
text only, no opclass/operator/function change, tested on both the
fresh-install path (`make installcheck`) and a real incremental upgrade
(`ALTER EXTENSION skycell UPDATE TO '0.22'` against `paper_bench`).

**STATUS**: shipped. All `pin*`/`xover*` scratch tables dropped after
measurement; none of this round's corpora are part of the committed
`bench/` scripts. The 5-8 degree bracket is itself still a bracket, not
a point estimate -- the close band (5-7 degrees) would need more probes
or more repeats to resolve further, not attempted since the decision
this round needed to inform (confirm the DEFAULT promotion, fix the
wrong comment text) didn't need more precision than that.

## Round fifty-five: the box opclass does not beat pgSphere -- "Round
forty-two"'s own claim to the contrary does not reproduce, and could
not be forensically explained

Asked directly, after round fifty-four's own crossover work: does the
box opclass still lose to pgSphere? Round fifty-one already measured a
1.1-1.9x gap favouring pgSphere on `rc_corpus`, but `rc_corpus` mixes
four radius bands into one index -- round fifty-four's own finding --
so that number alone doesn't settle whether box loses to pgSphere
everywhere, or only in the same mixed-scale regime where it also loses
to multicap. Checked directly rather than assuming either answer.

**Isolated single-scale corpora, box vs. pgSphere, same validated per-
probe methodology as round fifty-four**: built `pin8` (25,000 rows, 1
degree, matching a radius box already wins against multicap at) and
`pin9` (five bands, 0.05-0.5 degrees, matching `fpr`'s own scale). pgSphere
won every single band tried:

| corpus | radius | box ms | pgsphere ms |
|---|---|---|---|
| pin8 | 1.0 deg | 0.042 | 0.027 |
| pin9 | 0.05 deg | 0.047 | 0.023 |
| pin9 | 0.1 deg | 0.032 | 0.019 |
| pin9 | 0.2 deg | 0.042 | 0.022 |
| pin9 | 0.3 deg | 0.038 | 0.0295 |
| pin9 | 0.5 deg | 0.048 | 0.0385 |

Reproduced with a repeat run at 1 degree (0.042/0.027 both times,
stable). Box never wins here, at any radius tried -- not even in the
exact regime (sub-degree, isolated scale) where round fifty-four found
it winning decisively against multicap.

**This directly contradicts "Round forty-two"'s own claim** ("box beats
pgSphere 2.1-8.4x" on `fpr`, the project's own 50,000-row mixed circle/
polygon corpus). Didn't take either number on faith; re-ran the actual
`fpr` corpus, circle-only subset, two ways:

| query form | box ms | pgsphere ms |
|---|---|---|
| per-probe, literal (round fifty-one/four's own validated form) | 0.1055 | 0.0150 |
| batched nested-loop (round forty-two's own apparent query shape) | 7.658 | 4.779 |

pgSphere wins **both** ways -- including the batched form, which is the
*same* execution-shape artifact round fifty-four found inflating box's
own numbers against multicap. Reproducing that exact shape here and
still getting pgSphere ahead rules out "it's the same batched-query
bug" as the explanation.

**Checked every other explanation available, and ruled each one out**:
- JIT: `SHOW jit` reports `off` in this environment, and every query in
  this comparison costs far too little to reach `jit_above_cost` even
  if it were on (confirmed irrelevant, not just disabled).
- Cross-type query expressivity: pgSphere's own `scircle` GiST opclass
  indexes `&&(scircle, spoly)` and the reverse natively (`pg_amop`
  strategies 31-36), so a mixed circle/polygon corpus is not a
  structural limitation forcing pgSphere into multiple unindexed
  queries -- it can answer a mixed-type overlap test from one index,
  same as skycell's own unified type.
- Corpus staleness: `fpr` was rebuilt earlier this session (round
  fifty-two's `MAX_SUBCAPS` test), but re-ran the comparison against
  the corpus as it exists right now, not an assumed-stale one -- the
  current numbers are what the current corpus actually produces.

**Could not identify the actual cause.** "Round forty-two"'s own
benchmark was explicitly ad hoc ("registered ad hoc against
`splitcost_test`... this stays fully disposable pending a verdict"),
never committed to this repository's own `bench/` scripts, so its exact
query is not recoverable to re-run verbatim -- only a best-effort
reconstruction, which still doesn't reproduce the claimed result even
using that round's own apparent query shape. Reporting this honestly
rather than guessing at a specific root cause: something about that
original measurement does not describe current, repeatable reality,
and no mechanism tried here explains the gap between the two.

**Does not reopen round fifty-three's DEFAULT decision.** That decision
rests on the box-vs-multicap comparison, independently re-verified
multiple times this session (rounds fifty-one, fifty-three, fifty-
four) with methodology artifacts controlled for each time; the
pgSphere comparison was always a secondary claim layered on top, never
the basis for promoting `skyregion_box_gist_ops`. Retracting it doesn't
change which opclass should be default -- it changes what skycell can
honestly claim about pgSphere, which is currently nothing in this
opclass's favour.

**STATUS**: shipped as skycell 0.23 (`ext/sql/skycell--0.22--0.23.sql`).
Comment text only, both opclasses' `COMMENT ON OPERATOR CLASS` and
`skyregion_box_gist_ops`'s preceding comment block, retracting the
"closes the gap to pgSphere" claim and stating plainly that neither
region opclass is currently known to beat pgSphere's native GiST.
Tested on both the fresh-install path (`make installcheck`) and a real
incremental upgrade (`ALTER EXTENSION skycell UPDATE TO '0.23'` against
`paper_bench`). All `pin8`/`pin9` scratch tables and the ad hoc
`fpr_reg_circ_gist`/`fpr_reg_poly_gist` indexes dropped after
measurement. Open for whoever wants to chase it further: why does
`skyregion_box_gist_ops`, whose key and `consistent()` logic were built
to mirror pgSphere's own `Box3D` as closely as possible (round forty-
two's own stated goal), still lose to it on every corpus and scale
tried here.

## Round fifty-six: chased it down -- round forty-two measured against a
degraded pgSphere index, not a real property of either opclass

Asked to keep chasing round fifty-five's open question. `splitcost_test`
-- the actual disposable database round forty-two's own text named --
still exists on this machine, untouched since. Found it with a plain
`psql -l` rather than assuming it was gone, and it still has `fpr`,
`rox_probe`, `fpr_box_gist`, and pgSphere's own `rox_fpr_circ_gist`,
exactly as that round described. The extension version recorded there
(0.12) is old, but that only means the *catalog metadata* is old --
PostgreSQL loads whichever `skycell.so` is currently installed
regardless of which version string a database's `pg_extension` row
remembers, and `gist_region_box.c` has not changed since round forty-
two wrote it (0.19-0.23 touched `adql.c`, `gist_region.c`'s `MAX_
SUBCAPS`, and SQL comments only), so querying `splitcost_test` right
now runs today's box opclass code against that round's own, never-
rebuilt data.

**Ran the validated per-probe methodology directly against `splitcost_
test`'s own `fpr`/`rox_probe` -- box won, 0.038ms vs pgSphere's 0.256ms,
matching round forty-two's own 2.1-8.4x range almost exactly.** Not a
measurement error this time; genuinely reproduced the original claim,
on the original data, with current code. So the discrepancy is real,
and it is about that specific data/environment, not about the opclass
C code having changed.

**Ruled out the data values themselves first.** `fpr`'s aggregate
statistics (count, min/max/avg/stddev of `ra0`, `dec0`, `r`) are
identical between `splitcost_test` and the `paper_bench` copy rebuilt
this session, down to float precision -- both runs call `setseed(0.77)`
before the same deterministic formula. Physical row *order* differs
completely (checked directly: the first five rows by `fid` are totally
different sets in each database, since `CREATE TABLE AS` without an
`ORDER BY` doesn't guarantee which order a join materializes rows in,
even with a fixed seed), which matters because GiST's incremental,
one-tuple-at-a-time build is order-sensitive -- but copying `splitcost_
test`'s exact row order into `paper_bench` (`\copy` preserves physical
order) and rebuilding both indexes fresh still had box *losing*
(0.037ms vs pgSphere's 0.016ms). Row order wasn't it either.

**Found it by comparing `EXPLAIN` plans for the identical literal probe
side by side.** `splitcost_test` picks a plain `Index Scan` on pgSphere's
own `rox_fpr_circ_gist` (cost 29.38, 24 buffers); a fresh pgSphere index
built on the same (bloaty, never-rebuilt) `fpr` heap in `paper_bench`
picks a `Bitmap Heap Scan` instead (cost 56.44, only 4 buffers) --
the exact plain-Index-Scan-vs-Bitmap-Heap-Scan plan-choice pathology
this project has spent many rounds chasing for its *own* opclasses
(rounds forty-seven/forty-eight), now showing up in pgSphere's index
instead. Confirmed it explains the aggregate gap, not just this one
probe: forcing Bitmap Heap Scan on `splitcost_test`'s own pgSphere index
(`SET enable_indexscan = off`) dropped its median from 0.256ms to
0.018ms -- a 14x speedup, landing right next to every other pgSphere
number measured this session.

**Then found the actual root cause with one command: `REINDEX INDEX
rox_fpr_circ_gist`.** Page count barely changed (191 -> 189 -- not
simple bloat), but the *natural*, unforced plan on that exact query
flipped to fast: median dropped from 0.256ms to 0.015ms, a 17x
speedup, with no `SET`, no forcing, nothing but rebuilding the index
from its current heap scan order. Checked this wasn't just "REINDEX
helps everything in a long-lived database": REINDEXing `fpr_box_gist`
(skycell's own index, same database, same age) changed nothing (689 ->
685 pages, 0.038ms -> 0.040ms, within noise). The degradation was
specific to pgSphere's own index instance in this one disposable,
heavily-reused database -- plausibly accumulated picksplit damage from
however many rounds of ad hoc rebuilding happened in `splitcost_test`
over this session's long history, never once REINDEXed since -- not a
property of pgSphere's GiST implementation, box's, or anything this
session's own 0.19-0.23 changes touched.

**With `splitcost_test`'s pgSphere index healthy again, it beats box by
the same margin every other measurement this session has found**
(0.015ms vs 0.040ms, ~2.7x, right in line with round fifty-five's
0.027-0.256ms range elsewhere). Round forty-two's own claim was real,
reproducible, and entirely explained: it measured pgSphere against a
degraded index in a database nobody thought to `REINDEX`, not against
pgSphere's actual, healthy performance. Nothing about skycell's own
code -- not `gist_region_box.c`, not the 0.19/0.20 exact-test fix, not
anything in rounds fifty-three through fifty-five -- caused or
explains the original number; it was always a benchmark-hygiene gap in
one disposable test database.

**STATUS**: mechanism fully identified, nothing further to chase.
Updated both opclasses' `COMMENT ON OPERATOR CLASS` text (shipped as
skycell 0.24, `ext/sql/skycell--0.23--0.24.sql`, comment text only) to
state the resolved explanation rather than leave round fifty-five's
open question hanging -- the retraction stands, but it is no longer a
mystery. Lesson for this project's own benchmarking practice, worth
stating plainly: an index that sits in a long-lived, heavily-reused
test database and is never REINDEXed can silently degrade into a
pathological plan choice, and a *single* comparison run against it
(not repeated, not cross-checked against a fresh build) can produce a
confidently-wrong number -- exactly what happened here, and exactly
the kind of thing this file's own "measured, not assumed" discipline
exists to catch, just one round later than it should have.

## Round fifty-seven: confirming, via `pageinspect`, that `scircle`'s
## GiST key is the same lossy box as `spoint`'s -- not an exact cap

Round fifty-six's write-up ends by saying pgSphere's win is currently
unchallenged, but that whole comparison had been resting on an
unverified assumption: that pgSphere's own `scircle` GiST stores
something cap-shaped (centre + angular radius) at its leaves, the way
skycell's multi-cap opclass does, and might therefore be both tighter
*and* exact where skycell's box opclass is neither. Round forty-three
already checked this for `spoint` specifically (`pageinspect` showed a
24-byte, two-float4-triples degenerate box) but never followed up for
`scircle` or `spoly`, which are the types this round's own `fpr`
corpus and `20_region_xmatch.sql` actually exercise. Checked it
directly rather than assuming it generalizes.

**Method**: one-row disposable tables, each holding a single known
`scircle` (centre `(100d, 20d)`, radius `5d`) or `spoly` (a 95-105 x
15-25 degree box), each with a fresh `CREATE INDEX ... USING gist`,
inspected with `pageinspect`'s `gist_page_items()` on the raw leaf
page -- the same technique round forty-three used for `spoint`.

**Result, `scircle`**: leaf `itemlen=32` (8-byte tuple header + 24
bytes of payload), key printed as two 3-tuples of Cartesian
coordinates -- `(-0.254183844,0.877404499,0.258819045),
(-0.071547044,0.969907167,0.423919751)`. That's a min-corner/max-
corner axis-aligned box in 3D Cartesian space, not a centre-plus-
radius pair; the printed z-bounds (`0.258819`/`0.423919`) land right
on `sin(dec-r)=sin(15d)=0.258819` and close to `sin(dec+r)=sin(25d)
=0.422618` (the small gap consistent with pgSphere building the box
from a discretized polygonal approximation of the circle's boundary,
not the closed-form extremum). **Result, `spoly`**: same `itemlen=32`,
same two-3-tuple box shape, for a plain rectangle-ish polygon --
confirming this isn't something specific to `scircle`'s own code path.

**Confirmed structurally, not just by byte-counting**: `pg_opclass`
shows `scircle`, `spoly`, and the *default* `spoint` opclass all share
one `opckeytype`, `spherekey` -- `pg_type` gives it `typlen=24,
typbyval=f, typalign='i'`, i.e. a fixed 24-byte struct, int4-aligned,
matching six `float4`s exactly (two Cartesian 3-tuples at single
precision). There's a second, non-default point opclass (`spoint3`,
keytype `pointkey`, `typlen=-1`, varlena) that stores something
different, but it's not the one anything in this benchmark uses --
`cat_sphere_idx` picks the default `spoint` opclass, confirmed via
`pg_opclass.opcdefault`.

**So the premise behind calling pgSphere's win "unchallenged" doesn't
need qualifying, but the reason was wrong.** It isn't that pgSphere's
box key is somehow both exact and tight for a circle/polygon the way
an actual cap key would be -- it's exactly as lossy as skycell's own
`skyregion_box_gist_ops` (same shape: axis-aligned 3D box, approximate
for anything that isn't already box-shaped), just *smaller* (24 bytes
at float4 vs skycell's box key, and skycell's own multi-cap key, both
double-precision). pgSphere wins on raw key size and decades of
C-level optimization in `consistent()`/`picksplit()`, not on having a
structurally better representation. This matches round forty-three's
`spoint` finding exactly and removes the one remaining gap in that
finding's generality -- `scircle` and `spoly` use the identical
mechanism, not something more sophisticated.

**Does not change any shipped decision.** No opclass, comment, or
default is touched by this round; it's a verification of an
assumption flagged as open, not a new result about which opclass to
pick. Probe tables (`cap_probe`, `poly_probe`) dropped after
inspection; nothing persisted beyond this write-up.

**STATUS**: closed. The box-key-vs-cap-key question about pgSphere is
answered (box, confirmed for `spoint`, `scircle`, and `spoly` alike,
all via the shared `spherekey` GiST keytype) and needs no further
chasing.

## Round fifty-eight: cloning pgSphere's exact key layout (float4, fixed-
## length, no varlena) not only closes the gap, it reverses it

The user's own framing, directly: round fifty-seven confirmed pgSphere's
box key is the same *shape* `skyregion_box_gist_ops` already uses
(axis-aligned 3D Cartesian box), just physically smaller -- six `float4`s
in a fixed 24-byte type versus this opclass's six `float8`s in a `bytea`
(48 bytes of payload plus a varlena header). Asked directly: clone that
physical layout exactly, holding the geometry and all other logic fixed,
and see how much of round fifty-five's 1.3-7x pgSphere-wins-everywhere
gap that one change alone recovers -- a controlled before/after, not
another round of guessing from the outside at pgSphere's C code (which
isn't available as source here, only `pg_sphere.so`'s compiled bitcode;
round fifty-seven's reverse-engineering via `pageinspect`/`pg_opclass`
was already the practical ceiling for inspecting it directly).

**Built `skyregion_box4_gist_ops`** (`ext/src/gist_region_box4.c`,
shipped as skycell 0.25, `ext/sql/skycell--0.24--0.25.sql`): byte-for-
byte `skyregion_box_gist_ops`'s own `GistBox3D` struct and the identical
`consistent()`/`picksplit()`/`union()`/penalty logic, with exactly two
things changed -- `double` to `float`, and the `bytea`-wrapped varlena
key to a genuine fixed-length 24-byte type (`skyregion_box4`,
`INTERNALLENGTH = 24, ALIGNMENT = int4, STORAGE = plain`, the same
shell-type pattern `skypos`/`skyregion` already use, with a real
`OUT` function so `pageinspect` can still read it back -- the same
property round fifty-seven relied on to read pgSphere's own key).
Confirmed the clone is physically exact, not just byte-count-equal: a
one-row probe table's leaf `itemlen` is 32 (8-byte tuple header + 24
bytes of payload) -- identical to round fifty-seven's own measurement
of pgSphere's `scircle` leaf.

**Soundness was the one real risk this round had to get right**: casting
a double-precision bound to `float` with ordinary nearest-rounding can
round *inward*, silently shrinking the box below the true region and
dropping real matches -- a correctness bug, not a performance detail.
Every bound is rounded strictly outward instead (`round_down_f4`/
`round_up_f4`: cast, then one `nextafterf()` step away from the box
whenever the cast itself moved the value inward), on both the entry side
and the cached query side, and every comparison against a point promotes
the already-outward-rounded `float` bound back up to `double` rather
than narrowing the point down. `recheck` stays `true` throughout, same
as `skyregion_box_gist_ops`, so even a looser float4 box can never
produce a wrong final answer -- only, in principle, extra candidates.

**Correctness**: a direct per-probe cross-check (`box8` vs `box4` vs
pgSphere's own `scircle &&`, three physically separate tables built from
the exact same underlying point/radius draws so the three queries are
asking the literal same geometric question) found zero mismatches across
every band and probe tried.

**Measured with the validated per-probe/literal methodology (rounds
fifty-one/fifty-four/fifty-five -- a batched nested-loop gets a
misleading plan shape, round fifty-four's own finding) on five isolated
single-scale bands (0.05 deg through 1 deg, 25,000 rows/band, 20 fresh
probes/band), each opclass in its own dedicated table to remove any
"two indexes on one column, which does the planner pick" ambiguity.
Reproduced on two fully independent corpus/probe draws (different
seeds); both agree.** Median buffers (`EXPLAIN (ANALYZE, BUFFERS)`,
every plan node's `Buffers:` line summed, not just the top one) and
median wall-clock, run 1 / run 2:

| radius | box8 buf | box4 buf | pgSphere buf | box8 ms | box4 ms | pgSphere ms |
|---|---|---|---|---|---|---|
| 0.05 deg | 6 / 6 | **4 / 4** | 6 / 6 | 0.080 / 0.078 | **0.055 / 0.054** | 0.065 / 0.062 |
| 0.1 deg | 10 / 6 | **4 / 4** | 6 / 6 | 0.076 / 0.075 | **0.056 / 0.061** | 0.064 / 0.066 |
| 0.3 deg | 8 / 8 | **6 / 6** | 8 / 8 | 0.085 / 0.080 | **0.063 / 0.061** | 0.073 / 0.076 |
| 0.5 deg | 10 / 14 | **8 / 10** | 10 / 12.5 | 0.085 / 0.096 | **0.074 / 0.078** | 0.083 / 0.087 |
| 1.0 deg | 27 / 26 | **23 / 23** | 29.5 / 33.5 | 0.136 / 0.107 | **0.112 / 0.100** | 0.124 / 0.105 |

`box4` has the lowest median buffer count *and* the lowest median
wall-clock time in all five bands, on both independent runs, beating
not just `box8` (expected -- same opclass, smaller key) but pgSphere's
own native GiST as well, by a modest but consistent margin (roughly
10-25% fewer buffers, 10-20% less time). Spot-checked that this isn't
an artifact of `box8`/`box4` naturally picking a plain Index Scan where
pgSphere picks a Bitmap Heap Scan (a real difference in the plans the
planner chose, visible in every `EXPLAIN` capture this round took):
forcing `enable_indexscan = off` so `box4` and `box8` are also forced
onto a Bitmap Heap Scan still shows `box4` touching fewer buffers than
`box8` (2 vs 3 on a representative probe) -- the ordering holds under a
matched plan shape too, not just in each opclass's own natural choice.

**Answers the user's actual question directly**: yes, swapping only the
key's physical representation -- nothing about what it represents,
nothing about `consistent()`'s logic, nothing about which strategies are
supported -- recovers all of round fifty-five's gap and then some, on
this round's own isolated-single-scale methodology. That's strong
evidence the pgSphere-vs-`skyregion_box_gist_ops` gap found in rounds
fifty-one/fifty-four/fifty-five was substantially explained by key
density (bytes per entry -> leaf fanout -> fewer pages touched per
probe), not by pgSphere's `consistent()` embodying some geometrically
sharper test -- both opclasses run the exact same box-overlap arithmetic
shape, differing now only in which C type backs the six numbers. This
doesn't rule out pgSphere also having a leaner, more optimized
`consistent()`/`picksplit()` *on top* of that -- there's no source to
read, so a true instruction-for-instruction comparison was never
practical here (round fifty-seven's `pageinspect` reverse-engineering
was already the ceiling for what's inspectable without it) -- but it
does mean that hypothesis is no longer *needed* to explain the gap this
session has measured: the controlled swap alone, with identical logic
otherwise, already closes and reverses it.

**Does not change any shipped default or recommendation.**
`skyregion_box4_gist_ops` ships EXPERIMENTAL and non-default, exactly
like `skyregion_box_gist_ops` was before its own promotion -- this
round answers a mechanism question (why does the gap exist), it doesn't
yet carry the breadth of correctness/scale testing (pole proximity,
large radii, mixed-scale columns, polygon regions, 50,000+ row corpora)
that earned `skyregion_box_gist_ops` its own DEFAULT status across
rounds forty-two through fifty-four. That testing is the natural next
step for whoever wants to actually promote this key format, not
assumed here.

**STATUS**: shipped as skycell 0.25 (`ext/sql/skycell--0.24--0.25.sql`,
`ext/src/gist_region_box4.c`). Tested on both the fresh-install path
(`make installcheck`) and a real incremental upgrade (`ALTER EXTENSION
skycell UPDATE TO '0.25'` against `paper_bench`). All `b4_*` scratch
tables and the ad hoc `b4_run_buffers()` helper dropped after
measurement; nothing from this round's benchmark harness is part of the
committed `bench/` scripts, same convention as `pin8`/`pin9` before it.

## Round fifty-nine: round fifty-eight's win holds on a mixed circle/
## polygon corpus, and surfaces two separate, real costs neither single-
## type band could show

Asked directly: round fifty-eight only tested isolated, circle-only
bands -- does `skyregion_box4_gist_ops`'s win over pgSphere survive on
the mixed circle/polygon corpus that's skycell's own `skyregion` type
exists for (`20_region_xmatch.sql`'s own header: "pgSphere has no single
region supertype ... needs two typed columns and a query that UNIONs
one join per column")? Checked rather than assumed it generalizes.

**Built five bands (same 0.05-1 degree radii as round fifty-eight,
25,000 rows/band, 40 fresh probes/band), each row and each probe an
independent 50/50 coin flip between a circle and an axis-aligned
lon/lat-box polygon of the same nominal half-width** (same recipe `fpr`
uses: `polygon('ICRS', ra-r, dec-r, ra+r, dec-r, ra+r, dec+r, ra-r,
dec+r)`). skycell's `box8`/`box4` get one column, one index, one `&&`
per probe, same as round fifty-eight. pgSphere gets two typed columns
(`reg_circ scircle`, `reg_poly spoly`, one NULL per row depending on
kind) and two GiST indexes, and a probe's query is the sum of two
subqueries (one per column) -- `20_region_xmatch.sql`'s own
`'pgsphere:mixed'` shape, now benchmarked instead of just described.
Confirmed pgSphere's cross-type operators actually work and index
correctly first (`spoly && scircle` and the reverse both evaluate true,
and a literal `scircle` probe against an `spoly`-indexed column plans
as a normal `Bitmap Index Scan`) before relying on them in the harness.

**Correctness**: every method's count matched a brute-force `intersects()`
oracle exactly, zero mismatches, across both an initial run and an
independently-reseeded repeat.

**Reproduced on two independent corpus/probe draws; both agree closely.**
Median buffers and median wall-clock (`EXPLAIN (ANALYZE, BUFFERS)`, same
methodology as round fifty-eight), run 1 / run 2:

| radius | box8 buf | box4 buf | pgSphere buf | box8 ms | box4 ms | pgSphere ms |
|---|---|---|---|---|---|---|
| 0.05 deg | 6 / 6 | **4 / 4** | 16 / 16 | 0.364 / 0.341 | **0.093 / 0.100** | 0.294 / 0.300 |
| 0.1 deg | 6 / 6 | **4 / 4** | 16 / 16 | 0.350 / 0.381 | **0.091 / 0.092** | 0.279 / 0.307 |
| 0.3 deg | 8 / 10 | **6 / 6** | 19 / 19 | 0.343 / 0.378 | **0.100 / 0.094** | 0.316 / 0.304 |
| 0.5 deg | 14 / 12 | **10 / 8** | 26 / 22 | 0.419 / 0.404 | **0.119 / 0.118** | 0.379 / 0.351 |
| 1.0 deg | 31 / 30 | **29 / 26** | 48.5 / 49.5 | 0.388 / 0.436 | **0.145 / 0.138** | 0.390 / 0.432 |

`skyregion_box4_gist_ops` wins decisively on *both* metrics in every
band, on both runs -- by a wider margin than round fifty-eight's
circle-only numbers, not a narrower one. That headline holds, but
getting here surfaced two separate, genuinely different costs that a
single-type corpus can't show at all, and conflating them would be
wrong:

**1. pgSphere pays a real, measurable architecture tax for being
two-typed.** Its buffer count roughly *doubles to triples* going from
round fifty-eight's circle-only bands (6-29.5) to this round's mixed
ones (16-49.5) at the *same* radii -- not because the matching work
grew, but because every single probe, circle or polygon, now pays for
two separate index descents (`reg_circ`'s and `reg_poly`'s) instead of
one, even on the half of probes whose true matches can only live in one
of the two columns. `skyregion_box_gist_ops`/`box4_gist_ops`'s own
buffer counts barely move between the two rounds (e.g. `box8` at 0.05
deg: 6 circle-only, 6 mixed) -- one column, one index, same page-level
cost regardless of what kind of region is stored in it. This is exactly
the structural cost `20_region_xmatch.sql`'s own header predicted in
words; this round is the first time it's actually been measured.

**2. Recheck cost (CPU, not I/O) rose sharply for skycell's own box
opclasses, independent of buffers.** `box8` at 0.05 deg: 6 buffers in
*both* rounds, but 0.080ms (circle-only, round fifty-eight pass 2) vs
0.34-0.36ms (mixed) -- a ~4.5x slowdown with an *unchanged* buffer
count, so it isn't a paging/fanout effect at all. Both box opclasses
always set `recheck = true` (sound but not tight, same as every region
opclass in this project), so every candidate the box prunes to still
needs a real `intersects()`-equivalent call against the exact region --
and that call is pricier when either side is a polygon (point-in-
polygon / edge-normal tests against a variable-length vertex array)
than when both sides are a circle (one dot-product). In a 50/50 mixed
corpus, even a *circle* probe's candidates are half polygons, so this
cost rises for every probe, not just polygon ones -- confirmed by the
per-probe-kind breakdown: `box8`'s `circ`-probe median rose from
~0.047ms (round fifty-eight's all-circle corpus) to ~0.33-0.36ms here,
despite an identical buffer count on identical-shaped queries. `box4`
shows the same effect but much more mutedly (~0.055ms to ~0.09-0.10ms,
under 2x) -- plausibly because its tighter per-entry box (round fifty-
eight's own finding: fewer buffers touched for the same query) also
means fewer candidates reach the expensive recheck call in the first
place, so the *same* per-candidate CPU tax has less surface area to
act on. This is a separate, real cost from the key-density story round
fifty-eight told -- about `recheck()`'s own cost on mixed-kind regions,
not about leaf fanout -- and it affects every region opclass this
project ships, not something specific to this round's new one.

**Net picture, not a single number**: on a realistic mixed circle/
polygon corpus, `skyregion_box4_gist_ops` remains the clear winner on
both I/O and wall-clock against both `skyregion_box_gist_ops` and
pgSphere -- the round fifty-eight result generalizes, it doesn't
narrow. But the *reasons* the three methods differ are now known to be
two distinct effects stacked together (pgSphere's two-index tax on
buffers; every region opclass's heavier recheck on polygon-involving
candidates, on top of that) rather than one. Neither effect changes
which opclass wins here, but either could matter on its own in a
corpus shaped differently than this round's roughly-balanced 50/50 mix
(e.g. a corpus that's 95% circles would dilute the recheck effect
further; a corpus where pgSphere's two columns could each be indexed
far more selectively than the combined skycell column might narrow or
close its architecture tax) -- not tested here, flagged for whoever
wants to chase it.

**Does not change any shipped default.** Same status as round
fifty-eight: `skyregion_box4_gist_ops` remains EXPERIMENTAL and
non-default; this is further evidence for the mechanism, not a
promotion decision.

**STATUS**: shipped as a GIST_REGION_DESIGN.md entry only -- no opclass,
operator, function, or SQL change this round (skycell stays at 0.25).
All `bm_*` scratch tables and the ad hoc `bm_run()` helper dropped
after measurement; not part of the committed `bench/` scripts, same
convention as every disposable corpus before it.

## Round sixty: all four strategies, on the real 50,000-row `fpr`
## corpus, not a synthetic isolated-scale band -- `box4` wins every one

Rounds fifty-eight/fifty-nine used synthetic, isolated-single-scale
bands (uniform radius per band, 50/50 circle/polygon, fresh independent
draws) specifically to control confounds -- but that's not the corpus
this project's own prior rounds (forty-two, fifty-five) used when they
talked about "50,000 rows": `fpr` itself, built by the committed
`20_region_xmatch.sql` (power-law radius 0.02-0.3 degree, sampled from
real catalogue positions via `src`, not synthetic uniform-on-sphere).
Asked directly to use the real thing, and to cover all four strategies
`skyregion`'s GiST opclasses support (&&, `@>`/point, `@>`/region,
`<@`/region), not just overlap.

**Rebuilt `fpr` at the real scale** (`20_region_xmatch.sql -v
nfp=50000`) and ran the project's own already-committed comparison
scripts at that scale (`22/23/24_region_*.sql -v nprobe=500` --
`fpr_region_gist`, the DEFAULT `box8` opclass, is what these scripts
already measure against pgSphere and a brute-force oracle; this round
didn't need to write that half). Added the one thing those scripts
don't do: a `skyregion_box4_gist_ops`-indexed copy of the same 50,000
rows (`fpr4`, built with `CREATE TABLE fpr4 AS SELECT * FROM fpr`, so
every row and every probe comparison is against byte-identical
geometry) run through the identical queries, plus the one pgSphere
number `22_region_gist.sql` never computed (its own `&&`, which --
unlike the other three strategies, where only one side of the
comparison has a circle/polygon kind -- needed all four probe-kind x
corpus-kind combinations summed, since both `fpr`'s rows and `rox_
probe`'s own probes are a 50/50 circle/polygon mix).

**Correctness**: every method's count matched the brute-force oracle
(`intersects()`, `skycell_in_region()`, `skycell_region_covers()`,
`skycell_region_covered_by()`) exactly, zero mismatches, across all
four strategies.

**Plan-shape sanity check, before trusting the numbers**: all three
methods' queries plan as the identical shape here (a parameterized
nested-loop `Index Scan`, `rox_probe` on the outer side) -- confirmed
directly via `EXPLAIN`, not assumed -- so, unlike round fifty-eight's
circle-only bands (where `box8`/`box4` naturally picked a plain Index
Scan and pgSphere a Bitmap Heap Scan), this round's three-way ranking
isn't comparing different plan shapes. Cross-checked the batched
form's ranking against round fifty-four's own validated single-
literal methodology too (one probe, same geometry, all three methods):
`box4` 0.128ms/25 buffers, `box8` 0.248ms/27 buffers, pgSphere
0.395ms/32 buffers for that one probe -- same ordering as the batched
numbers below, so the batched form isn't producing a different
ranking than the literal one would, this time.

**Median-of-2 wall-clock, 500 probes/strategy, box8 (DEFAULT) / box4 /
pgSphere**:

| strategy | box8 ms | box4 ms | pgSphere ms |
|---|---|---|---|
| `&&` (overlap) | 17-21 | **8-11** | 66-67 |
| `@>`(region,point) | 16.6-17 | **8.8-9.1** | 9.8-11.7 |
| `@>`(region,region) "contains" | 14-16 | **6.5-8.0** | 11.5-11.8 |
| `<@`(region,region) "covered_by" | 39-43 | **27-28** | 104-107 |

`skyregion_box4_gist_ops` wins all four strategies outright, against
both `skyregion_box_gist_ops` and pgSphere, on the real corpus -- not
just the three isolated bands rounds fifty-eight/fifty-nine already
covered, and not just `&&`. Two things worth separating from that
headline:

**`box8` (the shipped DEFAULT) does not uniformly beat pgSphere here --
its standing is strategy-dependent, matching what `23_region_contains.
sql`'s own comment already documented** (box8 loses to pgSphere on
point-in-region containment, by ~1.5-1.7x at this scale, consistent
with that script's own prior finding of the gap "widening to ~5.5x at
50,000" in an earlier measurement) but *wins* on both region-region
directions and (newly measured here) on `&&` too. `box4` is the only
one of the three that wins everywhere, not by inheriting `box8`'s wins
and losing where `box8` loses, but by beating pgSphere on the one
strategy (`@>`/point) where `box8` itself could not.

**pgSphere's `&&` number (66-67ms) is the single biggest number in the
whole table, and it's almost certainly the two/four-typed-column tax
round fifty-nine already found, now worse.** Round fifty-nine's mixed-
corpus test only needed pgSphere to pay for two index descents per
probe (one kind on each side of a single column pairing); `&&` on this
corpus needs *four* subqueries (circle-circle, circle-polygon, polygon-
circle, polygon-polygon), since both `rox_probe` and `fpr` are
independently mixed -- paying for up to four index descents per probe
where skycell's own opclasses still pay for exactly one. Consistent
with, not a new mechanism beyond, what round fifty-nine already
identified.

**Does not change any shipped default.** Same status as rounds fifty-
eight/fifty-nine: a further, now corpus-realistic confirmation of
`skyregion_box4_gist_ops`'s mechanism, not a promotion decision --
promoting it would still need the breadth of scrutiny (pole proximity,
large-radius behaviour, mixed-scale columns) that earned `skyregion_
box_gist_ops` its own DEFAULT status, none of which this round or the
two before it attempted.

**STATUS**: shipped as a GIST_REGION_DESIGN.md entry only -- no
opclass/operator/function/SQL change (skycell stays at 0.25). `fpr`
itself (rebuilt at 50,000 rows) and the `rox_probe*`/`bench_region_*`
tables are the project's own normal, persistent bench artifacts (owned
by the committed `20/22/23/24_region_*.sql` scripts, not this round) and
were left as those scripts normally leave them; only this round's own
additions (`fpr4` and its ad hoc `allops_results`/`allops_run()`
harness) were dropped after measurement.

## Round sixty-one: promoting `skyregion_box4_gist_ops` to DEFAULT --
## the same scrutiny `box8` had before its own promotion, one real
## discrepancy found and flagged rather than papered over, and one
## confound reproduced

Asked directly to promote `skyregion_box4_gist_ops` to DEFAULT. Flagged
first that it hadn't had the breadth of scrutiny `skyregion_box_gist_
ops` went through before its own promotion (rounds forty-two, fifty-
three, fifty-four): pole proximity, large-radius behaviour, and a
mixed-scale column. Asked to run that scrutiny before promoting, not
instead of it.

**Part 1 -- pole proximity.** Reran round forty-two's own declination-
stratified design exactly (6,000 rows, 2,000 each at equator/<10,
mid-lat 40-50, near-pole 85-89.9, `fpr`'s own 0.02-0.3 degree radius
range, 300 stratified probes), box8/box4/multicap each in their own
dedicated table. Correctness held exactly in all three bands --
5/5/5/5, 8/8/8/8, 404/404/404/404 (brute/box8/box4/multicap) -- and
box4's buffer advantage over box8 held near the pole too (630 vs 645 at
the equator, 2196 vs 2271 near-pole), not eroding the way a float4-
precision regression might have. No surprises, no regression.

**Part 2 -- large-radius crossover.** Built isolated single-scale bands
(uniform-on-sphere centres, one radius per dedicated table, box8/box4/
multicap/pgSphere each with their own index) across 2.5 to 80 degrees.
**Found a real methodology hazard before trusting any number**: at 60
and 80 degrees the circle covers so much of a 15,000-row table that the
planner's Index-Scan-vs-Bitmap-Heap-Scan cost estimate becomes a near
coin flip, and it genuinely flips per opclass -- `box8` got a plain
`Index Scan` at 60 degrees (13,703 buffers, the exact "many redundant
false-positive rechecks" pathology rounds forty-seven/forty-eight/
fifty-four/fifty-six have each independently hit) while `box4` on
byte-identical data got a `Bitmap Heap Scan` (239 buffers) -- an
18x-looking "difference" that's actually a planner tie-break artifact,
not a geometry difference. Checked every radius's plan shape explicitly
before trusting its numbers (not assumed from one spot check): clean
and consistent (`Bitmap Heap Scan` for all four methods) from 2.5
through 50 degrees; erratic (different scan types per method) at 55
degrees and up, where the query is matching such a large fraction of
the table that no plan is efficient and whichever one a given opclass's
index happens to get is close to arbitrary. Restricted all conclusions
to the verified-clean 2.5-50 degree range.

**In that clean range, `box4` and `box8` both beat `skyregion_gist_ops`
(multicap) decisively, at every radius tried, up to 50 degrees** --
e.g. at 50 degrees, box8 778 buffers vs multicap 1612 (verified
identical Bitmap Heap Scan plan shape for both, re-checked directly).
Re-measured 10 degrees specifically at both this round's 15,000-row
scale and round fifty-four's own reported 25,000-row scale, both with
confirmed-clean plan shapes: box8 beat or tied multicap in both wall-
clock and buffers at that radius too (296 vs 390.5 buffers at 15,000
rows; 485 vs 643 at 25,000).

**This directly contradicts round fifty-four's own claim** ("multicap
won every one of [10, 25, 45, 60 degrees], decisively -- e.g. 10.627ms
vs 13.960ms at 10 degrees"). Not explained by a batched-query artifact
(this round's methodology is single-literal throughout, same validated
form round fifty-four itself used) or by corpus size (reproduced at
both 15,000 and 25,000 rows). **Could not fully root-cause the
discrepancy within this round's scope** -- round fifty-four's own
pin6_corpus no longer exists to re-inspect, and this round's own plan-
shape hazard (part of this exact finding) is precisely the kind of
thing that's easy to miss if only one spot-checked rather than every
radius -- plausible, given round fifty-six already found a different,
unrelated historical claim (round forty-two's own pgSphere comparison)
was an artifact of an unverified plan choice in that same long-lived
scratch database, that round fifty-four's own multicap-wins measurement
had the same unverified gap. **Reporting this honestly as an open,
unresolved discrepancy, not silently overriding five rounds of
established history with one fresh result**: no specific large-radius
crossover number is currently a safe claim for either box opclass.

**Part 3 -- mixed-scale column.** Reproduced round fifty-four's other
confound directly: one table, 15,000 rows, half 1-3 degree circles and
half 85-89 degree circles, ONE shared index, small-radius-only probes
(the regime isolation alone would favour the box opclasses in, per part
2). Confirmed clean, identical plan shape (`Bitmap Heap Scan`) for
box8/box4/multicap. Correctness exact (147,593 matches, all four
methods agree). **This confound did reproduce, for box4 same as box8**:
multicap touches far *more* buffers (823 vs box4's 338, box8's 525) but
still comes out fastest on wall-clock (2.15ms vs
box4's 2.20ms, essentially tied, vs box8's 2.71ms, clearly behind) --
qualitatively the same effect round fifty-four found (buffer count and
wall-clock disagree once an extreme-scale outlier shares the index),
smaller in magnitude than that round's own 8.601/5.796ms numbers but
the same direction and the same affected opclasses. `box4` narrows the
gap to multicap here (near-tied rather than clearly behind) but doesn't
reverse it.

**Promotion decision**: `box4` was not worse than `box8` in any regime
tested -- ties or wins on the mixed-scale confound (part 3), wins
outright everywhere else (part 1, part 2's clean range, and every
corpus in rounds fifty-eight through sixty). Swapping the DEFAULT from
`box8` to `box4` therefore carries no regression versus the status quo
in any tested regime, while being a strict improvement in the common
case (small-to-few-degree, realistic catalogue footprints) and in the
one strategy (`@>`(region,point)) where `box8` itself trailed pgSphere.
The open large-radius discrepancy (part 2) doesn't block this decision
either way: it doesn't newly justify demoting box4 (nothing found box4
*losing* to multicap at any radius, clean-plan-shape or not), and it
isn't resolved cleanly enough to assert a *wider* safe range than box8
already claimed. The mixed-scale caveat box8's own comment always
carried (`skyregion_gist_ops` wins for a column mixing very different
region scales) is carried over to box4's comment unchanged, now
confirmed directly for box4 rather than just inherited from box8's own
geometry.

**Shipped**: `skyregion_box4_gist_ops` promoted to `DEFAULT FOR TYPE
skyregion USING gist`, demoting `skyregion_box_gist_ops` (same DROP-
OPERATOR-FAMILY-and-recreate mechanism round twenty-one's own box8
promotion used -- PostgreSQL has no `ALTER OPERATOR CLASS ... SET
DEFAULT`). Shipped as skycell 0.26 (`ext/sql/skycell--0.25--0.26.sql`,
folded into `ext/sql/skycell--0.26.sql`). No C code change -- box4's
opclass functions and key type already existed from 0.25; only the
`DEFAULT` keyword's placement and the three affected opclasses'
`COMMENT ON` text changed. Tested on both the fresh-install path
(`make installcheck`) and a real incremental upgrade (`ALTER EXTENSION
skycell UPDATE TO '0.26'` against `paper_bench`), including a direct
check that `pg_opclass.opcdefault` flipped correctly and that a plain,
opclass-unqualified `CREATE INDEX ... USING gist (region)` now builds a
`box4` index. `fpr_region_gist` (dropped by the `CASCADE`, same
documented side effect round twenty-one's own promotion had) rebuilt
afterward; `rc_corpus`'s two explicitly-named indexes were left for
whoever next runs `26_region_crossover.sql` to recreate, same as that
round's own precedent. All of this round's own scratch tables (`pole_*`,
`lr_*`, `rc10*`, `ms_*`) and ad hoc helper functions dropped after
measurement.

**STATUS**: shipped. The large-radius discrepancy with round fifty-four
(part 2) is open for whoever wants to chase it further -- ideally by
finding or rebuilding something close to that round's own pin6_corpus
and checking every radius's plan shape explicitly, the exact thing this
round's own part 2 shows is easy to skip and easy to get a misleading
number from if skipped.

## Round sixty-two: does the box4 lesson transfer to *points* --
## `skypos_cap4_gist_ops`, and three real methodology bugs found and
## fixed in sequence before trusting any number

Asked whether `box4`'s key-density lesson extends to the OTHER native-
GiST-vs-pgSphere comparison this project has: `gist_point_cap.c`'s own
`skypos_cap_gist_ops` vs pgSphere's native `spoint` GiST, where round
forty-three found pgSphere winning below a ~10-30 arcmin crossover.
That file's own history had already tried and reverted floating the
*leaf* centre (3-5x slower -- it broke the exact, `recheck=false` leaf
test points get and regions never had), so the one lever left
untried, directly motivated by `box4`: shrink only the *internal*-node
cap (28 bytes: double centre + packed float radius) to float4 (16
bytes), leaving the leaf's double-precision exactness completely
untouched.

**Built `skypos_cap4_gist_ops`** (`ext/src/gist_point_cap4.c`,
registered ad hoc, same non-shipped status as `gist_point_cap.c`
itself): identical to that file except `cap_to_bytea()`'s internal
branch and a new `shrink_cap_f4()` that rounds the cap's centre to
float4 then inflates the radius by strictly more than the resulting
centre drift (plus the radius's own rounding), rounded up via
`nextafterf`, so the float4 cap always contains the exact double-
precision cap it was built from -- `cap_overlaps()` is a necessary-
condition pruning test, so this has to never shrink, only ever
(slightly) grow. Correctness: 0 mismatches against both the existing
cap-GiST and pgSphere, every radius tested.

**Three real methodology bugs, found and fixed in sequence, each one
capable of producing a confidently wrong number on its own:**

1. **A third, always-visible index silently won.** `cat_pos` carries
   `skypos_cap_gist_ops`, the new `skypos_cap4_gist_ops`, *and* the
   already-shipped `skypos_spgist_ops` on the same column/operator --
   toggling `pg_index.indisvalid` between the two cap variants did
   nothing, because SP-GiST was never hidden and the planner preferred
   it over both the whole time. Caught by capturing the actual index
   name used per probe (a regex over the `EXPLAIN` text), not assumed
   from the toggle -- the first full run's "cap vs cap4" gap was really
   "SP-GiST run first vs SP-GiST run second," a warm-cache artifact
   with nothing to do with either cap opclass.
2. **Hiding the wrong thing broke the fallback.** Also hiding
   `cat_pos_cellexpr` (skycell's own B-tree-rewrite index) to stop its
   cost-based rewrite from competing didn't disable the rewrite -- it
   still fired, found no B-tree to serve the resulting cell-range
   filter, and fell back to a 10-million-row `Seq Scan` evaluating that
   filter per row (187,000+ buffers at a 1-arcsecond radius). The fix
   was the GUC the rewrite's own cost model is built around,
   `skycell.rewrite_max_waste = 0`, which declines the rewrite outright
   and falls back cleanly to whichever GiST index is visible --
   `cat_pos_cellexpr` could stay valid throughout once this was set.
3. **Interleaving three oversized structures thrashes the cache.**
   `cat_pos_capgist` (870MB) + `cat_pos_cap4gist` (867MB) +
   `cat_sphere_idx` (682MB) together are more than `shared_buffers`
   (2GB); running all three methods interleaved per probe (deliberately,
   to avoid batching-order bias) meant each one evicted the last one's
   pages on every switch, inflating both buffer counts and wall-clock
   for whichever structure a given probe happened to hit right after a
   switch -- not a property of any opclass. Round forty-three's own
   methodology note ("`EXPLAIN (ANALYZE, BUFFERS)`, *warmed*") already
   named the fix: an untimed warm-up execution of each query
   immediately before the timed one, inside `cap4b_run2()` itself, so
   every timed measurement reflects steady state for its own structure.

**With all three fixed, and reproduced on two independently reseeded,
60-probes-per-radius runs (round forty-three's own 20-probe sweep had
already flagged median instability on this same clustered corpus; even
60 left real residual noise at sub-millisecond radii -- see below):**

- **Buffers**: `cap4` and `cap` are statistically indistinguishable at
  every radius from 1 arcsec through 1 degree (e.g. 1 arcsec: 22-24 vs
  23-24 across the two runs; 1 degree: 263-331 vs 269-329) -- no real
  win, unlike `box4`'s own clean, reproducible buffer reduction for
  regions. At 3 degrees the two runs disagree on direction by a margin
  smaller than their own run-to-run spread (448.5-465.5 vs 452.5-497).
- **Wall-clock**: genuinely noisy at these small absolute times (sub-
  millisecond to ~2ms) -- one run had `cap4` *slower* than `cap` at 5 of
  7 radii, the other had it faster at 6 of 7, a direction flip neither
  run's own buffer counts support. Not resolved by this round; reported
  as unresolved rather than picking whichever run looked better.
- **pgSphere wins decisively and consistently at every radius from 1
  arcsec through 30 arcmin, in both runs, by a stable ~1.4-2.7x** --
  unchanged from round forty-three's own finding, for `cap` and `cap4`
  alike. At 1 degree and 3 degrees the three methods are within noise
  of each other in both runs (sometimes `cap4` ahead, sometimes
  pgSphere, consistent with round forty-three's own observation that
  the cap family's relative standing improves at larger radii).

**Why this doesn't reproduce `box4`'s result, and the explanation is
structural, not a measurement gap**: `box4` shrank *every* key, leaf
included, because region leaves were never exact in the first place
(`recheck=true` regardless of key precision) -- the dominant leaf
population got cheaper at every level of the tree. Here, leaf exactness
is load-bearing (the project's own hard-won, already-reverted finding),
so only the *minority* internal-entry population shrank. Since leaf
entries vastly outnumber internal ones in any GiST tree, and most pages
a small-radius probe touches are leaf pages regardless, shrinking only
the internal layer has much less surface area to improve than shrinking
everything did for regions. The lesson transfers in principle (key
density still matters) but the two opclasses don't have the same amount
of shrinkable surface to spend it on.

**Does not change any shipped default or recommendation.**
`skypos_cap4_gist_ops` is EXPERIMENTAL, unregistered in any versioned
SQL file, same status as `gist_point_cap.c` itself -- a modest, safe
(no measured regression anywhere, same leaf exactness, same
correctness) variant worth keeping around, not a replacement.

**STATUS**: shipped as a GIST_REGION_DESIGN.md entry and
`ext/src/gist_point_cap4.c` only (no SQL/catalog change beyond the ad
hoc registration this round made and then left in place on
`paper_bench` for anyone who wants to keep poking at it -- unlike every
other round's own scratch tables, which were dropped). All of this
round's own probe/result scratch tables and helper functions
(`cap4b_probe`, `cap4b_realsample`, `cap4b_results`, `cap4b_results2`,
`cap4b_set_visible()`, `cap4b_run()`, `cap4b_run2()`) dropped after
measurement; `cat_pos`'s four indexes and `skycell.rewrite_max_waste`
both restored to their normal state.

## Round sixty-three: closing (not just narrowing) the B-tree rewrite's
## warm-cache loss at 6' -- an existing, already-off-by-default knob,
## not new code

Round forty-four found the B-tree rewrite loses to pgSphere warm at 1'
and 6' specifically (false-positive candidates burning CPU on the exact
test once I/O is free), while winning everywhere else, cold or warm.
Asked directly: can that specific loss be closed, not just explained?

**The lever already exists in `cover.c`, off by default, with its own
limitation already documented in the source.** `skycell.probe_orders`
(default 0) lets the covering algorithm *try* a finer HEALPix order
than its closed-form starting point and keep it if `rlist_score()`
judges it cheaper; a comment at the call site (`cover.c`, the
`probe_orders` loop) already states this finer candidate is "worth
30-48% at 6'-30'... with the range count unchanged" when forced, but
loses the internal cost comparison anyway because `rlist_score()`
charges `skycell.split_cost` (default 1.0 row per cell examined) for
every cell the finer probe enumerates -- roughly 4x as many cells per
extra order -- which outweighs the false positives it actually saves at
this scale. The comment names the fix directly: "calibrate split_cost
against measurement... not tune it until this looks good." This round
did exactly that, empirically, rather than picking a number.

**Method**: `bench/03_cone.sql`'s own existing harness (`bench_cone_
explain`, `apply_variant()` -- already built for exactly this kind of
GUC ablation), same 1,456-probe set, same seven radii, against the same
`cat_cell` (skycell B-tree rewrite) / `cat_sphere` (pgSphere) tables
round forty-four used. Two real measurement pitfalls, caught before
trusting any number:

1. **The server had restarted** (a VM-level restart mid-session, not a
   crash -- `dmesg` showed a fresh boot) between setting this up and
   running it; the first pass after any restart still measures
   PostgreSQL's own cold start regardless of `EXPLAIN ANALYZE`'s own
   "warm" framing -- confirmed directly (32.8s for a pass that should
   take under a second) and fixed by discarding that first pass and
   measuring the second, the same discipline round forty-four's own
   cold-vs-warm comparison already used.
2. **The obvious proxy metric for false-positive waste was wrong.**
   `bench_cone_explain`'s own `est_rows`/`act_rows` columns are the
   scan node's *post-recheck* row counts (the true result, which does
   not change with covering tightness), not the pre-filter candidate
   count -- tracking `act_rows - est_rows` across a `force_order`/
   `probe_orders` sweep showed no movement at all, which briefly looked
   like the knob doing nothing. It wasn't: median wall-clock *did* move
   substantially once measured directly, and a direct `EXPLAIN
   (ANALYZE, BUFFERS)` on an individual dense probe showed `Rows
   Removed by Filter` was the metric that actually mattered, just not
   one the existing harness happened to capture in its summary columns.

**Result: forcing `skycell.probe_orders = 3` together with `skycell.
split_cost = 0.1` (letting the finer candidate actually win its own
internal cost comparison, per the code comment's own diagnosis) closes
the 6' loss outright and widens the existing 30' win, with buffers
flat-to-improved everywhere, not worse:**

| radius | pgSphere | skycell (baseline) | skycell (tuned) |
|---|---|---|---|
| 1" | 0.036ms | 0.022ms (won) | 0.028ms (still won, small regression) |
| 10" | 0.027ms | 0.022ms (won) | 0.020ms (won, improved) |
| 1' | 0.026ms | 0.026ms (tied) | 0.028ms (still ~tied, small regression) |
| **6'** | **0.047ms** | **0.0575ms (lost)** | **0.047ms -- ties outright** |
| **30'** | **0.1335ms** | **0.1305ms (narrow win)** | **0.094ms -- 30% faster, not narrow** |
| 1 deg | 0.328ms | 0.2795ms (won) | 0.271ms (won, improved) |
| 3 deg | 1.790ms | 1.203ms (won) | 1.052ms (won, improved) |

Buffers moved the same direction as time at every radius that improved
(e.g. 6': 9.2 -> 8.2 average buffers; 30': 26.1 -> 24.6) -- this is not
the cold-cache-cost tradeoff flagged as the likely risk before running
this: a tighter covering here comes from a *better-chosen* boundary at
the *same* range count (the code comment's own "range count unchanged"
claim, confirmed rather than assumed), not from adding ranges, so there
is no reason to expect it costs more when cold either. Two radii (1",
1') show a small regression but neither stops winning or tying
pgSphere there.

**Correctness**: every one of the 1,456 probes' row count matched
exactly between tuned skycell, baseline skycell, and pgSphere -- zero
mismatches. Expected, not just hoped for: this knob only changes which
covering the cost model picks, never the soundness of the covering
itself (still a superset of the true region either way, same exact
filter applied regardless of which order produced the candidates).

**Does not change any shipped default.** `skycell.probe_orders` and
`skycell.split_cost` are both already-existing, already-documented
GUCs (`PGC_USERSET`, changeable per session) -- nothing new was built
this round, only a specific combination of existing settings measured
and found to work for this specific benchmark (`bench/03_cone.sql`'s
own corpus and query shape: cones only, one density model, one table
size). That is deliberately not grounds for changing the compiled-in
defaults (0 and 1.0 respectively) yet -- same bar as `box4`'s own
promotion scrutiny (round sixty-one): other query shapes (polygons, the
region-side strategies), other corpora/densities, and the cold-cache
side specifically (this round measured buffers as a proxy, not a real
cold run) would all need checking first, exactly the "test promotion
separately" the user asked for rather than folding it into this
round's own documentation.

**STATUS**: documented only. `bench_cone`/`bench_cone_x` now also carry
rows for `variant` values `force_order=16..26` (the dead-end tried
first -- `force_order` alone does not change the final covering's
tightness at these radii, confirmed, not just unexplored) and
`probe_orders=0..3`, `probe_orders=N,split_cost=M` for several `N`/`M`
(the one that worked) -- all left in place, the project's own normal
persistent bench artifacts, same convention as every prior round's use
of this file's tables. No code, SQL, or default changed. Promotion
(changing `skycell.probe_orders`/`skycell.split_cost`'s compiled-in
defaults, or making `rlist_score()` itself cost-aware of this tradeoff
rather than needing a hand-picked constant) is a separate, not-yet-
started round.

## Round sixty-four: promotion scrutiny for `probe_orders=3, split_
## cost=0.1` -- generalizes cleanly on two axes, confirmed on a real
## cold run, and one real side effect identified, not hand-waved past

Round sixty-three found that combination closes the B-tree rewrite's
one known warm-cache loss (6') and widens its 30' win, but flagged
three open questions before trusting it beyond that one benchmark:
other query shapes, other corpora/densities, and a real cold-cache
run (that round only had buffers as a cold-cache proxy). All three
checked this round.

**Part 1 -- polygons.** `bench/05_poly.sql`'s own 120-polygon corpus
(0.05-2 degrees across), extended with buffer capture
(`bench_poly_explain`, mirroring `bench_cone_explain`'s own pattern).
Unlike cones, polygons never had a warm-cache loss to begin with --
skycell's B-tree rewrite already beats pgSphere's `spoly` by 2x-10x at
every size bucket tried, un-tuned. The tuning still helps, modestly
and in the same direction as cones (8-25% faster, buffers flat to
improved), with zero correctness mismatches across all 120 polygons.
No regression found; nothing to close, since there was no loss here.

**Part 2 -- a different density.** Every round before this one measured
against `src`/`cat_cell`/`cat_sphere`'s own 60%-clustered corpus. Built
a fresh, fully uniform-on-sphere corpus (`cat_cell_u`/`cat_sphere_u`,
2 million rows -- a scoped density check, not a second 10-million-row
rebuild) and reran the same radius sweep. skycell already wins by large
margins at every radius here too (no dense clusters means no exact-
filter waste to begin with, the opposite extreme from the clustered
corpus's 6' problem), so again there was no loss for the tuning to
close. The tuning's own effect was genuinely mixed here -- faster at
6'/30'/3deg, flat at 1"/10", and one real regression at 1 degree
(0.137ms -> 0.1675ms, buffers improving slightly despite the slower
time) -- but never close to threatening skycell's own large lead over
pgSphere at any radius on this corpus. Zero correctness mismatches.

**Part 3 -- a real cold-cache run, not the buffer-count proxy.** Round
sixty-three's "buffers flat to improved, so probably fine cold too" was
a reasonable inference, not a measurement -- checked directly this
round, reusing round forty-four's own fix for the `drop_caches`-is-a-
no-op problem in this environment (fresh, never-before-touched probe
coordinates per trial, a full `pg_ctlcluster restart` immediately
before each). Four trials (baseline/tuned x 6'/30', 20 fresh probes
each, genuinely cold -- confirmed by per-probe times in the tens-to-
low-hundreds of milliseconds, matching round forty-four's own cold
ballpark, not warm-range numbers):

| trial | mean buffers | median buffers | mean ms | median ms |
|---|---|---|---|---|
| 6', baseline | 8.4 | 6.0 | 74.20 | 69.30 |
| 6', tuned | **5.7** | **5.0** | **62.08** | **30.89** |
| 30', baseline | 18.0 | 15.0 | 95.83 | 69.54 |
| 30', tuned | 18.0 | **13.0** | **50.84** | **24.54** |

Not a tradeoff -- tuned is equal-or-fewer buffers *and* faster, cold,
at both radii. This directly confirms the "range count unchanged, no
cold-cache cost" claim from round sixty-three's own source-comment
citation, now against a real restart-and-measure run rather than a
buffer-count inference.

**One real side effect, found by checking the mechanism, not just the
outcome, for the two small regressions parts 1-2 both showed at the
smallest radii.** `cover.c`'s own probe-order loop guards against
probing a tiny, nothing-to-win-back query one order deeper with
`cand_rows < SC_PROBE_MIN * (st + 1) * fmax(p->split_cost, 1e-3)` (`SC_
PROBE_MIN` = 8.0) -- lowering `split_cost` to 0.1 lowers this guard's
own threshold by the same 10x, letting the loop probe deeper than
intended for exactly the tiny-radius case the guard exists to skip.
This is the mechanism behind the 1"/1' regressions round sixty-three
already saw and this round's own 1-degree regression on the uniform
corpus -- `split_cost` is a single GUC doing two jobs (the probe loop's
own cost accounting, and this unrelated early-exit guard), and tuning
it for one job detunes the other. Not fatal -- every regression found,
on both corpora, stayed small and never flipped a win into a loss --
but a real, now-identified cost of promoting `split_cost=0.1` as a
*global* default rather than something narrower.

**Promotion recommendation, not a unilateral change**: the measured
case for promoting `probe_orders=3` outright is strong (closes the one
known loss, generalizes to polygons and a different density with no
new loss anywhere, confirmed cold not just warm, zero correctness
issues across three benchmarks). The case for `split_cost=0.1`
specifically is good but not clean -- it works by coupling into an
unrelated guard, which is why the small regressions exist at all. Two
honest paths forward, not decided here:
1. Promote both as the new compiled-in defaults now -- net clearly
   positive, every regression found is small and non-reversing.
2. Decouple the probe loop's own cost accounting from `skycell.split_
   cost` first (the fix `cover.c`'s own comment already named --
   "calibrate split_cost against measurement... not tune it until this
   looks good" -- taken one step further: give the probe loop its own
   calibrated constant instead of overloading the general one), then
   promote `probe_orders` alone without needing to touch `split_cost`'s
   default or risk its other guard at all.

**STATUS**: measured, not shipped. No code, SQL, GUC default, or
opclass changed this round. `cat_cell_u`/`cat_sphere_u`/`src_u`/
`bench_centers_u`/`bench_u_x`/`bench_u_explain()` and `bench_poly_x`/
`bench_poly_explain()` left in place as reusable bench infrastructure
(same convention as `bench_cone_x`/`bench_cone_explain()` before them)
for whoever wants to keep testing density/shape sensitivity;
`cold_trial`/`cold_results`/`cold_run()` (this round's own one-off
cold-cache harness, not reusable infrastructure the way the density/
shape ones are) dropped after measurement.

## Round sixty-five: decoupling the probe loop's cost accounting from
## `split_cost` -- done; promoting the result as a default -- not
## supported once planning time is counted honestly

Round sixty-four left two paths: promote `probe_orders=3, split_cost=
0.1` outright, or decouple the probe loop's own cost accounting from
`split_cost` first and promote `probe_orders` alone. Asked to do the
second, then promote and re-validate.

**The decoupling itself.** `cover.c`'s probe loop scored each candidate
order through `rlist_score()`, which charged `p->split_cost` per cell
examined, and separately gated its own early exit with `SC_PROBE_MIN *
fmax(p->split_cost, 1e-3)`. Both uses now read a new field,
`probe_split_cost` (`sc_cover_params`, wired through a new GUC
`skycell.probe_split_cost`), leaving `split_cost` to do only the one
job it was named for: the general descent's own split/keep decision at
the `gain > p->split_cost` test, untouched by this round. `rlist_score()`
itself now takes the step cost as an explicit argument rather than
reading `p->split_cost` off the params struct, since the descent has no
other caller for it. The three run-time range-slot caches (`slot_cache`,
`poly_slot_cache`, `region_slot_cache` -- memoised per backend for
non-constant cones/polygons/regions in joins) did not invalidate on
`skycell.probe_orders` changing at all, a latent gap that mattered more
once probing was a live default candidate; fixed by adding both new
fields to all three caches' keys. `make installcheck` passes; zero
correctness mismatches across every `probe_orders`/`probe_split_cost`/
`force_order` combination tried in this round's own sweep (`bench_cone_
x`'s `act_rows` -- the true, post-recheck result -- is identical for
every variant stored against a given `qid`), confirming what decoupling
a cost knob should always confirm: it changes which covering is chosen,
never whether the chosen one is sound.

**Promotion, attempted and then reverted.** `probe_orders = 3`,
`probe_split_cost = 0.1` (the direct analogue of round sixty-three's
coupled fix, now properly isolated) was set as the compiled-in default
and run back through `bench/03_cone.sql`'s own 1,456-probe sweep. The
regression suite passed and buffers looked the same as round sixty-
three's own measurement -- flat to improved at every radius. But this
round checked **total** time (`EXPLAIN`'s `Planning Time` + `Execution
Time`), not execution time alone, because every one of the 1,456 probe
centres is a distinct `(ra, dec, radius)`, which is also the realistic
shape of a TAP service answering different users' cones: the per-
backend covering cache (`cover_cache`) cannot amortise a covering that
is never asked for twice. Round sixty-three's own published table was
execution time only; its "Planning Time" column existed in the harness
but was never summed into the headline numbers. Doing that sum changes
the conclusion:

| radius | buffers (off \| default) | **total** ms, median (off \| default) |
|---|---|---|
| 1" | 4.1 \| 4.1 | 0.051 \| 0.055 (tied) |
| 10" | 4.5 \| 4.5 | 0.055 \| 0.066 (worse) |
| 1' | 5.7 \| 5.3 | 0.068 \| 0.084 (worse) |
| 6' | 9.2 \| 8.2 | 0.097 \| 0.203 (much worse) |
| 30' | 26.1 \| 24.6 | 0.212 \| 0.588 (much worse) |
| 1 deg | 50.8 \| 49.5 | 0.743 \| 0.792 (worse) |
| 3 deg | 330.9 \| 328.2 | 6.925 \| 7.263 (worse) |

Buffers move the direction round sixty-three measured -- probing
genuinely does pick a tighter covering, at the same range count, exactly
as claimed. But the probe loop's own enumeration is not free: trying one
order finer costs roughly four times the cell classifications of the
order just tried, paid at plan time, on every single query that does not
repeat. At 6' and 30' -- the two radii round sixty-three highlighted as
the clearest wins -- that planning cost outweighs the rows saved on the
exact-test filter by 2-6x. The execution-time-only view was not wrong
about what it measured; it was incomplete about what a user pays.

**`probe_orders = 1` is better, but still not a clean, corpus-
independent win.** Swept `probe_orders` over 0-3 (same total-time
accounting): one probe, not three, recovered most of the execution-time
gain at a quarter of the enumeration cost, and a repeat pass found it
at-or-ahead of `probe_orders = 0` at every radius tried (3 deg: 1.67ms
vs 1.83ms median, a real win; 30': tied within noise). That is a better
operating point than this round's attempted default, but: (a) its own
sensitivity to `probe_split_cost` was not monotonic in one quick check
(`probe_split_cost = 1.0`, i.e. uncalibrated, scored as well or better
than `0.1` at several radii in a single pass -- not yet run to the
confidence level the rest of this round holds itself to), and (b) it has
only been checked against this round's own clustered-catalogue cone
corpus, not polygons, a different density, or cold cache the way round
sixty-four checked the coupled fix. Recommending it as a default from
one afternoon's sweep would repeat exactly the mistake this round just
found and corrected in round sixty-three's own number.

**Decision: ship the decoupling, not a new default.** `skycell.
probe_orders` stays at 0 (off) and `skycell.probe_split_cost` stays at
1.0 (matching `split_cost`'s own default, inert while probing is off).
The fix that was promoted is architectural, not numerical: the probe
loop's own cost accounting no longer shares a knob with -- and silently
detunes -- the descent's unrelated split/keep decision and its `SC_
PROBE_MIN` guard, so whoever *does* calibrate `probe_orders` for a
workload where it amortises (a repeated query shape, or an offline
covering build done once and reused) gets a clean knob to do it with,
isolated from everything else `split_cost` controls. Both GUCs remain
`PGC_USERSET` and fully documented; this is not a dead end, only a
promotion this round could not honestly support on its own evidence.

**STATUS**: shipped -- `cover.c`, `cover.h`, `skycell.c` changed
(decoupling only; no default changed), `make installcheck` passes. The
`probe_orders = 1` lead is left as a documented, not-yet-promoted
candidate for whoever next studies this with the breadth round sixty-
four gave the coupled fix (polygons, density, cold cache, `probe_split_
cost` sensitivity at proper statistical power) -- not folded into this
round's own promotion, for the same reason round sixty-one and sixty-
four both declined to fold measurement into promotion without that
breadth first.

## Round sixty-six: round thirteen's "0% empty cells" re-run on real
## Gaia DR3 -- replicates on all-sky data, does not hold where there
## are real gaps

Round thirteen ruled out a data-aware covering because 0.0% of the cells
`cover_cone_direct()` visited were empty, with the caveat that its
synthetic corpus has a 35% uniform background and so no true gaps
(handoff open item 9). The real-Gaia corpora are now rebuildable
(`bench/19_gaia_real.sh`, `bench/19_gaia_load.sh`), so the measurement was
repeated on them.

**Method, as round thirteen**: a temporary `fprintf` (not committed) at
all three `classify` call sites in `cover_cone_direct()` -- seed, child,
final -- logging `(query, order, pix, verdict)`; `EXPLAIN` only, so the
covering is computed at plan time and nothing executes; 40 distinct
centres x {30', 1 deg, 3 deg}, so the covering memo never hits; every
distinct visited cell then checked with a real `EXISTS` on its order-29
range against the queried table. PostgreSQL 16, default GUCs
(`probe_orders` as shipped). Three workloads:

- **A** `gaia_realc` (10M real DR3 positions, all-sky), centres drawn from
  its rows -- the benchmarks' own protocol;
- **B** `gaia_realc`, centres uniform on the sphere;
- **C** `gaia_fields` (1.59M, complete DR3 in 8 fields of 0.25-0.5 deg
  radius), centres drawn from its rows -- cones larger than the fields,
  so they cross genuinely unobserved sky.

| | radius | visits | distinct cells | orders | visits on empty cells | inside-verdict visits empty | final kept cells empty |
|---|---|---|---|---|---|---|---|
| A | 30' | 5,586 | 3,552 | 6-9 | 0.00% | 0.00% | 0.00% |
| A | 1 deg | 12,621 | 9,463 | 6-9 | 0.00% | 0.00% | 0.00% |
| A | 3 deg | 28,883 | 20,338 | 6-8 | 0.00% | 0.00% | 0.00% |
| B | 30' | 1,911 | 1,148 | 6-9 | 0.00% | 0.00% | 0.00% |
| B | 1 deg | 4,134 | 2,695 | 6-9 | 0.00% | 0.00% | 0.00% |
| B | 3 deg | 15,043 | 10,136 | 5-8 | 0.03% | 0.03% | 0.00% |
| C | 30' | 22,925 | 3,557 | 9-10 | 57.5% | 49.1% | 48.7% |
| C | 1 deg | 22,950 | 2,859 | 7-9 | 83.8% | 79.2% | 79.7% |
| C | 3 deg | 32,777 | 3,321 | 6-8 | 97.2% | 96.7% | 95.6% |

(Measured with statistics target 1000 on the cell index, as
REPRODUCING.md section 4 specifies. A first pass ran on 100-bucket
histograms -- the loader's `ANALYZE` inside a `DO` block ignored the
target set on the index in the same block, fixed in
`bench/19_gaia_load.sh` -- and gave the same picture: A 0.00%, B 0.04%,
C identical to the last digit, since the fields' estimates moved by under
12% and the walk there is held by its cell budget, not by density.)

**On all-sky real data round thirteen replicates.** Zero empty cells in
47,090 visits with data-drawn centres, and 0.02% with uniform centres --
the latter all at 3 deg in the sparsest sky, where the visited
order-8 cells average about 60 rows and the thinned (0.55%) sample
occasionally leaves one at zero. That is sampling, not footprint. So the
0% was not an artefact of the synthetic corpus's uniform background:
`choose_order()` stops at cells that real all-sky density keeps populated.

**Where real gaps exist, the walk does not see them.** On `gaia_fields`
half to almost all of the visits, and of the final cells kept, are empty
sky. The cost model reads density from the ANALYZE histogram, which
round thirteen already found prorates a bucket over every sub-cell
inside it and so can never report emptiness -- near a field it reads
crowded-field density and refines further (orders 9-10 at 30', against
6-9 on `gaia_realc`), spending the extra resolution on empty sky. The
premise of round thirteen's data-aware covering -- that the walk wastes
work on unoccupied cells -- is therefore true for a catalogue with a
footprint, and false only for an all-sky one.

**What this does not establish.** `gaia_fields` is an extreme footprint
(eight small disks queried with cones up to 12x their radius); a real
survey footprint (stripes, tiles, pointed fields with partial overlap)
sits somewhere between B and C, and Gaia, being all-sky, cannot supply
one. Nor was cost measured: an empty cell visited costs one `classify`
step at plan time (microseconds, round thirteen's profile), and an empty
cell kept costs a B-tree descent that finds nothing at execution, before
`merge_gaps()` may absorb it into a neighbouring range. Whether skipping
them would pay is a separate measurement against a footprint-limited
catalogue; this round only removes "0% empty" as the reason not to try.

**STATUS**: measurement only; no code changed (instrumentation reverted,
`git status` clean on `ext/src/cover.c`). Handoff open item 9 updated.

**Addendum: the paper's Baade's Window row.** `bench/19_gaia_real.sh`
queried Baade's Window at RA 18.17 deg (its RA in hours), a high-latitude
cone of 578 sources, until this branch moved it to 270.904, -30.035
(225,522). The paper's estimator table (`tab:estimator`) is not produced by
any script in the repo, so it was reconstructed: rho-hat from
`skycell_cover_info()` against the counted density, radii 0.01-1 deg,
statistics target 1000, three independent `ANALYZE` samples.

- On `gaia_realc` the reconstruction reproduces the paper's other rows
  (omega Cen 0.06-0.08 vs 0.08; LMC 0.74-0.84 vs 0.76; Galactic centre
  3.71-3.81 vs 3.74), so that is the table it was measured on. Baade
  gives median 0.90-0.94 (range 0.44-1.16) at the corrected position and
  0.71-0.74 (0.55-1.03) at the old one; the paper's 0.80 (0.68-1.01) sits
  between and does not identify either. Both are ordinary-sky values, so
  the row's role in the paper -- an accurate field, not a failure case --
  holds at either position.
- On the full-density fields the old position cannot produce the second
  referee response's figures: its 578 sources are invisible to the
  histogram (rho-hat/rho = 0.00 at every radius), whereas the corrected
  field gives 0.96 at 0.05 deg and 0.23 at 0.2 deg, matching the response's
  "0.97-1.05 at <= 0.05 deg ... collapses to 0.15-0.23 at 0.2 deg". With the
  1.6M row count, this says the published corpus used the correct
  position and only the script had drifted.

No change to the paper is needed. The table remains unreproducible from
the repo until its measurement is scripted.
