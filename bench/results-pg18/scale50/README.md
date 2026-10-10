# tab:scale's 50M column on PostgreSQL 18.6, custom scan as the default

`run.sh`: 5e7 rows resampled from the DR3 order-9 map (`11_scale_corpus.sql`,
`../../data/gaia_map_hpx9.csv.gz`), the three layouts (`12_scale_build.sql`),
then `07_ab.sql`'s protocol: warm, one untimed pass then 5 repetitions of q3c,
pgsphere, skycell (the custom scan) and skycell-rw (the range rewrite,
`skycell.custom_scan = off`); cold, the server stopped and the page cache dropped
before one pass of q3c, pgsphere and skycell; the EXPLAIN pass. Same host and
versions as `../` (PostgreSQL 18.6, pgSphere 1.5.2, Q3C 2.0.5, skycell 0.26;
4 vCPU, 15 GB, shared_buffers 2 GB). `ab_report.md` is `ab_report.py`;
`rewrite_vs_custom.txt` is `../pair.py` on the warm trials.

Indexes (`bench_build50.csv`): skycell's B-tree 1071 MB, built in 16.7 s; Q3C's
1071 MB, 25.3 s; pgSphere's GiST 3439 MB, 387 s -- larger than shared_buffers,
as on the first host.

## skycell / pgSphere, paired median ratio, 95% bootstrap

| radius | n | custom, warm | rewrite, warm | custom / rewrite, warm | custom, cold |
|---|---|---|---|---|---|
| 1"   | 400 | **0.78** [0.73, 0.84] | 0.94 [0.89, 1.00] | **0.81** | **0.82** [0.79, 0.85] |
| 10"  | 400 | **0.78** [0.73, 0.86] | 0.96 [0.92, 1.00] | **0.85** | **0.86** [0.83, 0.92] |
| 1'   | 300 | **0.86** [0.79, 0.91] | 1.04 [0.99, 1.12] | **0.84** | 0.98 [0.92, 1.03] |
| 6'   | 200 | **0.86** [0.82, 0.94] | *1.09* [1.04, 1.14] | **0.81** | **0.81** [0.75, 0.86] |
| 30'  | 100 | **0.75** [0.71, 0.77] | **0.80** [0.76, 0.89] | **0.94** | **0.53** [0.46, 0.59] |
| 1deg |  40 | **0.69** [0.67, 0.71] | **0.68** [0.64, 0.75] | 1.02 | **0.37** [0.32, 0.45] |
| 3deg |  16 | **0.73** [0.66, 0.75] | **0.72** [0.67, 0.82] | 0.99 | **0.42** [0.37, 0.52] |

The rewrite here reproduces the first host's 50M rewrite column in direction
(0.89-1.02 below 30', 0.58-0.72 above). The custom scan's gain over it is at
small radii (0.81-0.85 below 30', from planning: 0.042 ms against the rewrite's
larger covering cost), and nil from 30' up, where the scan's pages decide and
both read the same ones (EXPLAIN: 188 buffers against pgSphere's 642 at 1 deg).
Cold, the custom scan is 0.37-0.53 of pgSphere from 30' up and 0.81-0.98 below.
The cold pass drops the guest page cache only; on this container type reads
were measured at about 0.1 ms (results-pg18/realcone), a host-level cache, so
"cold" means cold for PostgreSQL and the VM, as for `../`'s own cold columns.
