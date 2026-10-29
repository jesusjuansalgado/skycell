#!/bin/bash
# Re-measure the paper's region table (tab:gistregion) with the committed
# scripts 21-24, unchanged, for one or more skycell libraries, so a change to
# the GiST key can be attributed. For each library in turn (in alternating
# order, REPS times) the library file is swapped, fpr's skycell GiST index is
# dropped (the scripts rebuild it with that library's compress function), and
# 21-24 run with nprobe probes. Every script's bench_region_* table is then
# appended to results-regions/runs.tsv, tagged with the library and the
# repeat.
#
#   psql -v n=10000000 -f 01_data.sql; psql -v nfp=50000 -f 20_region_xmatch.sql
#   LIBS="v0.34:/path/skycell-v0.34.so v0.35:/path/skycell-v0.35.so" REPS=3 ./33_region_table.sh
#
# Connection from PG* variables; LIB_DIR is the server's pkglibdir.
set -euo pipefail
cd "$(dirname "$0")"
P="psql -X -q -At -v ON_ERROR_STOP=1"
LIB_DIR=${LIB_DIR:-$(pg_config --pkglibdir)}
REPS=${REPS:-3}
NPROBE=${NPROBE:-500}
OUT=${OUT:-results-regions}
mkdir -p "$OUT"
printf "library\trep\tscript\tscope\tmethod\tpass\tn\tms\n" > "$OUT/runs.tsv"
read -r -a libs <<< "$LIBS"
for rep in $(seq 1 "$REPS"); do
  order=("${libs[@]}")
  if [ $((rep % 2)) -eq 0 ]; then order=($(printf '%s\n' "${libs[@]}" | tac)); fi
  for spec in "${order[@]}"; do
    name=${spec%%:*}; so=${spec#*:}
    # copied, then renamed into place: overwriting the file that a backend
    # still exiting has mapped kills it with SIGBUS, and the server with it
    cp "$so" "$LIB_DIR/skycell.so.new" && mv -f "$LIB_DIR/skycell.so.new" "$LIB_DIR/skycell.so"
    $P -c "DROP INDEX IF EXISTS fpr_region_gist"
    for f in 21_region_overlap 22_region_gist 23_region_contains 24_region_contains_region; do
      $P -v nprobe="$NPROBE" -f "$f.sql" > "$OUT/$f.$name.$rep.out"
    done
    $P -F $'\t' -c "
      SELECT '$name', $rep, '21', '', method, pass, n, ms FROM bench_region_overlap
      UNION ALL SELECT '$name', $rep, '22', '', method, pass, n, ms FROM bench_region_gist
      UNION ALL SELECT '$name', $rep, '23', '', method, pass, n, ms FROM bench_region_contains
      UNION ALL SELECT '$name', $rep, '24', scope, method, pass, n, ms FROM bench_region_contains_region" >> "$OUT/runs.tsv"
    echo "$(date -u +%T) $name rep $rep done" >&2
  done
done
