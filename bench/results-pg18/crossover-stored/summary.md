
bench_cross_seed019.csv
| class | skycell/pgSphere 0.5M | 2M | 10M | rewrite/pgSphere 0.5M | 2M | 10M | median rows 0.5M | 2M | 10M | median buffers at 10M pgSphere / skycell | counts agree |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Q05 | 1.14 | 1.06 | 0.96 | 3.45 | 2.71 | 2.20 | 48 | 62 | 126 | 32 / 32 | True |
| Q06 | 0.88 | 0.92 | 0.74 | 1.25 | 1.03 | 0.75 | 576 | 1651 | 8350 | 939 / 591 | True |
| Q07 | 1.05 | 0.94 | 0.80 | 2.07 | 1.75 | 1.06 | 128 | 208 | 502 | 116 / 89 | True |
| Q12 | 0.66 | 0.65 | 0.41 | 1.17 | 1.16 | 0.73 | 0 | 0 | 0 | 8 / 3 | True |

bench_cross_seed053.csv
| class | skycell/pgSphere 0.5M | 2M | 10M | rewrite/pgSphere 0.5M | 2M | 10M | median rows 0.5M | 2M | 10M | median buffers at 10M pgSphere / skycell | counts agree |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Q05 | 1.13 | 1.07 | 0.98 | 3.59 | 2.91 | 2.08 | 48 | 62 | 126 | 29 / 32 | True |
| Q06 | 0.97 | 0.93 | 0.76 | 1.36 | 1.02 | 0.80 | 576 | 1651 | 8350 | 979 / 591 | True |
| Q07 | 0.86 | 1.02 | 0.81 | 1.74 | 1.60 | 1.05 | 128 | 208 | 502 | 104 / 89 | True |
| Q12 | 0.73 | 0.71 | 0.53 | 0.91 | 1.19 | 0.86 | 0 | 0 | 0 | 7 / 3 | True |
