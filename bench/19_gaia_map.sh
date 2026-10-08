#!/bin/bash
# Fetch the Gaia DR3 density map behind the paper's resampled "Gaia corpus":
# source counts per order-9 nested HEALPix cell, from the ESA archive.
#
# Gaia's source_id carries the level-12 nested HEALPix index in its top bits
# (source_id / 2^35), so the order-9 cell is source_id / 2^41; ESA's
# GAIA_HEALPIX_INDEX(9, source_id) computes the same thing (checked against
# the arithmetic).  ADQL allows only a column in GROUP BY, so it groups by the
# alias.
#
# The full aggregate over 1.8e9 rows runs about 90 minutes as one async job,
# and one such run failed at the very end (a lock timeout in ESA's own
# result-file bookkeeping for anonymous users).  So it is split into the 12
# HEALPix base pixels -- base pixel b holds source_id in [b*2^59, (b+1)*2^59)
# -- run one after another, each retried up to 3 times; finished parts are
# kept, so a rerun resumes.
#
# Output: $GAIA_DIR/gaia_map_hpx9.csv (hpx9,n), at most 3,145,728 rows,
# summing to the DR3 total 1,811,709,771.  Load with:
#   CREATE TABLE gaia_map (hpx9 bigint PRIMARY KEY, n bigint);
#   \copy gaia_map FROM 'gaia_map_hpx9.csv' CSV HEADER
# then bench/11_scale_corpus.sql resamples positions from it.
set -euo pipefail
OUT=${GAIA_DIR:-/tmp/gaia}; mkdir -p "$OUT/map_parts"
TAP=https://gea.esac.esa.int/tap-server/tap
F="$OUT/gaia_map_hpx9.csv"
[ -s "$F" ] && { echo "already have $F"; exit 0; }

run_part() {  # $1 = base pixel; 0 on success
  local b=$1 lo hi job p
  lo=$(( b * (1 << 59) )); hi=$(( (b + 1) * (1 << 59) ))
  [ "$b" -eq 11 ] && hi=9223372036854775807
  job=$(curl -sS -o /dev/null -w '%{redirect_url}' -X POST "$TAP/async" \
    --data-urlencode 'REQUEST=doQuery' --data-urlencode 'LANG=ADQL' \
    --data-urlencode 'FORMAT=csv' --data-urlencode 'PHASE=RUN' \
    --data-urlencode "QUERY=SELECT GAIA_HEALPIX_INDEX(9, source_id) AS hpx9, COUNT(*) AS n FROM gaiadr3.gaia_source WHERE source_id >= $lo AND source_id < $hi GROUP BY hpx9")
  [ -n "$job" ] || { echo "part $b: submission failed"; return 1; }
  echo "part $b: $job"
  while :; do
    p=$(curl -sS "$job/phase" || echo UNKNOWN)
    case "$p" in
      COMPLETED) break ;;
      ERROR|ABORTED) echo "part $b: $p"; curl -sS "$job/error" | grep -o 'CDATA\[[^]]*' | head -1 || true; return 1 ;;
    esac
    sleep 30
  done
  curl -sS -L "$job/results/result" -o "$OUT/map_parts/$b.tmp"
  head -1 "$OUT/map_parts/$b.tmp" | grep -q '^hpx9' || { echo "part $b: bad result"; return 1; }
  mv "$OUT/map_parts/$b.tmp" "$OUT/map_parts/$b.csv"
  echo "part $b: done, $(( $(wc -l < "$OUT/map_parts/$b.csv") - 1 )) cells, $(date +%H:%M:%S)"
}

for b in $(seq 0 11); do
  [ -s "$OUT/map_parts/$b.csv" ] && continue
  for try in 1 2 3; do run_part "$b" && break; [ "$try" -eq 3 ] && exit 1; sleep 60; done
done

{ echo "hpx9,n"; for b in $(seq 0 11); do tail -n +2 "$OUT/map_parts/$b.csv"; done; } | sort -t, -k1,1n > "$F.tmp"
mv "$F.tmp" "$F"
awk -F, 'NR>1 {c++; s+=$2} END {printf "cells %d, sources %d\n", c, s}' "$F"
echo GAIA_MAP_DONE
