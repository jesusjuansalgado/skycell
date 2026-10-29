#!/bin/bash
# tab:cones' synthetic columns, Fig. 1, tab:scale and tab:gaia with skycell
# 0.37: results-pg18/run.sh's cone protocol (and results-pg18/scale50/run.sh's
# for 50M) unchanged. Warm: one untimed pass, then 5 repetitions of q3c,
# pgsphere, skycell (the custom scan) and skycell-rw (the range rewrite).
# Cold: the server stopped and the page cache dropped, then one pass of q3c,
# pgsphere and skycell (skycell-rw shares cat_cell). Then the EXPLAIN pass.
#
#   PGHOST=/tmp PGPORT=5519 PGUSER=postgres PGDATABASE=cone36 \
#   STOP_START='...stop, drop the page cache, start...' ./run.sh designed gaia
#   ... ./run.sh gaia50        # on its own: it needs ~20 GB
#
# Stages:
#   designed  01_data.sql (10M) and 02_build.sql, then the cone protocol
#   gaia      10M rows resampled from the DR3 order-9 map (11, 02), then the
#             cone protocol
#   gaia50    5e7 rows (11, 12_scale_build.sql), then the cone protocol
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)
cd "$OUT/../.."
LOG=$OUT/run.log
q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }
step() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG"; }
WARM="ARRAY['q3c','pgsphere','skycell','skycell-rw']"
COLD="ARRAY['q3c','pgsphere','skycell']"
export_tables() { local dir=$1; shift; for t in "$@"; do q -c "\\copy (SELECT * FROM $t) TO STDOUT WITH CSV HEADER" > "$dir/$t.csv"; done; }
gaia_map() {
  q -c "CREATE TABLE IF NOT EXISTS gaia_map (hpx9 bigint PRIMARY KEY, n bigint)"
  if [ "$(q -At -c 'SELECT count(*) FROM gaia_map')" = 0 ]; then
    zcat data/gaia_map_hpx9.csv.gz | q -c "\\copy gaia_map FROM STDIN CSV HEADER"
  fi
}
cone_ab() {
  local corpus=$1
  step "  $corpus: query centres (03) + 07"
  q < 03_cone.sql > /dev/null; q < 07_ab.sql > /dev/null
  step "  $corpus: warm A/B, 5 repetitions"
  q -c "SELECT bench_ab_run('$corpus', 'warm', 5, 0.31, $WARM)" > /dev/null
  step "  $corpus: cold A/B"
  eval "$STOP_START"
  q -c "SELECT bench_ab_run('$corpus', 'cold', 1, 0.31, $COLD)" > /dev/null
  step "  $corpus: EXPLAIN pass"
  q -c "SELECT bench_ab_explain('$corpus', 0.41, $WARM)" > /dev/null
}
for stage in "$@"; do case $stage in
designed)
  step "STAGE designed: 01_data.sql (10M), 02_build.sql"
  q -c "DROP TABLE IF EXISTS bench_ab, bench_ab_x"
  q < 01_data.sql > /dev/null
  q < 02_build.sql > /dev/null
  q -c "\\copy (SELECT * FROM bench_build) TO STDOUT WITH CSV HEADER" > "$OUT/bench_build_designed.csv"
  cone_ab designed ;;
gaia)
  step "STAGE gaia: 1e7 rows resampled from the DR3 order-9 map"
  gaia_map
  q -v n=10000000 < 11_scale_corpus.sql > /dev/null
  q < 02_build.sql > /dev/null
  q -c "\\copy (SELECT * FROM bench_build) TO STDOUT WITH CSV HEADER" > "$OUT/bench_build_gaia.csv"
  cone_ab gaia
  export_tables "$OUT" bench_ab bench_ab_x
  python3 ab_report.py "$OUT/bench_ab.csv" "$OUT/bench_ab_x.csv" > "$OUT/ab_report.md" ;;
gaia50)
  mkdir -p "$OUT/scale50"
  step "STAGE gaia50: 5e7 rows resampled from the DR3 order-9 map"
  q -c "DROP TABLE IF EXISTS bench_ab, bench_ab_x"
  gaia_map
  q -v n=5e7 < 11_scale_corpus.sql > /dev/null
  q < 12_scale_build.sql > /dev/null
  q -c "\\copy (SELECT * FROM bench_build50) TO STDOUT WITH CSV HEADER" > "$OUT/scale50/bench_build50.csv"
  cone_ab gaia50
  export_tables "$OUT/scale50" bench_ab bench_ab_x
  python3 ab_report.py "$OUT/scale50/bench_ab.csv" "$OUT/scale50/bench_ab_x.csv" > "$OUT/scale50/ab_report.md" ;;
*) echo "unknown stage $stage" >&2; exit 2 ;;
esac; done
q -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
step DONE
