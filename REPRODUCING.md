# Reproducing the measurements

Everything in the paper is produced by the scripts in `bench/`. This file records
the environment they were run in, the order they run in, and the things that are
not deterministic — so that a difference between your numbers and ours can be
attributed rather than guessed at.

## 1. Environment

| | |
|---|---|
| Host | Apple M3 Pro (Mac15,6), macOS |
| Virtualisation | Docker Desktop 28.3.2, Linux 6.10.14-linuxkit aarch64 VM |
| VM memory | 7.65 GiB total, **shared with unrelated containers** (see §6) |
| Container limit | `--memory 3g --memory-swap 3g` |
| PostgreSQL | 18.6 (Debian 18.6-1.pgdg13+2), aarch64, gcc (Debian 14.2.0-19) 14.2.0 |
| Storage | single virtual disk on the host SSD; no separate device for WAL |
| Q3C | 2.0.5 |
| pgSphere | 1.5.2 |
| skycell | 0.6 (this repository) |

Extension build options: PGXS defaults, `PG_CPPFLAGS = -DSKYCELL_PG`, no
`-march` or LTO flags, `with_llvm=no` in the container image.

## 2. Server settings

Set on the command line; everything else is a PostgreSQL 18 default.

```
shared_buffers = 2GB            (262144 × 8 kB)
effective_cache_size = 5GB      (655360 × 8 kB)
random_page_cost = 1.1
seq_page_cost = 1
work_mem = 64MB
maintenance_work_mem = 1GB
max_parallel_workers_per_gather = 0     -- parallelism off, so that per-query
jit = off                               -- cost is measured, not scheduling
```

`max_parallel_workers_per_gather = 0` and `jit = off` are deliberate: both add
variance that is unrelated to the index being measured. Results with either
enabled are **not** covered by this paper.

The memory-pressure experiment (§6.4 of the paper) overrides these with
`shared_buffers = 768MB`, `effective_cache_size = 2GB` in a separate container.

## 3. Corpora

| corpus | rows | what it is |
|---|---|---|
| `src_designed` | 10M | synthetic, 40% uniform + 60% in six clusters incl. both poles and RA=0 |
| `gaia_map`-resampled | 10M / 50M | positions drawn from the **real Gaia DR3 density field** (counts per order-9 cell from the ESA archive); structure only above ~0.11° |
| `gaia_realc` / `gaia_realu` | 10M | **real Gaia DR3 positions**, `random_index < 10^7`: a uniform random thinning of the true point process, so structure is real at every scale |
| `gaia_fields` | 1.6M | **complete** Gaia DR3 in 8 fields (ω Cen, 47 Tuc, M4, M13, Baade's Window, Galactic centre, LMC, off-cluster control) — full crowding, not thinned |
| `oc20` | 20.5M | ObsCore-shaped, 23 columns, 22 GB heap, positions resampled from `gaia_map` |

`gaia_realc` is in cell order (**clustered**); `gaia_realu` is in `random_index`
order, i.e. random on the sky (**unclustered**). They hold the same rows, so
heap layout is an isolated variable.

Fetching the real positions: `bench/19_gaia_real.sh` (ESA TAP, ~700 MB, into
`$GAIA_DIR`, default `/tmp/gaia`); loading them into `gaia_realu`, `gaia_realc` and
`gaia_fields` with the §4 indexes: `bench/19_gaia_load.sh` (~5 min). A rebuild
should give exactly 10,000,000 / 10,000,000 / 1,593,958 rows (DR3 is frozen).
The `gaia_map` density map the resampled corpora are drawn from (source counts per
order-9 cell) is committed as `bench/data/gaia_map_hpx9.csv.gz` (3,145,727 non-empty
cells summing to 1,811,709,771; the one empty cell is the rho Ophiuchi dark cloud core)
and re-fetched by `bench/19_gaia_map.sh`. Load it as `gaia_map (hpx9 bigint, n bigint)`
before `bench/11_scale_corpus.sql`.
For a cold-cache comparison, `bench/19_gaia_load_split.sh` loads the same all-sky
rows into one table per method instead (`gaia_real_cell`, `gaia_real_sphere`, as
`02_build.sql` does for the synthetic corpora), so neither method warms the other's
pages; see GIST_REGION_DESIGN.md round sixty-nine.
Generation of the synthetic corpora: `bench/01_data.sql`, seeds fixed with
`setseed()` and recorded in each script.

## 4. Index creation

```sql
CREATE INDEX t_cell ON t (skycell_ang2cell(ra, dec));
ALTER INDEX t_cell ALTER COLUMN 1 SET STATISTICS 1000;   -- see §6
CREATE INDEX t_q3c  ON t (q3c_ang2ipix(ra, dec));
CREATE INDEX t_gist ON t USING gist (pos);               -- pos spoint
ANALYZE t;
```

Build tables with `CREATE TABLE ... AS SELECT`, not `ALTER TABLE ADD COLUMN`
plus `UPDATE`: the latter rewrites every row and was OOM-killed at 10M rows in a
3 GiB container with `maintenance_work_mem = 1GB`.

## 5. Running

```bash
bench/run.sh                 # 01..06: build corpora, indexes, the phase-ordered grid
psql -f bench/07_ab.sql      # randomized paired trials  (the protocol the paper uses)
psql -f bench/08_costmodel.sql
psql -f bench/09_joins.sql
(cd bench && psql -f 10_crossover.sql)   # reads data/oc_fields.csv.gz, data/oc_queries.csv
psql -f bench/11_xmatch_ab.sql
psql -f bench/13_pressure.sql        # driver: bench/13_pressure_run.sh
psql -f bench/14_shapes.sql
psql -f bench/16_sensitivity.sql     # cost parameters, density estimator
psql -f bench/17_ablation.sql        # clustering / covering / index representation
psql -f bench/18_xmatch_sweep.sql    # outer size × radius × target distribution
psql -f bench/29_estimator.sql       # density estimate vs truth per field (tab:estimator)
python3 bench/ab_report.py           # paired ratios, bootstrap intervals
```

Cold-cache runs need the server restarted **and** the VM page cache dropped
(`echo 3 > /proc/sys/vm/drop_caches` in a privileged container); `bench/run.sh`
does both. A PostgreSQL restart alone only clears `shared_buffers`.

## 6. Things that are not deterministic, and how we handle them

- **Measurement order.** Measuring one method's whole set and then the next
  gives whichever went second a warmer cache; on the ObsCore corpus that was
  worth more than the difference between the methods and reversing the order
  reversed the verdict. Every comparison is therefore a *trial* in which the
  methods answer the **same** query back to back in an order drawn per trial,
  analysed paired.
- **Other containers on the VM.** The host runs unrelated containers using
  ~4.3 GiB of the 7.65 GiB VM. Absolute timings depend on what else is running;
  paired ratios inside a trial do not.
- **A concurrent long-running transaction breaks index use.** `CREATE INDEX` on
  a table with broken HOT chains sets `indcheckxmin`, and PostgreSQL silently
  ignores such an index until its xmin falls below the oldest active snapshot.
  A benchmark left running in another session therefore makes the regression
  suite fail with seq-scan plans. Check `pg_stat_activity` before concluding
  anything from a plan that ignores an index.
- **`shared_buffers` must leave room inside the container limit.** This server
  ran `shared_buffers = 2GB` inside a `--memory 3g` container, which leaves under
  1 GiB for backends, WAL and the checkpointer. Three separate operations were
  OOM-killed (signal 9) in that configuration: `ANALYZE` with a statistics target
  of 10000, a 10M-row `UPDATE` adding a `spoint` column, and finally the
  *checkpointer*, which took the server down and forced crash recovery. Queries
  were unaffected throughout — it is the maintenance paths that need the
  headroom. The container limit was raised to 5 GiB; `shared_buffers` was left at
  2GB so that measurements stay comparable across the paper.
- **`ANALYZE` with a statistics target of 10000** was OOM-killed on a 10M-row
  table in this container. Results are reported at targets 10, 100 and 1000.
- **Bootstrap intervals** are seeded; `bench/ab_report.py` prints the seed.
- **The ObsCore crossover's sky.** `10_crossover.sql` used to take its field
  centres from the first rows of whichever `src` corpus was live, and its query
  centres from a random draw of the relation it had just built. The resampled
  Gaia `src` is generated in HEALPix order, so those first rows were one small
  patch of sky and every observation was packed into it; on the designed corpus
  they fell elsewhere, and the result moved with the corpus. Field and query
  centres are now a stored set (`bench/data/oc_fields.csv.gz`, `oc_queries.csv`,
  made once by `bench/data/make_oc_fields.sql` from the DR3 density map), the
  offsets are seeded, and every size answers the same 40 (or first `nq`)
  queries, so a size builds the same relation on every run.

## 7. Maturity

skycell is a **prototype**. Version 0.6, PostgreSQL 15+ (developed and tested on
18), no API stability guarantee across versions, no packaging beyond `make
install`. `make upgrade` moves existing databases to the installed version. The
regression suite (`make installcheck`) covers geometry, the rewrite, ADQL
translation and agreement with brute force; it is not a substitute for
operational experience, of which there is none.

The implementation was written with AI assistance. That is not evidence of
correctness: the evidence is the brute-force comparisons in `ext/test/` and the
independent re-derivations in `bench/`, which are what should be inspected.
