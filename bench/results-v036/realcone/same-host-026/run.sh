#!/bin/bash
# The real-cone protocol (31_real_cones.sh, warm and cold) under 0.26 and 0.36
# on this host, alternating 26, 36, 36, 26, each version in its own database
# (load.sh).  Each run's CSVs and report go to run<i>_0<v>/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../server.sh"
S=${S:-/home/user/build}
L=/home/user/pg18/lib/postgresql/skycell.so
swap() { cp "$S/sk0$1.so" "$L.new" && mv -f "$L.new" "$L"; }
cd "$HERE/../../.."
i=${FIRST:-0}
for v in ${VERSIONS:-26 36 36 26}; do
  i=$((i+1)); swap $v; eval "$RESTART"
  export PGDATABASE=real$v
  psql -X -q -c "DROP TABLE IF EXISTS bench_realcone, bench_realcone_cold"
  echo "run $i (0.$v) start $(date -u +%T)"
  SCALE=1 OUT="$HERE/run${i}_0$v" ./31_real_cones.sh ${STAGES:-warm cold} > "$HERE/run${i}_0$v.log" 2>&1
  echo "run $i (0.$v) done $(date -u +%T)"
done
swap 36; eval "$RESTART"
