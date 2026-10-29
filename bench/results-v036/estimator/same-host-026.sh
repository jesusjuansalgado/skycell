#!/bin/bash
# 29_estimator.sql (reps 10, decision 1) under 0.26 and 0.36 on this host,
# alternating 26, 36, 36, 26, each version in its own database
# (../realcone/same-host-026/load.sh: the same gaia_realc heap).  Per run: the
# full output, summary.txt's table, and the coverings part (b)'s 'chosen' used.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/server.sh"
L=/home/user/pg18/lib/postgresql/skycell.so
swap() { cp "/home/user/build/sk0$1.so" "$L.new" && mv -f "$L.new" "$L"; }
mkdir -p "$HERE/same-host-026"
cd "$HERE/../.."
i=0
for v in 26 36 36 26; do
  i=$((i+1)); swap $v; eval "$RESTART"
  o="$HERE/same-host-026/run${i}_0$v"
  psql -X -d real$v -v tbl=gaia_realc -v reps=10 -v decision=1 -f 29_estimator.sql > "$o.out" 2>&1
  psql -X -d real$v -v tbl=gaia_realc -f "$HERE/summary_tbl.sql" > "$o.summary.txt"
  psql -X -d real$v -v tbl=gaia_realc -f "$HERE/cover.sql" > "$o.cover.txt"
  echo "run $i (0.$v) done $(date -u +%T)"
done
swap 36; eval "$RESTART"
