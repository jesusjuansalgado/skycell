skycell_join: row count differs from q3c in 0 of 32 trials
skycell_cs: row count differs from q3c in 0 of 32 trials
skycell_lateral: row count differs from q3c in 0 of 32 trials
skycell_slots: row count differs from q3c in 0 of 32 trials
pgsphere: row count differs from q3c in 2 of 32 trials (by 1..1 rows)

10M rows, 1" (16 trials); median ms per block: pgsphere 410, q3c 148, skycell_cs 112, skycell_join 109, skycell_slots 173
  skycell_join     / q3c             0.73 [0.71, 0.76]
  skycell_join     / pgsphere        0.26 [0.26, 0.29]
  skycell_cs       / q3c             0.75 [0.72, 0.82]
  skycell_cs       / pgsphere        0.28 [0.27, 0.30]
  skycell_slots    / q3c             1.19 [1.17, 1.24]
  skycell_slots    / pgsphere        0.43 [0.39, 0.47]
  skycell_cs       / skycell_slots   0.63 [0.60, 0.73]
  skycell_join     / skycell_cs      0.95 [0.87, 1.02]

10M rows, 10" (16 trials); median ms per block: pgsphere 392, q3c 151, skycell_cs 125, skycell_join 127, skycell_slots 187
  skycell_join     / q3c             0.83 [0.80, 1.04]
  skycell_join     / pgsphere        0.33 [0.32, 0.37]
  skycell_cs       / q3c             0.84 [0.78, 0.88]
  skycell_cs       / pgsphere        0.33 [0.30, 0.34]
  skycell_slots    / q3c             1.25 [1.18, 1.29]
  skycell_slots    / pgsphere        0.48 [0.44, 0.50]
  skycell_cs       / skycell_slots   0.67 [0.65, 0.70]
  skycell_join     / skycell_cs      1.00 [0.96, 1.14]
