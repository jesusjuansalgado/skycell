#!/bin/bash
# Parallel cross-match A/B: skycell v0.35 (old, never parallel) against the
# parallel-safe library (new), PostgreSQL 18 defaults; lib+workers per row:
# old0/new4/old2/new2 = library and max_parallel_workers_per_gather
S=${S:-.}     # directory holding old.so (v0.35) and new.so (parallel-safe), both built for this server
LIB=${LIB:-$(pg_config --pkglibdir)/skycell.so}
P="psql -X -q -At"   # connection from PG* variables
OUT=${OUT:-ab.tsv}; : > $OUT
plan() {
  $P -c "$2" -c "EXPLAIN (COSTS OFF) $1" | awk '
    /Custom Scan \(SkycellCone\)/ {cs=1} /Mode: / {mode=$2} /Workers Planned: / {w=$3}
    END { printf "%s%s", (cs ? "custom/" mode : "no-custom"), (w ? " x" w : " serial") }'
}
timed() {
  local t
  t=$( { $P -c "$2" -c "$1" >/dev/null; for i in 1 2 3; do
        $P -c "$2" -c "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) $1" | sed -n 's/.*Execution Time: \([0-9.]*\) ms/\1/p'; done; } )
  echo "$t" | sort -n | sed -n 2p
}
k=0
for tbl in cat_clus cat_rand; do for np in 200k 1m; do for r in 0.000277778 0.001388889 0.008333333; do
  Q="SELECT count(*), sum(c.id) FROM probe_$np p JOIN $tbl c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, $r)"
  k=$((k+1))
  for v in $( [ $((k % 2)) -eq 0 ] && echo "new2 old2 new4 old0" || echo "old0 new4 old2 new2" ); do
    lib=${v%?}; w=${v#$lib}
    cp $S/$lib.so $LIB
    SETS="SET max_parallel_workers_per_gather = $w"
    pl=$(plan "$Q" "$SETS"); ms=$(timed "$Q" "$SETS"); n=$($P -c "$SETS" -c "$Q" | tail -1)
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$tbl" "$np" "$r" "$v" "$pl" "$ms" "$n" | tee -a $OUT
  done
done; done; done
cp $S/new.so $LIB
echo DONE
