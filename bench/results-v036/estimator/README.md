# Density estimator with skycell 0.36 (tab:estimator)

`run.sh` is `psql -v tbl=gaia_realc -v reps=10 -v decision=1 -f
bench/29_estimator.sql` unchanged, the protocol of
`../../results-pg18/estimator/`, on `gaia_realc` (10M real DR3 positions).
Host, builds and settings: as `../realcone/` (`versions.txt`, `server.sh`,
`setup.sh`). It ran last, after `../realcone/` and `../xmreal/`, because its
ten ANALYZEs replace the table's statistics.

- `summary.txt` (`summary.sql`): per field, the median over ten ANALYZE
  samples of the per-sample median rho-hat/rho over radii 0.01–1 deg, the
  spread of that median across samples, and the range over all radii and
  samples. Recomputed from results-pg18's `29_estimator.out` (per-sample
  medians rounded to 0.01), the same summary gives its `summary.txt` to
  within 0.01.
- `29_estimator.out`: the full output, including part (b), the cost-chosen
  covering against every fixed order 4–13 under the last sample's statistics,
  through the custom scan.
- `est_truth.csv`, `est_runs.csv`, `est_decision.csv`: the raw tables.

The truth counts are the same as 0.26's in every field (the heap is the same:
`../realcone/`), and the histogram has 1,000 buckets.

## 0.26 → 0.36, rho-hat / rho

| field | median | across samples | range |
|---|---|---|---|
| omega_cen  | 0.08 → 0.08 | 0.06–0.09 → 0.06–0.09 | 0.05–0.54 → 0.05–0.55 |
| 47tuc      | 0.02 → 0.02 | 0.01–0.11 → 0.01–0.12 | 0.00–0.94 → 0.01–0.94 |
| lmc        | 0.80 → 0.57 | 0.46–0.92 → 0.43–0.75 | 0.22–1.26 → 0.23–1.02 |
| baade      | 0.96 → 0.95 | 0.92–0.98 → 0.93–0.96 | 0.45–1.19 → 0.46–1.16 |
| ngp        | 1.27 → 1.22 | 1.18–1.49 → 1.13–1.31 | 0.48–1.69 → 0.46–1.49 |
| gal_centre | 3.71 → 3.71 | 3.41–3.80 → 3.60–3.82 | 1.95–5.20 → 2.06–5.66 |

The interval here is the spread across the ten samples. It overlaps 0.26's
in every field. The only median that moves is the LMC's (0.80 → 0.57),
inside both spreads.

## Part (b): chosen covering over the best fixed order

| field | r (deg) | 0.26 | 0.36 |
|---|---|---|---|
| omega_cen  | 0.0028 | 1.07 | 1.10 |
| omega_cen  | 0.05   | 1.19 | 1.14 |
| omega_cen  | 0.5    | 1.08 | 0.99 |
| 47tuc      | 0.0028 | 1.06 | *1.45* |
| 47tuc      | 0.05   | 1.53 | 1.56 |
| 47tuc      | 0.5    | 1.05 | 1.00 |
| lmc        | 0.0028 | 1.06 | 0.96 |
| lmc        | 0.05   | 1.00 | 1.20 |
| lmc        | 0.5    | 1.07 | 0.99 |
| baade      | 0.0028 | 1.31 | 1.09 |
| baade      | 0.05   | 1.10 | 1.00 |
| baade      | 0.5    | 0.98 | 0.99 |
| ngp        | 0.0028 | 1.00 | 1.00 |
| ngp        | 0.05   | 1.00 | 1.05 |
| ngp        | 0.5    | 1.06 | 1.08 |
| gal_centre | 0.0028 | 1.07 | 1.05 |
| gal_centre | 0.05   | 1.11 | *1.63* |
| gal_centre | 0.5    | 1.02 | *1.32* |

The paper's summary of this part is the range: 0.98–1.53 under 0.26,
0.96–1.63 under 0.36. Three cones read worse in this run (italic): 47 Tuc at
0.0028 deg, the Galactic centre at 0.05 and 0.5 deg. Part (b) has no interval
(one sample, nine timed repetitions per order, then the best of ten orders'
medians), so these were checked on the same host.

## The same host, 0.26 against 0.36

`same-host-026.sh` ran the whole script four times, alternating 0.26, 0.36,
0.36, 0.26, each version in its own database with the same `gaia_realc` heap
(`../realcone/same-host-026/load.sh`: skycell 0.26's library from commit
`4ea17ee` and its catalogue). Per run: `same-host-026/run<i>_0<v>.out`, its
`summary.txt` table (`summary_tbl.sql`), and `cover.txt` (`cover.sql`), the
covering the cost model chose for part (b)'s cones under the last sample.

| field | 0.26: run 1 / run 4, median (across samples) | 0.36: run 2 / run 3 |
|---|---|---|
| omega_cen  | 0.08 (0.06–0.09) / 0.08 (0.06–0.10) | 0.08 (0.06–0.09) / 0.07 (0.06–0.09) |
| 47tuc      | 0.02 (0.01–0.11) / 0.02 (0.01–0.13) | 0.08 (0.02–0.13) / 0.08 (0.01–0.13) |
| lmc        | 0.52 (0.39–0.93) / 0.56 (0.38–0.79) | 0.58 (0.48–0.83) / 0.62 (0.43–0.85) |
| baade      | 0.94 (0.89–0.98) / 0.94 (0.92–0.96) | 0.95 (0.94–0.98) / 0.95 (0.93–0.97) |
| ngp        | 1.27 (1.11–1.48) / 1.34 (1.21–1.53) | 1.26 (1.08–1.35) / 1.35 (1.12–1.54) |
| gal_centre | 3.76 (3.67–3.87) / 3.76 (3.64–3.90) | 3.71 (3.54–3.79) / 3.71 (3.68–3.85) |

- **The estimate:** the spreads overlap in every field. 0.26 itself gives the
  LMC 0.52 and 0.56 on this host, so results-pg18's 0.80 was its samples.
  47 Tuc's per-sample medians are bimodal (0.01–0.03 or 0.06–0.13), and the
  median of ten lands on either side.
- **Part (b):** chosen over best spans 0.95–1.50 and 1.00–1.45 in the two
  0.26 runs, and 0.95–1.31 and 1.00–1.46 in the two 0.36 runs. The three
  cones above read, under 0.36: 47 Tuc at 0.0028 deg 1.06 and 1.05; the
  Galactic centre at 0.05 deg 1.05 and 1.00, and at 0.5 deg 1.05 and 1.04. So
  they are not reproduced. Under both versions the cost model chose the same
  covering for the Galactic-centre cones (order 9, one range at 0.05 deg and
  six at 0.5 deg; `run1_026.cover.txt`, `run2_036.cover.txt`). These are
  sub-millisecond queries, and the best of ten orders' medians is biased
  toward whichever order had the luckiest repetitions.

## The cost of a bad fixed order

The paper's "a badly chosen fixed size is 243--830 times worse" came from
`17_ablation.sql`'s factor 2 (orders 3--13, 15 catalogue centres at 10",
0.1 and 1 deg), not from this script. `penalty.py` recomputes it from this
run's part (b) (`est_decision.csv`): per cone, each setting's median time,
then the worst fixed order (4--13) over the best. `penalty.md` has every cone.

- **All 18 cones:** 2--465, median 35. The worst order is always the
  coarsest, 4 (5 once).
- **By radius:** 2--465 at 0.0028 deg, 2--249 at 0.05 deg, 3--73 at 0.5 deg.
- **By field:** 4--51 in the cluster cores, 2--465 elsewhere. The low end is
  the Galactic pole, which holds 0--9 rows in these cones, so every order
  reads little.

The paper now quotes "2--465 times slower than the best (median 35)".
`penalty.py`'s chosen / best column matches part (b)'s to 0.01 (1.62 against
the SQL's 1.63 at the Galactic centre, 0.05 deg).
