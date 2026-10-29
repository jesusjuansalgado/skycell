#!/bin/bash
# xms_run under 0.26 and 0.36 on this host, alternating 26, 36, 36, 26, each
# version in its own database (../realcone/same-host-026/load.sh: real26 under
# skycell 0.26's library and catalogue, the same gaia_realc heap).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/server.sh"
S=${S:-/home/user/build}
L=/home/user/pg18/lib/postgresql/skycell.so
swap() { cp "$S/sk0$1.so" "$L.new" && mv -f "$L.new" "$L"; }
mkdir -p "$HERE/same-host-026"
cd "$HERE/../.."
i=0
for v in 26 36 36 26; do
  i=$((i+1)); swap $v; eval "$RESTART"
  export PGDATABASE=real$v
  psql -X -q -v ON_ERROR_STOP=1 -f 18_xmatch_sweep.sql
  echo "run $i (0.$v) start $(date -u +%T)"
  psql -X -q -v ON_ERROR_STOP=1 -c "SELECT xms_run('{1000,10000,100000}', '{0.5,1,1.5,5}', '{clustered,uniform}')"
  psql -X -q -c "\\copy (SELECT * FROM xms) TO STDOUT WITH CSV HEADER" > "$HERE/same-host-026/xms_run${i}_0$v.csv"
  echo "run $i (0.$v) done $(date -u +%T)"
done
swap 36; eval "$RESTART"
