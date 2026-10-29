#!/bin/bash
# Size against result size on the ObsCore crossover: Table 8's protocol
# (10_crossover.sql's bench_cross_run: 30 stored centres, 3 repetitions,
# pgsphere / skycell / skycell-rw, seeds 0.19 and 0.53) and threeway.sql
# (pgSphere as planned, pgSphere forced to a bitmap heap scan, skycell), on
# four tables built in turn:
#   dense05   build_obscore(500000), Table 8's 0.5M relation
#   sparse10  build_obscore_sparse() (sparse.sql): the same 0.5M rows plus 9.5M
#             filler rows no query cone reaches -- 10M rows, 0.5M's results
#   dense10   build_obscore(10000000), Table 8's 10M relation
#   sparse10b sparse10 rebuilt (a fresh ANALYZE sample; after dense10, against drift)
# Library: whatever is installed, or LIB=<path to a skycell.so>, swapped in
# (and the server restarted) first.  Results go to this directory, or to
# OUT=<directory>.  The run in this directory used skycell#46 at 2cc6c6e; the
# one in v036/ used tag v0.36 (both extversion 0.36).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mkdir -p "${OUT:-$HERE}" && cd "${OUT:-$HERE}" && pwd)
. "$HERE/server.sh"
if [ -n "${LIB:-}" ]; then
  L=/home/user/pg18/lib/postgresql/skycell.so
  cp "$LIB" "$L.new" && mv -f "$L.new" "$L"
  eval "$RESTART"
fi
export PGDATABASE=cross36
cd "$HERE/../.."
P="psql -X -q -v ON_ERROR_STOP=1"
step() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$OUT/run.log"; }
$P -f 10_crossover.sql -f "$HERE/sparse.sql" > /dev/null 2>&1
for t in dense05 sparse10 dense10 sparse10b; do
  step "$t: build"
  case $t in
    dense05)  $P -c "CALL build_obscore(500000)" ;;
    dense10)  $P -c "CALL build_obscore(10000000)" ;;
    sparse*)  $P -c "CALL build_obscore_sparse()" ;;
  esac
  $P -At -c "SELECT count(*), pg_relation_size('oc'), pg_relation_size('oc_gist'), pg_relation_size('oc_cell') FROM oc" > "$OUT/${t}_size.txt"
  label=$([ $t = dense05 ] && echo 0.5 || echo 10)
  for seed in 0.19 0.53; do
    step "$t: Table 8 protocol, seed $seed"
    $P -c "TRUNCATE bench_cross" -c "SELECT bench_cross_run($label, 30, $seed, 3, ARRAY['pgsphere','skycell','skycell-rw'])" > /dev/null
    $P -c "\\copy (SELECT * FROM bench_cross) TO STDOUT WITH CSV HEADER" > "$OUT/${t}_cross_seed0${seed#0.}.csv"
  done
  step "$t: three-way"
  $P -f "$HERE/threeway.sql" > "$OUT/${t}_threeway.txt"
done
$P -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
step DONE
