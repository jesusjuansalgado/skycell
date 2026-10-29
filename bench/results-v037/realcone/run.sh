#!/bin/bash
# tab:cones' real-corpus columns with skycell 0.37: ../../31_real_cones.sh
# (30_real_cones.sql) unchanged, as in ../../results-pg18/realcone/.
# server.sh: the PostgreSQL 18.6 build and REPRODUCING.md §2 settings on the
# command line; RESTART stops and starts the server, DROP_CACHES drops the
# page cache.  Corpora: 19_gaia_load.sh (gaia_realc), 19_gaia_load_split.sh
# (gaia_real_cell, gaia_real_sphere).
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)
. "$OUT/../server.sh"
cd "$OUT/../.."
psql -X -q -c "DROP TABLE IF EXISTS bench_realcone, bench_realcone_cold"
SCALE=1 OUT="$OUT" ./31_real_cones.sh warm cold 2>&1 | tee "$OUT/run.log"
psql -X -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
