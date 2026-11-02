#!/bin/bash
# skycell performance check: the library just built against a baseline (the
# last release tag by default), on the same server, in alternating rounds.
#
#   PGHOST=/tmp PGPORT=5519 PGUSER=postgres ./perfcheck.sh [baseline] [candidate.so] [rounds]
#
#   baseline   a git tag or commit to build (default: the latest v* tag), or a
#              path to a built skycell.so
#   candidate  a built skycell.so (default: ../../ext/skycell.so, `make` first)
#   rounds     default 20; each runs both libraries, the order alternating
#
# It installs each library in turn into the server's pkglibdir -- run it on a
# development server only -- and restores the candidate at the end.  Each
# workload runs in a fresh session, so each loads the library just installed.
# The database ($PERF_DB, default real36) needs gaia_realc (../19_gaia_real.sh)
# with its cell index; setup.sql builds the rest once.  report.py: per
# workload, the candidate's summed per-centre time over the baseline's with a
# 95% bootstrap interval over centres; a workload whose interval lies wholly
# above $PERF_LIMIT (1.10) is a regression, and the exit status is 1.  Both
# libraries must return the same counts.
set -euo pipefail
cd "$(dirname "$0")"
db=${PERF_DB:-real36}
base=${1:-$(git describe --tags --abbrev=0 --match 'v*')}
cand=${2:-../../ext/skycell.so}
rounds=${3:-20}
work=${PERF_WORK:-$(mktemp -d)}
pkglib=$(pg_config --pkglibdir)

# Installed by rename, never by copying over the file: a backend still exiting
# from the last workload has the old library mapped, and overwriting its pages
# in place crashes it (and with it the server).
install_lib() {
  cp "$1" "$pkglib/.skycell.so.perfcheck"
  mv -f "$pkglib/.skycell.so.perfcheck" "$pkglib/skycell.so"
}

if [ -f "$base" ]; then
  cp "$base" "$work/base.so"; label=$(basename "$base")
else
  label=$base
  rm -rf "$work/src"
  git worktree add -q "$work/src" "$base"
  (cd "$work/src/ext" && make -s PG_CONFIG="$(command -v pg_config)" >/dev/null)
  cp "$work/src/ext/skycell.so" "$work/base.so"
  git worktree remove --force "$work/src"
fi
cp "$cand" "$work/cand.so"
echo "baseline $label ($(md5sum < "$work/base.so" | cut -c1-12)), candidate $(md5sum < "$work/cand.so" | cut -c1-12), $rounds rounds, database $db"

psql -d "$db" -X -q -f setup.sql
echo lib,round,workload,c,us,n > "$work/perf.csv"
for r in $(seq 1 "$rounds"); do
  if [ $((r % 2)) = 1 ]; then order="base cand"; else order="cand base"; fi
  for lib in $order; do
    install_lib "$work/$lib.so"
    psql -d "$db" -X -q -c "SELECT count(*) FROM gaia_realc WHERE ra < 0" >/dev/null
    psql -d "$db" -X -q -f workloads.sql | sed "s/^/$lib,$r,/" >> "$work/perf.csv"
    echo "  round $r: $lib done"
  done
done
install_lib "$work/cand.so"
cp "$work/perf.csv" perf.csv
python3 report.py perf.csv "$label" "${PERF_LIMIT:-1.10}"
