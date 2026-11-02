#!/bin/bash
# Stored composite footprints against pgSphere (../35_footprint_composite.sql),
# on real36.   PGHOST=/tmp PGPORT=5519 PGUSER=postgres ./run.sh
set -euo pipefail
cd "$(dirname "$0")"
psql -d real36 -X -v nobs=50000 -v nfields=300 -v npos=600 -v n=5 -v reps=3 -f ../35_footprint_composite.sql > run.out 2>&1
python3 report.py footprint.csv > report.md
psql -d real36 -X -At -c "SELECT version()" -c "SELECT extname || ' ' || extversion FROM pg_extension ORDER BY 1" > versions.txt
md5sum "$(pg_config --pkglibdir)/skycell.so" >> versions.txt
