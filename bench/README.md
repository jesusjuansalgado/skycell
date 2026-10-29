# Benchmarks behind the skycell paper

This directory holds the benchmark scripts, their input data, and the
documentation of every measurement the paper quotes: for each one, its
protocol, report, drivers and recorded versions. `REPRODUCING.md` describes
the hosts, server settings and run order, and what is not deterministic.

The raw per-query CSVs, the runs of earlier skycell versions and the design
history are kept in
[skycell-paper](https://github.com/jesusjuansalgado/skycell-paper), at the same
paths under its `bench/`. Every report here was generated from those CSVs;
each directory's README names the script that did it.

## Where each result is

| paper | directory | skycell |
|---|---|---|
| Table 3 (cone searches), Table 4 (two catalogue sizes), Fig. 1 | `results-v037/cones/`, `results-v037/realcone/` | 0.37 |
| Table 5 (index size and build time) | `results-v036/cones/` (`bench_build*`, `scale50/`) | 0.36 |
| Table 6 (cross-match on real positions) | `results-v036/xmreal/` | 0.36 |
| cross-match on the resampled catalogue (text) | `results-v036/xmatch/` | 0.36 |
| Table 7 (parallel cross-match) | `results-parallel/` | 0.36 |
| Table 8 (ObsCore-shaped table), and its size effect | `results-v036/crossover/`, `results-v036/crossover-sparse/` | 0.36 |
| Table 9 (density estimate) | `results-v036/estimator/` | 0.36 |
| Table 10 (region queries) | `results-regions/` | 0.36 |
| polygons and footprints (text) | `results-v036/polyfp/`, `results-v036/regions-single/` | 0.36 |
| memory pressure (text) | `results-v036/pressure/` | 0.36 |
| run-to-run spread of the real-position cones (text) | `results-v036/realcone/same-host-026/` | 0.26, 0.36 |
| TAP service | `tap-ab/` | 0.6 |
| nearest neighbours, README only (not in the paper) | `results-nearest/` | main after #52 |
| non-convex polygons, README only (not in the paper) | `results-nonconvex/` (`../27_nonconvex.sql`) | main after #52 |

skycell 0.37 differs from 0.36 only in planning, and returns the same
coverings, so only the cone searches were re-measured with it
(`results-v037/README.md`).

## Running

The scripts run from this directory with the usual `PG*` connection
variables. `REPRODUCING.md` §5 gives the order. Each results directory's
`run.sh` (or `run_all.sh`) is the exact driver, and its `server.sh` the server
as it was started. `data/` holds the inputs that cannot be regenerated from a
seed:
- `gaia_map_hpx9.csv.gz`: the Gaia DR3 order-9 density map;
- `oc_fields.csv.gz`, `oc_queries.csv`: the ObsCore field and query centres.

The real Gaia DR3 positions are fetched by `19_gaia_real.sh`.
