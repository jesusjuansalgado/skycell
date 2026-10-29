#!/bin/bash
# The resampled-catalogue cross-match with skycell 0.36: results-pg18/xmatch's
# protocol (report_join.md) unchanged -- the 10M Gaia-density corpus (11, 02),
# 200,000 probes from 04_xmatch.sql in 8 blocks, radii 1" and 10", 2
# repetitions, methods q3c, pgsphere, skycell_slots (the range rewrite),
# skycell_cs (the custom scan) and skycell_join (Q3C's spelling).
#
#   PGHOST=/tmp PGPORT=5519 PGUSER=postgres PGDATABASE=cone36 ./run.sh
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)
cd "$OUT/../.."
q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }
step() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$OUT/run.log"; }
step "10M Gaia-density corpus (11, 02)"
q -c "CREATE TABLE IF NOT EXISTS gaia_map (hpx9 bigint PRIMARY KEY, n bigint)"
if [ "$(q -At -c 'SELECT count(*) FROM gaia_map')" = 0 ]; then
  zcat data/gaia_map_hpx9.csv.gz | q -c "\\copy gaia_map FROM STDIN CSV HEADER"
fi
q -v n=10000000 < 11_scale_corpus.sql > /dev/null
q < 02_build.sql > /dev/null
step "probes (04) and 11_xmatch_ab.sql"
q < 04_xmatch.sql > /dev/null
q < 11_xmatch_ab.sql > /dev/null
step "bench_xm_ab_run(10, {1,10}, 2)"
q -c "SELECT bench_xm_ab_run(10, ARRAY[1, 10]::float8[], 2)" > /dev/null
q -c "\\copy (SELECT * FROM bench_xm_ab) TO STDOUT WITH CSV HEADER" > "$OUT/bench_xm_ab_join.csv"
python3 results-pg18/xmatch/report.py "$OUT/bench_xm_ab_join.csv" > "$OUT/report_join.md"
q -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
step DONE
