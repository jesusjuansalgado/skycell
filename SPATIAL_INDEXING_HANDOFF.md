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
| **Region GiST (multi-cap)** | the *region* column (`skyregion`) | `&&`, `@>`(region,point), `@>`(region,region), `<@`(region,region) | `ext/src/gist_region.c` | shipped, default opclass for `skyregion` |
| **Region GiST (box)** | the *region* column (`skyregion`), alternate opclass | same four strategies | `ext/src/gist_region_box.c` | experimental, correctness-verified, `skyregion_box_gist_ops`, non-default — beats the multi-cap opclass on every strategy; see round forty-two in §3 |
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
| 20 | Farthest-point seeding for `merge_caps_greedy` (the greedy cluster-merge behind `union()`/`penalty()`), motivated by reading pgSphere's actual GiST source: its key is a plain 3D axis-aligned box, unioned by exact per-axis min/max — lossless at every tree level, unlike skycell's own capped-cluster merge, which its own header comment already calls "not a globally optimal clustering." Replacing its arbitrary first-k seeding with farthest-point (greedy k-center) selection looked like a small, safe tightening. | **Regression, not a win, on both strategies**: true A/B on identical 50,000-row corpus/probes, index rebuilt fresh under each binary — `@>` went 80-83ms → 225-273ms (2.9x worse), `<@` went from *beating* pgSphere 1.4x (160-174ms vs 232-252ms) to *losing* to it 1.2x (281-286ms). **Reverted.** Hypothesized cause: `merge_caps_greedy` backs `penalty()`, called on every incremental insert to choose subtree placement, not just to polish an already-decided split — farthest-point seeding is a better one-shot clustering but is also more outlier-sensitive, hypothesized to make the waste estimate a noisier placement signal across thousands of incremental calls than the first-k seeding it replaced. |
| 21 | Test that round-twenty hypothesis directly: stop `penalty()` calling `merge_caps_greedy` at all, score placement by `overall`-cap area growth instead (a plain two-cap union, exact, O(1), already computed, already used by picksplit's axis-sort) — no seeding, no clustering, fully deterministic. | **Worse than round twenty, not better — falsifies round twenty's own hypothesis.** Same true-A/B discipline, same-session back-to-back this time (see note below): `@>` 87-88ms → 243-256ms (2.9x worse); `<@` 172-197ms (beats pgSphere 1.3-1.5x) → 435-446ms (now *loses* 1.08-1.11x). **Reverted.** It wasn't the seeding's noise specifically — a simpler, deterministic, non-clustering metric hurt *more*. Real explanation: `overall` is a single bounding cap, exactly round one's original too-coarse key design; using it for placement throws away the sub-cap shape information round three's redesign exists to capture, and `consistent()`'s pruning at a badly-placed leaf can't undo a bad placement decision made further up the tree. Placement quality apparently needs the same rich information pruning does — simplifying it, or reseeding it, both hurt so far. |

| 22 | The untried direction rounds twenty/twenty-one both pointed at: farthest-point seeding for the sub-cap merge, but scoped to *only* `multicap_union_many()` (the path building the key `consistent()` actually reads) via a new `merge_caps_greedy_fp()`, while `multicap_penalty()` keeps calling the original, untouched `merge_caps_greedy()`. | **A real win — kept, not reverted, the first one in this thread.** Same-database/same-session A/B throughout (learned directly from round twenty-one's own methodological note). At 50,000 rows/500 probes: `@>` 116-117ms → **51-52ms (2.2-2.3x faster)**; `<@` 163-172ms (beat pgSphere 1.4-1.5x) → **134-136ms (beats pgSphere 1.76-1.91x)**. At 5,000 rows/200 probes: flat to modestly better, no regression (`&&` 7.25-7.30ms→6.56-6.74ms; `@>`/`<@` within noise). Correctness verified at both scales, all four strategies (brute-force count matches GiST and pgSphere exactly everywhere). The scale-dependence (win at 50k, neutral at 5k) makes sense: fewer rows means a shallower tree with fewer internal-node unions for a tighter merge to compound across — the mirror image of round two's single-cap gap "widening with scale." `@>`'s gap to pgSphere narrows from ~5x to ~2.3-2.9x — not closed, but the first real progress on it since round two. |

| 23 | Round twenty-two's own flagged follow-up: `merge_caps_greedy_fp()`'s single greedy sweep is order-sensitive by construction, so add a "second-look" refinement pass afterward — safe (unlike anything touching `penalty()`) since this only feeds the stored key. Approximates true Lloyd's/k-means reassignment (which would need a cluster's union with a cap *removed*, expensive since caps don't subtract) by comparing each cap's recorded join-time cost against the cost of joining a different, now-fully-formed cluster, all decided against one frozen snapshot and applied in one batch. | **Correctness held, but not a clear enough win to keep.** Wall-clock was contradictory — `@>` looked ~1.8x faster, `<@` looked ~1.6x *slower* — while pgSphere's own unmodified numbers shifted by a similar magnitude between the two passes, pointing at ambient noise (a second concrete instance of round twenty-one's own caution) rather than a real effect. Switched to `EXPLAIN (ANALYZE, BUFFERS)`: a real but modest tightening, `@>` 26,202→24,870 buffers (-5.1%), `<@` 68,976→67,954 buffers (-1.5%) — unlike round fifteen's `MAX_SUBCAPS` increase (which paid its extra cost per *query*), this round's extra cost (a second O(n·MAX_SUBCAPS) pass plus an O(n) rebuild) lands per index-build `union()` call, in principle a cheaper place to spend cycles — but a 1-5% buffer reduction with no confident wall-clock signal isn't this file's bar. **Reverted**, working tree and installed extension back to round twenty-two's shipped code. |

| 24 | The remaining untried idea from reading pgSphere's `gist.c`: its box union is exact/lossless at every tree level, unlike skycell's capped sub-cap merge. Added a `GistBox3D` field to `GistMultiCap`, populated via a closed-form exact spherical-cap bounding box, unioned by plain min/max (always exact, no clustering). Scoped narrowly per rounds twenty/twenty-one's evidence: feeds only `skyregion_gist_picksplit()`'s cost metric (Euclidean box volume, replacing the sub-cap-area-sum `multicap_total_area()` for axis and split-point choice) — never `penalty()`, never `consistent()`. | **Correctness held; a clear regression on buffers, not a win.** Same-database comparison: `@>` 25,874→29,725 buffers (**+14.9% worse**), `<@` 68,365→74,304 buffers (**+8.7% worse**). **Reverted.** The theoretical premise (exact composition beats approximate) didn't survive contact with the actual proxy used: Euclidean box volume `(hi.x-lo.x)(hi.y-lo.y)(hi.z-lo.z)` isn't a good stand-in for true spherical coverage the way `cap_area_proxy()`'s `1-cos(radius)` is — a box near a pole and one of similar true angular coverage near the equator can have very different volumes purely from where their coordinates sit in `[-1,1]`, and a thin band-shaped region's box can be near-zero along one axis while covering real angular extent in the others. pgSphere's own box union being exact isn't by itself what makes it work — pgSphere's simpler, more compact native shapes are a corpus where Euclidean volume happens to track true coverage reasonably well; skycell's polygon-decomposed, scattered sub-caps are not. A genuine spherical-area proxy for the box (not Euclidean volume) is the natural next attempt, not tried here. |

| 25 | Round twenty-two measured `@>`/`<@`(region,region) directly (what rounds twenty/twenty-one's regressions were about) but never re-checked `&&` or `@>`(region,point) against pgSphere under the shipped fix — even though both share the same `multicap_union_many()` path that changed. Filled the gap: fresh corpus, both scales, current shipped code. | **A genuinely new finding, not something round twenty-two claimed.** `&&` at 50,000 rows was skycell's one lingering loss to pgSphere since round three (~1.2x slower, untouched by rounds four through twenty-one) — round twenty-two's fix flipped it too: 63.70-65.07ms vs pgSphere's 83.23-98.76ms, **~1.28-1.55x faster**, an apparent side effect of tightening the same shared key-building path every strategy's `consistent()` prunes against. At 5,000 rows `&&` is unchanged (~3.9-4.1x faster, matches round three). `@>`(region,point) shows no comparable improvement — 73.64-75.38ms vs pgSphere's 16.30-18.24ms at 50,000 rows, **~4.0-4.6x slower**, consistent with round four's own diagnosis being a different mechanism (pgSphere's native point-in-shape test is inherently cheaper than a sub-cap loop) that round twenty-two's key-tightness fix doesn't reach. Correctness verified at both scales, both strategies. |

| 26 | Round fifteen's own conclusion, the last untried item: an O(MAX_SUBCAPS²) all-pairs overlap test is what capped the profitable sub-cap count at 4. Sorted each key's valid sub-cap prefix by centre x at build time (`sort_subcaps_by_cx()`), then rewrote `multicap_overlaps()`/`multicap_overlap_amount()` to skip pairs a real geometric bound proves can't overlap (a coordinate difference exceeding the chord of the radius sum) instead of checking all pairs. | **Correctness held at both scales, all four strategies — but buffers got consistently worse, not better**: `&&` +4.5%, `@>`(point) +4.6%, `@>`(region,region) +3.0%, `<@`(region,region) +2.0%, all measured same-database. **Reverted.** Not a flaw in the pruning bound itself (proven exact, not approximate — a skipped pair genuinely never contributes) but an unintended coupling with round twenty-two's `merge_caps_greedy_fp()`: its farthest-point seeding is deliberately order-sensitive (`caps[0]` becomes the first seed), and `multicap_union_many()` flattens `entries[i]->sub[j]` in entry order to build the next level's input — so sorting a stored key's `sub[]` for query-time benefit silently reseeds every future clustering decision that reads it back, producing a different (not wrong, just worse) tree shape. |

| 27 | `@>`(region,point) sat untouched since round four's own diagnosis (a wide, scale-independent gap: "pgSphere's cheap native point-in-shape test pulls further ahead... than pgSphere's own `&&` does of its own"), confirmed unmoved by round twenty-two's key-tightening fix (round twenty-five) — a different lever entirely: per-comparison *cost*, not tree shape. `multicap_contains_point()` tested each sub-cap via `cap_overlaps(cap, cap_make(p,0.0))`, calling `sc_angle()` (cross product + `sqrt` + `atan2`) to get a true angle for comparison against a radius *sum* — but a point is a zero-radius cap, so that sum is just the cap's own radius, and `angle <= radius` reduces exactly to `dot(centre,point) >= cos(radius)` — one dot product, one cosine, no cross product/`sqrt`/`atan2`. Added `cap_contains_point()` implementing that, used only inside `multicap_contains_point()` — `cap_overlaps()` itself and every seeding/union/penalty function untouched. | **A real, consistent, kept win — the largest single-round improvement to this strategy since round four.** Correctness held at both scales; buffers near-identical (+2.0% at 50,000 rows, expected GiST build noise since no `consistent()` boolean answer can change). Wall-clock, same-database same-session: 5,000 rows 5.10-6.21ms→3.06-3.27ms (~1.6-1.9x faster, gap to pgSphere ~1.7-2.1x→~1.1-1.2x, nearly closed); 50,000 rows 83-85ms→47-50ms (~1.7-1.8x faster, gap ~4.0-4.6x→~3.4-3.9x). Not fully closed — round twenty-eight (below) confirms the tree-tightness side is already accounted for, so the remainder is more likely structural. |
| 28 | Round twenty-seven's own closing question: does round twenty-two's key-tightening fix, measured only for `@>`/`<@`(region,region) and `&&`, also meaningfully tighten `@>`(region,point)'s tree, separately from round twenty-seven's own per-comparison speedup? Pure measurement, no code change: temporarily reverted `multicap_union_many()`'s call back to the original `merge_caps_greedy()` (isolating round twenty-two's change alone), rebuilt the index under each binary, same database. | **Confirmed yes — a real, previously-unmeasured effect, but purely explanatory, not a new lever.** Buffers: 5,000 rows 2,619→1,946 (**-25.7%**), 50,000 rows 34,117→26,036 (**-23.7%**). Round twenty-two was never scoped to the region-region strategies specifically. That this barely showed up in round twenty-five's wall-clock numbers makes sense in hindsight: round twenty-seven's per-comparison cost (`sc_angle()`'s cross product/`sqrt`/`atan2`, paid once per sub-cap at *every* visited page) was the dominant term, so a ~24-26% reduction in pages visited was a smaller lever than removing the per-comparison cost itself. Both rounds twenty-two and twenty-seven were already shipped before this measurement — it adds no new speedup on top of what's already measured, it just closes the question of whether a second, separate tree-tightness fix was still available for this strategy (no — both known levers are now pulled). |

**Where this leaves the region GiST opclass**: `&&`, `<@`, and now
`@>`(region,point) are meaningfully improved or solid wins (round
twenty-five found `&&`'s one lingering loss at 50,000 rows flipped by
round twenty-two's fix, previously unverified; round twenty-seven cut
`@>`(region,point)'s gap sharply, nearly closing it at 5,000 rows).
`@>`(region,region) still has a gap to pgSphere, narrowed substantially
by round twenty-two (~5x → ~2.3-2.9x at 50,000 rows) after three prior
targeted fixes (round fourteen's area check, round fifteen's sharper key,
round twenty's unscoped seeding change) failed to close it and two of
them (twenty, twenty-one) actively made it worse. Rounds twenty-three,
twenty-four, and twenty-six all tried to push `@>`(region,region) (and,
for twenty-six, all four strategies) further via tree/key-quality changes
and were all reverted — tightening `merge_caps_greedy_fp()` itself
(small, real, not worth its cost), adding an exact box summary for
picksplit's cost metric (a clear regression), and a sub-quadratic overlap
test (mathematically sound, regressed anyway via an unintended coupling
with round twenty-two's seeding). Round twenty-seven is a different kind
of lever entirely — per-comparison cost, not tree quality — which is
exactly why it worked where six tree/key-quality attempts on the
region-region strategies mostly didn't. The mechanism rounds
twenty/twenty-one diagnosed held up across all five of the tree/key
rounds: `merge_caps_greedy` (unchanged, first-k seeding) backs
`penalty()`'s incremental placement exclusively; `merge_caps_greedy_fp()`
(farthest-point seeding, single greedy sweep, no refinement pass, output
order untouched by round twenty-six's revert) backs `multicap_union_
many()`'s key-building exclusively; `multicap_overlaps()`/`multicap_
overlap_amount()` are the plain O(MAX_SUBCAPS²) all-pairs check,
unchanged from round twenty-two. Round two's original diagnosis —
overlapping bounding caps at internal/leaf pages — still applies to
whatever the tighter merge doesn't catch for `@>`(region,region). Every
tree/key-quality direction on this file's own original open-questions
list for that specific gap (a sub-quadratic overlap test, the
pgSphere-style exact box) has now been tried and found wanting. Round
forty-one then tried the specific fix round twenty-six's own closing
note proposed — decoupling `sub[]`'s stored order from `merge_caps_
greedy_fp()`'s clustering input, by making the first seed a
deterministic, order-independent choice (farthest cap from the batch's
own centroid) instead of the hardcoded `caps[0]` — and it regresses
`@>`/`<@`(region,region) on its own (+5-7% buffers, deterministic, not
noise), before even re-adding the overlap test it was meant to unblock.
Reverted without compounding a second change onto an already-worse
baseline. `caps[0]` isn't as arbitrary as it looks — GiST's own
page-level grouping tends to make it a locally representative seed
already — so "farthest from centroid" is a genuinely different
heuristic, not a neutral de-biasing of the same one, and this specific
choice clusters worse. Doesn't rule out every order-independent
criterion, but rules out this one; four attempts at this function
family now (rounds twenty, twenty-one, twenty-two, forty-one), three
regressions and one kept win. **Closed as of round forty-one**: rather
than guess at a second order-independent criterion, this gap is now
accepted as the opclass's structural floor — `@>`(region,region) stays
slower than pgSphere (~2.3-2.9x at 50,000 rows), not because a fix is
sitting unfound, but because every tree/key-quality direction this
file's original open-questions list named has now actually been tried.
For
`@>`(region,point), round twenty-eight settled round twenty-seven's own
closing question directly: round twenty-two's key-tightening fix *did*
also meaningfully tighten this strategy's tree (buffers -23.7% to -25.7%
across both scales, isolated by temporarily reverting just that one
change) — it just barely showed up in round twenty-five's wall-clock
numbers because round twenty-seven's per-comparison cost was the
dominant term at the time. Both known levers for this strategy have now
been pulled. **Closed alongside `@>`(region,region)**: whatever gap
remains (~3.4-3.9x at 50,000 rows) is accepted as structural — something
about pgSphere's own point-in-shape descent this opclass's cap-based key
doesn't have a matching answer for — rather than a further fix left
unfound in this file's code.

**Both region-GiST losses to pgSphere are now closed as accepted,
structural gaps for the *multi-cap key specifically*, not open questions
about that key — but not the last word on the region GiST opclass as a
whole.** `&&` and `<@` remain solid multi-cap wins; `@>`(region,region)
and `@>`(region,point) remain real, understood multi-cap losses, and no
further multi-cap fix is being chased. Round forty-two then asked a
different question — not "how do we fix the multi-cap key" but "is a
different key altogether cheaper" — and built `skyregion_box_gist_ops`,
a genuinely separate, non-default opclass using a plain axis-aligned
3D box key (same shape as pgSphere's own), on the same unified
`skyregion` type. Measured against both the multi-cap opclass and
pgSphere, same corpus, same probes, same session: the box key **wins
all four strategies against both**, closing the two gaps this section
just called structural and *also* beating the multi-cap key on the two
strategies it already won, while using ~0.32x the multi-cap key's index
size. See round forty-two in full below — the "structural floor" stands
for the multi-cap key, not for skycell's region GiST as an architecture.

| 29 | Extended round twenty-seven's trig-avoidance idea to genuine cap-vs-cap tests: `cap_overlaps()` (backing `&&`/`@>`/`<@`(region,region) via `multicap_overlaps()`) compares against a radius *sum*, not one radius, but the angle-sum identity `cos(ra+rb)=cos(ra)cos(rb)-sin(ra)sin(rb)` gives the same shortcut if `cos(radius)` is cached per cap. Added `cos_radius` to `GistCap` (computed once in `cap_make()`), rewrote `cap_overlaps()` to use it (with an explicit `ra+rb >= pi` guard — the identity's monotonic range, and a real case here since `cover.c` allows a single cone's own radius up to `pi`, not just `pi/2`), and got `cap_area_proxy()`'s existing `1-cos(radius)` sped up for free from the same cache. | **Exact and correct — verified two ways, including a dedicated 400-region, 30-179°-radius stress test built specifically to exercise the `pi` guard, brute force matching GiST exactly throughout — but a net loss anyway.** The cache costs 40 bytes/key (5 caps × 8 bytes); confirmed directly via index size, 17MB→21MB (~24% bigger, matching the key growth almost exactly). Same-database buffers: `&&` +13.9%, `@>`(region,region) +12.9%, `<@`(region,region) +7.8%, all worse; wall-clock mixed, not a clean win. **Reverted.** The same fanout-cost mechanism rounds two, fifteen, and twenty-four all hit — not a bad proxy this time (the computed values are provably identical to the original), just a bigger key; round twenty-seven's win avoided this entirely by needing zero new storage. |
| 42 | Not a multi-cap fix — a genuinely separate, non-default opclass (`skyregion_box_gist_ops`, new file `gist_region_box.c`) using a plain axis-aligned 3D box key (six doubles, exact closed-form cap→box conversion, no trig for union/overlap), directly mirroring pgSphere's own key shape, to test whether the multi-cap key's two structural losses (round forty-one) are inherent to skycell's region GiST or specific to that one key design. | **Wins all four strategies against both the multi-cap opclass and pgSphere** — buffers and wall-clock, same 50,000-row corpus/probes, same session, all three opclasses freshly rebuilt: `&&` 1,172/8.2ms vs multi-cap's 8,375/34.3ms vs pgSphere's 1,193/44.5ms; `@>`(region,point) 1,034/7.0ms vs 7,505/22.2ms vs 1,082/16.2ms; `@>`(region,region) 1,056/6.1ms vs 7,588/24.6ms vs 1,083/12.6ms; `<@`(region,region) 8,293/25.4ms vs 15,614/49.4ms vs 9,520/65.7ms. Correctness exact (286/222/217/4,900 matches, all four strategies matching brute force precisely) and buffer counts reproduced on a full index rebuild. Index size 5,472kB vs the multi-cap key's 17MB (~0.32x) and pgSphere's combined 3,024kB (~1.8x, but one index over one type vs pgSphere's two). Not reverted — kept as a second, non-default opclass pending a shipping decision; see "Round forty-two" in `GIST_REGION_DESIGN.md` for the full derivation and analysis. |

A methodological note from round twenty-one, carried forward and validated
twice since: round twenty-one's same-database baseline (rerun immediately
after its "after" pass) measured meaningfully different absolute numbers
from round twenty's own baseline on a separately-built database with
identical seeds and row counts — directionally consistent, not
bit-for-bit reproducible. Round twenty-two followed the same-database/
same-session discipline throughout and it's what made its result
trustworthy enough to keep. Round twenty-three hit the *same* noise even
within one same-database session (pgSphere's own unmodified numbers moved
between its two passes) and caught it by falling back to buffer counts —
a second reminder that wall-clock alone, even same-session, isn't always
enough; buffer counts are this file's tie-breaker when it isn't.

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

**Where this leaves the point SP-GiST opclass**: the standing "not
recommended for use, a net loss against the B-tree at every radius"
verdict (rounds nine through thirteen) **no longer holds**, as of rounds
thirty-eight and thirty-nine together. Round thirty-eight found building
the index against a table pre-sorted by HEALPix cell (a `CLUSTER`, no C
code changed) rather than natural/insertion order cuts buffer touches
4-17x at 30'-3 degrees (verified safe against the exact correctness risk
the opclass's own header comment documents for sorted-order builds, 0
mismatches across a 60-query corpus) — closing the buffer-count gap to
skycell's own B-tree almost entirely, even reversing it at 3 degrees
(1,226 vs 1,314) — but wall-clock didn't follow on its own, isolated to
a genuinely higher per-node CPU cost in `inner_consistent()`/
`leaf_consistent()` (real geometric classification vs. the B-tree's
plain integer comparisons), not tree shape. Round thirty-nine then found
that per-node cost was itself mostly avoidable waste: both functions
were re-parsing the query region **from scratch on every node visited**
(for a cone, `sc_region_cone()` alone is ~120 transcendental calls,
repeated dozens of times per query instead of once), the same shape
`gist_region.c`'s own `consistent()` already solved at round one of
that opclass. Applying the identical `fn_extra` query-cache fix (same
value-based, not pointer-based, cache-key discipline; verified safe
under a dedicated 200-region single-statement join stress test built
specifically to exercise the stale-cache risk that discipline guards
against) cut wall-clock 1.2-3.6x across the board with buffer counts
provably unchanged. **Combined with round thirty-eight's sorted build,
this reverses the verdict**: re-run three times for reproducibility,
SP-GiST-sorted-plus-cached now beats skycell's own B-tree path outright
at 30' through 3 degrees (1.2-2.5x, every run), with the two roughly
tied at 1"/1' (fixed per-query overhead dominates there regardless).
Neither fix alone was sufficient — sorted build without caching still
lost on wall-clock; caching without sorted build would still carry the
unsorted build's 4-17x worse buffer counts. This doesn't make the
opclass production-ready on its own (still marked EXPERIMENTAL in the
extension's SQL, the sorted-build discipline needs documenting or
enforcing rather than left to the caller, untested beyond this
investigation's corpus/radii), but it is no longer accurate to call it
a net loss. Round forty then re-measured against pgSphere directly —
round eleven's original reference point, not just skycell's own B-tree
— and closes essentially all of that gap too (2.9-3.9x slower originally,
narrowing to ~1.4x at 3 degrees): the sorted-build-plus-cached opclass
now runs within a tight band of pgSphere at every radius, with pgSphere
slightly ahead at 1"-30' and skycell's SP-GiST slightly ahead from 1
degree on (~1.1-1.6x). The first attempt at this measurement produced an
implausible 4.5-12.5x pgSphere loss and was correctly not trusted at
face value: `EXPLAIN (ANALYZE, BUFFERS)` showed pathological heap
locality (`Heap Blocks: exact=3407` for ~7,450 rows) traced to a real
bug in the comparison's own setup — a `JOIN` used to build the `spoint`
comparison table hadn't preserved the intended HEALPix-sorted physical
row order (nothing about a join's output order follows either input's
physical order without an explicit `ORDER BY`), crippling heap-fetch
locality for any index regardless of its own quality. Fixed with an
explicit `ORDER BY ctid`; the same single query dropped from 35ms to
3.2ms from that fix alone, nothing about pgSphere's own GiST changed.

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

**Round eighteen answered that directly: yes, it's already live.** 300
production-representative queries (60 each at 1″/1′/30′/1°/3°, mixed real-
data/uniform centers) against the unmodified, shipped
`cover_cone_direct()` path found `p->max_ranges=64` nowhere close to
binding (max seen: 26 ranges, at the largest radius tested) -- but 3 of
300 (1%, all at 3° in dense regions, 21-22 ranges each) crossed
`jit_above_cost` anyway. Measured those 3 directly with `SET jit =
on/off`: 3.1-6.0x slower with JIT on (133.0ms/58.2ms/61.6ms vs.
22.2ms/18.7ms/19.1ms), JIT compile time (all Emission, `Inlining: 0.000ms`
in every case since cost stays under `jit_inline_above_cost`) eating
68-83% of total execution time, `Workers Launched: 2` confirming the
per-worker redundant-compilation mechanism from round seventeen. Not a
hypothetical edge case from a reverted experiment -- a real, currently-
uninvestigated cost on the already-shipped path, for realistic large-
radius/high-density queries. Neither of round seventeen's two candidate
fixes has been attempted yet.

**Round nineteen tried the cheaper of those two fixes directly: disabling
or raising the threshold on JIT for this query shape.** A fresh 10M-row
corpus and a resampled 300-query benchmark (IID `ORDER BY random()`
sampling this time, not block sampling, which had undersampled the rare
extreme-density hits) reproduced 2/300 queries (0.67%) crossing
`jit_above_cost`. Ran three configurations head-to-head on the same
connection: default GUCs, `SET jit = off`, and `SET jit_above_cost =
400000` (JIT left on, threshold raised past every cost this benchmark
produced). Result: **raising the threshold matched or beat a full JIT
disable** (22.2ms/29.5ms vs. 25.2ms/30.7ms vs. 44.3ms/68.1ms baseline,
for the 2 triggering queries) — expected, since it produces the identical
plan-time no-JIT decision while leaving JIT available for anything that
actually costs more than 400,000 elsewhere in the database. Zero
regressions across the other 298 queries. **But this is confirmed to be
an operational fix, not one skycell's own code can apply**: PostgreSQL's
JIT go/no-go decision is made once, at the end of planning, strictly
before any skycell C function ever executes — nothing skycell does at
scan time (nor any of its existing GUCs) can reach back and change a
decision already baked into the `PlannedStmt`. The only mechanism that
*could* intervene at the right point is a `planner_hook` wrapping
`standard_planner()` to selectively clear `jitFlags` for plans built from
skycell's own operators — not implemented, and a materially bigger kind
of change (a new extension-wide hook, not a `cover.c` tweak) than
anything else in this investigation. Actionable today with zero code
changes: document that applications running skycell cone/region queries
at large radii on dense catalogs should raise `jit_above_cost` (or
disable JIT) for that session/connection pool — confirmed non-regressive
and at least as good as a blanket disable.

This section's own gap (B-tree losing to pgSphere at large radii) is
still open on the B-tree path itself — nothing here has changed that.
But round forty's fresh 3-way measurement (§4) found the same direction
holds now too (B-tree trailing pgSphere at 1 degree and up on this
session's corpus), and that the point SP-GiST opclass (§4), once given
a sorted build and query caching, closes this exact gap from the *other*
structure — not by making the B-tree path faster, but by giving skycell
a second index that doesn't have this weakness at large radii at all.

## 6. Open questions and concrete untried directions

In rough order of how well-scoped/promising they seem from this history,
not in priority order — pick what matches the actual goal:

1. ~~Give `cover.c`'s cost model a JIT-aware term~~ — tried (round
   thirty-seven), structurally impossible to implement usefully, not just
   unmeasured. Round nineteen already closed the operational half
   (raising `jit_above_cost`/disabling JIT is a confirmed, non-regressive
   fix, but not something skycell's own code can apply per-query). Round
   thirty-seven tested the other half directly: for an actual
   jit_above_cost-crossing query (10M rows, 3.5° radius, dense
   real-data-cluster center, cost 99,165 at the default order),
   systematically forcing every order from 5 through 15 showed the
   covering already converges to its best available shape at the default
   choice (orders 7-15 all produce the identical 30-range, 107,478-row
   covering — no finer order helps further) and *every* coarser
   alternative makes the real, Postgres-reported plan cost *worse*, not
   better (order 6: 12 ranges but cost 262,125 — 2.6x higher, not lower).
   The reason is structural, not specific to this query: `merge_gaps()`
   already greedily merges every gap cheaper than `range_cost` before
   `rlist_score()` ever compares candidates, so by the time a covering is
   chosen, no cheaper-to-merge gap is left — forcing *more* merging past
   that point necessarily absorbs gaps whose row-cost exceeds
   `range_cost`, which can only raise total cost, for any query, not just
   this one. There is no "duck under the JIT cliff by picking a coarser
   covering" move available: the query's cost is a genuine, unavoidable
   reflection of how many rows it truly matches, which is exactly the
   case where JIT normally helps amortize a big scan — the real defect is
   PostgreSQL's own parallel-JIT interaction (each worker independently
   pays the compile cost with no amortization across workers), an
   execution-side architectural gap a covering-time cost term in `cover.c`
   cannot reach regardless of how it's designed. A `planner_hook` (or a
   PostgreSQL core fix to that interaction) remains the only path to a
   code-level fix; not attempted, and this round's finding makes clear no
   scoped alternative exists inside `cover.c` itself.
2. **Recalibrate `SC_COVER_MAX_DRY_STREAK`** (round sixteen's reverted fix,
   `cover.c`) against a broad query set, not one failing query -- the
   underlying bug (density alone justifying unbounded speculative
   refinement in a cell with no boundary nearby) is real and precisely
   diagnosed; the specific cap chosen (2) demonstrably regressed a nearby
   case. Also untested against the adaptive path's actual existing callers
   (polygon coverings) at all.
3. ~~A sub-quadratic overlap test for the region GiST multi-cap key~~ —
   **tried, round twenty-six, reverted.** Sorted sub-caps by centre x,
   skipped pairs a real (not approximate) geometric bound proves can't
   overlap. Correctness held; buffers got consistently *worse* (+2.0% to
   +4.6%) across all four strategies — not from the pruning bound (exact)
   but from an unintended coupling with round twenty-two's order-sensitive
   farthest-point seeding, which silently reseeds differently once a
   stored key's `sub[]` is reordered. Untried follow-up if revisited:
   decouple by keeping a second, unsorted copy for clustering input, or
   make the seeding itself order-independent — both bigger than this
   round's fix, not attempted.
4. ~~Scope any future `merge_caps_greedy` improvement to `union()` alone,
   never `penalty()`~~ — **done, shipped, round twenty-two.** A new
   `merge_caps_greedy_fp()` (farthest-point seeding) is used only by
   `multicap_union_many()`; `multicap_penalty()` still calls the original,
   unmodified `merge_caps_greedy()`. Measured a real win: `@>` 2.2-2.3x
   faster, `<@`'s existing win over pgSphere widened, both at 50,000 rows,
   no regression at 5,000. `@>`'s gap to pgSphere narrowed (~5x → ~2.3-2.9x)
   but not closed — see §3 for the numbers, and the two items below for
   what's left.
5. ~~Push the now-decoupled `union()`-only merge further with a Lloyd's-style
   refinement pass~~ — **tried, round twenty-three, reverted.** A real but
   small buffer-count tightening (`@>` -5.1%, `<@` -1.5%), no confident
   wall-clock signal (contaminated by ambient noise even same-session, a
   second instance of round twenty-one's own caution), not worth the extra
   per-`union()`-call cost. `merge_caps_greedy_fp()` stays a single greedy
   sweep. Further tuning inside this same greedy-merge family looks like
   diminishing returns now — item 6 and the sub-quadratic overlap test
   above look more promising for narrowing `@>`'s remaining gap.
6. ~~A pgSphere-style exact composable summary for internal nodes only~~ —
   **tried, round twenty-four, reverted.** Added `GistBox3D` (exact,
   closed-form spherical-cap bounding box, unioned by plain min/max) as
   `GistMultiCap`'s new field, scoped only to `picksplit`'s cost metric
   (never `penalty()`, matching round twenty-one's caution; never
   `consistent()`). Measured a clear regression on both strategies (`@>`
   +14.9% buffers, `<@` +8.7% buffers), not a wash. The likely reason:
   Euclidean box volume, the cheap proxy used, isn't a good stand-in for
   true spherical coverage — pgSphere's own simpler native shapes are a
   corpus where that correlation happens to hold; skycell's polygon-
   decomposed, scattered sub-caps are not. Exact composition turned out not
   to be the missing ingredient by itself; the *proxy* built on top of it
   mattered more, and the cheap one tried here was the wrong one. A real
   spherical-area proxy for the box (not volume) is the natural next
   attempt for anyone who wants to pick this back up — not tried.
7. ~~A `min_area` field for `CONTAINED_BY_REGION` (`<@`)~~ — tried (round
   thirty-six), reverted: same outcome as round fourteen's `max_area`
   mirror. Correct, and a real (if small, ~5%) key-size cost, for a
   measured effect that was a regression at 5,000 rows (2099→2197
   buffers) and noise-level at 50,000 (three independent rebuilds landed
   within a handful of buffers of each other in no consistent direction)
   — the overlap test already rejects almost everything the area test
   would additionally catch, the same redundancy round fourteen found in
   the other direction.
8. ~~A bulk-loaded, statically-packed structure for the point
   predicate~~ — the cheap version tried (round thirty-eight): building
   against a table pre-sorted by HEALPix cell, no C code changed. Confirmed
   the core hypothesis emphatically (4-17x fewer buffer touches, closing
   the gap to the B-tree almost entirely) and precisely diagnosed the
   remaining wall-clock gap as a separate, orthogonal per-node CPU cost
   (`inner_consistent()`/`leaf_consistent()` re-parsing the query region
   from scratch on every node visited — ~120 transcendental calls per
   node for a cone, repeated dozens of times per query), not tree shape —
   making a full custom `ambuild` (hand-rolled page/WAL layout, materially
   bigger than anything else in this file) a poor bet on its own, since it
   would only improve the thing that was no longer the bottleneck. Round
   thirty-nine then closed that exact gap instead: the same `fn_extra`
   query-caching fix `gist_region.c` already used (round one of that
   opclass), applied here, cut wall-clock 1.2-3.6x with buffer counts
   unchanged, verified safe under a dedicated join-based stale-cache
   stress test. Combined, this **reverses** the opclass's standing
   verdict — SP-GiST-sorted-plus-cached now beats skycell's own B-tree
   path outright at 30' through 3 degrees (1.2-2.5x, reproduced across
   three independent runs), not just "closes the gap." See §4's rewritten
   closing verdict. The expensive custom-`ambuild` version remains
   untried and is now even less clearly worth it, given the cheap
   combination already wins.
9. **Real survey-footprint sparsity** for the point SP-GiST's covering-walk
   idea (round thirteen's own caveat): the corpus used throughout this
   whole investigation is synthetic with a substantial uniform-sky
   component, so it has no true coverage gaps. A real archive's actual
   observed-footprint structure (genuine unobserved sky, not just
   low-density sky) might make round thirteen's "0% empty cells" result
   corpus-specific rather than general — untested, no corpus available in
   this repo to test it with.
10. ~~`skycell.probe_orders` (`cover.c`): calibrate `split_cost`~~ — tried
   (round thirty), no net win. Buffer counts *do* improve monotonically as
   `split_cost` drops from `1.0` toward `0.0` (6-8% fewer buffers at
   6'-30', matching the docstring's own cited range, vanishing by 60'-
   120'), confirming the cost-model imbalance the docstring describes is
   real. But `probe_orders=3`'s own fixed planning-time cost (confirmed via
   `skycell_cover_info()`'s `steps` counter jumping 18→79 with the chosen
   order unchanged) outweighs that buffer saving at this corpus/query
   scale in wall-clock terms, regardless of `split_cost`'s value — so no
   calibrated value of `split_cost` alone unlocks the win. Left as shipped
   (`split_cost=1.0`, `probe_orders=0`). Getting a real win here would mean
   cutting the probing loop's own fixed overhead, not retuning its score.
   Round thirty-one tried exactly that — capping the probe at depth 1
   instead of 3 — and found it proves the deeper probe is pure waste
   (buffer counts byte-identical between depth 1 and depth 3 at every
   radius, 6'-120', both split_cost values tested) but still doesn't clear
   the bar against leaving `probe_orders` off entirely (wall-clock
   direction consistent with less waste, not statistically significant at
   this scale). Conclusion holds at the cheapest possible probe depth too;
   still no code change, still shipped as `split_cost=1.0`/`probe_orders=0`.

## 7. Where things stand right now (as of this document)

- Region GiST opclass: `MAX_SUBCAPS=4`, shared overlap test for all
  containment strategies, original first-k-seeded `merge_caps_greedy` (used
  only by `multicap_penalty()`) plus a new farthest-point-seeded
  `merge_caps_greedy_fp()`, single greedy sweep, no refinement pass (used
  only by `multicap_union_many()`, round twenty-two, kept — a real,
  measured win, not experimental). `picksplit`'s cost metric is
  `multicap_total_area()` (sub-cap-area sum), unchanged from round
  twenty-two — no `GistBox3D` field (round twenty-four reverted), `sub[]`
  unsorted and `multicap_overlaps()`/`multicap_overlap_amount()` plain
  O(MAX_SUBCAPS²) all-pairs (round twenty-six reverted). New `cap_contains_
  point()` (round twenty-seven, kept) — a trig-free point-in-cap test
  (`dot(centre,point) >= cos(radius)`, no cross product/`sqrt`/`atan2`),
  used only by `multicap_contains_point()`, nothing else touched. Rounds
  fourteen, fifteen, twenty, twenty-one, twenty-three, twenty-four, and
  twenty-six's changes were all reverted after measurement (twenty and
  twenty-one regressed `@>` and `<@`; twenty-three measured a real but
  too-small-to-justify buffer improvement; twenty-four and twenty-six both
  measured clear buffer regressions, the latter from an unintended coupling
  with round twenty-two's order-sensitive seeding — see §3). No
  `cos_radius` field on `GistCap` — round twenty-nine's cap-vs-cap
  trig-caching extension of round twenty-seven's idea was exact and
  correct (verified against a dedicated large-radius stress test) but
  reverted: caching costs 40 bytes/key, confirmed as ~24% more index size,
  and buffers got consistently worse (+7.8% to +13.9%) despite the
  per-comparison math being provably identical — the same key-bloat/
  fanout mechanism rounds two, fifteen, and twenty-four hit, this time
  from pure size growth rather than a bad proxy. Round thirty-six's
  `min_area` field for `<@` (the mirror of round fourteen's `max_area`)
  hit the exact same redundancy round fourteen already found in the
  other direction — reverted, no `min_area` field either. Rounds
  twenty-two and twenty-seven remain the only two changes from this
  whole investigation actually shipped. `@>`(region,region)'s gap to pgSphere is
  narrowed (~5x → ~2.3-2.9x at 50,000 rows) but not closed; every
  tree/key-quality direction from this file's original open-questions list
  for it has now been tried and found wanting — round twenty-six's own
  closing note (decoupling `sub[]`'s stored order from clustering input)
  is the least-tried remaining idea, bigger than anything attempted so
  far. Round twenty-five confirmed round twenty-two's fix also flipped
  `&&` at 50,000 rows from a ~1.2x loss (unclosed since round three) to a
  ~1.28-1.55x win, a side effect never directly measured until now.
  `@>`(region,point)'s gap — wide and untouched by rounds four through
  twenty-six — narrowed sharply in round twenty-seven (5,000 rows:
  ~1.7-2.1x → ~1.1-1.2x slower, nearly closed; 50,000 rows: ~4.0-4.6x →
  ~3.4-3.9x slower) via a completely different lever (per-comparison cost,
  not tree quality) than anything tried on the region-region strategies.
  Round twenty-eight then confirmed (pure measurement, no code change)
  that round twenty-two's tree-tightening fix *also* helped this strategy
  all along (buffers -23.7% to -25.7%, both scales) — both known levers
  for `@>`(region,point) are now pulled, so its remaining gap is more
  likely structural than a further fix waiting in this file's code. Round
  forty-one tried the one concretely-diagnosed remaining lever for
  `@>`(region,region) (order-independent seeding, to unblock round
  twenty-six's reverted overlap test) and it regressed on its own
  (+5-7% buffers) before even reaching the overlap test — reverted, and
  both `@>`(region,region) and `@>`(region,point)'s gaps are now closed
  out as this opclass's accepted structural floor, not open questions.
  **Final scorecard for the multi-cap key vs. pgSphere, both scales**:
  `&&` and `<@` win outright; `@>`(region,region) loses but the gap
  narrowed substantially (~4.9-5.2x → ~2.3-2.9x at 50,000 rows);
  `@>`(region,point) loses by a margin cut sharply by rounds twenty-seven
  and twenty-eight together (~4.0-4.6x → ~3.4-3.9x), nearly closed at the
  smaller scale. **Superseded by round forty-two**: a separate box-key
  opclass (`skyregion_box_gist_ops`, `gist_region_box.c`) wins all four
  strategies against both the multi-cap opclass and pgSphere, on both
  buffers and wall-clock, at ~0.32x the multi-cap key's index size — not
  a further multi-cap fix, but proof the multi-cap key's own gaps aren't
  inherent to skycell's region GiST architecture. Not yet shipped as the
  default; see round forty-two in §3 for the full numbers and status.
- Point SP-GiST opclass: shipped as a correctness-verified, documented
  negative result (`SPLIT_WIDTH=1`, effectively single-order splits; the
  bytea-prefix/explicit-width fix from round twelve is real and kept even
  though wide splitting itself isn't used).
- B-tree covering path (`cover.c`): shipped as-is (`p->direct=1`, the
  non-adaptive cone fast path, unchanged for every radius). Round sixteen's
  dry-streak fix to the adaptive fallback path was reverted, not validated
  as a net win — see §5. Round eighteen confirmed a real, live, unaddressed
  cost on this exact shipped path: ~1% of realistic large-radius/high-
  density queries already cross PostgreSQL's `jit_above_cost` on range
  count alone (nowhere near `p->max_ranges=64`) and pay a 3-6x parallel-
  JIT tax as a result. Round nineteen confirmed the cheap fix (raise
  `jit_above_cost`/disable JIT for the session) works cleanly with zero
  regressions, but it's a GUC applications must set themselves — skycell's
  own code has no hook into a decision PostgreSQL finalizes before any
  skycell function runs. Round thirty-seven then tested the other
  candidate fix directly — a JIT-aware term in `cover.c`'s own cost model
  — and found it structurally impossible, not just unmeasured: for an
  actual crossing query, every order from 5 to 15 was tried directly, and
  the covering was already at its best available shape (orders 7-15 all
  converge to the same covering) while every coarser alternative made the
  real, Postgres-reported cost *worse* (order 6: 2.6x higher, not lower)
  — because `merge_gaps()` already greedily merges every gap cheaper than
  `range_cost` before a covering is ever chosen, so forcing more merging
  past that point can only raise cost, for any query. No scoped
  `cover.c`-internal fix exists; the operational GUC guidance from round
  nineteen is the only actionable mitigation shipped from this line of
  investigation. Round thirty attempted the other open `cover.c` item — calibrating
  `split_cost` so `skycell.probe_orders`'s scored mechanism picks the
  already-proven-better finer covering order automatically — and found no
  net win: real buffer savings from lower `split_cost` are real but small
  (6-8% at 6'-30', the docstring's own cited window) and are outweighed by
  `probe_orders`' own fixed per-query planning overhead at this corpus
  scale, so `split_cost=1.0`/`probe_orders=0` ship unchanged. The attempt
  also surfaced two measurement-methodology traps worth remembering for
  any future micro-benchmark at this query scale (sub-millisecond,
  tens-to-hundreds of buffers): a sequential-block A/B design picked up a
  ~40% pure time-drift artifact, and even an interleaved-by-center design
  picked up a within-cycle first-position cache bias large enough to look
  like a real, statistically significant 18% effect until checked against
  `skycell_cover_info()`'s actual covering decisions. Only a per-(rep,
  center) randomized visit order, cross-checked against buffer counts
  (immune to timing noise entirely), gave a trustworthy answer. Round
  thirty-one confirmed the same conclusion holds even at the cheapest
  possible probe depth (buffer counts byte-identical between
  `probe_orders=1` and `=3` at every radius, proving the deeper probe is
  pure waste — but still not enough to beat leaving it off). Round
  thirty-two asked whether pgSphere's BRIN opclass points at a cheaper
  index for this same path and found no at the radii tested (6'-120',
  all under 0.03% of the sky): BRIN loses to the B-tree by ~3.3x buffers
  / ~3.5x wall-clock at every one of those radii even on a table
  deliberately `CLUSTER`ed on `cell` for BRIN's own best case — its
  lossy, block-granular bitmap (`Heap Blocks: lossy=...`, thousands of
  rows pulled per query just for the range recheck) can't match the
  B-tree's row-exact result, and a finer `pages_per_range` made it worse,
  not better. Round thirty-three swept radius up into BRIN's actual
  proposition (1-90 degrees, ~0.008%-50% of the sky) and found buffers do
  cross over cleanly around 15-20 degrees (~2-3% of the sky), growing to
  29% fewer buffers at 90 degrees — the textbook BRIN-vs-B-tree condition
  (bitmap-build cost scaling with result size for a B-tree, near-fixed
  for BRIN's tiny index) once `skycell.max_ranges` caps range *count* and
  each range's own result size keeps growing with radius. But wall-clock
  doesn't track that cleanly and can reverse (BRIN's lossy bitmap forces
  real CPU-bound recheck work a B-tree's exact bitmap never pays), so the
  net win depends on whether the workload is I/O- or CPU-bound. Round
  thirty-four then checked pgSphere's own BRIN test suite directly rather
  than assuming its C source generalizes to every shape, and found
  pgSphere's BRIN opclass isn't built for cone search at all — it's
  registered only for `spoint`/`sbox`, tested only against a rectangular
  RA/Dec box query (`sbox <@ sbox`), never a circle. Tested the actual
  analogous case — composite B-tree(ra,dec) vs multi-column BRIN(ra,dec)
  for a plain box range query — and BRIN wins decisively and *cleanly* on
  both buffers and wall-clock from ~3 degrees on (9.4x fewer buffers,
  2.5x faster at 30 degrees half-width), with no cone case's CPU-bound
  reversal, because a box query's own recheck condition costs nothing
  extra beyond the predicate itself (no separate trig-based geometric
  filter riding on top the way `skycell_in_cone()` has). A bonus finding
  along the way: skycell's existing `cell`-based physical clustering
  (already the natural choice for cone search) serves a BRIN(ra,dec) box
  index better than re-clustering by the composite `(ra,dec)` B-tree
  itself would — a composite-index `CLUSTER` effectively sorts by its
  first column only, leaving the second dimension's BRIN summary close
  to useless. `cat_cell_idx` (B-tree) remains the only index skycell's
  own cone-search path needs; BRIN is a real option for skycell users who
  need wide, low-selectivity cone queries or plain rectangular RA/Dec
  range queries, neither of which skycell's own operators currently
  provide dedicated support for (no `sbox`-equivalent region type). Round
  thirty-five closed out this line of investigation by checking pgSphere's
  actual exact per-row distance math (`spoint_dist()`, Vincenty's formula
  on stored `(lat,lng)` — six trig calls and a sqrt per comparison,
  re-derived every call) against skycell's own (Cartesian unit vectors
  computed once, so a point-in-cap test is one dot product against a
  cached `cos(radius)`, zero trig calls) — confirmation that skycell is
  already ahead here, not a gap. Three rounds of mining pgSphere's source
  (index architecture/query-key caching originally; BRIN's real scope in
  rounds thirty-two through thirty-four; this round's exact-math check)
  land on the same pattern throughout: pgSphere sometimes wins on the
  *indexing-structure* axis for specific query shapes it's actually built
  for, but skycell's per-comparison math is already ahead or matched
  everywhere checked. No further pgSphere-mining planned without a new,
  more specific lead.
- `GIST_REGION_DESIGN.md` is the source of truth for exact numbers, code
  reasoning, and anything this summary compressed or left out.
