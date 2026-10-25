#!/usr/bin/env bash
# Build skycell against one PostgreSQL major version, install it, and run the
# regression suite (make installcheck) on a fresh cluster of that version.
#
#   .github/ci/pg-regress.sh 17
#
# Expects postgresql-$PG and postgresql-server-dev-$PG installed (Debian/Ubuntu
# packaging: pg_createcluster, /usr/lib/postgresql/$PG). The CI workflow
# installs them from apt.postgresql.org; run locally, any source works.
set -euo pipefail

PG=${1:?usage: $0 <postgresql major version>}
PG_CONFIG=/usr/lib/postgresql/$PG/bin/pg_config
PORT=${PGPORT:-5432}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)

"$PG_CONFIG" --version

# One cluster, of this version, on $PORT: a runner image may already carry a
# cluster of another version on the default port.
pg_lsclusters --no-header | while read -r ver name _; do
  sudo pg_dropcluster --stop "$ver" "$name"
done
sudo pg_createcluster "$PG" ci --port "$PORT" --start

cd "$ROOT/ext"
make PG_CONFIG="$PG_CONFIG"
sudo make install PG_CONFIG="$PG_CONFIG"

# pg_regress connects over the Unix socket as this user (peer authentication)
sudo -u postgres psql -p "$PORT" -qc "CREATE ROLE \"$(id -un)\" SUPERUSER LOGIN"
status=0
make installcheck PG_CONFIG="$PG_CONFIG" PGPORT="$PORT" || status=$?
if [ "$status" -ne 0 ] && [ -f test/regression.diffs ]; then
  cat test/regression.diffs
fi
exit "$status"
