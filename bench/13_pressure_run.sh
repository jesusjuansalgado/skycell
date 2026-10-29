#!/bin/bash
OUT=/tmp/iso_results.txt
: > $OUT
phase () {
  local ph=$1 m=$2
  docker restart skycell-pg3 >/dev/null
  until docker exec skycell-pg3 pg_isready -U postgres >/dev/null 2>&1; do sleep 1; done
  sleep 3
  echo "############ phase $ph : $m only ############" >> $OUT
  date -u +%H:%M:%S >> $OUT
  docker exec -i skycell-pg3 psql -U postgres -d skycell -v method="$m" -v phase="$ph" \
    < /tmp/iso.sql >> $OUT 2>&1
}
phase A1 pgsphere
phase A2 skycell
phase B1 skycell
phase B2 pgsphere
echo "ISO_ALL_DONE" >> $OUT
