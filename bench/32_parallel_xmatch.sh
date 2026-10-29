#!/bin/bash
# Parallel cross-matching (paper, Table tab:parallel): the same library and data,
# serial against PostgreSQL's default two workers and against three (the
# probe table's parallel_workers), 1M targets, 1" and 30" cones, both heaps.
# Median of three warm runs after an untimed one; count and id sum recorded.
#
#   psql -d pbench -f 32_parallel_data.sql
#   PGDATABASE=pbench ./32_parallel_xmatch.sh > results-parallel/scale.tsv
#
# Connection from the usual PG* variables. Server: PostgreSQL 18 defaults,
# shared_buffers = 2GB, max_worker_processes and max_parallel_workers >= 3.
P="psql -X -q -At"
$P -c "ALTER TABLE probe_1m SET (parallel_workers = 3)"
printf "heap\tradius_deg\tworkers\tlaunched\tmedian_ms\tcount|sum\n"
for tbl in cat_clus cat_rand; do for r in 0.000277778 0.008333333; do
  Q="SELECT count(*), sum(c.id) FROM probe_1m p JOIN $tbl c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, $r)"
  for w in 3 2 0; do
    SETS="SET max_parallel_workers_per_gather = $w"
    $P -c "$SETS" -c "$Q" >/dev/null
    t=$(for i in 1 2 3; do $P -c "$SETS" -c "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) $Q" \
          | sed -n 's/.*Execution Time: \([0-9.]*\) ms/\1/p'; done | sort -n | sed -n 2p)
    l=$($P -c "$SETS" -c "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF) $Q" | sed -n 's/.*Workers Launched: \([0-9]*\).*/\1/p')
    n=$($P -c "$SETS" -c "$Q")
    printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$tbl" "$r" "$w" "${l:-0}" "$t" "$n"
  done
done; done
$P -c "ALTER TABLE probe_1m RESET (parallel_workers)"
