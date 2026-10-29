#!/bin/bash
# tab:xmreal with skycell 0.36: ../../18_xmatch_sweep.sql's xms_run unchanged,
# as in ../../results-pg18/xmreal/, on gaia_realc (19_gaia_load.sh).
# server.sh: the PostgreSQL 18.6 build and REPRODUCING.md §2 settings.
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)
. "$OUT/server.sh"
cd "$OUT/../.."
psql -X -q -v ON_ERROR_STOP=1 -f 18_xmatch_sweep.sql
psql -X -q -v ON_ERROR_STOP=1 -c "SELECT xms_run('{1000,10000,100000}', '{0.5,1,1.5,5}', '{clustered,uniform}')" 2>&1 | tee "$OUT/run.log"
psql -X -q -c "\\copy (SELECT * FROM xms) TO STDOUT WITH CSV HEADER" > "$OUT/xms.csv"
python3 "$OUT/report.py" "$OUT/xms.csv" | tee "$OUT/report.md"
psql -X -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
