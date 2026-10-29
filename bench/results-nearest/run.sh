#!/bin/bash
# Nearest neighbours against pgSphere's GiST ordering (README, Performance).
#
#   PGHOST=/tmp PGPORT=5519 PGUSER=postgres ./run.sh [passes]
#
# Two tables, each with an expression index on skycell_ang2cell(ra, dec) and
# pgSphere's GiST on pos, ANALYZEd:
#   - real36.gaia_realc: the 10M real Gaia DR3 positions (../19_gaia_real.sh,
#     ../results-v036/realcone/);
#   - oc05.oc: the 0.5M-row ObsCore-shaped table (../10_crossover.sql,
#     CALL build_obscore(500000)), observations clustered in fields, its heap
#     out of cell order.
# Centres: 30 per table, mk_centres -- oc_queries' first 30 for oc, a
# REPEATABLE sample of gaia_realc's own positions for gaia_realc (setup below).
# Per centre, k and method, micro_knn.sql times 20 executions, planned and
# run, each at a new centre 1e-9 deg away, so no plan or covering is reused;
# k = -10 is the 10 nearest more than 2 deg away, with the same condition for
# both methods.  report_knn.py: per table and k, the ratio of skycell's to
# pgSphere's summed per-centre medians, with a 95% bootstrap interval over
# centres.  Server: ../REPRODUCING.md section 2.
set -euo pipefail
cd "$(dirname "$0")"
passes=${1:-2}
out=nearest.csv
psql -d oc05 -X -q -c "DROP TABLE IF EXISTS mk_centres;
  CREATE TABLE mk_centres AS SELECT qid, ra, dec FROM oc_queries WHERE qid <= 30"
psql -d real36 -X -q -c "DROP TABLE IF EXISTS mk_centres; SELECT setseed(0.42);
  CREATE TABLE mk_centres AS SELECT row_number() OVER ()::int qid, ra + 0.0001 ra, dec
  FROM (SELECT ra, dec FROM gaia_realc TABLESAMPLE SYSTEM (0.01) REPEATABLE (7) LIMIT 30) s"
echo run,k,qid,method,us,n > $out
for p in $(seq 1 "$passes"); do
  for db in oc05 real36; do
    t=$([ $db = oc05 ] && echo oc || echo gaia_realc)
    psql -d $db -X -q -c "SELECT count(*) FROM $t" >/dev/null   # warm the heap
    psql -d $db -X -q -f micro_knn.sql -c "SELECT setseed(0.5)" \
      -c "SELECT mk_run('$t', '$t', 20, 3, ARRAY[1, 10, 100, 1000, -10])" \
      -c "\copy mk to stdout csv" | grep -E "^(oc|gaia_realc)," >> $out
  done
done
python3 report_knn.py $out > report.md
psql -d real36 -X -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > versions.txt
md5sum "$(pg_config --pkglibdir)/skycell.so" >> versions.txt
