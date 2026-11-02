#!/bin/bash
# Cross-matching a partitioned catalogue (../37_partition_xmatch.sql), on
# real36, once per library: install the library to measure, restart, run.
#   PGHOST=/tmp PGUSER=postgres PGPORT=5519 ./run.sh <label>   (v043, keytest, pruning)
set -euo pipefail
cd "$(dirname "$0")"
psql -d real36 -X -q -v reps=5 -f ../37_partition_xmatch.sql > "run-$1.out" 2>&1
mv partition.csv "partition-$1.csv"
