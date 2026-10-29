#!/bin/bash
# plans.sql under 0.26 (real26) and 0.36 (real36), alternating 26, 36, 36, 26
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../server.sh"
L=/home/user/pg18/lib/postgresql/skycell.so
swap() { cp "/home/user/build/sk0$1.so" "$L.new" && mv -f "$L.new" "$L"; }
i=0
for v in 26 36 36 26; do
  i=$((i+1)); swap $v; eval "$RESTART"
  psql -X -At -d real$v -f "$HERE/plans.sql" > "$HERE/plans_run${i}_0$v.txt"
done
swap 36; eval "$RESTART"
