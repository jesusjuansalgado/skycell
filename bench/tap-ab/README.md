# skycell inside a TAP service (A/B)

The measurements in Sect. 7 of the paper ("Inside a TAP service"): the same
skycell build, exercised through [egernia](https://github.com/ska-telescope/egernia)
against its published 500,096-row ObsCore corpus, in an A/B where one database
holds both indexes and two otherwise identical service processes differ only in
how they translate ADQL `CONTAINS`.

- [PROTOCOL.md](PROTOCOL.md) — what was run, how, and the rules for calling a
  cell a win, a loss or a tie (disjoint 95% CIs *and* a margin of at least 10%).
- [RESULTS.md](RESULTS.md) — gates, database-level medians, the end-to-end
  rungs, and why the answer is a tie at this corpus size.

The harness itself (translator switch, `docker-compose.skycell-ab.yml`,
`dbbench.py`, `abreport.py`, scenario and target definitions) lives in the
egernia fork it patches, under `benchmarks/tap-compare/skycell-ab/`, because it
is an egernia deployment rather than a skycell one.

Short version: **at 500k rows skycell is 1.2–1.45× slower per query than
pgSphere and all 18 end-to-end cells tie**, because the database is 1–3% of a
TAP request there. The same build wins at 10M rows (see
[../results/summary.md](../results/summary.md)). The crossover is the size of
the relation, not the query.
