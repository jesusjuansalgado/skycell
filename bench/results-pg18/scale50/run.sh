#!/bin/bash
# tab:scale's 50M column on PostgreSQL 18.6: vm/run_paper.sh's gaia50 cone protocol
# with ../run.sh's methods -- warm q3c, pgsphere, skycell (custom scan) and
# skycell-rw (the range rewrite); cold q3c, pgsphere, skycell (skycell-rw shares
# cat_cell, so whichever ran second would find it warm); the EXPLAIN pass.
# Expects the PG* variables for the PostgreSQL 18 server and two commands:
# STOP_START (stop, drop the page cache, start) for the cold pass.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd); cd "$REPO/bench"
OUT=$REPO/bench/results-pg18/scale50; LOG=$OUT/run.log; DB=${DB:-g50}
q() { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" "$@"; }
step() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
WARM="ARRAY['q3c','pgsphere','skycell','skycell-rw']"
COLD="ARRAY['q3c','pgsphere','skycell']"
step "STAGE gaia50: 5e7 rows resampled from the DR3 order-9 map"
q -c "CREATE TABLE IF NOT EXISTS gaia_map (hpx9 bigint PRIMARY KEY, n bigint)"
if [ "$(q -At -c 'SELECT count(*) FROM gaia_map')" = 0 ]; then
  zcat "$REPO/bench/data/gaia_map_hpx9.csv.gz" | q -c "\\copy gaia_map FROM STDIN CSV HEADER"
fi
q -v n=5e7 < 11_scale_corpus.sql | tee -a "$LOG"
q < 12_scale_build.sql | tee -a "$LOG"
q -c "\\copy (SELECT * FROM bench_build50) TO STDOUT WITH CSV HEADER" > "$OUT/bench_build50.csv"
step "  query centres (03) + 07"
q < 03_cone.sql >/dev/null; q < 07_ab.sql >/dev/null
step "  warm A/B, 5 repetitions"
q -c "SELECT bench_ab_run('gaia50', 'warm', 5, 0.31, $WARM)" >/dev/null
step "  cold A/B"
eval "$STOP_START"
q -c "SELECT bench_ab_run('gaia50', 'cold', 1, 0.31, $COLD)" >/dev/null
step "  EXPLAIN pass"
q -c "SELECT bench_ab_explain('gaia50', 0.41, $WARM)" >/dev/null
for t in bench_ab bench_ab_x; do q -c "\\copy (SELECT * FROM $t) TO STDOUT WITH CSV HEADER" > "$OUT/$t.csv"; done
python3 ab_report.py "$OUT/bench_ab.csv" "$OUT/bench_ab_x.csv" > "$OUT/ab_report.md"
q -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
step DONE
