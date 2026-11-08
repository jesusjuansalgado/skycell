# Tutorials

Each tutorial is a sequence of SQL blocks that runs as written, in order, on an
empty database, with the plans it promises (`-- expect:` lines in the blocks).
They assume skycell is installed on the server (`make install`; see the
[README](../../README.md#installation)).

1. [A source catalogue](01-source-catalogue.md): installing, indexing, cone,
   polygon and region searches, cross-matches, nearest neighbours.
2. [Regions and footprints](02-regions-and-footprints.md): the `skyregion`
   type, an ObsCore `s_region` column, the region indexes, footprints against
   positions, fields and catalogues.
3. [Density maps, counts and MOCs](03-density-and-mocs.md): estimates without
   reading rows, exact per-cell counts, MOCs out and in.
4. [Large catalogues](04-scaling-parallel-partitions.md): parallel queries,
   partitioning by sky region, upkeep, reading a plan.
5. [ADQL, TAP services and migration](05-adql-tap-and-migration.md): what a
   TAP service needs, the IVOA user-defined functions, moving from Q3C or
   pgSphere.

## Checking them

`check.py` runs every tutorial in a fresh database named `skycell_tutorial`,
which it drops and creates again, and reports the first failing block or unmet
expectation:

```bash
PGHOST=/tmp PGPORT=5432 PGUSER=postgres docs/tutorials/check.py
```

It needs a superuser (the set-up blocks set `session_preload_libraries`) and
takes about ten seconds.
