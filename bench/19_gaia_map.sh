#!/bin/bash
# Fetch the Gaia DR3 density map behind the paper's resampled "Gaia corpus":
# source counts per order-9 nested HEALPix cell, from the ESA archive.
#
# Gaia's source_id carries the level-12 nested HEALPix index in its top bits
# (source_id / 2^35), so the order-9 cell is source_id / 2^35 / 4^3 =
# source_id / 2^41.  The query aggregates all 1.8e9 rows, too long for the
# synchronous endpoint 19_gaia_real.sh uses, so it runs as an async TAP job.
#
# Output: $GAIA_DIR/gaia_map_hpx9.csv (hpx9,n), at most 3,145,728 rows,
# summing to the DR3 total 1,811,709,771.  Load with:
#   CREATE TABLE gaia_map (hpx9 bigint PRIMARY KEY, n bigint);
#   \copy gaia_map FROM 'gaia_map_hpx9.csv' CSV HEADER
# then bench/11_scale_corpus.sql resamples positions from it.
set -euo pipefail
OUT=${GAIA_DIR:-/tmp/gaia}; mkdir -p "$OUT"
TAP=https://gea.esac.esa.int/tap-server/tap
F="$OUT/gaia_map_hpx9.csv"
[ -s "$F" ] && { echo "already have $F"; exit 0; }

JOB=$(curl -sS -o /dev/null -w '%{redirect_url}' -X POST "$TAP/async" \
  --data-urlencode 'REQUEST=doQuery' --data-urlencode 'LANG=ADQL' \
  --data-urlencode 'FORMAT=csv' --data-urlencode 'PHASE=RUN' \
  --data-urlencode 'QUERY=SELECT source_id / 2199023255552 AS hpx9, COUNT(*) AS n FROM gaiadr3.gaia_source GROUP BY source_id / 2199023255552')
[ -n "$JOB" ] || { echo "job submission failed" >&2; exit 1; }
echo "job: $JOB"
while :; do
  P=$(curl -sS "$JOB/phase")
  echo "$(date +%H:%M:%S) $P"
  case "$P" in
    COMPLETED) break ;;
    ERROR|ABORTED) curl -sS "$JOB/error" >&2 || true; exit 1 ;;
  esac
  sleep 30
done
curl -sS -L "$JOB/results/result" -o "$F.tmp"
head -1 "$F.tmp" | grep -q '^hpx9' || { echo "unexpected result header" >&2; head -3 "$F.tmp" >&2; exit 1; }
mv "$F.tmp" "$F"
awk -F, 'NR>1 {c++; s+=$2} END {printf "cells %d, sources %d\n", c, s}' "$F"
echo GAIA_MAP_DONE
