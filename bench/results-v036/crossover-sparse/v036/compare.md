## Table 8's measure: summed total time, skycell / pgSphere (seed 0.19 / 0.53)

| class | dense 0.5M | sparse 10M | dense 10M | sparse 10M, rebuilt |
|---|---|---|---|---|
| Q05 | 1.18 / 1.29 | 0.85 / 0.88 | 0.96 / 0.99 | 0.98 / 0.96 |
| Q06 | 0.92 / 0.93 | 0.55 / 0.59 | 0.79 / 0.82 | 0.59 / 0.55 |
| Q07 | 0.98 / 0.96 | 0.82 / 0.67 | 0.86 / 0.89 | 0.78 / 0.83 |
| Q12 | 0.79 / 0.84 | 0.59 / 0.58 | 0.61 / 0.58 | 0.63 / 0.59 |

## Per query, both seeds pooled: planning skycell - pgSphere (ms), execution skycell / pgSphere (medians), buffers pgSphere / skycell (means), rows (median)

| class | dense 0.5M | sparse 10M | dense 10M | sparse 10M, rebuilt |
|---|---|---|---|---|
| Q05 | +0.0045, 1.15, 7 / 12, 48 | +0.0020, 0.76, 10 / 13, 48 | +0.0080, 0.89, 53 / 45, 126 | +0.0020, 0.78, 10 / 13, 48 |
| Q06 | +0.0030, 0.89, 59 / 86, 576 | +0.0000, 0.41, 227 / 144, 576 | +0.0060, 0.77, 3709 / 1180, 8350 | -0.0005, 0.41, 224 / 143, 576 |
| Q07 | +0.0030, 0.84, 11 / 21, 128 | +0.0030, 0.71, 15 / 28, 128 | +0.0040, 0.77, 285 / 139, 502 | +0.0030, 0.66, 16 / 28, 128 |
| Q12 | +0.0020, 0.38, 3 / 3, 0 | +0.0020, 0.24, 6 / 3, 0 | +0.0020, 0.25, 6 / 3, 0 | +0.0020, 0.24, 6 / 3, 0 |

(table, seed, class) cells where the methods returned different row counts: 0 (expect 0).

## threeway.sql: median time, skycell / pgSphere as planned, skycell / pgSphere bitmap; buffers pgSphere / pgSphere bitmap / skycell

| class | dense 0.5M | sparse 10M | dense 10M | sparse 10M, rebuilt |
|---|---|---|---|---|
| Q05 | 1.18, 1.11; 7 / 6 / 13 | 0.92, 0.94; 10 / 9 / 13 | 0.96, 0.92; 53 / 24 / 45 | 0.89, 0.88; 10 / 9 / 13 |
| Q06 | 0.86, 0.86; 59 / 41 / 86 | 0.42, 0.45; 227 / 105 / 144 | 0.81, 0.94; 3709 / 737 / 1180 | 0.41, 0.42; 224 / 103 / 143 |
| Q07 | 0.97, 0.98; 11 / 9 / 21 | 0.88, 0.85; 15 / 12 / 28 | 0.89, 0.91; 285 / 76 / 139 | 0.79, 0.85; 16 / 12 / 28 |
| Q12 | 0.81, 0.79; 3 / 3 / 3 | 0.62, 0.61; 6 / 6 / 3 | 0.52, 0.52; 6 / 6 / 3 | 0.60, 0.60; 6 / 6 / 3 |

Shared buffer reads in the three-way runs, all tables: 0.0 (0: every page a hit).
