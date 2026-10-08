# Reproducing the paper's measurements on a virtual machine

This directory turns a fresh Linux VM into the measurement setup the paper
describes (REPRODUCING.md) and runs the benchmarks behind each of its tables.

| file | what it does |
|---|---|
| `provision.sh` | installs PostgreSQL 18 with Q3C and pgSphere from the PostgreSQL project's own apt repository (PGDG); builds and installs skycell from this checkout; applies the paper's server settings; creates the `skycell` database; runs the regression suite |
| `run_paper.sh` | runs the measurements stage by stage and exports every result table as CSV |

## 1. The VM

The paper was measured in a Linux VM with 7.65 GiB of memory (a Docker
Desktop VM on an Apple M3 Pro), with `shared_buffers = 2GB`. To land in the
same memory regime, where pgSphere's 50M-row index no longer fits in
`shared_buffers` but skycell's does (`tab:scale`):

| | recommended |
|---|---|
| OS | Ubuntu 24.04 LTS or Debian 12/13 (amd64 or arm64) |
| vCPU | 4 or more (queries run single-threaded: parallelism is off) |
| memory | **8 GiB** (more makes the OS page cache hide disk reads; less risks OOM in `ANALYZE`) |
| disk | **150 GB**, a single virtual disk on SSD; not a network or overlay filesystem |
| network | outbound HTTPS to `apt.postgresql.org`, `github.com` and `gea.esac.esa.int` (the ESA Gaia archive, `real` stage only) |

Keep the VM otherwise idle while measuring. The paper's host ran other
containers alongside, which moves absolute times but not the paired ratios
(REPRODUCING.md section 6).

**Cold-cache runs need a VM that owns its disk cache.** `run_paper.sh` restarts
PostgreSQL and drops the guest's page cache before every cold pass. That only
makes reads cold if the hypervisor does not cache the virtual disk on the host
as well. Check this once, after the `real` stage has run (look for "Disk-cold
check" below). Round sixty-nine in GIST_REGION_DESIGN.md is what a host cache
looks like: about 0.08 ms per read instead of milliseconds.

## 2. Provision

```bash
git clone https://github.com/jesusjuansalgado/skycell.git
cd skycell
git checkout claude/zealous-cerf-mkda2j     # the branch with these scripts
sudo -E vm/provision.sh                     # ~5 min; ends with PROVISION_DONE
```

This installs PostgreSQL 18 (set `PG_MAJOR=17` to use another version) and
whatever Q3C and pgSphere versions PGDG currently ships for it. The paper used
Q3C 2.0.5 and pgSphere 1.5.2; the script prints the installed versions, so
record them. It also writes `/etc/postgresql/18/main/conf.d/skycell-bench.conf`
with REPRODUCING.md section 2's settings (`shared_buffers = 2GB`,
`effective_cache_size = 5GB`, `random_page_cost = 1.1`, `work_mem = 64MB`,
`maintenance_work_mem = 1GB`, parallelism and JIT off), and creates a
PostgreSQL superuser named after your login, so `psql` works without a password.

## 3. Smoke test first

```bash
SMOKE=1 PGDATABASE=smoke vm/run_paper.sh designed gaia gaia50 report
```

Create the `smoke` database first (`createdb smoke && psql -d smoke -c 'CREATE
EXTENSION skycell; CREATE EXTENSION q3c; CREATE EXTENSION pg_sphere'`).
`SMOKE=1` shrinks every corpus and count so the whole pipeline runs in about
ten minutes. The numbers mean nothing, but every script, call and export gets
exercised. Drop the database afterwards (`dropdb smoke`).

## 4. The full run

```bash
vm/run_paper.sh 2>&1 | tee run.out          # all stages; results in results-vm/<date>/
vm/run_paper.sh real report                 # or one stage at a time, in this order
```

Each stage rebuilds the corpus it measures, so stages can be rerun on their
own; `report` exports and summarises whatever has been measured so far.

| stage | corpus | paper | scripts | rough time |
|---|---|---|---|---|
| `designed` | synthetic, 10M rows (`01_data.sql`) | `tab:cones` designed columns, Fig. 1a/b, joins (Sect. joins), polygons, footprints, `tab:gistregion` | `01, 02, 03, 07, 08, 09, 05, 06, 20-24` | 3-5 h |
| `gaia` | 10M resampled from the DR3 order-9 map (`11_scale_corpus.sql`) | `tab:cones` Gaia columns, `tab:scale` 10M column, `tab:gaia` 10M row, Fig. 1c, `tab:crossover` | `11, 02, 03, 07, 08, 09, 10` | 3-5 h |
| `gaia50` | 50M resampled | `tab:scale` 50M column, `tab:gaia` extrapolation, cross-match | `11, 12, 03, 07, 04, 11_xmatch_ab` | 4-8 h |
| `real` | 10M real DR3 positions + 8 complete fields (`19_gaia_real.sh`, ~700 MB download) | `tab:estimator`, `tab:xmreal`, `tab:cones` real columns (warm and cold), ablation, sensitivity | `19*, 29, 16, 17, 18, 30/31` | 3-5 h |
| `report` | | summaries | `ab_report.py`, `realcone_report.py` | minutes |

The times are estimates for an 8 GiB VM on SSD. `gaia50` needs about 30 GB of
disk while it runs.

## 5. Where each number comes from

All CSVs land in `results-vm/<date>/`; `run.log` timestamps every step.

| paper | file | how to read it |
|---|---|---|
| `tab:cones` designed / Gaia, warm and cold; `tab:scale` | `ab_report.md` (from `bench_ab.csv`, `bench_ab_x.csv`) | paired skycell/pgSphere ratio with bootstrap interval, per corpus (`designed`, `gaia`, `gaia50`), cache and radius |
| `tab:cones` real corpus, warm and cold | `realcone/report.md` | same layout, plus planning, execution, buffers and, cold, pages read and I/O time |
| Fig. 1b, buffers at 1° | `ab_report.md`, EXPLAIN section | buffers per method and radius |
| Fig. 1c, `range_cost` sweep | `cm_sweep.csv` | time against `range_cost`, normalised per radius |
| `tab:gaia` | `bench_build_gaia.csv`, `bench_build50.csv` | index MB and build seconds per method |
| `tab:crossover` | `bench_cross.csv` | ratio skycell/pgSphere per class and size; buffers at 10M |
| `tab:estimator` | `29_estimator.out` | median and spread of rho-hat/rho per field; part (b) the decision cost |
| `tab:xmreal` | `xms.csv` | per (size, radius, target) cell; take geometric means of the paired ratios |
| `tab:gistregion` | `22_region_gist.out`, `23_region_contains.out`, `24_region_contains_region.out` | box4 (the default opclass) against pgSphere, per strategy |
| joins (134 of 135 plans) | `bench_join.csv` | estimate error and plan strategy per method |
| ablation, sensitivity | `abl.csv`, `sens_cost.csv`, `sens_dens.csv` | referee 2's questions |

To redraw Fig. 1 from your run, copy the CSVs into `bench/results-ab/`
(keep the originals elsewhere) and run `python3 paper/figures.py`.

### Disk-cold check

```bash
python3 bench/realcone_report.py results-vm/<date>/realcone | tail -3
awk -F, 'NR>1 && $7>0 {s+=$8; r+=$7} END {printf "%.2f ms per page read\n", s/r}' \
    results-vm/<date>/realcone/bench_realcone_cold.csv
```

Milliseconds per read means the cold passes reached the disk. A few hundredths
of a millisecond means a host-level cache served them, and the cold columns
should be read as round sixty-nine describes.

## 6. What this does not reproduce

- **The memory-pressure experiment** (Sect. "When the index no longer fits in
  memory", `bench/13_pressure*.{sql,sh}`). It ran in a separate 3 GiB Docker
  container with `shared_buffers = 768MB`. On a VM, restart PostgreSQL with
  those settings and run `13_pressure.sql` by hand.
- **The TAP-service A/B** (Sect. 7). Its harness lives in the egernia fork;
  see `bench/tap-ab/`.
- **`14_shapes.sql`**. It needs the 20M-row ObsCore relation `oc20`, which no
  committed script builds.
- **`20_region_xmatch.sql`'s own cross-match runs.** The stage uses `20` only
  to build `fpr`. Against `cat_cell`'s stored `cell` column, its
  `point(ra, dec) <@ s_region` join has no index path, so it runs as a full
  nested loop: at paper scale, 50,000 footprints × 10M rows, that is
  5×10¹¹ evaluations. No paper table comes from it. (Running it also
  exposed a memory leak in the operator's region cache, fixed in commit
  `a76609b`.)

## 7. Caveats for anyone comparing numbers

- **Absolute times are properties of the machine.** Only the paired ratios and
  the buffer counts are meant to travel (REPRODUCING.md section 6). Buffer
  counts are deterministic and should match the paper closely; ratios can
  move with the host's balance of planning against execution
  (GIST_REGION_DESIGN.md rounds sixty-six and sixty-eight).
- **Some driver arguments were reconstructed.** The calls to `08`, `09`, `10`
  and `18`'s functions were never committed. `run_paper.sh` uses the arguments
  recorded in `bench/results-ab/*.csv` (labels, costs, sizes, repetitions)
  and the paper's text (`tab:xmreal`'s radii and sizes, 500 region probes,
  a 50,000-row `fpr`).
- **The cold A/B pass follows the paper, not round forty-four.** `07_ab.sql`'s
  cold pass restarts once and then runs every query, so later queries find
  some pages already cached. Round forty-four's stricter per-radius protocol
  is what `31_real_cones.sh` uses for the real corpus.
- **The region benchmarks run on the designed corpus.** `fpr` is drawn from
  whichever `src` is loaded; the paper does not say which corpus it used.
- **`gaia_map` is a refetch.** It comes from `bench/data/gaia_map_hpx9.csv.gz`,
  fetched in 2026-10 by `bench/19_gaia_map.sh`. Its total matches DR3 exactly,
  but the original map's fetch was never committed, so it can't be compared
  cell by cell.
- **If a plan ignores an index**, check `pg_stat_activity` for a long-running
  transaction (REPRODUCING.md section 6: `indcheckxmin`).
