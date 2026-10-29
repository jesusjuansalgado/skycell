
## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell / pgsphere

| radius | n | total, skycell/pgsphere | execution only | planning, skycell - pgsphere (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 0.81 [0.79, 0.83] | 0.52 [0.50, 0.54] | +0.004 | 4.3 / 5.6 |
| 10" | 400 | 0.82 [0.79, 0.84] | 0.54 [0.51, 0.55] | +0.005 | 4.7 / 5.8 |
| 1' | 300 | 0.88 [0.85, 0.90] | 0.62 [0.57, 0.64] | +0.005 | 5.3 / 6.2 |
| 6' | 200 | 0.95 [0.90, 0.99] | 0.80 [0.76, 0.84] | +0.006 | 10.6 / 11.8 |
| 30' | 100 | 0.85 [0.81, 0.90] | 0.78 [0.74, 0.83] | +0.007 | 41.9 / 93.9 |
| 1deg | 40 | 0.82 [0.72, 0.89] | 0.77 [0.68, 0.82] | +0.008 | 90.3 / 308.0 |
| 3deg | 16 | 0.66 [0.62, 0.70] | 0.64 [0.61, 0.69] | +0.006 | 544.1 / 963.0 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell-rw / pgsphere

| radius | n | total, skycell-rw/pgsphere | execution only | planning, skycell-rw - pgsphere (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 1.27 [1.24, 1.30] | 0.67 [0.64, 0.69] | +0.017 | 4.2 / 5.6 |
| 10" | 400 | 1.30 [1.26, 1.35] | 0.68 [0.65, 0.70] | +0.019 | 4.7 / 5.8 |
| 1' | 300 | 1.32 [1.29, 1.36] | 0.76 [0.72, 0.79] | +0.019 | 5.1 / 6.2 |
| 6' | 200 | 1.54 [1.49, 1.62] | 1.04 [0.97, 1.13] | +0.027 | 9.5 / 11.8 |
| 30' | 100 | 1.40 [1.14, 1.51] | 1.01 [0.86, 1.10] | +0.057 | 35.6 / 93.9 |
| 1deg | 40 | 1.22 [0.82, 1.39] | 0.94 [0.71, 1.14] | +0.083 | 82.7 / 308.0 |
| 3deg | 16 | 0.68 [0.64, 0.94] | 0.64 [0.61, 0.78] | +0.173 | 537.8 / 963.0 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell / skycell-rw

| radius | n | total, skycell/skycell-rw | execution only | planning, skycell - skycell-rw (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 0.62 [0.61, 0.65] | 0.79 [0.77, 0.81] | -0.013 | 4.3 / 4.2 |
| 10" | 400 | 0.62 [0.59, 0.65] | 0.78 [0.77, 0.81] | -0.015 | 4.7 / 4.7 |
| 1' | 300 | 0.64 [0.63, 0.67] | 0.78 [0.76, 0.80] | -0.014 | 5.3 / 5.1 |
| 6' | 200 | 0.60 [0.58, 0.64] | 0.74 [0.71, 0.77] | -0.022 | 10.6 / 9.5 |
| 30' | 100 | 0.65 [0.61, 0.74] | 0.83 [0.75, 0.90] | -0.053 | 41.9 / 35.6 |
| 1deg | 40 | 0.72 [0.64, 0.87] | 0.86 [0.77, 0.96] | -0.076 | 90.3 / 82.7 |
| 3deg | 16 | 0.95 [0.84, 1.00] | 1.01 [0.92, 1.03] | -0.175 | 544.1 / 537.8 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, cold (gaia_real_cell / gaia_real_sphere, restart before each pass): skycell / pgsphere

| radius | n | total, skycell/pgsphere | execution only | planning, skycell - pgsphere (ms) | pages read, skycell / pgSphere | buffers, skycell / pgSphere | I/O ms, skycell / pgSphere |
|---|---|---|---|---|---|---|---|
| 1" | 400 | 0.78 [0.74, 0.83] | 0.74 [0.70, 0.78] | +0.007 | 1.7 / 2.6 | 4.1 / 6.0 | 0.192 / 0.254 |
| 10" | 400 | 0.93 [0.88, 0.99] | 0.86 [0.81, 0.91] | +0.018 | 1.8 / 2.6 | 4.4 / 6.0 | 0.209 / 0.247 |
| 1' | 300 | 0.92 [0.87, 0.99] | 0.94 [0.85, 1.00] | -0.005 | 2.5 / 2.9 | 5.2 / 6.1 | 0.228 / 0.239 |
| 6' | 200 | 0.82 [0.77, 0.88] | 0.78 [0.73, 0.84] | +0.017 | 4.5 / 5.7 | 8.9 / 10.1 | 0.382 / 0.493 |
| 30' | 100 | 0.56 [0.51, 0.62] | 0.53 [0.48, 0.58] | +0.002 | 17.1 / 26.6 | 30.3 / 58.3 | 0.707 / 1.348 |
| 1deg | 40 | 0.51 [0.44, 0.60] | 0.49 [0.44, 0.57] | +0.043 | 56.4 / 97.3 | 78.1 / 249.9 | 1.033 / 2.128 |
| 3deg | 16 | 0.49 [0.43, 0.58] | 0.49 [0.43, 0.56] | +0.070 | 432.6 / 802.5 | 470.8 / 805.1 | 3.149 / 6.236 |

Row-count mismatches between the methods: 0 (expect 0).

Cold reads only count as cold if the I/O time per page read is disk-like (milliseconds); round sixty-nine saw about 0.08 ms, a host cache.
