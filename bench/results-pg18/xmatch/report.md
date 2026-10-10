skycell_lateral: row count differs from q3c in 0 of 32 trials
skycell_slots: row count differs from q3c in 0 of 32 trials
pgsphere: row count differs from q3c in 4 of 32 trials (by 1..1 rows)

10M rows, 1" (16 trials); median ms per block: pgsphere 488, q3c 186, skycell_lateral 157, skycell_slots 213
  skycell_lateral  / q3c      0.83 [0.77, 0.92]
  skycell_lateral  / pgsphere 0.33 [0.30, 0.34]
  skycell_slots    / q3c      1.15 [1.06, 1.18]
  skycell_slots    / pgsphere 0.41 [0.39, 0.46]

10M rows, 10" (16 trials); median ms per block: pgsphere 568, q3c 237, skycell_lateral 192, skycell_slots 279
  skycell_lateral  / q3c      0.81 [0.75, 0.91]
  skycell_lateral  / pgsphere 0.34 [0.31, 0.39]
  skycell_slots    / q3c      1.17 [0.88, 1.35]
  skycell_slots    / pgsphere 0.50 [0.43, 0.54]
