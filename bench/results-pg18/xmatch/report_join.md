skycell_join: row count differs from q3c in 0 of 32 trials
skycell_cs: row count differs from q3c in 0 of 32 trials
skycell_lateral: row count differs from q3c in 0 of 32 trials
skycell_slots: row count differs from q3c in 0 of 32 trials
pgsphere: row count differs from q3c in 4 of 32 trials (by 1..1 rows)

10M rows, 1" (16 trials); median ms per block: pgsphere 595, q3c 226, skycell_cs 152, skycell_join 153, skycell_slots 227
  skycell_join     / q3c             0.67 [0.60, 0.77]
  skycell_join     / pgsphere        0.26 [0.23, 0.30]
  skycell_cs       / q3c             0.71 [0.64, 0.79]
  skycell_cs       / pgsphere        0.27 [0.23, 0.30]
  skycell_slots    / q3c             0.99 [0.85, 1.15]
  skycell_slots    / pgsphere        0.39 [0.35, 0.44]
  skycell_cs       / skycell_slots   0.68 [0.60, 0.80]
  skycell_join     / skycell_cs      0.97 [0.88, 1.08]

10M rows, 10" (16 trials); median ms per block: pgsphere 628, q3c 248, skycell_cs 186, skycell_join 189, skycell_slots 299
  skycell_join     / q3c             0.73 [0.63, 0.84]
  skycell_join     / pgsphere        0.31 [0.26, 0.33]
  skycell_cs       / q3c             0.75 [0.69, 0.86]
  skycell_cs       / pgsphere        0.31 [0.24, 0.35]
  skycell_slots    / q3c             1.17 [0.94, 1.40]
  skycell_slots    / pgsphere        0.52 [0.42, 0.60]
  skycell_cs       / skycell_slots   0.68 [0.57, 0.74]
  skycell_join     / skycell_cs      0.96 [0.90, 1.08]
