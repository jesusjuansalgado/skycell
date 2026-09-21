# Response to the second referee

We thank the referee for a report that is unusually precise about *what kind* of
claim each of our results can support. Three of its requests changed conclusions
in the paper rather than only its wording:

- **Real positions change one headline number.** We now measure on 10 million
  real Gaia DR3 positions rather than only on positions resampled from the DR3
  density field. The index-size and large-region results reproduce; the
  small-cone comparison against pgSphere is **worse** on real positions than on
  the resampled corpus, and we now report that.
- **The density estimator fails exactly where the referee predicted.** On real
  positions it is accurate to ~25% on ordinary sky but underestimates by 8–12×
  in compact globular-cluster cores and overestimates by 3.7× at the Galactic
  centre. We had claimed the estimator was adequate; it is not, in those places.
- **But the covering decision survives that failure**, and we can now show why
  with an ablation rather than an argument. This is the distinction the referee
  asked us to draw (major 4), and it turns out to be the most useful result in
  the revision.

Everything below is reproducible from the repository. New material:
`bench/16_sensitivity.sql`, `bench/17_ablation.sql`, `bench/18_xmatch_sweep.sql`,
`bench/19_gaia_real.sh`, and `REPRODUCING.md`, which is the checklist requested
in major 10.

---

## Major 2 — hardware and configuration dependence

> *"A sensitivity analysis varying PostgreSQL cost parameters, particularly
> `random_page_cost` and `seq_page_cost`."*

Done, on the real Gaia corpus. `c_range` is derived from `relpages/reltuples`
and the planner's cost GUCs, so the question is whether a misconfigured planner
produces a bad covering.

| `random_page_cost` | derived `c_range` | chosen order, 10″ → 3° |
|---|---|---|
| 1.1 | 137.2 | 14 11 9 9 9 |
| 2 | 125.1 | 14 11 9 9 9 |
| 4 | 104.6 | 14 11 9 9 9 |
| 10 | 70.1 | 14 11 9 9 9 |

The derived cost responds to `random_page_cost` by a factor two; **the covering
does not change at all**. This is not a coincidence and not a null result: the
optimum is *s\* ∝ √(c_range)*, and the orders available are powers of two, so a
factor two in `c_range` is a factor 1.4 in cell size — under one order. The
practical consequence is the one that matters for major 2: *the method does not
require the planner's cost parameters to describe the hardware correctly.* A
site that has never tuned `random_page_cost` gets the same coverings as one that
has.

We also report a limitation the sweep exposed: **`seq_page_cost` has no effect
whatever** (0.5 and 1.0 give identical `c_range`). The derivation ignores it.
That is defensible — the cost we are estimating is the cost of *one more index
range*, which is a random access — but it was previously implicit, and §5.3 now
says so.

We have not been able to answer the other half of major 2. All measurements
remain from one machine, one storage device and one PostgreSQL build. §7 now
states this as a bound on the claims rather than as a caveat at the end, and the
conclusions no longer describe the measured ratios as representative of archive
deployments generally.

---

## Major 4 — the density estimate

> *"The paper should distinguish the accuracy of the estimated density from the
> accuracy of the final covering decision. The latter is the quantity most
> relevant to the algorithm's practical performance."*

This was the most productive comment in the report. Taking the two halves
separately, on **real Gaia DR3 positions**:

### The estimate itself is poor in exactly the places the referee named

Estimated ρ against ρ counted from the table, both in rows sr⁻¹:

| field | median est/true | range |
|---|---|---|
| ω Cen | **0.08** | 0.05–0.51 |
| 47 Tuc | **0.12** | 0.06–0.93 |
| LMC | 0.76 | 0.25–0.93 |
| Baade's Window | 0.80 | 0.68–1.01 |
| N galactic pole | 0.90 | 0.53–1.28 |
| Galactic centre | **3.74** | 2.53–3.95 |

On ordinary sky the estimate is good to about ±25%. In compact cluster cores it
is **8–12× too low**, and at the Galactic centre **3.7× too high**. On the
complete, full-density fields the mechanism is visible directly: at radii
≤ 0.05° the estimate is excellent (0.97–1.05 at ω Cen, the LMC and Baade), and
collapses to 0.15–0.23 at 0.2°. The histogram resolves a cluster when the query
sits inside it and averages it away when the query is larger than it — which is
precisely the referee's "structures smaller than the effective histogram
resolution", now measured rather than conceded.

We withdraw the word *unbiased* (minor 3.13). What we observed previously was a
median ratio near unity on corpora that contained no structure below 0.11°; that
is a property of those corpora, not of the estimator.

### The covering decision does not inherit that error

The relevant question is whether a wrong ρ produces a wrong covering. We
measured the cost model's chosen order against **every** fixed order 4–13, at
the same places, three repetitions, each method warmed on its own query:

| field | 10″ | 0.05° | 0.5° |
|---|---|---|---|
| 47 Tuc | 1.09 | 1.43 | 1.10 |
| Baade's Window | 1.04 | 1.17 | 1.16 |
| Galactic centre | 1.02 | 1.34 | 1.08 |
| ω Cen | 1.17 | 1.21 | 1.17 |
| N galactic pole | 0.88 | 1.58 | 1.48 |

(chosen ÷ best fixed order; 1.00 = the model picked the optimum.)

Two things follow. First, **the cost of picking a fixed order badly is 345×,
243× and 830×** at 10″, 0.05° and 0.5° respectively, and the best fixed order
moves from 12–13 at small radii to 7–8 at 0.5° — so no single fixed resolution
is defensible, which is the case for having a cost model at all.

Second, and directly answering the referee: at ω Cen and 47 Tuc, **where ρ is
wrong by 8–12×**, the chosen order is within 1.09–1.43 of the best — no worse
than at Baade's Window (1.04–1.17) where ρ is accurate to 20%. The two worst
cases (1.48–1.58) are at the *sparse* galactic pole, where the estimate was
good. Estimator accuracy and decision quality are essentially uncorrelated,
because *s\* ∝ √ρ*: an order of magnitude in ρ is a factor 3 in cell size, about
1.5 orders, and the cost curve is flat over that range.

The paper now makes this the claim, in place of the previous assertion that the
estimator is accurate enough.

---

## Major 5 — real astronomical positions

> *"If feasible, additional tests with at least one real catalogue of positions
> would strengthen the paper considerably."*

Done, and it changed a result. Two new corpora, both real Gaia DR3 positions:

- **10 million all-sky positions**, `random_index < 10^7`. Because
  `random_index` is a precomputed random permutation, this is a uniform random
  *thinning* of the true point process: every position is a real measured
  position and the two-point structure is preserved at **every** angular scale,
  not only above the 0.11° cells of the density map we used before.
- **1.6 million positions in eight complete fields** — ω Cen, 47 Tuc, M4, M13,
  Baade's Window, the Galactic centre, the LMC, and an off-cluster control —
  at full density, since thinning dilutes exactly the crowding the referee
  asks about.

What reproduces, on real positions: the index-size result (22.5 B/row for
skycell and for Q3C, 61.0 B/row for pgSphere on the same 10M rows), and the
degree-scale advantage (0.64–0.69 of pgSphere at 1°).

**What does not reproduce is the small-cone claim.** On the resampled corpus we
reported the sub-arcminute comparison against pgSphere as level. On real
positions skycell is **1.0–1.8× slower** at 10″ and 0.1°. We checked that this
is not an artifact of using an expression index rather than a stored key column:
the two differ by 0.47, 1.14 and 0.99 at the three radii, i.e. in no consistent
direction. The deficit is a property of the data, not of the index form. §6.2
now reports the real-position numbers as the primary result and the resampled
ones as the earlier, structurally poorer corpus.

We think the referee's diagnosis of *why* is right: sub-cell clustering
increases the number of rows a coarse covering admits, and it is exactly that
structure the resampled corpus lacked.

---

## Question 1 — where does the advantage come from?

> *"Is the observed advantage primarily due to the adaptive covering resolution,
> the smaller B-tree representation, heap clustering, or the interaction?"*

We separated the three, on real Gaia positions.

**Heap clustering is a large absolute effect and almost no differential one.**
The same 10M rows were built twice: in cell order (clustered) and in
`random_index` order, which is random on the sky (unclustered). Cost of the
unclustered layout, per method:

| radius | pgSphere | skycell | Q3C |
|---|---|---|---|
| 10″ | 1.55 | 1.38 | 0.97 |
| 0.1° | 1.69 | 1.66 | 0.97 |
| 1° | 2.22 | 2.08 | 1.20 |

Clustering is worth about a factor two at 1°, but it is worth nearly the same to
pgSphere as to skycell, and the skycell/pgSphere ratio barely moves with it
(0.64 clustered, 0.60 unclustered). **The paper's use of physically ordered
tables therefore does not bias the comparison**, which is what minor 3.8 asks.
Q3C is nearly immune only because its own per-query cost dominates its heap
access.

**Covering adaptivity is worth up to a factor 830** and cannot be replaced by a
fixed resolution (table above).

**Index representation** is isolated by comparing against Q3C rather than
pgSphere: both are B-trees over an int8 key, so the difference between them is
the covering strategy alone, with the index structure held fixed. On this
corpus skycell is 0.02–0.57 of Q3C 2.0.5 — but see major 3: that comparison is
dominated by a property of that Q3C version, not of the quad-tree key.

---

## Major 1 — novelty and positioning

We accept that the manuscript asserted a combination without establishing which
parts are new. §1.2 is rewritten around the table below, and the claim is now
narrow enough to be falsified.

| approach | key | resolution strategy | index | regions |
|---|---|---|---|---|
| HTM | trixel id | fixed depth, covering by recursion | B-tree | cone, polygon |
| Q3C | cube quad-tree ipix | fixed depth; fixed range count per query | B-tree | cone, ellipse |
| pgSphere | geometry | none (bounding boxes) | GiST | general |
| MOC | NUNIQ set | multi-order, but authored, not query-derived | set ops | stored regions |
| S2 / H3 | cell id | covering to a *caller-supplied* cell budget | B-tree/KV | general |
| **skycell** | HEALPix order-29 | **cell size minimising a cost balancing range count against false-positive rows, from the relation's own statistics** | B-tree | cone, convex polygon, stored MOC |

What we claim is new is the **resolution rule**, not the ingredients: covering a
region to a depth chosen by minimising *n·c_range + ρ·ΔA*, where ρ comes from
the indexed relation's existing statistics and *c_range* from the planner's cost
model, so that neither is a tuning knob. S2 and H3 take a cell budget from the
caller; Q3C fixes the range count in the implementation; MOC's multi-order
structure describes a region that is already known rather than one being
searched for.

**A caveat we ask the editor to weigh.** We have not been able to perform an
exhaustive literature search of the database-systems literature, where a
cost-based covering over a hierarchical spatial key may well have been described
under different terminology. We state the claim in the specific form above so
that a single counter-example refutes it, and we would welcome the referee
pointing us to one.

---

## Major 6 — separating proof from validation

Adopted in full. §4 is now explicitly three-tiered:

1. **Proven.** For a cone, no cell whose centre lies further than *r + R_cell*
   from the cone centre can intersect it, where *R_cell* is the exact bound on a
   HEALPix cell's circumradius at that order. This is the only part that is a
   theorem, and it holds at every order and every position, including the polar
   caps and face boundaries.
2. **Numerically validated, not proven.** The floating-point margin
   (`SC_ANG_EPS = 2·10⁻¹³`) is a bound we tested, not one we derived through the
   whole classification path. The *inclusion* test (all four corners and the
   edge midpoints inside) is also validated rather than proven: it is
   conservative in the direction that matters — a cell wrongly called
   "partially inside" is refined, not dropped — but we do not have a proof that
   it never wrongly calls a cell fully inside.
3. **Regression-tested.** Agreement with brute force over the adversarial corpus
   (poles, RA=0, face boundaries, degenerate and nearly-degenerate polygons),
   and agreement of the indexed answer with a sequential scan for every query in
   the benchmark suite.

We note, as the referee does, that the earlier version of this work stated (1)
with a bound that was *not* a bound — a factor derived from a bulge argument
which we measured at 64× at order 18. It was found by the brute-force test in
tier 3, not by inspection, which is the practical argument for keeping the three
tiers separate.

Scope of the guarantee, now stated in §4.1: cones of any radius; convex
spherical polygons with vertices given in either winding order; regions up to
but not including a hemisphere. Concave polygons, regions larger than a
hemisphere, and unions of regions are **not** supported and now say so in the
abstract (minor 3.4, 3.5).

---

## Major 3 — fairness of the Q3C and pgSphere comparison

We accept this and have changed how the Q3C result is presented, because the
referee is right that our own evidence undercuts the general reading.

The 4–50× advantage over Q3C we reported is **a property of Q3C 2.0.5's
`q3c_radial_query`**, which expands every cone into 100 key ranges whatever the
radius. That is an implementation choice in one version, not a property of the
cube quad-tree. The results are now split:

- *Against `q3c_radial_query` in 2.0.5*: skycell is 0.02–0.57 of its time on the
  real Gaia corpus. Reported as a version-specific implementation comparison.
- *Against `q3c_join`*: level (intervals reach 1.00). This is the comparison we
  regard as informative about the indexing approaches, because `q3c_join` is
  what the Q3C authors intend for that workload and it does not carry the
  fixed-100-range behaviour.
- *About the quad-tree approach generally*: **no claim.** A covering-aware
  `q3c_radial_query` would plausibly close most of the cone gap, and we say so.

pgSphere was used with its documented `spoint`/`spoly` types and a GiST index on
`spoint`, which is the configuration its documentation prescribes; we are not
aware of a tuning option we have left unset, but we did not attempt to tune it
beyond defaults and now say that explicitly.

We did not test prepared statements or plan reuse for any method (major 3, last
bullet). Since skycell's planning cost is the component that most differs
between methods, plan reuse would help skycell more than its rivals, so our
numbers are the conservative case. §5.2 now states this.

---

## Major 7 — cross-matching: method versus formulation

> *"Present the cross-match results with greater separation between the indexing
> method and the query formulation."*

Done, on real Gaia positions, across outer-table size (1k, 100k), radius (0.2″,
1″, 30″) and target distribution (drawn from the catalogue, so clustered; and
uniform on the sphere). Randomized method order within each cell, each method
warmed on its own query, three repetitions.

Two findings, one of which corrects the manuscript.

**The claim that survives.** Against `q3c_join` the two are level: the ratio
ranges from 0.60 to 1.73 across the twelve cells with no consistent winner.
Against pgSphere skycell is 2–6× faster throughout. Both reproduce what the
paper reported on the resampled corpus, now on real positions and over a much
wider matrix.

**The claim that does not.** The manuscript describes the `LATERAL` form as
"the fastest one measured". That is true only at small radii. At 30″ with
clustered targets it takes 5682 ms against the fixed-slot form's 896 ms — six
times worse — while at 0.2″ it is the best of the four. The ordering between
*our own two formulations* reverses with radius, which is precisely the referee's
point: the formulation is a variable in its own right and cannot be folded into
a statement about the index. §6.5 now reports both forms across the matrix and
says which regime each suits.

**A methodological note we think worth recording.** Our first version of this
sweep ran the four formulations in a fixed order with no per-method warm-up. It
made whichever formulation ran third appear 6–45× faster than it is — in that
run `skycell_join` looked 6.7× faster than `q3c_join` at 100k probes, where the
corrected measurement puts it 1.7× slower. This is the same confound that
Sect.~5 was written to remove, and we reproduced it in a new script a week after
writing that section. We report it because it bears on how much weight any
single cross-match number in this literature should carry.

No formulation was planned as a sequential scan in any of the 36 measurements,
and none timed out, which localises the planning defect of major 9 to wide
relations rather than to the range-join formulation itself.

---

## Major 8 — the Gaia-scale extrapolation

Every extrapolated quantity is now labelled as an estimate, with its assumption
stated, and the "server RAM to keep it warm" column is gone from the headline
table — it conflated an index-size arithmetic with a deployment requirement,
which is exactly the conflation the referee's closing section warns against.

What we retain: per-row index cost is flat to 0.5% between 10M and 50M rows and
reproduces on real positions (22.5 B/row on 10M real Gaia sources, against 61.0
B/row for pgSphere on the same table), so **index size** extrapolates with a
stated assumption. What we withdraw: any statement about the memory an archive
needs, which depends on heap residency, partitioning, concurrency and the rest
of the workload.

We can now add a measured caveat rather than a hypothetical one. At 10M rows, in
a container where *queries ran without difficulty*, the index **build** path was
OOM-killed three times (`ANALYZE` at a high statistics target, a 10M-row
`UPDATE`, and finally the checkpointer, which took the server down). Build
memory, not index size, was the binding constraint at this scale. At 1.8×10⁹
sources it is the constraint an archive would meet first, and the size
extrapolation says nothing about it. §6.4 now says so.

---

## Major 9 — better estimates are not better plans

Accepted, and the claim is narrowed to what we measured. The manuscript now
says: skycell produces row estimates closer to truth (1.22× median error against
1.9× for Q3C and 2.8× for pgSphere) **and that this did not change the chosen
plan in all but one of the shapes we tested**. The sentence suggesting improved
estimates act as insurance against planner error has been removed; it was
speculation.

We can offer one concrete case where estimation quality does change a plan, and
it runs against us. On a wide ObsCore relation the cross-match range join
(`cell BETWEEN lo AND hi` with non-constant bounds) has **no** selectivity
estimator, so PostgreSQL falls back to a ~11% default and predicted 2.7×10⁹ rows
against a true 3642. The planner then chose a sequential scan that did not
finish in 90 s, over an index path that runs in 145 ms. This is a defect in our
integration, not a success of it, and §5.4 now records it as such along with the
workaround.

---

## Major 10 — reproducibility

`REPRODUCING.md` is new and covers every item on the referee's list: exact
versions (PostgreSQL 18.6, gcc 14.2.0, Q3C 2.0.5, pgSphere 1.5.2, skycell 0.6),
build options, host and VM configuration, server settings with the reasoning for
the non-default ones, corpus generation and seeds, index creation, warm-up and
cache-dropping procedure, the analysis scripts, and a section on
non-deterministic behaviour.

On maturity: the extension is stated to be a prototype, with no API stability
guarantee and no operational deployment experience.

On AI assistance: we agree with the referee's framing and have adopted it. The
acknowledgement now says that AI assistance is not evidence of correctness, and
points the reader to the brute-force comparisons and independent re-derivations
as the things to inspect. Two of the errors corrected in this revision cycle —
an unsound geometric bound, and a density-estimator claim that did not survive
real positions — were found by those tests.

---

## Minor comments

**3.1 Index vs covering resolution.** Clarified in §2.1: every position is
stored as its order-29 cell and nothing else; coarser orders appear only as
*ranges* of order-29 ids in the covering. The index has one resolution; the
covering chooses among many.

**3.2 The cost function.** Equation (2) now carries units (*c_range* in units of
the cost of scanning one row; ρ in rows sr⁻¹; ΔA in sr) and a worked example: a
0.1° cone at the Galactic-centre density, costed at orders 8, 10 and 12, showing
the minimum at 10 and the cost within 20% of it over orders 9–11 — which is why
a factor-two error in either input does not move the answer.

**3.3 The exact predicate.** §2.4 now states that it runs on every candidate row
the ranges produce, in ICRS, with the same angular tolerance as the
classification, and that no filtering happens between the ranges and the exact
test.

**3.6 Confidence intervals.** The polygon and stored-footprint comparisons now
use the same paired-trial protocol and bootstrap intervals as the cone
benchmark. Where a measurement is a single run we now say so instead of
reporting a median as if it were an estimate with an interval.

**3.7 Cache state vs index size.** Separated. The cold-cache advantage is now
reported as what it is — fewer pages touched *and* a smaller index *and* a
storage subsystem whose behaviour we measured once — rather than attributed to
index size. The memory-pressure experiment (§6.4) is the one place we can
attribute it, because there the index size is the only variable that changes.

**3.9 Planning vs execution vs end-to-end.** Every performance statement now
names which of the four it refers to. The TAP result is the clearest case: a
1.2–1.45× database-level difference is a 0.96–1.06 service-level difference,
and both numbers are correct.

**3.10 Index maintenance under positional updates.** Added to §5.5. A position
change moves a row's key and therefore its index entry, so bulk astrometric
reprocessing rewrites the index — the same cost as a rebuild, and the reason we
now report build time as an operational quantity rather than a curiosity. We do
not have measurements of incremental proper-motion updates and say so.

**3.11 Astrometric propagation.** §2.6 now separates *supported* from
*performance-tested*: epoch propagation and frame transformation are
implemented and regression-tested for correctness, and are not in any benchmark.

**3.12 Stored footprints.** §6.6 now reports how the MOC refinement parameter
trades index size against ancestor lookups, and notes that the stored-region
path has different scaling from the point-catalogue path — it is linear in the
number of stored cells, not in the number of rows.

**3.13 "Unbiased".** Withdrawn; see major 4.

**3.14 Limitations in the abstract.** The abstract now names the three that
bound every number in the paper: one machine, a prototype implementation, and
(until this revision) no real catalogue positions.

---

## Questions 2–5

**Q2 — stability of the covering across sky-density distributions.** Answered by
the ablation above: the chosen order is within 1.02–1.58 of the best fixed order
across four decades of surface density, including cluster cores where the
density input is wrong by an order of magnitude.

**Q3 — operational cost.** Partly answered, and we are explicit about the part
that is not. Build time and build *memory* are measured (the latter by having
exceeded it). Behaviour across repeated `ANALYZE` on a changing distribution is
not measured, and the cost model reads whatever statistics exist, so a stale
histogram gives a stale ρ — which, by the ablation, matters less than one might
fear, but we have not measured it directly.

**Q4 — generalising beyond PostgreSQL.** The SP-GiST direction is a *proposal*,
not a result. It addresses a demonstrated bottleneck — planning cost that grows
with density, 3% of query time at 10M rows and 13% at 50M — but we have not
implemented it and cannot say it would work.

**Q5 — expected benefit for real archives.** Stated as conditions rather than a
general claim. The benefit is largest when: the query stream sweeps enough sky
that index residency matters; regions are degree-scale or larger; the archive
would otherwise carry both pgSphere and Q3C; or the service is thin enough that
database time is a material fraction of request time. It is smallest, or
negative, for sub-arcminute cones on a thin service — which is the common case
for a simple cone-search endpoint, and we say so in the conclusions.
