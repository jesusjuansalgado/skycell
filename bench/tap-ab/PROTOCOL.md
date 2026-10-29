# skycell A/B: does a B-tree cell index improve egernia's cone searches?

Written before the first measurement (2026-09-19). Not a registered protocol
of the paper: no tag, and it runs on developer hardware. It borrows the final
comparison's workload verbatim so its numbers can be read beside the paper's
tables, but it answers a narrower question.

## Question

egernia translates `CONTAINS(POINT(s_ra, s_dec), CIRCLE(ra, dec, r))` to
pg_sphere (`spoint(...) @ scircle(...)`), answered by the expression GiST
index `obscore_spoint_gist`. The skycell prototype extension (unpublished,
vendored in `db/skycell/`) offers
`skycell_cone(skycell_ang2cell(s_ra, s_dec), s_ra, s_dec, ra, dec, r)`. A
planner support function turns that into B-tree range conditions on an
expression index over HEALPix cell ids, plus an exact test, with a covering
sized from the index's ANALYZE histogram.

Does switching the translation change egernia's measured numbers on the
paper's corpus and query classes?

## Design: one database, two APIs, one difference

- **One database** (`db/Dockerfile`): egernia's pinned `postgres:18` +
  pgsphere image, plus skycell. After seeding, `setup.sql` adds
  `obscore_skycell` beside egernia's own indexes. It is outside egernia's
  `INDEXES`, so the relation's fingerprint and bootstrap do not change.
- **Two API services from one image** (`../docker-compose.skycell-ab.yml`):
  - `tap-api` on :8080 is the release translation.
  - `tap-api-skycell` on :8082 is identical except `TAP_ADQL_SKYCELL=true`.

  The switch changes only point-in-circle `CONTAINS` on two column references. Every other geometry keeps pg_sphere (`tests/unit/test_adql_skycell.py`).
- **Interleaving:** the harness interleaves the two targets rung by rung (A,B,A,B), so one target is under load at a time, and both read the same shared buffers.

## Fixed from the paper

`corpus`, `mix` and `guards` are `final/scenarios.yaml` verbatim:
- corpus seed 424242, 400 combinations, 3906 projects;
- the 1:3:3:2:5:2:2:1 mix;
- the 60% generator guard.

The corpus is the D1 seed, which must give 500096 `ivoa.obscore` rows (`run.sh` refuses otherwise). Both gates run: taplint and the agreement gate. Agreement between the two targets is the correctness check of the translation.

## Deviations from the paper (stated, not corrected)

- **Host.** One Docker Desktop VM with 11 vCPUs and 8 GB. The load generator runs on the macOS host and shares the physical cores with the VM, so there are no disjoint cpusets and absolute throughputs are not comparable to Tables 2–3. Only the A/B ratio is interpreted.
- **Budget.** Stack cpuset 0–7 with 3 GiB db (shared_buffers 768 MB by the quarter rule), 1 GiB per API, 1 GiB executor. One uvicorn worker per API: the shape of Table A.
- **Grid.** Classes `mix`, Q03 (control), Q05, Q06, Q07, Q12; CSV only; c ∈ {1, 8, 32}; 3 repetitions; 10 s warm-up + 40 s windows; 108 rungs. Verdicts use the paper's tie rule (disjoint 95% intervals and ≥ 10%).
- **Generator.** `runner._client` is built with `trust_env=False`: on this host httpx's proxy discovery segfaulted the generator's worker processes through macOS SystemConfiguration. Every target is a localhost port, so no proxy can apply.
- **Database level.** `dbbench.py` also times every cone-class corpus entry inside PostgreSQL with `EXPLAIN ANALYZE`, both translations, median of 5, with row counts compared.

## Hypotheses (before measuring)

- **H1 (database).** skycell lowers execution time for Q05, Q06 and Q07, and raises planning time (the covering is computed at plan time). Q12 (0.0002° cones, empty) is equal within noise.
- **H2 (end to end).** Effects are small. The paper measures ~10 ms of CPU per request, mostly Python (translation, serialisation), and sub-millisecond cone queries on 500k rows. At one worker the API is the bottleneck, so throughput differences on cone classes should be < 10%: mostly ties under the paper's rule.
- **H3 (control).** Q03 ties at every concurrency.
