# Sources inside a stored footprint, by a join (0.54)

`../39_region_join.sql`: `SELECT ... FROM cat c JOIN footprints o ON
CONTAINS(POINT(c.ra, c.dec), o.s_region) = 1 WHERE o.obs_id = ...`, the
footprint another table's row. Since 0.54 the custom scan covers each outer
row's region as it runs, and its rows are estimated from a sample of the
footprints. Before, the region reached the catalogue as the rewrite's range
slots, each estimated at a ninth of the table, and the planner could prefer
reading the whole catalogue (`skycell.custom_scan = off` gives that plan).

| catalogue | before | 0.54 |
|---|---|---|
| 10M real Gaia DR3 positions, heap in cell order (the slots' bitmap scan) | 0.47 ms median | 0.24 ms |
| 1M synthetic, clustered, heap out of cell order (a parallel scan of the table) | 110 ms | 2.1 ms |

Gaia: 2000 footprints of 0.1°–0.4° (circles, boxes, two-detector mosaics)
centred on sources, 30 of them, three repetitions, each query planned and run
after an untimed one; the same counts for all 30. Planning takes 0.22 ms; the
first query of a session to join a given footprint table samples 8 of its
footprints, about 2.8 ms, and keeps the estimate for the session.

perfcheck against v0.53 (`perfcheck.md`): nothing moved.
