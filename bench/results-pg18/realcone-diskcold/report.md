
## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell / pgsphere

| radius | n | total, skycell/pgsphere | execution only | planning, skycell - pgsphere (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 0.83 [0.81, 0.84] | 0.52 [0.50, 0.54] | +0.004 | 4.3 / 5.7 |
| 10" | 400 | 0.85 [0.82, 0.86] | 0.55 [0.53, 0.57] | +0.004 | 4.7 / 5.7 |
| 1' | 300 | 0.90 [0.87, 0.92] | 0.63 [0.61, 0.66] | +0.004 | 5.3 / 6.2 |
| 6' | 200 | 0.96 [0.93, 0.99] | 0.82 [0.80, 0.85] | +0.004 | 10.5 / 12.3 |
| 30' | 100 | 0.81 [0.78, 0.86] | 0.76 [0.74, 0.79] | +0.004 | 41.9 / 95.5 |
| 1deg | 40 | 0.80 [0.75, 0.88] | 0.80 [0.72, 0.83] | +0.003 | 90.4 / 312.6 |
| 3deg | 16 | 0.69 [0.64, 0.75] | 0.68 [0.64, 0.72] | +0.004 | 543.5 / 959.9 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell-rw / pgsphere

| radius | n | total, skycell-rw/pgsphere | execution only | planning, skycell-rw - pgsphere (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 1.29 [1.25, 1.33] | 0.65 [0.63, 0.67] | +0.016 | 4.2 / 5.7 |
| 10" | 400 | 1.34 [1.31, 1.40] | 0.68 [0.65, 0.71] | +0.017 | 4.7 / 5.7 |
| 1' | 300 | 1.27 [1.23, 1.31] | 0.73 [0.71, 0.75] | +0.014 | 5.1 / 6.2 |
| 6' | 200 | 1.56 [1.49, 1.64] | 1.06 [1.00, 1.11] | +0.024 | 9.5 / 12.3 |
| 30' | 100 | 1.52 [1.31, 1.71] | 1.12 [1.00, 1.22] | +0.058 | 35.6 / 95.5 |
| 1deg | 40 | 1.25 [0.98, 1.51] | 0.96 [0.83, 1.18] | +0.076 | 82.6 / 312.6 |
| 3deg | 16 | 0.74 [0.73, 1.06] | 0.73 [0.68, 0.82] | +0.169 | 538.0 / 959.9 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, warm (gaia_realc, per-method blocks, two orders): skycell / skycell-rw

| radius | n | total, skycell/skycell-rw | execution only | planning, skycell - skycell-rw (ms) | buffers, a / b |
|---|---|---|---|---|---|
| 1" | 400 | 0.64 [0.63, 0.65] | 0.82 [0.80, 0.83] | -0.011 | 4.3 / 4.2 |
| 10" | 400 | 0.62 [0.60, 0.64] | 0.80 [0.79, 0.82] | -0.013 | 4.7 / 4.7 |
| 1' | 300 | 0.70 [0.68, 0.72] | 0.86 [0.84, 0.88] | -0.010 | 5.3 / 5.1 |
| 6' | 200 | 0.62 [0.60, 0.64] | 0.77 [0.75, 0.79] | -0.019 | 10.5 / 9.5 |
| 30' | 100 | 0.57 [0.52, 0.61] | 0.71 [0.66, 0.77] | -0.056 | 41.9 / 35.6 |
| 1deg | 40 | 0.66 [0.63, 0.76] | 0.77 [0.73, 0.90] | -0.074 | 90.4 / 82.6 |
| 3deg | 16 | 0.92 [0.69, 0.98] | 0.97 [0.74, 1.01] | -0.166 | 543.5 / 538.0 |

Row-count mismatches between the methods: 0 (expect 0).

## Real corpus, cold (gaia_real_cell / gaia_real_sphere, restart before each pass): skycell / pgsphere

| radius | n | total, skycell/pgsphere | execution only | planning, skycell - pgsphere (ms) | pages read, skycell / pgSphere | buffers, skycell / pgSphere | I/O ms, skycell / pgSphere |
|---|---|---|---|---|---|---|---|
| 1" | 400 | 0.80 [0.72, 0.87] | 0.76 [0.69, 0.82] | +0.018 | 1.7 / 2.5 | 4.1 / 5.5 | 0.641 / 1.271 |
| 10" | 400 | 0.98 [0.91, 1.07] | 0.94 [0.87, 1.04] | +0.021 | 1.8 / 2.6 | 4.4 / 5.7 | 0.606 / 0.677 |
| 1' | 300 | 1.22 [1.12, 1.34] | 1.20 [1.12, 1.35] | +0.024 | 2.5 / 3.0 | 5.2 / 5.9 | 0.671 / 0.545 |
| 6' | 200 | 0.96 [0.87, 1.06] | 0.93 [0.84, 1.04] | +0.020 | 4.5 / 5.8 | 8.8 / 9.8 | 0.809 / 0.848 |
| 30' | 100 | 0.98 [0.85, 1.08] | 0.96 [0.84, 1.06] | +0.037 | 17.1 / 25.9 | 30.6 / 55.4 | 1.426 / 1.796 |
| 1deg | 40 | 0.83 [0.70, 1.07] | 0.80 [0.68, 1.05] | +0.079 | 56.4 / 97.1 | 77.1 / 250.7 | 3.587 / 3.689 |
| 3deg | 16 | 0.67 [0.37, 0.85] | 0.67 [0.35, 0.85] | +0.091 | 432.6 / 800.1 | 469.8 / 802.1 | 4.494 / 13.590 |

Row-count mismatches between the methods: 0 (expect 0).

Cold reads only count as cold if the I/O time per page read is disk-like (milliseconds); round sixty-nine saw about 0.08 ms, a host cache.
