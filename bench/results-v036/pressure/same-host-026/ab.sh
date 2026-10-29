#!/bin/bash
# skycell 0.26 against 0.36 in the memory test, on the same relation: the
# extension is swapped inside the `press` database itself, so the heap and
# pgSphere's GiST stay the very same files. Per version: the library swapped
# (copy, then rename), DROP EXTENSION skycell CASCADE (which drops only
# oc20_cell), CREATE EXTENSION skycell VERSION, oc20_cell rebuilt with its
# statistics target, ANALYZE, then ../run.sh's four phases. Order 26, 36; the
# 0.36 run in ../iso_results.txt came before both.
set -euo pipefail
S=${S:?scratch directory holding ab026.so / ab036.so}
OUT=$(cd "$(dirname "$0")" && pwd)
BIN=/opt/pg18/bin
D=/var/lib/postgresql/press18
L=/opt/pg18/lib/postgresql/skycell.so
q() { $BIN/psql -h /tmp -p 5520 -U postgres -X -q -v ON_ERROR_STOP=1 -d press "$@"; }
stop() { su postgres -c "$BIN/pg_ctl -D $D -m fast -w stop" > /dev/null 2>&1 || true; }
start() { su postgres -c "$BIN/pg_ctl -D $D -l $D/log -o '-p 5520 -k /tmp -c shared_buffers=1GB -c maintenance_work_mem=1GB' -w start" > /dev/null; }
swap() { cp $S/ab0$1.so $L.new && mv -f $L.new $L; }
for v in 26 36; do
  stop; swap $v; start
  q -c "DROP EXTENSION skycell CASCADE" \
    -c "CREATE EXTENSION skycell VERSION '0.$v'" \
    -c "CREATE INDEX oc20_cell ON oc20 (skycell_ang2cell(s_ra, s_dec))" \
    -c "ALTER INDEX oc20_cell ALTER COLUMN 1 SET STATISTICS 1000" \
    -c "ANALYZE oc20"
  q -At -c "SELECT 'skycell ' || extversion FROM pg_extension WHERE extname = 'skycell'" \
        -c "SELECT relname || ' filenode ' || pg_relation_filenode(oid) || ' ' || pg_relation_size(oid) FROM pg_class WHERE relname IN ('oc20', 'oc20_gist', 'oc20_cell') ORDER BY 1" \
        | tee $OUT/check_0$v.txt
  stop
  RES=$OUT/iso_results_0$v.txt LOG=$OUT/run.log "$OUT/../run.sh"
done
swap 36
