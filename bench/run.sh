#!/usr/bin/env bash
# Full benchmark: Q3C vs pgSphere vs skycell, inside the skycell-pg container.
#   N=10000000 NPROBE=200000 ./run.sh
set -euo pipefail
cd "$(dirname "$0")"
N=${N:-10000000}
NPROBE=${NPROBE:-200000}
C=${CONTAINER:-skycell-pg}
OUT=results
mkdir -p "$OUT"
psql() { docker exec -i "$C" psql -U postgres -d skycell -v ON_ERROR_STOP=1 -X -q "$@"; }
step() { echo "[$(date +%H:%M:%S)] $*"; }

if [ "${SKIP_BUILD:-0}" != 1 ]; then
  step "data: $N rows";   psql -v n="$N" < 01_data.sql
  step "build tables and indexes"; psql < 02_build.sql
fi

step "cone searches"; psql < 03_cone.sql
for m in q3c pgsphere skycell; do
  step "  cone $m"
  psql -c "SELECT bench_cone_run('$m', '', 1)" -c "SELECT bench_cone_run('$m', '', 2)" -c "SELECT bench_cone_explain('$m', '')"
done
for v in "use_stats=off" "max_area_ratio=0" "range_cost=3" "range_cost=30"; do
  step "  cone skycell [$v]"
  psql -c "SELECT bench_cone_run('skycell', '$v', 1)" -c "SELECT bench_cone_run('skycell', '$v', 2)" -c "SELECT bench_cone_explain('skycell', '$v')"
done

step "polygons"; psql < 05_poly.sql
for m in q3c pgsphere skycell; do
  step "  poly $m"; psql -c "SELECT bench_poly_run('$m', 1)" -c "SELECT bench_poly_run('$m', 2)"
done

step "cross-match: $NPROBE probes"; psql -v nprobe="$NPROBE" < 04_xmatch.sql
psql -c "TRUNCATE bench_xmatch"
for r in 1 10; do
  for m in q3c pgsphere skycell_slots skycell_lateral; do
    step "  xmatch $m r=${r}\""; psql -c "SELECT bench_xmatch_run('$m', $r, 1)" -c "SELECT bench_xmatch_run('$m', $r, 2)"
  done
done
step "  covering indexes (index-only cross-match)"
psql -c "CREATE INDEX cat_cell_cov ON cat_cell (cell) INCLUDE (ra, dec, id)" \
     -c "INSERT INTO bench_build SELECT 'skycell', 'covering index', NULL, pg_relation_size('cat_cell_cov') / 1048576.0"
# (Q3C with an INCLUDE covering index was tried: the planner abandons the
#  index for q3c_join and runs a seq-scan nested loop, so it is omitted.)
for r in 1 10; do
  for m in skycell_lateral_ios; do
    step "  xmatch $m (covering) r=${r}\""
    psql -c "SELECT bench_xmatch_run('$m', $r, 1)" -c "SELECT bench_xmatch_run('$m', $r, 2)"
  done
done
psql -c "DROP INDEX cat_cell_cov"

step "stored footprints"; psql < 06_footprints.sql
for m in pgsphere skycell; do
  step "  footprints $m"; psql -c "SELECT bench_fp_run('$m', 1)" -c "SELECT bench_fp_run('$m', 2)"
done

step "export"
for t in bench_build bench_centers bench_cone bench_cone_x bench_poly bench_polys bench_xmatch bench_fp; do
  psql -c "\\copy (SELECT * FROM $t) TO STDOUT WITH CSV HEADER" > "$OUT/$t.csv"
done
psql -c "\\copy (SELECT kind, count(*) FROM src GROUP BY kind) TO STDOUT WITH CSV HEADER" > "$OUT/src_kinds.csv"
step "done -> $OUT/"
