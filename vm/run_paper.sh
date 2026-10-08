#!/bin/bash
# Run the paper's measurements on a VM provisioned by vm/provision.sh, stage by
# stage, and export every result table as CSV.  See vm/README.md for which stage
# produces which table or figure, how long each takes, and what to check.
#
#   vm/run_paper.sh                       # all stages, in order
#   vm/run_paper.sh designed gaia         # just these stages
#   SMOKE=1 vm/run_paper.sh designed      # tiny sizes: checks the pipeline in minutes
#
# Stages (each rebuilds the corpus it needs, so run them in this order or one
# at a time):
#   designed  synthetic 10M corpus: tab:cones (designed), Fig. 1, cost model,
#             joins, polygons, footprints, region opclasses (tab:gistregion)
#   gaia      10M corpus resampled from the DR3 density map: tab:cones (Gaia),
#             tab:scale (10M), cost model, joins, tab:crossover
#   gaia50    50M resampled corpus: tab:scale (50M), tab:gaia, cross-match
#   real      real DR3 positions: tab:estimator, tab:xmreal, tab:cones (real,
#             warm and cold), ablation and sensitivity
#   report    export CSVs and run the report scripts (also run after each stage)
#
# Environment:
#   PGDATABASE (default skycell), and the usual PG* connection variables
#   OUT          results directory (default results-vm/<date>)
#   RESTART      command restarting PostgreSQL and waiting for it
#                (default: sudo systemctl restart postgresql)
#   DROP_CACHES  command dropping the OS page cache
#                (default: sync; echo 3 | sudo tee /proc/sys/vm/drop_caches)
#   GAIA_DIR     where Gaia CSVs are downloaded (default /var/tmp/gaia; ~700 MB)
#   SMOKE=1      shrink every size, for a quick end-to-end check
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO/bench"
export PGDATABASE=${PGDATABASE:-skycell}
export GAIA_DIR=${GAIA_DIR:-/var/tmp/gaia}
OUT=${OUT:-$REPO/results-vm/$(date +%Y%m%d)}
RESTART=${RESTART:-"sudo systemctl restart postgresql"}
DROP_CACHES=${DROP_CACHES:-"sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null"}
STAGES=${*:-designed gaia gaia50 real report}
mkdir -p "$OUT"
LOG="$OUT/run.log"

if [ "${SMOKE:-0}" = 1 ]; then
  N10=200000; N50=400000; NPROBE=4000; AB_REPS=2; NFP=2000; NPROBE_RG=50
  SCALE=0.05; XMS_SIZES='{100,1000}'; LIM=3; NQ_CROSS=5; CROSS_ROWS='{0.05,0.2}'; EST_REPS=2
else
  N10=10000000; N50=50000000; NPROBE=200000; AB_REPS=5; NFP=50000; NPROBE_RG=500
  SCALE=1; XMS_SIZES='{1000,10000,100000}'; LIM=15; NQ_CROSS=30; CROSS_ROWS='{0.5,2,10}'; EST_REPS=10
fi

q()    { psql -X -q -v ON_ERROR_STOP=1 "$@"; }
step() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
wait_up() { for _ in $(seq 1 120); do pg_isready -q && return 0; sleep 1; done; echo "server did not come back" >&2; exit 1; }
cold_restart() { eval "$RESTART"; eval "$DROP_CACHES"; wait_up; }
export_tables() {  # export_tables t1 t2 ...  (only those that exist)
  for t in "$@"; do
    if [ "$(psql -X -At -c "SELECT to_regclass('$t') IS NOT NULL")" = t ]; then
      q -c "\\copy (SELECT * FROM $t) TO STDOUT WITH CSV HEADER" > "$OUT/$t.csv"
    fi
  done
}

# the controlled cone protocol of 07_ab.sql on the corpus currently in cat_*:
# warm (one untimed pass, then AB_REPS repetitions), cold (server restarted and
# the page cache dropped, one repetition), and the EXPLAIN pass
cone_ab() {
  local corpus=$1
  step "  $corpus: query centres (03_cone.sql)"
  q -f 03_cone.sql >/dev/null
  q -f 07_ab.sql >/dev/null
  step "  $corpus: warm A/B, $AB_REPS repetitions"
  q -c "SELECT bench_ab_run('$corpus', 'warm', $AB_REPS)" >/dev/null
  step "  $corpus: cold A/B (restart + drop caches, one repetition)"
  cold_restart
  q -c "SELECT bench_ab_run('$corpus', 'cold', 1)" >/dev/null
  step "  $corpus: EXPLAIN pass"
  q -c "SELECT bench_ab_explain('$corpus')" >/dev/null
}

cost_model() {  # 08_costmodel.sql on the current corpus
  local corpus=$1
  step "  $corpus: cost model sweep, density estimate, order curve (08)"
  q -f 08_costmodel.sql >/dev/null
  q -c "SELECT cm_run_sweep('$corpus', '{0.3,1,3,10,30,100,300,1000}', ARRAY['1\"', '1'||chr(39), '30'||chr(39), '1deg'])" >/dev/null
  q -c "SELECT cm_run_rho('$corpus', ARRAY['1\"', '1'||chr(39), '6'||chr(39), '30'||chr(39), '1deg'])" >/dev/null
  q -c "SELECT cm_run_curve('$corpus', ARRAY['1\"', '1'||chr(39), '30'||chr(39), '1deg'])" >/dev/null
}

joins() {
  local corpus=$1
  step "  $corpus: estimates, plans and joins (09)"
  q -f 09_joins.sql >/dev/null
  q -c "SELECT bench_join_run('$corpus', ARRAY['filter','join','join3'], ARRAY['1'||chr(39), '30'||chr(39), '1deg'], $LIM)" >/dev/null
}

for stage in $STAGES; do
  case $stage in

  designed)
    step "STAGE designed: synthetic corpus, $N10 rows"
    q -v n="$N10" -f 01_data.sql >/dev/null
    q -f 02_build.sql | tee -a "$LOG"
    q -c "\\copy (SELECT * FROM bench_build) TO STDOUT WITH CSV HEADER" > "$OUT/bench_build_designed.csv"
    cone_ab designed
    cost_model designed
    joins designed
    step "  polygons (05)"
    q -f 05_poly.sql >/dev/null
    for m in q3c pgsphere skycell; do q -c "SELECT bench_poly_run('$m', 1)" -c "SELECT bench_poly_run('$m', 2)" >/dev/null; done
    # 04 builds the probe table 06 joins against, as in run.sh
    step "  cross-match, $NPROBE probes (04) and stored footprints (06)"
    q -v nprobe="$NPROBE" -f 04_xmatch.sql >/dev/null
    for r in 1 10; do for m in q3c pgsphere skycell_slots skycell_lateral; do
      q -c "SELECT bench_xmatch_run('$m', $r, 1)" -c "SELECT bench_xmatch_run('$m', $r, 2)" >/dev/null; done; done
    q -f 06_footprints.sql >/dev/null
    for m in pgsphere skycell; do q -c "SELECT bench_fp_run('$m', 1)" -c "SELECT bench_fp_run('$m', 2)" >/dev/null; done
    step "  region opclasses on fpr, $NFP footprints (20-24): tab:gistregion"
    q -v nfp="$NFP" -f 20_region_xmatch.sql >/dev/null
    for m in skycell pgsphere q3c; do for s in circle poly mixed; do
      q -c "SELECT bench_region_xmatch_run('$m', '$s', 1)" -c "SELECT bench_region_xmatch_run('$m', '$s', 2)" >/dev/null; done; done
    for f in 21_region_overlap 22_region_gist 23_region_contains 24_region_contains_region; do
      step "    $f"; q -v nprobe="$NPROBE_RG" -f "$f.sql" > "$OUT/$f.out"
    done
    export_tables bench_ab bench_ab_x cm_sweep cm_rho cm_curve bench_join bench_poly bench_xmatch bench_fp \
      bench_region_xmatch bench_region_overlap bench_region_gist bench_region_contains bench_region_contains_region
    ;;

  gaia)
    step "STAGE gaia: $N10 rows resampled from the DR3 order-9 map"
    q -c "CREATE TABLE IF NOT EXISTS gaia_map (hpx9 bigint PRIMARY KEY, n bigint)"
    if [ "$(psql -X -At -c 'SELECT count(*) FROM gaia_map')" = 0 ]; then
      q -c "\\copy gaia_map FROM PROGRAM 'zcat $REPO/bench/data/gaia_map_hpx9.csv.gz' CSV HEADER"
    fi
    q -v n="$N10" -f 11_scale_corpus.sql | tee -a "$LOG"
    q -f 02_build.sql | tee -a "$LOG"
    q -c "\\copy (SELECT * FROM bench_build) TO STDOUT WITH CSV HEADER" > "$OUT/bench_build_gaia.csv"
    cone_ab gaia
    cost_model gaia
    joins gaia
    step "  ObsCore-shaped crossover (10): tab:crossover"
    q -f 10_crossover.sql >/dev/null
    for r in $(echo "$CROSS_ROWS" | tr -d '{}' | tr , ' '); do
      step "    $r M rows"; q -c "SELECT bench_cross_run($r, $NQ_CROSS)" >/dev/null
    done
    export_tables bench_ab bench_ab_x cm_sweep cm_rho cm_curve bench_join bench_cross
    ;;

  gaia50)
    step "STAGE gaia50: $N50 rows resampled from the DR3 map"
    q -v n="$N50" -f 11_scale_corpus.sql | tee -a "$LOG"
    q -f 12_scale_build.sql | tee -a "$LOG"
    cone_ab gaia50
    step "  cross-match, $NPROBE probes (04, 11_xmatch_ab)"
    q -v nprobe="$NPROBE" -f 04_xmatch.sql >/dev/null
    q -f 11_xmatch_ab.sql >/dev/null
    q -c "SELECT bench_xm_ab_run($N50 / 1e6, ARRAY[1/3600.0, 10/3600.0])" >/dev/null
    export_tables bench_ab bench_ab_x bench_build50 bench_xm_ab
    ;;

  real)
    step "STAGE real: real Gaia DR3 positions"
    bash 19_gaia_real.sh | tail -3 | tee -a "$LOG"
    bash 19_gaia_load.sh | tail -25 | tee -a "$LOG"
    bash 19_gaia_load_split.sh | tail -12 | tee -a "$LOG"
    step "  density estimator (29): tab:estimator"
    q -v reps="$EST_REPS" -v decision=1 -f 29_estimator.sql > "$OUT/29_estimator.out"
    step "  sensitivity (16) and ablation (17)"
    q -f 16_sensitivity.sql >/dev/null
    q -c "SELECT sens_cost_run('real', 'gaia_real_cell', 'cell')" \
      -c "SELECT sens_dens_run('real', 'gaia_real_cell', 'gaia_real_cell_idx', 'cell', 'cell')" >/dev/null
    q -f 17_ablation.sql >/dev/null
    q -c "SELECT abl_layout()" -c "SELECT abl_order()" >/dev/null
    step "  cross-match sweep (18): tab:xmreal"
    q -f 18_xmatch_sweep.sql >/dev/null
    q -c "SELECT xms_run('$XMS_SIZES', '{0.5,1,1.5,5}', '{clustered,uniform}')" >/dev/null
    step "  real-corpus cones, warm and cold (30/31): tab:cones real columns"
    SCALE=$SCALE OUT="$OUT/realcone" RESTART="$RESTART" DROP_CACHES="$DROP_CACHES" \
      bash 31_real_cones.sh | tee -a "$LOG"
    export_tables est_truth est_runs est_decision sens_cost sens_dens abl xms
    ;;

  report)
    step "REPORT"
    export_tables bench_ab bench_ab_x
    if [ -s "$OUT/bench_ab.csv" ]; then
      python3 ab_report.py "$OUT/bench_ab.csv" "$OUT/bench_ab_x.csv" > "$OUT/ab_report.md"
      step "  $OUT/ab_report.md"
    fi
    [ -d "$OUT/realcone" ] && python3 realcone_report.py "$OUT/realcone" > "$OUT/realcone_report.md"
    q -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
    ;;

  *) echo "unknown stage: $stage" >&2; exit 1 ;;
  esac
done
step "DONE -> $OUT"
