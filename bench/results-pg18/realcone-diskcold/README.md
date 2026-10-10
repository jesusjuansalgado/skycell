# Real-corpus cold cones after the container was moved: remote-storage cold

The cold stage of `../../31_real_cones.sh` (custom scan vs pgSphere, per-method
tables `gaia_real_cell` / `gaia_real_sphere`, the same 1,456 fresh centres and
protocol as `../realcone/`: server restarted and page cache dropped before each
method's pass at each radius, first touch only), run right after this container
had been suspended and resumed on another host. Same software as `../`
(PostgreSQL 18.6, skycell at fce8dd4 with the custom scan on). Row counts agree
on every query. `report.md` is `../../realcone_report.py` (its warm sections are
`../realcone/`'s run, still in the database; only the cold section is new).

## Read cost: not flash, not a steady disk

Before the move, a first-touch read cost 0.06 ms whatever was done (an 18 GB
direct-I/O filler changed nothing): local flash. After it, a probe away from
every test centre read at **70 ms per page**: the disk image was being fetched
from remote storage. Over the run itself:

| | value |
|---|---|
| mean, all 37,274 reads | 3.47 ms per read (skycell 4.24, pgSphere 3.01) |
| median per query, by radius in run order (1" first) | 1": 0.48 / 0.88 ms (skycell / pgSphere), 10": 0.39 / 0.33, 1': 0.29 / 0.22, 6': 0.22 / 0.19, 30': 0.15 / 0.13, 1 deg: 0.16 / 0.12, 3 deg: 0.13 / 0.15 |

The cost fell as the run went on, from tens of milliseconds to a few tenths, as
the image was fetched back in chunks larger than a page: a read is expensive or
cheap depending on whether a neighbouring block has already brought its chunk
in, not only on how many pages a query reads. So this is a third regime,
between the fast flash of `../realcone/` (0.06-0.1 ms) and the first host's
steady slow disk (about 16 ms per read, GIST_REGION_DESIGN.md round forty-four),
and it is not a substitute for the latter.

## skycell (custom scan) / pgSphere, cold, paired median ratio, 95% bootstrap

| radius | n | remote-storage cold (this run) | flash cold (`../realcone/`) | pages read, skycell / pgSphere |
|---|---|---|---|---|
| 1"   | 400 | **0.80** [0.72, 0.87] | **0.87** [0.82, 0.94] | 1.7 / 2.5 |
| 10"  | 400 | 0.98 [0.91, 1.07]     | 1.01 [0.94, 1.09]     | 1.8 / 2.6 |
| 1'   | 300 | *1.22* [1.12, 1.34]   | 0.98 [0.90, 1.04]     | 2.5 / 3.0 |
| 6'   | 200 | 0.96 [0.87, 1.06]     | **0.61** [0.55, 0.65] | 4.5 / 5.8 |
| 30'  | 100 | 0.98 [0.85, 1.08]     | **0.67** [0.60, 0.72] | 17.1 / 25.9 |
| 1deg |  40 | 0.83 [0.70, 1.07]     | **0.53** [0.44, 0.66] | 56.4 / 97.1 |
| 3deg |  16 | **0.67** [0.37, 0.85] | **0.48** [0.43, 0.54] | 432.6 / 800.1 |

skycell reads fewer pages at every radius (0.54-0.85 of pgSphere's), the same
counts as on flash, but here a read's cost depends on the chunk it falls in more
than on the read itself, and skycell paid more per read on average. The
result is level or better at every radius except 1', and wider-intervalled
than on flash. What a steady slow disk gives remains to be measured with
`vm/run_paper.sh` on a VM with its own disk (vm/README.md, "Disk-cold check").
