#!/bin/bash
# Polygons (05_poly.sql) and stored footprints as MOC cells (06_footprints.sql)
# with skycell 0.36, on the cloud machine. The protocol is ../../run.sh's,
# which the paper's laptop numbers came from: each method in sequence, an
# untimed-in-the-paper first pass and a reported second pass. Here the
# polygon sequence then runs a second time in the reverse order (passes 3
# and 4), to show whether the order matters.
#
#   PGHOST=/tmp PGPORT=5519 PGUSER=postgres PGDATABASE=cone36 ./run.sh designed gaia
#
# Stages:
#   designed  01_data.sql (10M) and 02_build.sql
#   gaia      10M rows resampled from the DR3 order-9 map (11, 02): the paper's
#             0.28 / 0.77 / 1.86 ms and its 200,474 footprint pairs
# then, on each: 03_cone.sql (the query pool), 05_poly.sql, 04_xmatch.sql
# (the 200,000-row probe table), 06_footprints.sql.
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)
cd "$OUT/../.."
LOG=$OUT/run.log
q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }
step() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG"; }
export_tables() { local sfx=$1; shift; for t in "$@"; do q -c "\\copy (SELECT * FROM $t) TO STDOUT WITH CSV HEADER" > "$OUT/${t}_$sfx.csv"; done; }
for stage in "$@"; do
  case $stage in
  designed)
    step "STAGE designed: 01_data.sql (10M), 02_build.sql"
    q < 01_data.sql > /dev/null
    q < 02_build.sql > /dev/null ;;
  gaia)
    step "STAGE gaia: 1e7 rows resampled from the DR3 order-9 map"
    q -c "CREATE TABLE IF NOT EXISTS gaia_map (hpx9 bigint PRIMARY KEY, n bigint)"
    if [ "$(q -At -c 'SELECT count(*) FROM gaia_map')" = 0 ]; then
      zcat data/gaia_map_hpx9.csv.gz | q -c "\\copy gaia_map FROM STDIN CSV HEADER"
    fi
    q -v n=10000000 < 11_scale_corpus.sql > /dev/null
    q < 02_build.sql > /dev/null ;;
  esac
  step "  $stage: query pool (03), polygons (05)"
  q < 03_cone.sql > /dev/null
  q -c "DROP TABLE IF EXISTS bench_poly"
  q < 05_poly.sql > /dev/null
  for m in q3c pgsphere skycell; do
    step "  poly $m"; q -c "SELECT bench_poly_run('$m', 1)" -c "SELECT bench_poly_run('$m', 2)" > /dev/null
  done
  for m in skycell pgsphere q3c; do
    step "  poly $m (reverse order)"; q -c "SELECT bench_poly_run('$m', 3)" -c "SELECT bench_poly_run('$m', 4)" > /dev/null
  done
  step "  $stage: probes (04), footprints (06)"
  q -v nprobe=200000 < 04_xmatch.sql > /dev/null
  q < 06_footprints.sql > /dev/null
  for m in pgsphere skycell; do
    step "  footprints $m"; q -c "SELECT bench_fp_run('$m', 1)" -c "SELECT bench_fp_run('$m', 2)" > /dev/null
  done
  for m in skycell pgsphere; do
    step "  footprints $m (reverse order)"; q -c "SELECT bench_fp_run('$m', 3)" -c "SELECT bench_fp_run('$m', 4)" > /dev/null
  done
  export_tables "$stage" bench_poly bench_polys bench_fp
done
step DONE
