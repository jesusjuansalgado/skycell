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
# Why 192 synchronous queries and not one async job: the whole aggregate as
# one async job ran 90 minutes and then failed storing its result (a lock
# timeout in the archive's file-quota bookkeeping, shared by all anonymous
# users), and the same error recurred on a twelfth-size async job.
# Synchronous queries skip that step.  One per order-2 pixel p -- source_id
# in [p*2^55, (p+1)*2^55), about 1e7 sources and up to 16,384 order-9 cells --
# takes seconds.  Parts are kept, so a rerun resumes; each is retried up to 3
# times.
#
# Output: $GAIA_DIR/gaia_map_hpx9.csv (hpx9,n), summing to the DR3 total
# 1,811,709,771.  Fetched 2026-10-08: 3,145,727 rows -- every order-9 cell but
# one, 2750834 (RA 246.7, Dec -24.5, the rho Ophiuchi dark cloud core), which
# holds no DR3 source at all (checked directly) and so is absent from the
# GROUP BY; treat it as n = 0.  A copy is committed as
# bench/data/gaia_map_hpx9.csv.gz.  Load with:
#   CREATE TABLE gaia_map (hpx9 bigint PRIMARY KEY, n bigint);
#   \copy gaia_map FROM 'gaia_map_hpx9.csv' CSV HEADER
# then bench/11_scale_corpus.sql resamples positions from it.
set -euo pipefail
OUT=${GAIA_DIR:-/tmp/gaia}; P="$OUT/map_parts"; mkdir -p "$P"
F="$OUT/gaia_map_hpx9.csv"
[ -s "$F" ] && { echo "already have $F"; exit 0; }

for p in $(seq 0 191); do
  [ -s "$P/$p.csv" ] && continue
  lo=$(( p * (1 << 55) )); hi=$(( (p + 1) * (1 << 55) ))
  for try in 1 2 3; do
    curl -sS --max-time 900 -G 'https://gea.esac.esa.int/tap-server/tap/sync' \
      --data-urlencode 'REQUEST=doQuery' --data-urlencode 'LANG=ADQL' \
      --data-urlencode 'FORMAT=csv' --data-urlencode 'MAXREC=100000' \
      --data-urlencode "QUERY=SELECT GAIA_HEALPIX_INDEX(9, source_id) AS hpx9, COUNT(*) AS n FROM gaiadr3.gaia_source WHERE source_id >= $lo AND source_id < $hi GROUP BY hpx9" \
      -o "$P/$p.tmp" || true
    if [ -s "$P/$p.tmp" ] && head -1 "$P/$p.tmp" | grep -q '^hpx9'; then
      mv "$P/$p.tmp" "$P/$p.csv"; break
    fi
    echo "part $p try $try failed: $(head -c 200 "$P/$p.tmp" 2>/dev/null || true)"
    [ "$try" -eq 3 ] && exit 1
    sleep 30
  done
  echo "part $p: $(( $(wc -l < "$P/$p.csv") - 1 )) cells, $(date +%H:%M:%S)"
done

# header first, then the data rows sorted (sorting the header with them would
# place it after cell 0)
{ echo "hpx9,n"; for p in $(seq 0 191); do tail -n +2 "$P/$p.csv"; done | sort -t, -k1,1n; } > "$F.tmp"
mv "$F.tmp" "$F"
awk -F, 'NR>1 {c++; s+=$2} END {printf "cells %d, sources %d\n", c, s}' "$F"
echo GAIA_MAP_DONE
