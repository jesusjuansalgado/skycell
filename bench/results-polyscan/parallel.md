# Large polygons in parallel (0.43)

Constant polygons through the custom scan inherit the parallel chunked scan of
0.42. On gaia_realc (10M real Gaia DR3 positions, heap in cell order, 4 vCPU,
PostgreSQL 18.6), shapes centred on the Galactic centre, best of 5 warm runs
(`pbench`, as for the 0.42 cone table in the README), counts identical in
every row. "rewrite" is the same query with `skycell.custom_scan = off`, the
path every polygon took up to 0.42; with workers allowed PostgreSQL runs it
as a Parallel Bitmap Heap Scan.

| search | rows | rewrite, serial | scan, serial | rewrite, 2 workers | scan, 2 workers |
|---|---|---|---|---|---|
| BOX 10° × 10° | 375,610 | 113.2 ms | 58.3 ms | 43.7 ms | 22.8 ms |
| U, 11° × 10°, 8 vertices | 303,212 | 51.1 ms | 48.4 ms | 28.4 ms | 20.9 ms |
| BOX 5° × 5° | 77,116 | — | 10.9–19.1 ms | — | planned serial |
| BOX 2° × 2° | 4,807 | — | 1.0 ms | — | planned serial |

The 5° box is below the page count at which PostgreSQL gives a scan a worker
and stays serial; its two serial timings, minutes apart, show this host's
run-to-run spread.
