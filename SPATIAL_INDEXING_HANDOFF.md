# Spatial indexing in skycell: a handoff summary

This document exists so a fresh reader — human or AI — can understand the
whole landscape of spatial-indexing work in this repository without
re-deriving it from source or replaying ~15 rounds of experiments. It is a
map, not the territory: `GIST_REGION_DESIGN.md` (1,500+ lines) has the full
narrative, exact numbers, and code-level reasoning for every round mentioned
here, organized as "Round one" through "Round fifteen" in the order they
happened. Read this file first to orient, then go to `GIST_REGION_DESIGN.md`
for whichever round is relevant to what you're about to try, so you don't
repeat something already tried and measured.

## 1. What skycell is, and what's actually being tested

skycell is a PostgreSQL extension for indexed astronomical sky queries
(cone search, cross-match, footprint containment) on the sphere, built
around HEALPix. Its headline mechanism, already shipped and not
experimental, is: index a point catalogue's own HEALPix cell id
(`skycell_ang2cell(ra, dec)`) with a plain B-tree, and rewrite spatial
predicates at *planning time* (a `SupportRequestSimplify` hook) into a
small number of `cell BETWEEN lo AND hi` ranges — a cost-based covering
computed from a density map built from `ANALYZE`'s own histogram
(`ext/src/cover.c`). No custom index access method is needed for that path
at all; it rides on top of PostgreSQL's ordinary B-tree.

Four comparison points recur throughout every round below:

- **pgSphere**: a mature, independent PostgreSQL extension with its own
  `spoint`/`scircle`/`spoly` types and native GiST R-tree indexes. The
  "incumbent" skycell is trying to beat.
- **Q3C**: a cube-quad-tree-based extension, generally the slowest of the
  real competitors in this repo's own numbers, though see the caveat below
  about `q3c_radial_query`'s specific implementation.
- **The MOC-ranges recipe**: a *manual*, hand-maintained skycell recipe
  (`skycell_region_moc_ranges`) for region-region joins — explode both
  sides into `[lo,hi]` HEALPix-cell ranges via a MOC decomposition and join
  on a plain `int8range` GiST. Useful as "the B-tree-shaped approach applied
  to region-region," which turns out to behave very differently from the
  point-indexing case (see §3).
- **Brute force**: the honest sequential-scan baseline for anything with no
  index at all yet, used to establish correctness (exact match count) before
  any speed claim is trusted.

**The specific claim under test** (from `paper/response-to-referee-2.md`,
answering the referee's Q4): skycell's planning-time covering computation is
a real, *growing-with-scale* cost — 3% of query time at 10M rows, 13% at
50M — and "an SP-GiST opclass over the HEALPix cell hierarchy" was proposed,
*not yet built*, as a way to remove it by turning that computation into a
genuine index descent (planning cost near zero, like pgSphere's single
index qual, instead of a covering recomputed from scratch every query).
Building and testing that proposal is what most of the rounds below (ten
through thirteen) actually did.

## 2. Two independent index families — don't conflate them

This repo contains **two separate custom index opclasses**, answering two
different predicate shapes, built and evaluated independently. Mixing up
their results is the single easiest way to misread this history:

| | indexes | predicates | file | status |
|---|---|---|---|---|
| **Region GiST** | the *region* column (`skyregion`) | `&&`, `@>`(region,point), `@>`(region,region), `<@`(region,region) | `ext/src/gist_region.c` | shipped, default opclass for `skyregion` |
| **Point SP-GiST** | the *point* column (`skypos`) | `<@`(point,region) | `ext/src/spgist_region.c` | experimental, correctness-verified, **not recommended** — see §4 |

The region GiST opclass answers "many candidate regions/footprints, probe
against one or a few" — cross-matching a footprint catalogue, `region &&
region` joins. The point SP-GiST opclass answers "one query region, many
candidate points" — cone search against a point catalogue, the same
workload the B-tree-rewrite path already answers without any custom index
at all. **The point SP-GiST opclass and the B-tree-rewrite path compete for
the exact same query shape**; the region GiST opclass has no B-tree-rewrite
equivalent to compete with (region-region joins have no single sortable
axis — see round two's diagnosis in §3).

## 3. Region GiST opclass (`gist_region.c`): round-by-round

**Design**: a GiST key is a "multi-cap" — up to `MAX_SUBCAPS` (4, currently)
bounding spherical caps per region, playing the same role `box2df` plays for
PostGIS or `spherekey` for pgSphere's own types. A cone gets one exact cap;
a polygon is decomposed via the extension's own MOC builder
(`moc_for_region`) into up to `MAX_SUBCAPS` HEALPix cells. `consistent()`
checks all-pairs sub-cap overlap (O(MAX_SUBCAPS²)); `picksplit` is
R*-tree-style (margin/overlap search on the overall-cap centroid,
final keys built from the full multi-cap union). Explicitly modeled on
PostGIS's own bounding-key idea, not invented from scratch.

| round | what was tried | result |
|---|---|---|
| 1 | Single bounding cap per region, Quadratic split + cached `consistent()` | Closed most of the gap to pgSphere at 5,000 rows (2-3x → 1.2-1.4x) |
| 2 | R*-tree-style split (Beckmann et al.) replacing Quadratic, same single-cap key | Flat at 5,000 rows (no measurable win over Quadratic). At 50,000 rows/500 probes: gap to pgSphere **widens to 5.3x**, and even the manual MOC-ranges recipe ends up faster in absolute terms. `EXPLAIN (BUFFERS)` diagnosis: too many internal/leaf pages have overlapping caps — **the key itself, not the split algorithm, is the bottleneck**. Kept R*-tree split anyway (sounder, cheaper to build) even though it didn't move this number. |
| 3 | Multi-cap key: up to 4 sub-caps/region instead of 1 | **The headline win.** 5,000 rows: ~3.7x *faster* than pgSphere. 50,000 rows: ~1.2x slower (down from 5.3x). Beats the MOC-ranges recipe at both scales. |
| 4 | Second strategy: `@>`(region,point) | ~110-160x faster than brute force; gap to pgSphere widens 2x→5.5x with scale, for a checked reason: pgSphere's native point-in-shape test is cheaper than its shape-shape test, so pgSphere's own `<@` pulls further ahead of its own `&&` than this opclass's `<@` does of its own. |
| 5 | Sub-cap-aware `picksplit` cost functions (not just final keys) | Real, modest win: ~10-15% fewer buffer visits at both scales, both strategies (measured via `EXPLAIN BUFFERS`, since wall-clock was too noisy to trust at this query speed — a recurring theme). |
| 6 | Third strategy: `@>`(region,region) ("does this region contain that one") | Reuses `&&`'s own overlap test rather than a dedicated one — deliberately: "A contains B" implies "A and B overlap," a valid necessary condition; a tighter per-sub-cap test would be a *sufficient*, not necessary, condition and risks a false negative with no recheck to catch it. Real win over brute force (~250-390x); gap to pgSphere **widens with scale** (1.5x at 5,000 rows → 4.9x at 50,000). |
| 7 | Fourth strategy: `<@`(region,region), the commutator direction | Needs no new pruning logic (same necessary condition, direction-agnostic) — just a new strategy number and commutator wiring (which also backfills `@>`'s previously-missing commutator). **Wins outright against pgSphere at both scales** (~1.6x and ~1.5x faster) — the one region-region direction where this opclass beats pgSphere at scale, not just at 5,000 rows. |
| 8 | Automatic GIN alternative for `@>`(region,point) | (see `GIST_REGION_DESIGN.md` for detail not repeated here) |
| 9 | *(GiST-side round nine is the variable-length key attempt — see §5, it targeted the **point** predicate via the GiST region opclass infrastructure and was reverted; documented in full under "Round nine" in the design doc)* | reverted |
| 14 | Dedicated `@>`(region,region) pruning test: an area-monotonicity reject (`A ⊇ B ⟹ area(A) ≥ area(B)`) on top of round six's shared overlap test, using `sc_region`'s own exact area field | Sound, correctness-verified at 5,000 and 50,000 rows. **A/B'd directly against pre-change code: noise-level identical timing.** `EXPLAIN BUFFERS` on a fixture built specifically to trigger it showed why — of ~2,135 buffer touches, it eliminated exactly *one* false candidate. The overlap test already rejects almost everything the area test would additionally catch; on realistic data the two failure modes are correlated (too-small-to-contain is usually also too-far-to-overlap), not independent. **Reverted.** |
| 15 | `MAX_SUBCAPS`: 4 → 8 | Correctness clean. Buffer counts *improve* (14-18% fewer across `&&`/`@>`, 3% for `<@`) — but repeated wall-clock runs are *consistently worse* (`&&` ~121ms→~153ms, `@>` ~78ms→~89ms). Root cause: the overlap test is O(MAX_SUBCAPS²); doubling the cap count quadruples that cost (16→64 pairs/comparison), which outgrows the pruning benefit since this corpus is fully cache-resident (buffer count here tracks tuple-visits, not I/O). Index also grew 69% (16MB→27MB). **Reverted.** Not tried: 16 (the O(k²) trend was already consistently negative and would only get worse). |

**Where this leaves the region GiST opclass**: `&&` and `<@` are solid wins
(the latter outright beats pgSphere at scale); `@>`(region,region) has a
persistent, unclosed, *widening* gap to pgSphere that two different
targeted fixes (round fourteen's area check, round fifteen's sharper key)
both failed to close. Round two's original diagnosis — "too many
internal/leaf pages have overlapping bounding caps" — is still the
standing, unaddressed explanation. See §6 for what's untried.

## 4. Point SP-GiST opclass (`spgist_region.c`): round-by-round

**Design**: a PATRICIA trie over the HEALPix NESTED pixel hierarchy — one
inner tuple per branch point, prefix-compressed (an explicit `(order,
width, pix)` per tuple, not one order per tree level), built via a
from-scratch longest-common-prefix search at every split (never trusting
SP-GiST's own hop-count `level`, which provably drifts from true HEALPix
order once splits accumulate). Indexes `skypos`, answers `skypos <@
skyregion` via real index descent (`inner_consistent()` prunes subtrees
against the query region using the same `sc_region_classify()` cover.c
already uses for the B-tree covering).

| round | what was tried | result |
|---|---|---|
| 9 | *(region GiST, not point SP-GiST, but same "sharper/leaner key" family of idea)* A variable-length GiST key shrinking a plain circle's stored size to match pgSphere's own `scircle` | Reverted: even under a genuinely cold cache, the CPU cost of decoding a variable-length key outweighed the I/O saved by smaller pages, in every regime tested. Directly informs round fifteen's finding fifteen rounds later: making a key operation cheaper isn't automatic just because the key gets smaller/bigger — measure the actual tradeoff. |
| 10 | Build the SP-GiST opclass from scratch. Also: direct empirical comparison against PostGIS's own real GiST/SP-GiST implementations (`spgist_geography_ops_nd`, an octree over 3D Cartesian coordinates) | PostGIS's GiST was "genuinely competitive" with skycell's region GiST on region-contains-point (~17-24ms vs ~14-15ms at 5,000 footprints). PostGIS's SP-GiST on the *same* data was 20-25x slower than its own GiST (~400-435ms) — motivated building a HEALPix-native SP-GiST rather than assuming a generic Cartesian octree would transfer. New opclass correctness-verified at 2,923/50,000 rows. |
| 11 | Test at 10M-row scale (the scale the whole planning-cost argument is about) | Found and fixed **three real correctness bugs**, all invisible below ~500K rows / not dense enough to trigger: (1) trusting SP-GiST's `in->level` as an order proxy — it drifts, in both directions, once `spgSplitTuple` is in play; (2) a split's upper tuple created with only one node — the SP-GiST core marks any fresh `nNodes==1` tuple all-the-same and refuses `spgAddNode` against it, even for splits; (3) the SP-GiST core itself synthesizes placeholder all-the-same inner tuples the opclass never asked for. Fixed by (1) always searching from scratch at order 0 in `picksplit`, never trusting `level`; (2) giving a split's upper tuple both labels atomically; (3) checking `allTheSame` first, unconditionally, in both `choose()` and `inner_consistent()`. Re-verified: 10M rows × 80 regions, 0 mismatches. **The core empirical result**: the referee-response hypothesis is *confirmed* — planning cost really does drop from 60-66% to 9-13% of query time — but it **doesn't translate into a win**: execution cost (tree descent) is higher than the B-tree covering it was meant to replace, at every radius tested (skycell_spg 2.9-3.9x slower than pgSphere at small radii, narrowing to ~1.4x at 3°, where skycell's own B-tree also starts losing to pgSphere — see §6). |
| 12 | Wide-radix splitting: combine several HEALPix orders into one node-selection decision, to shrink tree depth | Exposed a **fourth correctness bug** while implementing: `split_width()` derived a tuple's decision width from `order` alone, but `spgSplitTuple`'s upper tuple can be capped narrower than that by the old tuple's own established order, with nothing distinguishing a capped tuple from a normal one later — 2,869 false negatives at 50,000 rows before the fix. Fixed by storing the actual width explicitly per tuple (never re-deriving it). Also fixed a **latent bit-packing bug** found while designing that fix: the old `(order, pix)` int8 packing capped `pix` at 58 bits, but a full HEALPix id needs up to 62 at deep orders — silent truncation, never triggered by any prior shallow reproduction. Fixed by switching the prefix to a small `bytea` (`order, width, pix` verbatim, no bit-squeezing). **Once correctness was solid, the performance measurement was negative**: wide-radix (`SPLIT_WIDTH=3`) made buffer touches *worse*, not better — 1.1-3.4x more, growing with query radius. Likely cause: a PATRICIA trie's branching already tracks exactly where data disagrees; forcing every decision to span extra orders regardless inflates node count past what the data needs, and that extra breadth costs more for boundary-crossing range queries than the shallower depth saves. **`SPLIT_WIDTH` left at 1** (single-order splits); the bytea/explicit-width infrastructure kept since it's what makes revisiting wide splitting safe later. |
| 13 | A coarse, execution-time covering index: reuse the tree (or a dedicated shallow structure) to answer "which coarse cell ranges does this region touch" as an index descent, replacing `cover.c`'s from-scratch planning-time recursive walk, feeding the same cheap B-tree range scan skycell already uses | **Ruled out before being built**, via two measurement passes. First, profiled the actual planning-time cost directly (in-process timing, `perf` unavailable): negligible under 1 arcminute (~3μs against ~32-35μs total planning time — bottleneck is elsewhere, generic planner overhead), genuinely dominant at 30'+ (60-140μs, 60-75% of measured planning time) — so the idea could only possibly help at larger radii. Second, tested its core premise directly: does the covering walk waste work on cells with no real data? Measured against *exact* table occupancy (not the ANALYZE-histogram density map, which turned out to structurally never report zero due to equi-depth-bucket proration — a false lead caught by reading its own implementation before trusting the number): **0.0% of 16,318 visited cells were empty.** `choose_order()`'s own cost model already stops refining once cells are reasonably populated, and this synthetic corpus has a substantial uniform background component with no true coverage gaps — so there's no sparsity for a data-aware structure to exploit, at least on this corpus. |

**Where this leaves the point SP-GiST opclass**: correctness-verified and
architecturally validated (the planning-cost hypothesis is confirmed), but
a net loss against the B-tree-rewrite path it was meant to replace, and
three independent, targeted attempts to close the execution-cost gap
(rounds nine's underlying lesson, twelve, thirteen) all failed for
different, specific, measured reasons. **Not recommended for use**; kept in
the repo as a correctness-verified, well-documented negative result.

## 5. The other, separate open gap: skycell's own B-tree path vs. pgSphere
## at large radii

This is **not** about either custom opclass — it's about the *already-
shipped*, non-experimental B-tree-rewrite path, and it's a live, unclosed
question nobody has attacked directly yet. Round eleven's own 10M-row cone
search numbers:

| radius | pgSphere | skycell (B-tree) | winner |
|---|---|---|---|
| 1″ | 0.070ms | 0.061ms | skycell |
| 1′ | 0.070ms | 0.064ms | skycell |
| 30′ | 0.363ms | 0.293ms | skycell |
| 1° | 1.026ms | 0.661ms | skycell |
| 3° | 6.314ms | 9.976ms | **pgSphere** |

skycell wins at every radius except the largest tested. The likely mechanism
(not yet directly attacked or measured further): the B-tree approach's
advantage is locality-preserving ranges being cheap to scan; at large query
sizes the covering needs more/coarser ranges and the scan becomes more
scattered, while pgSphere's balanced GiST R-tree keeps doing compact
descents regardless of query size.

**Round sixteen dug into this directly** (prompted by an external proposal
to build a new "adaptive density-aware covering" -- worth reading in full in
`GIST_REGION_DESIGN.md`, summarized here). First finding, before any code:
the proposed mechanism already exists. `cover.c`'s `sc_cover_compute()` has
always had a `refine:` heap-based path (currently used only for polygons,
or when the cone fast path bails) that already does real per-cell
adaptive KEEP-vs-SPLIT decisions using the density map -- the *default*
cone path (`cover_cone_direct()`, used at every radius including 3°)
instead picks one global target order and subdivides uniformly. Testing
whether routing cones through the existing adaptive path already closes
the gap (10M rows, 40 real 3° queries, `EXPLAIN (ANALYZE, BUFFERS)`) found:
a wash on 39 of 40 queries, but one query (an exceptional density overlap,
137,535 true matches) was 5-7x worse under the adaptive path (62-71ms ->
393-436ms), from generating 50 ranges against the direct path's 21 for a
similar candidate volume.

Traced with `cover.c`'s own pre-existing `SC_COVER_TRACE` facility directly
against the failing query and found a precise, real bug: of 362 split
decisions, 85 (23%) excluded *zero* false-positive area yet were accepted
anyway, through a `delta <= 0 && pot > range_cost` fallback ("no extra
ranges now, worth looking deeper") that scores a cell by expected row count
alone, with no signal for whether real boundary uncertainty remains --
inside a dense cluster's interior, where density stays high at every order,
this cascades many levels deep for zero benefit. A targeted fix (cap
consecutive "no progress" levels a lineage is allowed to chase on density
alone) verified the mechanism exactly (zero-gain splits and range count
both dropped substantially for the failing query) but did **not** validate
as a net win: two *nearby* query centers that were fine before the fix
regressed badly after it (25-26ms -> 287-298ms), and splitting that
regression apart (parallelism forced off for a clean read) surfaced a
second, separate, arguably bigger effect entirely independent of the
density-fallback bug: **the direct path's covering shape parallelizes
beautifully (4x speedup from PostgreSQL's parallel bitmap heap scan) while
the adaptive path's -- fixed or not -- barely parallelizes at all (~6%)**.
That's about how ranges of a given shape divide across parallel workers,
not about false-positive count or range count, and it was invisible in the
39-of-40-queries-are-fine read above because that never isolated parallel
from non-parallel execution. Parked: the fix was reverted (not validated,
and never checked against the adaptive path's actual existing callers --
polygon coverings -- at all), but both findings (the precise density-vs-
boundary-uncertainty conflation, and the parallel-scan shape sensitivity)
are now on record.

**Round seventeen ran the parallel-scan sensitivity down to a precise,
general cause: PostgreSQL's own JIT compilation, not covering quality at
all.** A controlled experiment (one contiguous ~200,000-row cell range,
split into 2 vs. 50 equal OR'd sub-ranges -- identical data, only range
*count* differs) found a 9.3x gap under default (parallel, JIT-on)
execution that vanished entirely (27.2ms vs. 26.6ms) with `SET jit = off`.
The mechanism: PostgreSQL JIT-compiles the `Recheck Cond`/`Filter`
expression once plan cost crosses `jit_above_cost`, that expression is one
OR-branch per range, and compilation cost scales steeply with branch count
(21ms total for 2 branches, 264ms+ for 50, even serially -- 83% of that
query's whole execution time). Under parallel execution specifically,
*every worker independently JIT-compiles its own copy*, so an already-real
serial cost gets paid two or three times over, outweighing whatever the
extra workers save on the actual scan. This is not specific to either
covering algorithm or round sixteen's density-fallback bug -- it's generic
PostgreSQL behavior any sufficiently wide OR'd-range predicate would hit,
**including the shipped `cover_cone_direct()` path**, at whatever range
count and worker count combination crosses the threshold. `cover.c`'s own
cost model (`range_cost`) has no term for this at all -- it prices a range
by index-descent cost, never by what the executor pays to JIT-compile and
multiply across parallel workers. Untested: whether `p->max_ranges=64`'s
existing budget already routinely produces enough ranges at real
production radii to trigger this on the default, shipped path today.

## 6. Open questions and concrete untried directions

In rough order of how well-scoped/promising they seem from this history,
not in priority order — pick what matches the actual goal:

1. **Give `cover.c`'s cost model a JIT-aware term** (§5, round seventeen).
   Round sixteen's parallel-scan sensitivity is now precisely explained:
   PostgreSQL JIT-compiles the OR'd range predicate once plan cost crosses
   `jit_above_cost`, that compile cost scales steeply with range count, and
   parallel workers each pay it independently, so it multiplies rather than
   amortizes. `range_cost` (the covering's own per-range price, calibrated
   from B-tree descent cost) has no term for this at all. Two concrete,
   untried options: (a) add a range-count-and-expected-parallel-worker-count
   term to the cost model so the covering computation itself prices in
   execution-side JIT cost, not just descent cost; (b) check whether
   raising `jit_above_cost`/`jit_inline_above_cost` for skycell's own
   wide-OR query shape, or disabling JIT for it specifically, is a cheap
   win with no covering-algorithm change needed at all.
2. **Check whether this is already live on the shipped path, not just the
   reverted experiment.** `cover_cone_direct()` (the default, unchanged by
   round sixteen) can produce up to `p->max_ranges=64` ranges. Nobody has
   checked whether real production radii/densities already routinely
   generate enough ranges, combined with default parallelism, to pay this
   same JIT tax today. If so, this is not a hypothetical concern from a
   reverted experiment — it is a live, uninvestigated cost on the
   already-shipped B-tree path.
3. **Recalibrate `SC_COVER_MAX_DRY_STREAK`** (round sixteen's reverted fix,
   `cover.c`) against a broad query set, not one failing query -- the
   underlying bug (density alone justifying unbounded speculative
   refinement in a cell with no boundary nearby) is real and precisely
   diagnosed; the specific cap chosen (2) demonstrably regressed a nearby
   case. Also untested against the adaptive path's actual existing callers
   (polygon coverings) at all.
4. **A sub-quadratic overlap test for the region GiST multi-cap key**
   (§3, round fifteen's own conclusion). The O(MAX_SUBCAPS²) all-pairs
   check is what capped the profitable cap count at ~4. A key whose own
   sub-caps are spatially sorted/indexed (even something as simple as
   sorting by one coordinate and using it to skip non-overlapping pairs)
   could let MAX_SUBCAPS grow without paying the full quadratic cost —
   untried.
5. **A `min_area` field for `CONTAINED_BY_REGION` (`<@`)**, the mirror of
   round fourteen's reverted `max_area` idea — except `<@` already wins
   against pgSphere at both scales tested (round seven), so this is lower
   priority; worth checking only if a future benchmark finds `<@` losing
   somewhere round seven didn't test.
6. **A bulk-loaded, statically-packed structure for the point predicate**,
   instead of the point SP-GiST's incremental `choose`/`picksplit`
   construction (flagged, not attempted, when round twelve/thirteen closed
   out — see the "so, the conclusion is that SP-GiST is a dead end?"
   exchange in this session's own transcript, or just the closing
   paragraph of round thirteen). Bigger undertaking, uncertain payoff, and
   risks converging on "just reimplement the B-tree as an index AM" —
   flagged as the one idea with a real mechanism behind it that hasn't
   been measured, not as something expected to obviously win.
7. **Real survey-footprint sparsity** for the point SP-GiST's covering-walk
   idea (round thirteen's own caveat): the corpus used throughout this
   whole investigation is synthetic with a substantial uniform-sky
   component, so it has no true coverage gaps. A real archive's actual
   observed-footprint structure (genuine unobserved sky, not just
   low-density sky) might make round thirteen's "0% empty cells" result
   corpus-specific rather than general — untested, no corpus available in
   this repo to test it with.
8. **`skycell.probe_orders`** (`cover.c`): an existing, off-by-default GUC
   for scoring several candidate covering orders instead of trusting the
   closed-form choice — its own code comment says forcing a finer order
   directly measures a real 30-48% win at 6'-30' that the *scored* probe
   version doesn't yet capture, because `split_cost`'s calibration
   (1 row/cell examined) isn't measured the way `range_cost` now is. A
   real, already-diagnosed, already-partially-measured gain sitting
   uncollected — closing it is calibration work, not new design.

## 7. Where things stand right now (as of this document)

- Region GiST opclass: shipped as-is (`MAX_SUBCAPS=4`, shared overlap test
  for all containment strategies). Working tree matches this exactly —
  rounds fourteen and fifteen's experimental changes were both reverted
  after measurement.
- Point SP-GiST opclass: shipped as a correctness-verified, documented
  negative result (`SPLIT_WIDTH=1`, effectively single-order splits; the
  bytea-prefix/explicit-width fix from round twelve is real and kept even
  though wide splitting itself isn't used).
- B-tree covering path (`cover.c`): shipped as-is (`p->direct=1`, the
  non-adaptive cone fast path, unchanged for every radius). Round sixteen's
  dry-streak fix to the adaptive fallback path was reverted, not validated
  as a net win — see §5.
- `GIST_REGION_DESIGN.md` is the source of truth for exact numbers, code
  reasoning, and anything this summary compressed or left out.
