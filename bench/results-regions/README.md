# Region table re-measured (tab:gistregion)

`../33_region_table.sh` ran the committed scripts 21–24, unchanged, against the
designed catalogue: `01_data.sql`, 10M rows, then `20_region_xmatch.sql`,
50,000 footprints, 500 probes per operator. It ran three skycell libraries, 8
repeats each, in alternating order, with `fpr`'s GiST index rebuilt by each
library:

- **v0.34**: the polygon cap centred on the vertex mean, reaching the farthest
  vertex.
- **v0.35**: centred on the centre of mass, reaching the farthest point.
- **main**: skycell commit b3bb7be, the library code of 0.36. Its
  point-in-region test converts the query point to a unit vector once per
  index scan; v0.34 and v0.35 converted it at every index entry visited (four
  trigonometric calls, against six comparisons for the box test itself).

This run replaces the two-library run of v0.34 and v0.35 that this directory
held before (skycell-paper commit 07817e7); for every operator and library
the two runs' intervals overlap.

## Host and server

- The cloud machine type of `../results-pg18`: x86-64, 4 vCPU Intel Xeon @
  2.80 GHz, 15 GiB, Linux 6.18.
- PostgreSQL 18.6 built from source.
- pgSphere 1.5.2 built from its `1.5.2` tag, with HEALPix C++ from Ubuntu.
- The paper's settings (`../../REPRODUCING.md` §2): parallelism and JIT off,
  `random_page_cost = 1.1`, 2 GB buffers.

The published table was measured on the Apple M3 laptop.

## Results

`runs.tsv` has every timed pass: library, repeat, script, scope, method,
pass, rows, ms. `summary.tsv` has the per-repeat median ratio, skycell box
key over pgSphere's native GiST, with a 95% bootstrap interval over the 8
repeats. `summarize.py` computes `summary.tsv` from `runs.tsv`.

| operator | published (laptop, 0.6) | v0.34 | v0.35 | main |
|---|---|---|---|---|
| `&&` | 0.12–0.17 | 0.11 [0.11, 0.12] | 0.11 [0.10, 0.12] | 0.11 [0.10, 0.12] |
| `@>`(region, point) | 0.75–0.93 | 0.98 [0.96, 1.00] | 0.95 [0.66, 0.99] | 0.57 [0.55, 0.60] |
| `@>`(region, region) | 0.55–0.70 | 0.56 [0.51, 0.65] | 0.55 [0.52, 0.57] | 0.55 [0.52, 0.61] |
| `<@`(region, region) | 0.26–0.27 | 0.23 [0.23, 0.26] | 0.24 [0.20, 0.35] | 0.22 [0.19, 0.24] |

- **Row counts** agree for skycell, pgSphere and brute force on every
  operator and every run: 718, 555, 561 and 12,193.
- **The v0.35 key** changes no ratio beyond the run-to-run spread.
- **Point-in-footprint** was within 5% of pgSphere on this host with v0.34
  and v0.35; the laptop measured 0.75–0.93 with 0.6. With the point
  converted once per scan, main takes 0.57 of pgSphere's time (7.4 ms against
  12.8 ms, median over the runs). The other three operators take a region as
  their query, and main leaves them unchanged.

## A note on script 24

`rox_probe_big` is meant to hold circles of 2–5° (`2.0 + 3*random()`). It is
built with `ORDER BY random() LIMIT`, and PostgreSQL evaluates that
expression with the same draw as the sort key. The 500 rows kept are those
with the smallest draws, so every big probe is 2.000–2.0003° across. Both
methods see the same circles, so the comparison is fair; the script is left
as it is, so that this re-run reproduces the published one.
