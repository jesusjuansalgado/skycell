# perfcheck against v0.53

| workload | baseline v0.53, ms a round | candidate | candidate / baseline [95%] | |
|---|---|---|---|---|
| cone 1" | 1.12 | 1.12 | 0.95 [0.82, 1.11] |  |
| cone 1 deg | 7.35 | 7.16 | 0.94 [0.82, 1.08] |  |
| native cone 6' | 1.22 | 1.19 | 0.92 [0.80, 1.05] |  |
| polygon 0.5 deg | 3.08 | 2.93 | 0.91 [0.78, 1.07] |  |
| annulus 0.3 deg | 2.07 | 2.07 | 0.86 [0.73, 1.03] |  |
| xmatch 300 x 1" | 39.92 | 38.47 | 1.04 [0.95, 1.14] |  |
| nearest k=1 | 1.88 | 1.84 | 0.99 [0.87, 1.12] |  |
| nearest k=100 | 3.95 | 4.00 | 1.00 [0.90, 1.11] |  |
| partition cone 0.5 deg | 23.35 | 22.38 | 1.06 [0.98, 1.14] |  |
| partition xmatch 300 x 5" | 113.05 | 110.24 | 0.99 [0.97, 1.01] |  |
| ObsCore nearest k=100 | 3.34 | 3.31 | 0.97 [0.90, 1.06] |  |
| ObsCore nearest k=1000 | 18.61 | 17.93 | 0.98 [0.93, 1.02] |  |
