#!/bin/bash
# tab:estimator with skycell 0.36: ../../29_estimator.sql unchanged, as in
# ../../results-pg18/estimator/, on gaia_realc (19_gaia_load.sh).
# Run after realcone and xmreal: its ten ANALYZEs replace gaia_realc's statistics.
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)
. "$OUT/server.sh"
cd "$OUT/../.."
psql -X -v tbl=gaia_realc -v reps=10 -v decision=1 -f 29_estimator.sql > "$OUT/29_estimator.out" 2>&1
psql -X -f "$OUT/summary.sql" > "$OUT/summary.txt"
for t in est_truth est_runs est_decision; do
  psql -X -q -c "\\copy (SELECT * FROM $t WHERE tbl = 'gaia_realc') TO STDOUT WITH CSV HEADER" > "$OUT/$t.csv"
done
psql -X -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
