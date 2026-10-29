
## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell / pgsphere

| radius | n | total, skycell/pgsphere | execution only | planning, skycell - pgsphere (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 0.54 [0.53, 0.56] | 0.29 [0.28, 0.30] | +0.002 | 4.3 / 5.6 |
| 10" | 400 | 0.52 [0.50, 0.53] | 0.28 [0.27, 0.29] | +0.001 | 4.7 / 5.8 |
| 1' | 300 | 0.57 [0.55, 0.60] | 0.33 [0.32, 0.35] | +0.003 | 5.3 / 6.2 |
| 6' | 200 | 0.57 [0.54, 0.59] | 0.44 [0.41, 0.46] | -0.000 | 10.5 / 11.8 |
| 30' | 100 | 0.57 [0.54, 0.59] | 0.52 [0.50, 0.54] | -0.002 | 41.9 / 93.9 |
| 1deg | 40 | 0.60 [0.55, 0.65] | 0.58 [0.54, 0.60] | +0.002 | 90.6 / 308.0 |
| 3deg | 16 | 0.59 [0.55, 0.62] | 0.57 [0.54, 0.62] | +0.001 | 543.2 / 963.0 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell-rw / pgsphere

| radius | n | total, skycell-rw/pgsphere | execution only | planning, skycell-rw - pgsphere (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 1.00 [0.95, 1.04] | 0.39 [0.38, 0.43] | +0.021 | 4.2 / 5.6 |
| 10" | 400 | 1.00 [0.96, 1.04] | 0.40 [0.38, 0.43] | +0.024 | 4.7 / 5.8 |
| 1' | 300 | 0.93 [0.89, 0.96] | 0.43 [0.40, 0.45] | +0.019 | 5.1 / 6.2 |
| 6' | 200 | 1.38 [1.30, 1.47] | 0.80 [0.73, 0.85] | +0.045 | 9.5 / 11.8 |
| 30' | 100 | 1.04 [0.95, 1.13] | 0.71 [0.64, 0.78] | +0.076 | 35.6 / 93.9 |
| 1deg | 40 | 0.81 [0.64, 0.95] | 0.63 [0.55, 0.71] | +0.087 | 82.5 / 308.0 |
| 3deg | 16 | 0.57 [0.52, 0.76] | 0.54 [0.48, 0.57] | +0.253 | 538.2 / 963.0 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell / skycell-rw

| radius | n | total, skycell/skycell-rw | execution only | planning, skycell - skycell-rw (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 0.55 [0.53, 0.58] | 0.74 [0.72, 0.76] | -0.019 | 4.3 / 4.2 |
| 10" | 400 | 0.51 [0.49, 0.53] | 0.69 [0.67, 0.70] | -0.023 | 4.7 / 4.7 |
| 1' | 300 | 0.63 [0.60, 0.64] | 0.79 [0.77, 0.82] | -0.017 | 5.3 / 5.1 |
| 6' | 200 | 0.41 [0.40, 0.43] | 0.53 [0.51, 0.56] | -0.046 | 10.5 / 9.5 |
| 30' | 100 | 0.55 [0.50, 0.58] | 0.70 [0.65, 0.74] | -0.076 | 41.9 / 35.6 |
| 1deg | 40 | 0.73 [0.68, 0.81] | 0.90 [0.79, 0.97] | -0.088 | 90.6 / 82.5 |
| 3deg | 16 | 1.06 [0.78, 1.10] | 1.10 [0.94, 1.15] | -0.253 | 543.2 / 538.2 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, cold (gaia_real_cell / gaia_real_sphere, restart before each pass): skycell / pgsphere

| radius | n | total, skycell/pgsphere | execution only | planning, skycell - pgsphere (ms) | pages read, skycell / pgSphere | buffers, skycell / pgSphere | I/O ms, skycell / pgSphere |
|---|---|---|---|---|---|---|---|
| 1" | 400 | 0.66 [0.62, 0.70] | 0.62 [0.59, 0.67] | -0.002 | 1.7 / 2.6 | 4.1 / 6.0 | 0.158 / 0.242 |
| 10" | 400 | 0.64 [0.61, 0.68] | 0.63 [0.59, 0.66] | -0.012 | 1.8 / 2.6 | 4.4 / 6.0 | 0.168 / 0.250 |
| 1' | 300 | 0.89 [0.83, 0.94] | 0.85 [0.80, 0.89] | +0.007 | 2.5 / 2.9 | 5.2 / 6.1 | 0.216 / 0.247 |
| 6' | 200 | 0.74 [0.69, 0.79] | 0.69 [0.66, 0.74] | +0.008 | 4.5 / 5.7 | 8.9 / 10.1 | 0.279 / 0.401 |
| 30' | 100 | 0.90 [0.73, 1.01] | 0.86 [0.70, 0.96] | +0.043 | 17.1 / 26.6 | 30.3 / 58.3 | 0.804 / 0.938 |
| 1deg | 40 | 0.44 [0.38, 0.59] | 0.43 [0.37, 0.56] | +0.051 | 56.4 / 97.3 | 78.1 / 249.9 | 0.900 / 2.308 |
| 3deg | 16 | 0.54 [0.43, 0.73] | 0.54 [0.43, 0.71] | +0.097 | 432.6 / 802.5 | 470.8 / 805.1 | 2.553 / 5.913 |

Row-count mismatches between the methods: 0 (expect 0).

Cold reads only count as cold if the I/O time per page read is disk-like (milliseconds); round sixty-nine saw about 0.08 ms, a host cache.
