#!/bin/bash
# tab:crossover with skycell 0.36: results-pg18/crossover-stored's protocol
# unchanged (10_crossover.sql, stored field and query centres, 0.5M / 2M / 10M
# rows, 30 centres, 3 repetitions, pgsphere / skycell / skycell-rw), two
# complete runs that differ only in the seed ordering the trials (0.19, 0.53).
#
#   PGHOST=/tmp PGPORT=5519 PGUSER=postgres PGDATABASE=paper36 ./run.sh
#
# The database needs skycell and pg_sphere created, and the server the
# settings of REPRODUCING.md section 2.
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)
cd "$OUT/../.."
P="psql -X -q -v ON_ERROR_STOP=1"
step() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$OUT/run.log"; }

step "10_crossover.sql"
$P < 10_crossover.sql > /dev/null
for seed in 0.19 0.53; do
  for r in 0.5 2 10; do
    step "seed $seed: $r M rows"
    $P -c "CALL build_obscore(($r * 1e6)::bigint)" \
       -c "SELECT bench_cross_run($r, 30, $seed, 3, ARRAY['pgsphere','skycell','skycell-rw'])" > /dev/null
  done
  $P -c "\\copy (SELECT * FROM bench_cross) TO STDOUT WITH CSV HEADER" \
     > "$OUT/bench_cross_seed0${seed#0.}.csv"
done
$P -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > "$OUT/versions.txt"
step DONE
