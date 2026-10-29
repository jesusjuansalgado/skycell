#!/bin/bash
# Table 8's protocol at 10M, 0.26 and 0.36 alternating, each on a freshly
# rebuilt relation (as run.sh does per seed and size): 26/0.19, 36/0.19,
# 36/0.53, 26/0.53
set -euo pipefail
S=${S:?scratch directory holding ab026.so / ab036.so and ab/}
L=/opt/pg18/lib/postgresql/skycell.so
P="/opt/pg18/bin/psql -h /tmp -p 5519 -U postgres -X -q -At -v ON_ERROR_STOP=1"
swap() { cp $S/ab0$1.so $L.new && mv -f $L.new $L; }
for spec in 26:0.19 36:0.19 36:0.53 26:0.53; do
  v=${spec%%:*}; seed=${spec#*:}
  swap $v
  $P -d paper$v -c "CALL build_obscore(10000000)" \
     -c "SELECT bench_cross_run(10, 30, $seed, 3, ARRAY['pgsphere','skycell','skycell-rw'])" >/dev/null
  $P -d paper$v -c "\\copy (SELECT * FROM bench_cross WHERE rows_m = 10) TO STDOUT WITH CSV HEADER" > $S/ab/fair_0${v}_seed0${seed#0.}.csv
  echo "$spec done $(date -u +%T)"
done
swap 36
