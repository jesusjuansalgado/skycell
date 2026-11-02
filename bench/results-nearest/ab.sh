#!/bin/bash
# Same-day A/B of two skycell libraries on the nearest-neighbour benchmark:
# each round installs one library and then the other (order alternating),
# and times micro_knn.sql on both tables with each.  pgSphere's times in the
# same runs show the host's drift.
#   PGHOST=/tmp PGPORT=5519 PGUSER=postgres ./ab.sh <libA.so> <labelA> <libB.so> <labelB> [rounds]
set -euo pipefail
cd "$(dirname "$0")"
rounds=${5:-2}
out=ab.csv
echo lib,round,table,k,qid,method,us,n > $out
for r in $(seq 1 "$rounds"); do
  if [ $((r % 2)) = 1 ]; then order="1 2"; else order="2 1"; fi
  for i in $order; do
    if [ $i = 1 ]; then lib=$1; label=$2; else lib=$3; label=$4; fi
    cp "$lib" "$(pg_config --pkglibdir)/.skycell.so.ab" && mv -f "$(pg_config --pkglibdir)/.skycell.so.ab" "$(pg_config --pkglibdir)/skycell.so"   # by rename: see ../perfcheck/perfcheck.sh
    for db in oc05 real36; do
      t=$([ $db = oc05 ] && echo oc || echo gaia_realc)
      psql -d $db -X -q -c "SELECT count(*) FROM $t" >/dev/null
      psql -d $db -X -q -f micro_knn.sql -c "SELECT setseed(0.5)" \
        -c "SELECT mk_run('$t', '$t', 20, 3, ARRAY[1, 10, 100, 1000, -10])" \
        -c "\copy mk to stdout csv" | grep -E "^(oc|gaia_realc)," | sed "s/^/$label,$r,/" >> $out
    done
  done
done
