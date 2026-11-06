# perfcheck: 0.52 (best-first) against v0.51

`make perfcheck`, 20 rounds, on the bench server (`../perfcheck/README.md`). First run: the projection rebuilt for every query; second run (the one shipped): only for queries of 32 rows or more.

## First run

| workload | baseline v0.51, ms a round | candidate | candidate / baseline [95%] | |
|---|---|---|---|---|
| cone 1" | 1.02 | 1.03 | 0.98 [0.94, 1.02] |  |
| cone 1 deg | 6.42 | 6.45 | 1.00 [0.98, 1.02] |  |
| native cone 6' | 1.13 | 1.14 | 0.99 [0.96, 1.02] |  |
| polygon 0.5 deg | 2.79 | 2.84 | 1.02 [1.00, 1.05] |  |
| annulus 0.3 deg | 1.93 | 1.97 | 1.01 [1.00, 1.03] |  |
| xmatch 300 x 1" | 36.73 | 36.84 | 0.99 [0.96, 1.03] |  |
| nearest k=1 | 1.73 | 1.79 | 1.11 [1.02, 1.21] |  |
| nearest k=100 | 3.83 | 3.70 | 1.01 [0.94, 1.10] |  |
| partition cone 0.5 deg | 21.11 | 21.08 | 1.06 [0.98, 1.15] |  |
| partition xmatch 300 x 5" | 105.28 | 104.74 | 1.00 [0.98, 1.01] |  |
| ObsCore nearest k=100 | 3.82 | 3.19 | 0.83 [0.79, 0.88] | faster |
| ObsCore nearest k=1000 | 24.83 | 17.48 | 0.73 [0.68, 0.79] | faster |

## Second run (shipped)

| workload | baseline v0.51, ms a round | candidate | candidate / baseline [95%] | |
|---|---|---|---|---|
| cone 1" | 1.02 | 1.03 | 1.00 [0.91, 1.10] |  |
| cone 1 deg | 6.31 | 6.46 | 1.02 [0.92, 1.12] |  |
| native cone 6' | 1.09 | 1.13 | 1.01 [0.97, 1.05] |  |
| polygon 0.5 deg | 2.75 | 2.82 | 1.01 [0.98, 1.04] |  |
| annulus 0.3 deg | 1.94 | 1.96 | 1.00 [0.98, 1.03] |  |
| xmatch 300 x 1" | 36.12 | 35.97 | 1.01 [0.96, 1.07] |  |
| nearest k=1 | 1.67 | 1.68 | 1.02 [0.99, 1.04] |  |
| nearest k=100 | 3.69 | 3.60 | 0.98 [0.92, 1.05] |  |
| partition cone 0.5 deg | 21.57 | 20.68 | 0.99 [0.93, 1.05] |  |
| partition xmatch 300 x 5" | 103.61 | 101.53 | 0.96 [0.93, 1.00] |  |
| ObsCore nearest k=100 | 3.68 | 3.14 | 0.84 [0.78, 0.90] | faster |
| ObsCore nearest k=1000 | 23.69 | 17.10 | 0.74 [0.69, 0.79] | faster |
