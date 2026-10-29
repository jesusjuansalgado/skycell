#!/bin/bash
# skycell 0.26 vs 0.36 on the same host, resampled-catalogue cross-match: the
# 10M Gaia-density corpus and 04's probes built under 0.26 too, then
# bench_xm_ab_run(10, {1,10}, 2) alternating 26, 36, 36, 26
set -euo pipefail
S=${S:?scratch directory holding ab026.so / ab036.so and ab/}
L=/opt/pg18/lib/postgresql/skycell.so
cd /home/user/skycell-paper/bench
q() { /opt/pg18/bin/psql -h /tmp -p 5519 -U postgres -X -q -v ON_ERROR_STOP=1 "$@"; }
swap() { cp $S/ab0$1.so $L.new && mv -f $L.new $L; }
swap 26
q -d postgres -c "DROP DATABASE IF EXISTS cone26" -c "CREATE DATABASE cone26"
q -d cone26 -c "CREATE EXTENSION skycell VERSION '0.26'" -c "CREATE EXTENSION pg_sphere" -c "CREATE EXTENSION q3c"
q -d cone26 -c "CREATE TABLE gaia_map (hpx9 bigint PRIMARY KEY, n bigint)"
zcat data/gaia_map_hpx9.csv.gz | q -d cone26 -c "\\copy gaia_map FROM STDIN CSV HEADER"
q -d cone26 -v n=10000000 < 11_scale_corpus.sql > /dev/null
q -d cone26 < 02_build.sql > /dev/null
q -d cone26 < 04_xmatch.sql > /dev/null; q -d cone26 < 11_xmatch_ab.sql > /dev/null
echo "cone26 built $(date -u +%T)"
i=0
for v in 26 36 36 26; do
  i=$((i+1)); swap $v
  q -d cone$v -c "SELECT bench_xm_ab_run(10, ARRAY[1, 10]::float8[], 2, ARRAY['q3c','pgsphere','skycell_cs','skycell_join'], 0.5$i)" > /dev/null
  q -d cone$v -c "\\copy (SELECT * FROM bench_xm_ab) TO STDOUT WITH CSV HEADER" > $S/ab/xm_run${i}_0$v.csv
  echo "run $i ($v) done $(date -u +%T)"
done
swap 36
