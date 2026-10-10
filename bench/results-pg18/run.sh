#!/bin/bash
# vm/run_paper.sh's designed and gaia cone protocol plus the crossover, on PG 18
set -euo pipefail
. /tmp/claude-0/-home-user-skycell/fc2671ec-28bc-548e-a785-8c0c7d38ab52/scratchpad/pg18_env.sh
REPO=/home/user/skycell; cd $REPO/bench
OUT=$REPO/bench/results-pg18; mkdir -p $OUT; LOG=$OUT/run.log
DB=paper18
q() { q18 -d $DB "$@"; }
step() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
cold_restart() { sudo -u postgres $PG18BIN/pg_ctl -D $PG18DATA -m fast -w stop >/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches; pg18_up; }
WARM="ARRAY['q3c','pgsphere','skycell','skycell-rw']"
COLD="ARRAY['q3c','pgsphere','skycell']"
export_tables() { for t in "$@"; do q -c "\\copy (SELECT * FROM $t) TO STDOUT WITH CSV HEADER" > "$OUT/$t.csv"; done; }
cone_ab() {
  local corpus=$1
  step "  $corpus: query centres (03) + 07"
  q < 03_cone.sql >/dev/null; q < 07_ab.sql >/dev/null
  step "  $corpus: warm A/B, 5 repetitions"
  q -c "SELECT bench_ab_run('$corpus', 'warm', 5, 0.31, $WARM)" >/dev/null
  step "  $corpus: cold A/B"
  cold_restart
  q -c "SELECT bench_ab_run('$corpus', 'cold', 1, 0.31, $COLD)" >/dev/null
  step "  $corpus: EXPLAIN pass"
  q -c "SELECT bench_ab_explain('$corpus', 0.41, $WARM)" >/dev/null
}
for stage in ${*:-designed gaia}; do case $stage in
designed)
  step "STAGE designed (corpus from 01/02 already in $DB)"
  q -c "DELETE FROM bench_ab" -c "DELETE FROM bench_ab_x" 2>/dev/null || true
  q -c "ANALYZE cat_q3c" -c "ANALYZE cat_sphere" -c "ANALYZE cat_cell"
  q -c "\\copy (SELECT * FROM bench_build) TO STDOUT WITH CSV HEADER" > "$OUT/bench_build_designed.csv"
  cone_ab designed
  export_tables bench_ab bench_ab_x
gaia)
  step "STAGE gaia: 1e7 rows resampled from the DR3 order-9 map"
  q -c "CREATE TABLE IF NOT EXISTS gaia_map (hpx9 bigint PRIMARY KEY, n bigint)"
  if [ "$(q -At -c 'SELECT count(*) FROM gaia_map')" = 0 ]; then
    zcat $REPO/bench/data/gaia_map_hpx9.csv.gz | q -c "\\copy gaia_map FROM STDIN CSV HEADER"
  fi
  q -v n=10000000 < 11_scale_corpus.sql | tee -a "$LOG"
  q < 02_build.sql | tee -a "$LOG"
  q -c "\\copy (SELECT * FROM bench_build) TO STDOUT WITH CSV HEADER" > "$OUT/bench_build_gaia.csv"
  cone_ab gaia
  step "  ObsCore crossover (10)"
  q < 10_crossover.sql >/dev/null
  for r in 0.5 2 10; do
    step "    $r M rows"
    q -c "CALL build_obscore(($r * 1e6)::bigint)" -c "SELECT bench_cross_run($r, 30, 0.19, 3, ARRAY['pgsphere','skycell','skycell-rw'])" >/dev/null
  done
  export_tables bench_ab bench_ab_x bench_cross ;;
esac; done
q -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
step DONE
