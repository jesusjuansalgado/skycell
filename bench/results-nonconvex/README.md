# Non-convex polygons against pgSphere (README, Performance)

Not in the paper, whose polygon figures are for convex quadrilaterals
(`../results-v036/polyfp/`). `run.sh` runs `../27_nonconvex.sql` on
`gaia_realc`, the 10M real Gaia DR3 positions of `../results-v036/realcone/`,
which carries both skycell's cell index and pgSphere's GiST on `pos`.

- **Shapes:** U, L, a four-tooth comb and a five-pointed star, with the convex
  quadrilateral of `../05_poly.sql` for reference, each at the same 60
  centres (catalogue sources) and sizes (0.05–2° across, log-uniform).
- **Spellings:** skycell as ADQL writes it, `1 = CONTAINS(POINT('ICRS', ra,
  dec), POLYGON('ICRS', ...))`, through the polygon rewrite; pgSphere as `pos
  <@ spoly`.
- **Protocol:** per polygon, both methods in random order, each warmed once
  and then executed 10 times, planned and run, each at a new 1e-9° shift so
  that no plan or covering is reused; three repetitions.
- **Report** (`report.py`, `report.md`): skycell's summed per-polygon median
  time over pgSphere's, with a 95% bootstrap interval over the polygons; per
  shape, and per shape and size.
- **Answers:** both methods count the same rows on all 300 polygons, and on
  the 40 also counted by a sequential scan of the exact test, so does that
  (`run.out`).
- **Host and server:** as `../results-v036/realcone/` (`../REPRODUCING.md`
  section 2); skycell from main after #52 (`versions.txt`).

## skycell / pgSphere

| shape | all sizes | 0.05–0.2° | 0.2–0.6° | 0.6–2° |
|---|---|---|---|---|
| convex quadrilateral | 0.38 [0.31, 0.48] | 0.81 [0.75, 0.88] | 0.51 [0.43, 0.60] | 0.23 [0.20, 0.27] |
| U | 0.23 [0.19, 0.29] | 0.49 [0.45, 0.53] | 0.32 [0.28, 0.37] | 0.13 [0.10, 0.16] |
| L | 0.29 [0.25, 0.37] | 0.62 [0.58, 0.67] | 0.39 [0.34, 0.46] | 0.17 [0.15, 0.20] |
| comb | 0.13 [0.11, 0.15] | 0.22 [0.19, 0.24] | 0.16 [0.15, 0.18] | 0.08 [0.06, 0.10] |
| star | 0.21 [0.17, 0.25] | 0.38 [0.34, 0.43] | 0.26 [0.24, 0.29] | 0.11 [0.10, 0.13] |

- **The more complex the shape, the larger skycell's lead.** pgSphere's time
  grows with the polygon's edges (a comb's median 1.8 ms against the
  quadrilateral's 0.27 ms). skycell's barely does (0.32 against 0.17 ms):
  its covering costs more per step for a non-convex polygon, but the rows it
  reads, and the exact test on them, hardly change.
- **The larger the polygon, too:** from 0.22–0.81 below 0.2° to 0.08–0.23 at
  0.6–2°.
- **The convex quadrilateral reads 0.38 here, against the paper's 0.21–0.23.**
  The protocol differs: a new polygon every execution, planning included,
  where `../05_poly.sql` times each polygon's second pass. Both runs show
  skycell well ahead.
