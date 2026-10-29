
## gaia corpus: 120 convex polygons, 0.05-2 deg (rows agree across methods and passes: True)

| method | pass | median ms | mean ms | p95 ms |
|---|---|---|---|---|
| q3c | 1 | 4.385 | 4.957 | 8.474 |
| q3c | 2 | 3.413 | 4.009 | 7.202 |
| q3c | 3 | 3.378 | 3.938 | 7.916 |
| q3c | 4 | 3.513 | 4.295 | 7.515 |
| pgsphere | 1 | 1.417 | 8.729 | 50.595 |
| pgsphere | 2 | 1.255 | 8.692 | 50.073 |
| pgsphere | 3 | 1.203 | 9.453 | 53.635 |
| pgsphere | 4 | 1.273 | 10.061 | 54.390 |
| skycell | 1 | 0.502 | 0.916 | 3.248 |
| skycell | 2 | 0.262 | 0.582 | 2.460 |
| skycell | 3 | 0.257 | 0.559 | 2.199 |
| skycell | 4 | 0.251 | 0.508 | 2.260 |

## gaia corpus: point-in-footprint join, 200,000 probes, 20,000 circles

| method | pass | pairs | s | build s | index MB |
|---|---|---|---|---|---|
| pgsphere | 0 | | | 0.05 | 1.2 |
| skycell | 0 | | | 1.84 | 9.5 |
| pgsphere | 1 | 198596 | 1.50 | | |
| pgsphere | 2 | 198596 | 1.46 | | |
| skycell | 1 | 198596 | 1.41 | | |
| skycell | 2 | 198596 | 1.42 | | |
| skycell | 3 | 198596 | 1.40 | | |
| skycell | 4 | 198596 | 1.55 | | |
| pgsphere | 3 | 198596 | 1.55 | | |
| pgsphere | 4 | 198596 | 1.45 | | |

## designed corpus: 120 convex polygons, 0.05-2 deg (rows agree across methods and passes: True)

| method | pass | median ms | mean ms | p95 ms |
|---|---|---|---|---|
| q3c | 1 | 3.608 | 4.130 | 8.468 |
| q3c | 2 | 3.380 | 3.711 | 6.880 |
| q3c | 3 | 3.258 | 3.877 | 7.644 |
| q3c | 4 | 3.523 | 4.026 | 6.989 |
| pgsphere | 1 | 1.127 | 5.628 | 44.107 |
| pgsphere | 2 | 1.042 | 5.758 | 42.769 |
| pgsphere | 3 | 0.942 | 6.021 | 42.839 |
| pgsphere | 4 | 0.956 | 6.280 | 47.421 |
| skycell | 1 | 0.374 | 0.577 | 2.239 |
| skycell | 2 | 0.240 | 0.396 | 1.808 |
| skycell | 3 | 0.215 | 0.388 | 1.908 |
| skycell | 4 | 0.221 | 0.390 | 1.814 |

## designed corpus: point-in-footprint join, 200,000 probes, 20,000 circles

| method | pass | pairs | s | build s | index MB |
|---|---|---|---|---|---|
| pgsphere | 0 | | | 0.06 | 1.2 |
| skycell | 0 | | | 1.76 | 9.6 |
| pgsphere | 1 | 142645 | 1.49 | | |
| pgsphere | 2 | 142645 | 1.42 | | |
| skycell | 1 | 142645 | 1.45 | | |
| skycell | 2 | 142645 | 1.24 | | |
| skycell | 3 | 142645 | 1.30 | | |
| skycell | 4 | 142645 | 1.32 | | |
| pgsphere | 3 | 142645 | 1.37 | | |
| pgsphere | 4 | 142645 | 1.44 | | |
