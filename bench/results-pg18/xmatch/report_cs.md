skycell_cs: row count differs from q3c in 0 of 32 trials
skycell_lateral: row count differs from q3c in 0 of 32 trials
skycell_slots: row count differs from q3c in 0 of 32 trials
pgsphere: row count differs from q3c in 4 of 32 trials (by 1..1 rows)

10M rows, 1" (16 trials); median ms per block: pgsphere 472, q3c 186, skycell_cs 135, skycell_lateral 154, skycell_slots 207
  skycell_cs       / q3c             0.73 [0.62, 0.84]
  skycell_cs       / pgsphere        0.28 [0.27, 0.32]
  skycell_lateral  / q3c             0.79 [0.69, 0.85]
  skycell_lateral  / pgsphere        0.32 [0.28, 0.34]
  skycell_slots    / q3c             1.15 [0.95, 1.33]
  skycell_slots    / pgsphere        0.44 [0.41, 0.50]
  skycell_cs       / skycell_lateral 0.92 [0.78, 1.04]
  skycell_cs       / skycell_slots   0.62 [0.59, 0.69]

10M rows, 10" (16 trials); median ms per block: pgsphere 522, q3c 197, skycell_cs 149, skycell_lateral 168, skycell_slots 230
  skycell_cs       / q3c             0.80 [0.64, 0.83]
  skycell_cs       / pgsphere        0.29 [0.26, 0.31]
  skycell_lateral  / q3c             0.84 [0.72, 0.92]
  skycell_lateral  / pgsphere        0.33 [0.31, 0.34]
  skycell_slots    / q3c             1.17 [0.97, 1.35]
  skycell_slots    / pgsphere        0.45 [0.40, 0.48]
  skycell_cs       / skycell_lateral 0.89 [0.85, 0.93]
  skycell_cs       / skycell_slots   0.66 [0.59, 0.70]
