# Density estimator on PostgreSQL 18.6 (tab:estimator)

`psql -v tbl=gaia_realc -v reps=10 -v decision=1 -f bench/29_estimator.sql`,
same host and versions as `../`. `summary.txt`: per field, the median over ten
ANALYZE samples of the per-sample median rho-hat/rho over radii 0.01-1 deg, the
spread of that median across samples, and the range over all radii and
samples. `29_estimator.out`: the full output, including part (b), the cost-chosen
covering against every fixed order 4-13 under the last sample's statistics
(through the custom scan, which honours skycell.force_order: checked, a 0.5 deg
cone gives 2 ranges at order 4 and 6 at order 13).
