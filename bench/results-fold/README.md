# Booleans of regions folded into one region (0.53)

`run.sh` times the shapes of `../results-composite/` (the paper's Table 9: an
annulus, two circles, their lens, a square less a circle, five circles in a
cross; 60 each, 0.05°–2°, round catalogue sources of the 10M real Gaia DR3
positions) written four ways (`../38_fold.sql`):

- **region:** one `REGION('<STC-S composite>')`;
- **boolean:** the pieces as ADQL `CONTAINS` predicates joined by `AND`,
  `OR` and `NOT`, which skycell folds into the composite
  (`skycell.fold_regions = on`, the default);
- **unfolded:** the same boolean with the fold off, as 0.52 answered it;
- **pgsphere:** pgSphere's boolean of `pos <@ scircle / spoly`.

Protocol as `../results-composite/`: per shape the methods in random order,
10 executions each, planned and run, each shifted 1e-9° so nothing is reused;
three repetitions. `report.py`: per shape, the folded boolean's summed
per-shape median over each other method's, 95% bootstrap interval over shapes.

All methods counted the same rows for all 300 shapes (`run.out`).

| shape | folded ÷ unfolded | folded ÷ REGION | folded ÷ pgSphere |
|---|---|---|---|
| annulus | 0.84 [0.79, 0.91] | 1.05 [0.99, 1.15] | 0.89 [0.82, 0.98] |
| two circles | 0.32 [0.29, 0.36] | 1.04 [1.02, 1.07] | 0.90 [0.88, 0.93] |
| lens | 0.76 [0.73, 0.80] | 1.05 [1.01, 1.08] | 0.50 [0.43, 0.60] |
| square less circle | 0.84 [0.82, 0.86] | 1.01 [0.99, 1.04] | 0.15 [0.12, 0.20] |
| five circles | 0.29 [0.27, 0.32] | 1.17 [1.12, 1.22] | 0.71 [0.69, 0.73] |

- **Unions** (two circles, five circles) gain most, 3–3.5 times: one covering
  of the whole shape replaces a covering per circle and the BitmapOr joining
  them.
- **Intersections and differences** gain 16–24%: the unfolded form was already
  one cone scan with the other pieces as a filter.
- **Against one `REGION`** the folded boolean is level to 17% slower, the most
  for five circles. Planning costs the same (on five circles, a median 32 µs
  folded against 36 µs for the `REGION` and 104 µs unfolded; execution 17 µs
  for both); the rest is parsing a query of five `CONTAINS` calls instead of
  one.

## perfcheck against v0.52

`make perfcheck`, 20 rounds: the fold runs in the planner hook on every query; nothing moved (every interval holds 1).

| workload | baseline v0.52, ms a round | candidate | candidate / baseline [95%] | |
|---|---|---|---|---|
| cone 1" | 0.81 | 0.83 | 1.02 [0.89, 1.17] |  |
| cone 1 deg | 5.36 | 5.40 | 0.99 [0.91, 1.08] |  |
| native cone 6' | 0.93 | 0.96 | 0.98 [0.88, 1.09] |  |
| polygon 0.5 deg | 2.41 | 2.41 | 1.01 [0.91, 1.12] |  |
| annulus 0.3 deg | 1.62 | 1.58 | 0.98 [0.87, 1.09] |  |
| xmatch 300 x 1" | 32.97 | 32.76 | 1.02 [0.94, 1.10] |  |
| nearest k=1 | 1.42 | 1.46 | 1.05 [0.95, 1.15] |  |
| nearest k=100 | 3.03 | 3.17 | 1.08 [0.98, 1.20] |  |
| partition cone 0.5 deg | 18.98 | 19.14 | 1.04 [0.96, 1.12] |  |
| partition xmatch 300 x 5" | 89.46 | 91.28 | 1.00 [0.94, 1.06] |  |
| ObsCore nearest k=100 | 2.75 | 2.64 | 0.98 [0.88, 1.08] |  |
| ObsCore nearest k=1000 | 15.43 | 15.00 | 0.95 [0.82, 1.10] |  |
