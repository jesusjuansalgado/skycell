#!/bin/bash
# skycell 0.26 vs 0.36 on the same host, for polygons (05_poly.sql) and the
# footprint join (06_footprints.sql): the gaia corpus (11_scale_corpus.sql,
# 1e7) built in a 0.26 database (pf26) and again in the 0.36 one (cone36),
# then four runs alternating 26, 36, 36, 26, each with passes 1 and 2 of
# pgsphere and skycell for both scripts.
set -euo pipefail
S=${S:?scratch directory holding ab026.so / ab036.so}
OUT=$(cd "$(dirname "$0")" && pwd)
L=/opt/pg18/lib/postgresql/skycell.so
cd /home/user/skycell-paper/bench
q() { /opt/pg18/bin/psql -h /tmp -p 5519 -U postgres -X -q -v ON_ERROR_STOP=1 "$@"; }
swap() { cp $S/ab0$1.so $L.new && mv -f $L.new $L; }
db() { [ $1 = 26 ] && echo pf26 || echo cone36; }
build() {
  local d=$1
  q -d $d -c "CREATE TABLE IF NOT EXISTS gaia_map (hpx9 bigint PRIMARY KEY, n bigint)"
  if [ "$(q -d $d -At -c 'SELECT count(*) FROM gaia_map')" = 0 ]; then
    zcat data/gaia_map_hpx9.csv.gz | q -d $d -c "\\copy gaia_map FROM STDIN CSV HEADER"
  fi
  q -d $d -v n=10000000 < 11_scale_corpus.sql > /dev/null
  q -d $d < 02_build.sql > /dev/null
  q -d $d < 03_cone.sql > /dev/null
  q -d $d -c "DROP TABLE IF EXISTS bench_poly" > /dev/null
  q -d $d < 05_poly.sql > /dev/null
  q -d $d -v nprobe=200000 < 04_xmatch.sql > /dev/null
  q -d $d < 06_footprints.sql > /dev/null
  q -d $d -At -c "SELECT 'src ' || count(*) || ' ' || sum(hashtext(ra::text || dec::text)::bigint) FROM src" \
              -c "SELECT 'probe ' || count(*) || ' ' || sum(hashtext(ra::text || dec::text)::bigint) FROM probe" \
              -c "SELECT 'fp_cells ' || count(*) || ' ' || sum(nuniq % 1000003) FROM fp_cells"
}
swap 26
q -d postgres -c "DROP DATABASE IF EXISTS pf26" -c "CREATE DATABASE pf26"
q -d pf26 -c "CREATE EXTENSION skycell VERSION '0.26'" -c "CREATE EXTENSION pg_sphere" -c "CREATE EXTENSION q3c"
echo "pf26:";   build pf26   | tee $OUT/check_026.txt
swap 36
echo "cone36:"; build cone36 | tee $OUT/check_036.txt
i=0
for v in 26 36 36 26; do
  i=$((i+1)); swap $v; d=$(db $v)
  q -d $d -c "TRUNCATE bench_poly" -c "DELETE FROM bench_fp WHERE pass > 0" > /dev/null
  for m in pgsphere skycell; do
    q -d $d -c "SELECT bench_poly_run('$m', 1)" -c "SELECT bench_poly_run('$m', 2)" > /dev/null
    q -d $d -c "SELECT bench_fp_run('$m', 1)" -c "SELECT bench_fp_run('$m', 2)" > /dev/null
  done
  q -d $d -c "\\copy (SELECT * FROM bench_poly) TO STDOUT WITH CSV HEADER" > $OUT/poly_run${i}_0$v.csv
  q -d $d -c "\\copy (SELECT method, pass, n, ms, build_ms, index_mb FROM bench_fp) TO STDOUT WITH CSV HEADER" > $OUT/fp_run${i}_0$v.csv
  echo "run $i ($v) done $(date -u +%T)"
done
swap 36
