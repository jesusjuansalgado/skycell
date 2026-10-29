
## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell / pgsphere

| radius | n | total, skycell/pgsphere | execution only | planning, skycell - pgsphere (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 0.76 [0.75, 0.78] | 0.47 [0.45, 0.49] | +0.003 | 4.3 / 5.6 |
| 10" | 400 | 0.78 [0.76, 0.81] | 0.47 [0.46, 0.49] | +0.003 | 4.7 / 5.7 |
| 1' | 300 | 0.82 [0.80, 0.84] | 0.55 [0.54, 0.57] | +0.004 | 5.3 / 5.9 |
| 6' | 200 | 0.97 [0.94, 1.00] | 0.81 [0.77, 0.83] | +0.005 | 10.5 / 11.9 |
| 30' | 100 | 0.78 [0.73, 0.83] | 0.73 [0.69, 0.77] | +0.003 | 41.7 / 94.7 |
| 1deg | 40 | 0.74 [0.67, 0.83] | 0.70 [0.66, 0.76] | +0.003 | 90.2 / 295.8 |
| 3deg | 16 | 0.69 [0.64, 0.76] | 0.68 [0.63, 0.75] | +0.005 | 543.2 / 962.3 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell-rw / pgsphere

| radius | n | total, skycell-rw/pgsphere | execution only | planning, skycell-rw - pgsphere (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 1.23 [1.20, 1.28] | 0.56 [0.54, 0.57] | +0.019 | 4.2 / 5.6 |
| 10" | 400 | 1.21 [1.18, 1.23] | 0.56 [0.54, 0.57] | +0.017 | 4.7 / 5.7 |
| 1' | 300 | 1.18 [1.15, 1.20] | 0.60 [0.58, 0.63] | +0.016 | 5.1 / 5.9 |
| 6' | 200 | 1.50 [1.44, 1.61] | 1.01 [0.95, 1.04] | +0.031 | 9.6 / 11.9 |
| 30' | 100 | 1.33 [1.15, 1.48] | 0.92 [0.84, 1.02] | +0.066 | 35.5 / 94.7 |
| 1deg | 40 | 1.07 [0.81, 1.28] | 0.83 [0.66, 0.92] | +0.092 | 82.3 / 295.8 |
| 3deg | 16 | 0.67 [0.63, 0.87] | 0.63 [0.58, 0.68] | +0.266 | 538.2 / 962.3 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell / skycell-rw

| radius | n | total, skycell/skycell-rw | execution only | planning, skycell - skycell-rw (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 0.62 [0.60, 0.64] | 0.84 [0.81, 0.86] | -0.015 | 4.3 / 4.2 |
| 10" | 400 | 0.66 [0.64, 0.67] | 0.87 [0.85, 0.88] | -0.014 | 4.7 / 4.7 |
| 1' | 300 | 0.70 [0.68, 0.71] | 0.90 [0.88, 0.92] | -0.013 | 5.3 / 5.1 |
| 6' | 200 | 0.64 [0.62, 0.68] | 0.79 [0.76, 0.85] | -0.025 | 10.5 / 9.6 |
| 30' | 100 | 0.61 [0.57, 0.64] | 0.77 [0.72, 0.83] | -0.064 | 41.7 / 35.5 |
| 1deg | 40 | 0.73 [0.70, 0.80] | 0.89 [0.84, 1.00] | -0.090 | 90.2 / 82.3 |
| 3deg | 16 | 1.07 [0.84, 1.15] | 1.16 [0.98, 1.19] | -0.265 | 543.2 / 538.2 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, cold (gaia_real_cell / gaia_real_sphere, restart before each pass): skycell / pgsphere

| radius | n | total, skycell/pgsphere | execution only | planning, skycell - pgsphere (ms) | pages read, skycell / pgSphere | buffers, skycell / pgSphere | I/O ms, skycell / pgSphere |
|---|---|---|---|---|---|---|---|
| 1" | 400 | 0.57 [0.55, 0.59] | 0.52 [0.49, 0.55] | -0.008 | 1.7 / 2.6 | 4.1 / 5.7 | 0.135 / 0.237 |
| 10" | 400 | 0.90 [0.85, 0.94] | 0.81 [0.76, 0.83] | +0.020 | 1.8 / 2.6 | 4.4 / 5.8 | 0.145 / 0.178 |
| 1' | 300 | 0.71 [0.66, 0.75] | 0.72 [0.65, 0.75] | -0.011 | 2.5 / 3.0 | 5.2 / 6.0 | 0.227 / 0.340 |
| 6' | 200 | 0.77 [0.73, 0.82] | 0.73 [0.67, 0.78] | +0.013 | 4.5 / 5.8 | 8.8 / 10.0 | 0.260 / 0.345 |
| 30' | 100 | 0.81 [0.72, 0.88] | 0.75 [0.66, 0.84] | +0.026 | 17.1 / 25.9 | 30.7 / 55.1 | 0.492 / 0.663 |
| 1deg | 40 | 0.52 [0.38, 0.70] | 0.50 [0.37, 0.65] | +0.038 | 56.4 / 96.8 | 78.0 / 245.1 | 1.009 / 2.189 |
| 3deg | 16 | 0.46 [0.40, 0.63] | 0.46 [0.40, 0.61] | +0.067 | 432.6 / 802.6 | 470.8 / 804.8 | 2.125 / 5.838 |

Row-count mismatches between the methods: 0 (expect 0).

Cold reads only count as cold if the I/O time per page read is disk-like (milliseconds); round sixty-nine saw about 0.08 ms, a host cache.
