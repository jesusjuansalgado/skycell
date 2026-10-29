#!/bin/bash
# The memory-pressure experiment (13_pressure.sql, unchanged), scaled from the
# original 20.5M rows to 10M (setup.sql) and run with skycell 0.36.
#
# The original ran in a 3 GiB container with shared_buffers = 768MB and
# effective_cache_size = 2GB, where pgSphere's GiST was 1.5 times the buffer
# pool and skycell's B-tree 0.57 times. Here, on its own cluster:
# - shared_buffers is set from the built relation so that pgSphere's index is
#   again 1.5 times the pool (skycell's then falls at about 0.57 of it too, if
#   the two indexes keep their size ratio);
# - the server runs in a cgroup v1 memory group limited to 3 GiB x 10/20.5,
#   which charges the page cache as a container's limit does;
# - effective_cache_size is 2GB x 10/20.5; the other settings are
#   REPRODUCING.md section 2's.
# Four phases, as 13_pressure_run.sh: pgsphere, skycell, skycell, pgsphere,
# each after a restart with the page cache dropped, so shared_buffers starts
# empty and the index not under test occupies none of it.
#
#   ./run.sh            # needs root, /opt/pg18; pg_buffercache installed before the phases
#   RES=... LOG=... ./run.sh   # the phases' output and log elsewhere (same-host-026/)
set -euo pipefail
OUT=$(cd "$(dirname "$0")" && pwd)
cd "$OUT/../.."
BIN=/opt/pg18/bin
D=/var/lib/postgresql/press18
PORT=5520
CG=/sys/fs/cgroup/memory/pgpress
MEM=$(( 3 * 1024 * 1024 * 1024 * 100 / 205 ))       # 3 GiB x 10/20.5
ECS=$(( 2 * 1024 * 100 / 205 ))MB                    # 2 GB x 10/20.5
q() { $BIN/psql -h /tmp -p $PORT -U postgres -X -q -v ON_ERROR_STOP=1 -d press "$@"; }
RES=${RES:-$OUT/iso_results.txt}
LOG=${LOG:-$OUT/run.log}
step() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG"; }
opts() { echo "-p $PORT -k /tmp -c shared_buffers=$1 -c effective_cache_size=$ECS -c random_page_cost=1.1 -c seq_page_cost=1 -c work_mem=64MB -c maintenance_work_mem=1GB -c max_parallel_workers_per_gather=0 -c jit=off"; }
stop() { su postgres -c "$BIN/pg_ctl -D $D -m fast -w stop" > /dev/null 2>&1 || true; }
start() { su postgres -c "$BIN/pg_ctl -D $D -l $D/log -o '$(opts "$1")' -w start" > /dev/null; }

if [ ! -f "$D/PG_VERSION" ]; then
  step "initdb, build the relation (outside the memory limit)"
  mkdir -p $D && chown postgres $D
  su postgres -c "$BIN/initdb -D $D -A trust -U postgres" > /dev/null
  start 1GB
  $BIN/psql -h /tmp -p $PORT -U postgres -X -q -c "CREATE DATABASE press"
  q -f "$OUT/setup.sql" | tee "$OUT/setup.out"
  stop
fi
start 1GB
GIST_MB=$(q -At -c "SELECT pg_relation_size('oc20_gist') / 1048576")
CELL_MB=$(q -At -c "SELECT pg_relation_size('oc20_cell') / 1048576")
stop
SB_MB=$(( GIST_MB * 2 / 3 ))
step "oc20_gist ${GIST_MB} MB, oc20_cell ${CELL_MB} MB: shared_buffers ${SB_MB}MB (gist/sb 1.5, cell/sb $(python3 -c "print(round($CELL_MB/$SB_MB, 2))")), memory limit $((MEM / 1048576)) MB, effective_cache_size $ECS"
[ "${BUILD_ONLY:-}" ] && exit 0      # BUILD_ONLY=1 ./run.sh: build the relation only

start 1GB
q -c "CREATE EXTENSION IF NOT EXISTS pg_buffercache"   # 13_pressure.sql's residency report
stop
mkdir -p $CG
echo $MEM > $CG/memory.limit_in_bytes
: > "$RES"
phase() {
  local ph=$1 m=$2
  stop; sync; echo 3 > /proc/sys/vm/drop_caches
  echo 0 > $CG/memory.max_usage_in_bytes
  # start the postmaster inside the memory group: its children and the page
  # cache they fault in are charged to it
  ( echo $BASHPID > $CG/cgroup.procs; start ${SB_MB}MB )
  echo "############ phase $ph : $m only ############" >> "$RES"
  date -u +%H:%M:%S >> "$RES"
  q -v method="$m" -v phase="$ph" -f 13_pressure.sql >> "$RES" 2>&1
  echo "memory.max_usage_in_bytes $(cat $CG/memory.max_usage_in_bytes)" >> "$RES"
  step "phase $ph ($m) done"
}
phase A1 pgsphere
phase A2 skycell
phase B1 skycell
phase B2 pgsphere
stop
q() { :; }
echo "ISO_ALL_DONE" >> "$RES"
step DONE
