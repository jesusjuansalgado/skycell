#!/bin/bash
# Non-convex polygons against pgSphere (../27_nonconvex.sql), on real36's
# gaia_realc.  PGHOST=/tmp PGPORT=5519 PGUSER=postgres ./run.sh
set -euo pipefail
cd "$(dirname "$0")"
psql -d real36 -X -q -c "SELECT count(*) FROM gaia_realc" > /dev/null   # warm the heap
psql -d real36 -X -v ncentres=60 -v n=10 -v reps=3 -v ntruth=8 -f ../27_nonconvex.sql > run.out 2>&1
python3 report.py nonconvex.csv > report.md
psql -d real36 -X -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > versions.txt
md5sum "$(pg_config --pkglibdir)/skycell.so" >> versions.txt
