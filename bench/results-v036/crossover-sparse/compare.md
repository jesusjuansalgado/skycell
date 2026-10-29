## Table 8's measure: summed total time, skycell / pgSphere (seed 0.19 / 0.53)

| class | dense 0.5M | sparse 10M | dense 10M | sparse 10M, rebuilt |
|---|---|---|---|---|
| Q05 | 1.09 / 1.14 | 0.93 / 0.87 | 0.71 / 0.85 | 0.88 / 0.85 |
| Q06 | 0.90 / 0.97 | 0.59 / 0.54 | 0.76 / 0.75 | 0.53 / 0.55 |
| Q07 | 1.01 / 1.00 | 0.80 / 0.51 | 0.74 / 0.76 | 0.79 / 0.90 |
| Q12 | 0.77 / 0.79 | 0.54 / 0.63 | 0.49 / 0.50 | 0.56 / 0.69 |

## Per query, both seeds pooled: planning skycell - pgSphere (ms), execution skycell / pgSphere (medians), buffers pgSphere / skycell (means), rows (median)

| class | dense 0.5M | sparse 10M | dense 10M | sparse 10M, rebuilt |
|---|---|---|---|---|
| Q05 | +0.0030, 1.07, 7 / 11, 48 | +0.0010, 0.82, 10 / 13, 48 | +0.0000, 0.87, 60 / 45, 126 | +0.0010, 0.75, 10 / 13, 48 |
| Q06 | +0.0040, 0.87, 59 / 86, 576 | +0.0000, 0.38, 231 / 143, 576 | +0.0050, 0.76, 3841 / 1180, 8350 | +0.0000, 0.43, 218 / 143, 576 |
| Q07 | +0.0030, 0.86, 11 / 21, 128 | +0.0020, 0.64, 16 / 28, 128 | +0.0045, 0.69, 309 / 138, 502 | +0.0020, 0.70, 15 / 28, 128 |
| Q12 | +0.0010, 0.46, 3 / 3, 0 | +0.0005, 0.27, 6 / 3, 0 | +0.0015, 0.20, 8 / 3, 0 | +0.0010, 0.29, 5 / 3, 0 |

(table, seed, class) cells where the methods returned different row counts: 0 (expect 0).

## threeway.sql: median time, skycell / pgSphere as planned, skycell / pgSphere bitmap; buffers pgSphere / pgSphere bitmap / skycell

| class | dense 0.5M | sparse 10M | dense 10M | sparse 10M, rebuilt |
|---|---|---|---|---|
| Q05 | 1.14, 1.11; 7 / 6 / 11 | 0.90, 0.88; 10 / 9 / 13 | 0.91, 0.91; 60 / 26 / 45 | 0.86, 0.84; 10 / 8 / 13 |
| Q06 | 0.87, 0.79; 59 / 41 / 86 | 0.45, 0.47; 231 / 105 / 143 | 0.81, 0.84; 3841 / 741 / 1180 | 0.46, 0.48; 218 / 101 / 143 |
| Q07 | 0.94, 0.99; 11 / 9 / 21 | 0.80, 0.94; 16 / 12 / 28 | 0.70, 0.77; 309 / 79 / 138 | 0.77, 0.78; 15 / 11 / 28 |
| Q12 | 0.78, 0.78; 3 / 3 / 3 | 0.58, 0.58; 6 / 6 / 3 | 0.49, 0.46; 8 / 8 / 3 | 0.57, 0.57; 5 / 5 / 3 |

Shared buffer reads in the three-way runs, all tables: 0.0 (0: every page a hit).
