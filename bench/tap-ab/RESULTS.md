# skycell A/B: results

Run `20260919T185401Z-a2d20771-tap-compare` (end to end) and
`results/db-20260919T185324Z` (database level), 2026-09-19/20, on the
developer host described in [PROTOCOL.md](PROTOCOL.md). Corpus: the paper's
D1 seed, 500096 `ivoa.obscore` rows.

**Answer: no. On this corpus and these query classes, translating cone
searches to skycell does not improve egernia's numbers.** All 18 comparison
cells tie; at the database level skycell is 1.4–1.6× *slower* per query.

## Gates

- `stilts taplint`: PASS on both targets, 0 blocking and 0 total errors.
- Agreement: 11 of 11 portable classes agree; none excluded.
- `dbbench.py`: 203 cone queries, row counts identical between the
  translations in every one.

So the translation is correct; what follows is only about speed.

## Database level (EXPLAIN ANALYZE, median of 5, planning + execution)

| class | rows | pg_sphere | skycell | skycell plan / exec | buffers |
|---|---|---|---|---|---|
| Q05 small cone (0.05–0.25°) | 12.7 | **0.133 ms** | 0.212 ms | 0.120 / 0.093 | 16 → 59 |
| Q06 medium cone (1–3°) | 237.1 | **0.323 ms** | 0.460 ms | 0.167 / 0.293 | 116 → 163 |
| Q07 cone + time + calib | 67.2 | **0.275 ms** | 0.419 ms | 0.177 / 0.241 | 81 → 122 |
| Q12 empty cone (0.0002°) | 0.0 | **0.096 ms** | 0.143 ms | 0.120 / 0.023 | 2.2 → 3.5 |

pg_sphere plans in 0.062–0.097 ms; skycell plans in 0.120–0.177 ms. The gap
*is* the planning: skycell computes the covering, and reads its 1000-bucket
density histogram, on every plan. Execution is close (Q06 0.293 vs 0.261 ms)
and skycell's is lower only where there is nothing to fetch (Q12, 0.023 vs
0.030 ms).

Tightening the covering makes it worse, because the covering is what costs:

| `skycell.range_cost` | Q05 plan / exec / buffers | Q06 plan / exec |
|---|---|---|
| 30 (default) | 0.120 / 0.093 / 59 | 0.167 / 0.293 |
| 5 | 0.139 / 0.083 / 48 | 0.328 / 0.292 |
| 1 | 0.164 / 0.080 / 40 | 0.507 / 0.307 |

Fewer false positives, more plan-time work, and the second grows faster.

## End to end (the harness's rungs, CSV, 3 repetitions, tie rule)

18 of 18 cells tie. Ratios (skycell / pg_sphere throughput) run 0.96–1.10,
with intervals that overlap in every cell:

| class | c=1 | c=8 | c=32 |
|---|---|---|---|
| mix | 1.005 | 1.018 | 1.102 |
| Q03 (control, no geometry) | 0.948 | 1.004 | 1.008 |
| Q05 | 1.030 | 0.987 | 1.010 |
| Q06 | 0.965 | 1.007 | 0.963 |
| Q07 | 0.965 | 0.989 | 1.008 |
| Q12 | 0.980 | 1.000 | 1.003 |

The control class behaves like the cone classes, which is the point: at this
corpus size the spatial index is not what the numbers measure.

## Why

A cone query here touches tens to hundreds of rows out of 500096, and
`ivoa.obscore` is physically ordered by project, so a field's 128 products
sit on a few heap pages: a GiST bitmap scan reads almost exactly the pages it
needs. Against that, skycell brings a plan-time cost (the covering) and wins
nothing back at execution.

End to end the difference disappears entirely. The paper measures ~10 ms of
CPU per request, most of it Python (translation, result writers), against a
sub-millisecond query — so a 0.08 ms difference in the database is ~1% of a
request, well inside the tie rule.

## What would have to change for skycell to matter here

- **A much larger relation.** The skycell prototype's own benchmark (10M
  rows, Gaia-like crowding) has it level with pgSphere on cone searches and
  ahead on polygons, cross-matches and index size/build (214 MB vs 683 MB,
  1.6 s vs 46 s). ObsCore relations of 10^8–10^9 rows are where the index,
  not the API, sets the pace — untested here.
- **Amortising the covering.** egernia plans every request afresh (constants
  are inlined into the SQL), so skycell pays its plan-time covering per
  request. Caching the density statistics per backend, or reusing coverings
  for repeated query text, would remove most of the 0.06–0.1 ms gap.
- **Workloads the paper does not run.** Cross-matching (`TAP_UPLOAD` joined
  to ObsCore by position) is where the cell index was 4× faster in the
  prototype's own benchmark. No class in this corpus does that.

## Follow-up: what two optimisations bought (2026-09-20)

Two costs were measured directly (`EXPLAIN` planning time over the same corpus,
`skycell.use_stats` and `skycell.max_ranges` switched one at a time):

- reloading the 1000-bucket density histogram and re-finding the expression
  index on every plan: ~45–70 µs;
- each extra range arm, as a separate index path with its own selectivity
  estimate: ~15–20 µs (Q06 averages 4.4 arms, Q07 3.1, Q05 1.5).

Both were addressed in the extension: the density model and the matched index
are now cached per backend (invalidated by the statistics, relation and
relcache callbacks), and coverings are memoised by cone and statistics source
(`skycell.cache_coverings`, default on). Same corpus, medians of 5:

| class | pg_sphere | skycell before | skycell after | ratio after |
|---|---|---|---|---|
| Q05 | 0.118 ms | 0.212 ms | 0.169 ms | 1.43 |
| Q06 | 0.190 ms | 0.460 ms | 0.272 ms | 1.43 |
| Q07 | 0.216 ms | 0.419 ms | 0.302 ms | 1.40 |
| Q12 | 0.118 ms | 0.143 ms | 0.145 ms | 1.23 |

Raising `range_cost` to trade arms for false positives makes it worse
(range_cost 100: Q06 total 0.373 ms, buffers 163 → 228), so the model's
calibration is not the problem.

What remains is structural: the covering is expressed as OR-ed B-tree ranges,
so the planner costs one index path per arm, and the conservative cell test
leaves 1.4–3.7× more rows to fetch than the cone contains. Removing both means
not planning ranges at all: an SP-GiST opclass over the HEALPix cell hierarchy
would present one index qual, refine during the index descent (only where rows
exist), and decide at order-29 leaves, where a cell pins a position to 0.4 mas.
That is the only route to beating pgSphere on this workload — and it would
still not move egernia's end-to-end numbers, which are API-bound.

## Follow-up 2: the covering rewritten (2026-09-20)

Two changes to the extension itself, aimed at the two costs above rather than
at their constants:

1. **Exact cell geometry.** A cell is now classified against a cone by its own
   four corners and the chords between them, with the edge bulge measured per
   cell from the edge midpoints, instead of a cap of the worst-case pixel
   radius over the whole sphere. (Near the poles an edge leaves its chord by a
   fraction of the cell size rather than its square, which a global constant
   gets wrong — the brute-force test caught exactly that.)
2. **The order in closed form.** `Cost(s) = ranges·range_cost + ρ·waste`
   minimises at `s* = √(α·range_cost/ρ)` for cells smaller than the cone; the
   cost is evaluated at all 30 orders and the cheapest taken, so the greedy
   top-down refinement disappears.

Effect on the covering itself (`ext/test/cover_selftest.c`, clustered 2M-point
catalogue): refinement steps 15–1131 → 2–96, time per covering 5–540 µs →
1–40 µs, and in a dense field a 1′ cone now covers 1.3× its own area at order
14 instead of stopping much coarser.

On this corpus, skycell per query relative to pg_sphere:

| | before | + caches | + rewrite |
|---|---|---|---|
| Q05 | 1.59 | 1.43 | ~1.25 |
| Q06 | 1.42 | 1.43 | ~1.3 |
| Q07 | 1.52 | 1.40 | ~1.3 |
| Q12 | 1.49 | 1.23 | ~1.2 |

(Ratios, because the absolute numbers move with what else the laptop is doing;
a `range_cost` sweep of 10/30/60/120 stays inside that band, so the default is
unchanged.) Correctness is unchanged: 0 row-count mismatches over the 203 cone
queries, and the PostgreSQL regression suite passes.

At 10M rows — the scale the extension was built for — the same build beats
pg_sphere on the same tables (identical row counts, colder cache):
0.71 / 1.42 / 0.56 / 0.45 / 0.31 times pg_sphere's time at 1″ / 1′ / 30′ / 1° / 3°.

So the rewrite helps most where the scan dominates. On a 500k-row relation
skycell is still ~1.25× slower per query, and the residue is the per-arm
planner cost of an OR of B-tree ranges: the `= ANY` single-scan form or an
SP-GiST opclass, not more geometry.

## Full re-run with the rewritten covering (2026-09-20)

Everything repeated end to end on the same corpus and grid: run
`20260920T011641Z-a2d20771-tap-compare`, database level in
`results/db-20260920T011606Z`.

- **Gates:** taplint PASS on both targets (0 blocking, 0 total errors),
  agreement 11 of 11 classes, 0 row-count mismatches over the 203 cone
  queries.
- **End to end: 18 of 18 cells still tie**, with the ratios tighter than
  before — 0.96–1.06 against 0.95–1.10. Per class at c = 1 / 8 / 32: mix
  0.97 / 0.98 / 1.06, Q03 (control) 1.00 / 0.99 / 0.99, Q05 1.00 / 1.03 /
  1.00, Q06 0.97 / 0.99 / 0.99, Q07 1.01 / 1.00 / 1.00, Q12 1.02 / 0.96 /
  1.00.
- **Database level:** Q05 0.152 → 0.191 ms, Q06 0.284 → 0.412, Q07 0.256 →
  0.342, Q12 0.104 → 0.127 (pg_sphere → skycell): still ~1.2–1.45×, the same
  band as before the rewrite on this corpus.

The rewrite does not change the answer here, and the reason is now measured
rather than argued: at 500096 rows a cone query costs a fraction of a
millisecond either way, against ~10 ms of Python per request. The same build,
on the same laptop, wins at 10M rows (skycell's own benchmark: 0.61–0.92× of
pgSphere's time interleaved and warm, and ~2× fewer buffers at 1°). The
crossover is the size of the relation, not the query.
