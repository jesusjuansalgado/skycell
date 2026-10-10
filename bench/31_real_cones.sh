#!/bin/bash
# Driver for 30_real_cones.sql: tab:cones' real-corpus columns, warm and cold
# (GIST_REGION_DESIGN.md rounds sixty-eight and sixty-nine).
#
# Needs gaia_realc (19_gaia_load.sh) for warm, gaia_real_cell and
# gaia_real_sphere (19_gaia_load_split.sh) for cold, connection from the usual
# PG* variables, and two commands for the cold passes:
#   RESTART      restarts the PostgreSQL server and waits until it accepts
#                connections   (default: sudo systemctl restart postgresql)
#   DROP_CACHES  drops the OS page cache
#                (default: sync; echo 3 | sudo tee /proc/sys/vm/drop_caches)
# On a real VM with its own disk, DROP_CACHES makes reads genuinely cold; in a
# container it may not reach the host's cache (round sixty-nine measured about
# 0.08 ms per read there, not disk latency) -- check with the I/O times the
# report prints.
#
#   SCALE=1 OUT=results-realcone bench/31_real_cones.sh [warm] [cold]
set -euo pipefail
cd "$(dirname "$0")"
SCALE=${SCALE:-1}
OUT=${OUT:-results-realcone}
RESTART=${RESTART:-"sudo systemctl restart postgresql"}
DROP_CACHES=${DROP_CACHES:-"sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null"}
STAGES=${*:-warm cold}
mkdir -p "$OUT"
q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }
wait_up() { for _ in $(seq 1 60); do pg_isready -q && return 0; sleep 1; done; echo "server did not come back" >&2; exit 1; }
cold_restart() { eval "$RESTART"; eval "$DROP_CACHES"; wait_up; }

q -v scale="$SCALE" -f 30_real_cones.sql

if [[ " $STAGES " == *" warm "* ]]; then
  # each method's block runs alone; two orders, so no method always goes first.
  # skycell-rw (the range rewrite) only where the build has a custom scan.
  WARM_METHODS=${WARM_METHODS:-"skycell skycell-rw pgsphere"}
  rev=$(echo $WARM_METHODS | tr ' ' '\n' | tac | tr '\n' ' ')
  for order in "A $WARM_METHODS" "B $rev"; do
    set -- $order; blk=$1; shift
    for m in "$@"; do
      echo "warm block $blk: $m"
      q -c "SELECT realcone_block('$m', '$blk')" >/dev/null
    done
  done
fi

if [[ " $STAGES " == *" cold "* ]]; then
  RANDOM=69
  for l in '1"' '10"' "1'" "6'" "30'" 1deg 3deg; do
    if (( RANDOM % 2 )); then order="skycell pgsphere"; else order="pgsphere skycell"; fi
    for m in $order; do
      cold_restart
      echo "cold $l: $m"
      # the label goes through a psql variable: 1' and 1" break a quoted literal
      echo "SET track_io_timing = on; SELECT realcone_cold_pass('$m', :'lbl');" | q -v lbl="$l" >/dev/null
    done
  done
fi

for t in bench_realcone bench_realcone_cold realcone_centers realcone_cold_centers; do
  q -c "\\copy (SELECT * FROM $t) TO STDOUT WITH CSV HEADER" > "$OUT/$t.csv"
done
python3 realcone_report.py "$OUT" | tee "$OUT/report.md"
