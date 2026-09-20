# Response to the referee

We thank the referee for a report that was specific enough to act on. Two of its
requests changed the paper's conclusions and one changed the software: the controlled
protocol reduced the headline advantage we had claimed, and the request for a formal
correctness statement led us to find, and remove, an unsound bound in the cell
classification. Both are described below and in the manuscript.

Everything quoted here is reproducible from the repository
(<https://github.com/jesusjuansalgado/skycell>); the new experiments are
`bench/07_ab.sql` through `bench/10_crossover.sql` with `bench/ab_report.py`.

## Summary of what changed

1. **The benchmark protocol was rebuilt** (referee §4.1, experiment 1). Methods are no
   longer measured in phases. Every query is now a trial in which all three methods run
   back to back in a randomized order, repeated five times, analysed paired, with
   bootstrap intervals, and repeated with caches dropped. **This reduced our claimed
   advantage**: what was "faster at every radius by 15–55%" is now "6–11% at
   1″–1′, indistinguishable between 6′ and 1°, 31–37% at 3°" warm — and a clear win at
   every radius only when caches are cold. Section 6.2 reports the size of the protocol
   effect explicitly.
2. **A second corpus** (§4.2, experiment 2), resampled from the real Gaia DR3 density
   field (source counts per order-9 cell fetched from the ESA archive; they sum to the
   published DR3 total). Every cone result is reported on both corpora. On the Gaia
   field skycell is 3% *slower* at 6′, which we report.
3. **The cost model is now characterised, not asserted** (§4.3, experiment 3):
   `c_range` defined with units and swept over four decades on both corpora; the
   density estimate compared against counted truth; and, using a new
   `skycell.force_order`, the model's chosen order compared with the empirically best
   order for the same query.
4. **A correctness statement with a proof sketch** (§4.6, experiment 5), and an error
   found while writing it: the previous OUT test was not conservative. See below.
5. **Estimates, plans and execution separated** (§4.5, experiment 4), with an honest
   negative: better estimates did not change plan choice on the shapes we ran.
6. **The crossover claim is measured, not asserted** (§4.7/§5.4, experiment 6): the same
   ObsCore-shaped relation at three sizes.
7. Presentation: a notation table, the key encoding and its relation to NUNIQ, a
   reproducibility table, a maturity column on the query-surface table, a rescoped
   abstract and conclusions, and a fuller statement of the AI-assistance question.

## Point by point

### §4.1 Benchmark fairness and experimental ordering (critical)

Adopted in full, and it cost us the headline.

- *Randomized order rather than skycell first*: the method order is now drawn per trial,
  so no method is systematically first. Section 5.3.
- *Repetitions, dispersion, confidence intervals*: five repetitions per query warm;
  paired analysis (ratio formed within a trial, so query difficulty cancels); 95%
  percentile bootstrap over queries, 4000 resamples. Table 4 reports every ratio with
  its interval, and we call a ratio a difference only when the interval excludes 1.
- *Planning separated from execution*: Table 5.
- *Cold and warm separately*: the server is restarted (empty shared buffers) and the VM
  page cache dropped; only the first repetition is used. This turned out to be the most
  informative single change — skycell's advantage is much larger cold, because the
  buffer-count difference becomes I/O.
- *Distributions rather than medians*: intervals are given for every cell; the raw
  per-query records are in `bench/results-ab/bench_ab.csv`.
- *A control that equalises order and cache treatment across all three methods*: the
  trial structure does this by construction, and we additionally report the residual
  order effect (mean time by position in the trial): going first costs pgSphere 19% and
  skycell −5%, Q3C is flat.
- *Direction of ratios*: now stated explicitly in the caption and the text ("a value
  below 1 means skycell is faster"), with the figure axis labelled and annotated.

We also did what the referee asked in the last paragraph: the phase-ordered numbers are
no longer the primary evidence. Section 6.2 exists only to report how much they
differed, because that difference is larger than most of the effects in the paper.

### §4.2 The synthetic catalogue (critical)

We could not obtain a real 10-million-row catalogue and benchmark it within this
revision, and we say so. What we did instead:

- Built a second corpus from the **real Gaia DR3 density field** — counts in all
  3,145,728 order-9 cells, summing to 1,811,709,771, the published DR3 total — by
  giving each cell a number of sources proportional to its Gaia count and placing them
  uniformly inside it. The crowding structure (disk, bulge, Clouds, scanning-law
  texture) is real down to 0.11°; below that it is Poisson by construction, and the
  manuscript says so plainly (§5.1).
- Reported **every** cone result on both corpora, not a summary.
- Reported the cost-model calibration on both corpora, which is the specific claim the
  referee was worried about: the optimum `c_range` is the same on both, supporting our
  claim that it is a property of the machine rather than of the sky.

We also added a **second scale** (§6.2): the same Gaia density field at 50 million
rows, where pgSphere's index (3442 MB) exceeds `shared_buffers` (2048 MB) and
skycell's (1072 MB) does not. This tests the size argument rather than asserting it,
and it splits the answer by radius: for regions ≥30′ the advantage roughly doubles
(1°: 0.85 → 0.60 warm, 0.53 → 0.31 cold), while for cones ≤1′ nothing changes and
1′ becomes a wash (0.93 → 1.02), because both methods touch 5–8 pages at any table
size. We now state the rule as being about pages rather than rows.

Limitations we now state rather than imply: no real positions; no structure below
0.11°; a single machine; two scales, not a curve.

### §4.3A Calibration of `c_range` (critical)

- Defined with units in §2.2: both terms of the cost function are in rows, and
  `c_range` is the ratio between the cost of opening one more B-tree range scan and the
  cost of fetching and discarding one heap row. It is a property of the installation.
- Swept over **four decades** (0.3 to 1000) on both corpora, four radii
  (Table 7, Fig. 2c).
- Result: the default of 30 is *not* optimal — about 100 is, at 30′ and 1° — and the
  default costs 11–21% there. We now say this rather than reporting a narrow sweep that
  made the default look fine. The curve is flat-bottomed, so being wrong by a factor 3
  costs little; cold, the differences vanish into the noise of a single pass, so the
  parameter is a warm-cache refinement.

### §4.3B The density estimator (critical)

- The estimator is now derived explicitly (§2.3, Eqs. 3–4), with its units and its two
  assumptions stated.
- Compared against counted truth in the same footprint (Table 8): median
  ρ̂/ρ_true of 0.89–1.02 (designed) and 0.91–1.00 (Gaia), so unbiased, with scatter
  growing as the footprint empties.
- The referee's specific worry — a cluster smaller than a bucket — is exactly the case
  the area guard exists for, and we now explain the guard as the answer to it.
- We also quantify what estimator error *costs*, which we think is the more useful
  number: using `skycell.force_order`, the model's chosen order is about one order
  coarser than the empirically fastest, and that costs 0.8–2.0% of query time at the
  median, at most 11% at the 90th percentile. The cost curve is flat near its minimum,
  which is why an imperfect density estimate is tolerable.
- Below about an arcminute the comparison is undefined: the footprint the model reads
  holds half a row. We report that rather than a meaningless ratio.

We did not implement an alternative estimator (a MOC-with-counts `typanalyze`). The
force_order experiment bounds what one could buy — at most the few percent between the
model's choice and the best order — which we judged the more informative answer.

### §4.4 Interpreting the comparison (major)

- A reproducibility table (Table 3) now records host, storage, PostgreSQL version and
  settings, extension versions, the exact schema and index definition per method,
  clustering, maintenance, repetitions and the uncertainty method.
- The Q3C small-cone result is now explicitly attributed to that version's query path
  (2.0.5 expands every `q3c_radial_query` into 100 ranges regardless of radius) and not
  to the cube quad-tree, with the observation that Q3C's cross-match path does not pay
  it. §5.5 states that a different formulation or configuration could move these
  numbers.
- §5.5 ("What this protocol still does not control") is new and lists the residual
  threats, including that the comparison is of these configurations, not of the designs
  in general.

### §4.5 Planner integration (major)

Done, with a negative result we report as such (§6.5). Three shapes — cone plus
catalogue cut, cone joined to an observation table, three-way join — through all three
methods, recording estimate, plan strategy and time separately.

- Estimates: skycell 1.15–1.88 against Q3C 1.63–2.33 and pgSphere 2.50–3.00.
- Plans: identical strategy for all three methods on all 135 queries at 1′–1°. Only at
  3° did one query in sixteen cross a threshold, where skycell's estimate produced a
  hash join that ran 1.29× faster.
- Times: within noise of pgSphere in the join shapes.

We now distinguish the three claims explicitly and state that only the first is
demonstrated.

### §4.6 Correctness (major)

This request changed the software.

Writing the formal statement, we found that the previous OUT test was **not** a bound.
It inflated the corner chords by four times the edge's departure from its chord measured
at the edge midpoint; where an edge crosses its chord plane near the midpoint that
measurement collapses towards zero while the true departure does not. A dense scan over
every order finds the true/midpoint ratio reaching 64 (order 18). The absolute error was
~10⁻⁸ rad — far below a 0.4 mas leaf cell, and no test we had could see it — but it sat
exactly where the paper claimed a guarantee.

The OUT test now uses `R_cell = min(maxpixrad(k), max corner distance)`, which is
bounded, plus an explicit numerical margin of 2×10⁻¹³ rad on every OUT decision. Cost:
8.1 → 8.3 index ranges per covering, no measurable time. §2.4 describes this, including
the error, because the referee is right that readers need to know which kind of
guarantee they have.

The manuscript now contains:

- a **Proposition** with a **proof sketch** (§4.1), stating exactly which cell
  classifications are conservative and under which two conditions;
- the status of each condition: (i) is a HEALPix property, analytic in the reference
  implementation, verified here over 1.6×10⁶ boundary points at every order; (ii) is an
  assumption about floating-point error, with ε chosen five orders of magnitude above
  the plausible accumulated error and five below the leaf cell;
- an explicit note that the *tighter* corner term additionally assumes the
  distance-extremum is at a corner — tested, not proven — and that
  `skycell.exact_cells = off` makes the proposition rest on (i) alone;
- the statement that the exact predicate compensates for any over-inclusion, so only
  OUT can lose rows;
- new adversarial tests for the polygon and box path (the previous adversarial sampling
  covered cones only): 7266 polygons at poles, zone boundary, RA wrap and face edges,
  866,537 points drawn inside them, half within a part per billion of an edge, zero
  misses.

### §4.7 Scope of the query surface (major)

Table 2 now carries an evidence column with three levels — benchmarked, verified
(indexed answer required to equal a sequential scan), exercised (round-trip or
published-value check only) — so no reader can infer that epoch propagation has been
characterised to the same depth as the cone path. The astrometry functions are marked
"exercised" and we do not claim more.

### §5.1 Key encoding

§2.1 now states the encoding precisely: the plain nested index at a fixed order 29,
63 bits in a signed `bigint`, order implicit; the ancestor/descendant relation as an
explicit interval formula (Eq. 1); and the relation to NUNIQ (`4·4^k + p`), with the
note that we convert rather than store two representations.

### §5.2 Histogram-based density estimation

§2.3 now gives the equi-depth histogram, the interpolation formula, and the conversion
to rows per steradian, and distinguishes the four quantities the referee listed
(density in key space, density per solid angle, expected rows in a covering, and the
planner's selectivity estimate, which PostgreSQL computes separately).

### §5.3 Index build and maintenance

A new paragraph in §3 covers incremental ingestion, update behaviour, the absence of
method-specific rebalancing, the benefit of loading in key order, and the one real
operational difference: the covering depends on statistics, so `ANALYZE` matters more
than for a GiST index, bounded by the area guard.

### §5.4 Framing the TAP experiment

§7 now gives the workload composition (six classes, three concurrencies, three
repetitions), the tie rule (disjoint 95% intervals *and* >10%), the gates, and the
measured split between database and Python (1–3% of a request). And it now contains the
experiment that tests the explanation rather than asserting it — see experiment 6 below.

### §6.1 Abstract

Rewritten. It now states the controlled numbers, names the radii where the two methods
are indistinguishable and the one where skycell is slower, gives the cold-cache result
separately, and ends on the workload dependence rather than on a summary advantage.

### §6.2 Reproducibility

Raw results for every measurement are now committed (`bench/results/`,
`bench/results-ab/`), together with the harness, the hardware and configuration table,
and the scripts that regenerate every table and figure. We will tag a release matching
the accepted version.

### §6.3 AI-assisted development

The acknowledgement now states what was done about it rather than only that it
happened: the geometric predicates are checked against independent computations over
millions of adversarially placed points rather than against expected outputs; every
indexed answer in the regression suites must equal a sequential scan with the exact
predicate; the classification error described above was found by those tests rather
than by inspection; and the benchmark harness was audited by reproducing its headline
comparison under a different protocol — which is how the protocol effect of §6.2 came
to light.

### §6.4 Terminology

A notation table (Table 1) now precedes the method.

### §6.5 References

Added the Hierarchical Triangular Mesh, the zones algorithm, space-filling-curve
clustering analysis, and a TAP service implementation, and the introduction now places
the contribution relative to that work rather than only relative to the two extensions
benchmarked.

## The six suggested experiments

| # | Status |
|---|---|
| 1. Randomized ordering, repetitions, intervals | Done. Now the primary evidence (§5.3, §6.1). |
| 2. Independent density distributions | Done for a real *density field* (Gaia DR3 counts), at two scales (10M and 50M rows). Not done for real catalogue positions; stated as a limitation. |
| 3. Cost-model sensitivity | Done: `c_range` over four decades on both corpora; area guard and density model on/off; plus the order-by-order cost curve via the new `force_order`. Histogram resolution was not swept. |
| 4. Query planning and joins | Done (§6.5), with a negative result. |
| 5. Correctness boundary tests | Done, and it found a real defect (§2.4, §4). |
| 6. Realistic archive workload | Done as a database-level crossover measurement on an ObsCore-shaped relation at three sizes (§7). We did not re-run the end-to-end TAP rungs at 10M rows: the existing measurement already shows the database is 1–3% of a request, so the rungs cannot move, and the crossover experiment tests the claim that was actually in question. |

## The referee's proposed conclusions (§8)

Adopted essentially verbatim as the structure of §8 and §9. The conclusions now state
the workload dependence, the measured crossover, the TAP result as a separate
deployment finding, and the need for broader validation. We have also added a
subsection, "What they do not [support]", which lists the negative results in one
place: slower on small relations, slower on small cones over field-clustered data,
indistinguishable at 6′–1° warm, slightly slower at 6′ on the Gaia field, no
demonstrated plan improvement, one machine only.

## Added after the first revision: the Gaia Archive references and cross-match

Two further points, raised after the report:

- The introduction now places the work against the **ESA Gaia Archive**
  \citep{gaiaarchive1, gaiaarchive2}: an archive serving order 10^9 sources through an
  extended TAP interface, whose published description puts cross-match among the
  server-side capabilities implemented beside the query interface, under a
  "move the code close to the data" principle. That is the motivation for treating
  cross-match as a first-class result rather than an afterthought.
- **Cross-match is now measured with the same protocol as the cone searches** (§6.5,
  Table 9), and this corrected a claim. The previous number — a single end-to-end run
  per method — reported skycell at 6–11× pgSphere and well ahead of `q3c_join`. That was
  an artefact of cache residency: at 50M rows the catalogues do not fit in
  `shared_buffers`, the same block query costs 6.2 s cold and 0.32 s warm, and whichever
  method touched a block first paid for the rest. Warming each method on its own block
  before timing it, and pairing over 16 block-repetitions, gives: `q3c_join` 1.23 s,
  skycell `LATERAL` 1.08 s (ratio 0.78, interval reaching 1.00), skycell join form
  1.50 s, pgSphere 2.95 s. **skycell is Q3C's equal at cross-matching, not its better**,
  and 2.5–3.3× faster than pgSphere. We report the discarded number and why it was
  wrong.

## Requests we did not fulfil

We prefer to name these rather than leave them to be discovered:

- **Real catalogue positions.** We used a real density field, not real astrometry.
- **More than one machine or storage device.** All numbers remain from one laptop-class
  host; absolute values should not be transported.
- **An alternative density estimator**, implemented and compared. We bounded what one
  could gain instead.
- **Histogram resolution sweep.** Not run; the statistics target is fixed at 1000
  throughout and recorded in the reproducibility table.
- **End-to-end TAP rungs at 10M rows.** Argued above to be uninformative; the
  database-level crossover is measured instead.
