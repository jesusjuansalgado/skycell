# Real-DR3 cross-match on PostgreSQL 18.6 (tab:xmreal)

`18_xmatch_sweep.sql`'s `xms_run('{1000,10000,100000}', '{0.5,1,1.5,5}',
'{clustered,uniform}')` on `gaia_realc` (10M real DR3 positions, one shared heap
with the cell expression index, Q3C's index and pgSphere's GiST), same host and
versions as `../`. Per cell (size, targets, radius) and repetition the five
methods run in random order, each warmed on the cell's query and then timed.

Methods: `skycell_cs`, the join written with `skycell_cone`, answered by the
custom scan's covering per probe; `skycell_join`, Q3C's spelling, also through
the custom scan; `skycell_slots`, the `skycell_cone` join through the range
rewrite (`skycell.custom_scan` off); `q3c_join`; pgSphere. Plans were checked
before timing (custom scan, bitmap OR, Q3C's bitmap scan, GiST index scan).

`report.md` is `report.py xms.csv`: geometric mean of the per-cell ratio of
medians, per radius and over all 24 cells. Row counts agree in every cell.
The paper's earlier table, the rewrite on the first host and PostgreSQL 16, was
1.32 of `q3c_join` and 0.44 of pgSphere over all cells.
