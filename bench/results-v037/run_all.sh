#!/bin/bash
# tab:cones (all columns), tab:scale and Fig. 1 with skycell 0.37 (tag v0.37 of
# jesusjuansalgado/skycell; its extension catalogue is still extversion 0.36,
# so versions.txt also records the tag and the library's checksum).
#   cones/     ../results-v036/cones/run.sh's protocol: designed and Gaia-resampled
#              10M corpora, then the 5e7 stage, in database cone37
#   realcone/  ../results-v036/realcone/run.sh's protocol (31_real_cones.sh) on
#              real36's gaia_realc, gaia_real_cell and gaia_real_sphere
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/server.sh"
export STOP_START="pg_stop; $DROP_CACHES; pg_start"
ver() {
  psql -X -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$1/versions.txt"
  echo "skycell library: tag v0.37 (1465080), md5 $(md5sum /home/user/pg18/lib/postgresql/skycell.so | cut -d' ' -f1)" >> "$1/versions.txt"
}
psql -X -q -d postgres -c "DROP DATABASE IF EXISTS cone37" -c "CREATE DATABASE cone37"
PGDATABASE=cone37 psql -X -q -c "CREATE EXTENSION skycell" -c "CREATE EXTENSION q3c" -c "CREATE EXTENSION pg_sphere"
PGDATABASE=cone37 "$HERE/cones/run.sh" designed gaia
PGDATABASE=cone37 "$HERE/cones/run.sh" gaia50
PGDATABASE=cone37 ver "$HERE/cones"
PGDATABASE=real36 "$HERE/realcone/run.sh"
PGDATABASE=real36 ver "$HERE/realcone"
echo ALL_DONE
