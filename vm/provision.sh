#!/bin/bash
# Provision a fresh Debian 12/13 or Ubuntu 22.04/24.04 VM to reproduce the
# paper's measurements: PostgreSQL (default 18, the paper's version) from the
# PGDG apt repository with Q3C and pgSphere, skycell built from this checkout,
# the server configured as REPRODUCING.md section 2, a `skycell` database with
# the three extensions, and the regression suite run once.
#
#   cd skycell && sudo -E vm/provision.sh            # PG_MAJOR=18 by default
#   PG_MAJOR=17 sudo -E vm/provision.sh              # another major version
#
# Re-running is safe: packages are skipped if present, the config file is
# rewritten, the database is created only if missing.
set -euo pipefail
PG_MAJOR=${PG_MAJOR:-18}
SHARED_BUFFERS=${SHARED_BUFFERS:-2GB}
EFFECTIVE_CACHE_SIZE=${EFFECTIVE_CACHE_SIZE:-5GB}
REPO=$(cd "$(dirname "$0")/.." && pwd)
RUN_AS=${SUDO_USER:-$(id -un)}
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }

echo "== packages"
apt-get update -q
apt-get install -y -q --no-install-recommends ca-certificates curl gnupg git make gcc \
  build-essential python3 postgresql-common lsb-release
# PGDG repository (the same packages the paper's Docker image used, docker/Dockerfile)
[ -e /etc/apt/sources.list.d/pgdg.list ] || [ -e /etc/apt/sources.list.d/pgdg.sources ] \
  || /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y
apt-get update -q
apt-get install -y -q --no-install-recommends \
  "postgresql-$PG_MAJOR" "postgresql-server-dev-$PG_MAJOR" \
  "postgresql-$PG_MAJOR-q3c" "postgresql-$PG_MAJOR-pgsphere"

echo "== skycell"
PG_CONFIG=/usr/lib/postgresql/$PG_MAJOR/bin/pg_config
make -C "$REPO/ext" clean >/dev/null
make -C "$REPO/ext" PG_CONFIG="$PG_CONFIG" -j"$(nproc)"
make -C "$REPO/ext" PG_CONFIG="$PG_CONFIG" install

echo "== server configuration (REPRODUCING.md section 2)"
CONF=/etc/postgresql/$PG_MAJOR/main/conf.d/skycell-bench.conf
cat > "$CONF" <<CONFEOF
# skycell paper measurements -- REPRODUCING.md section 2
shared_buffers = $SHARED_BUFFERS
effective_cache_size = $EFFECTIVE_CACHE_SIZE
random_page_cost = 1.1
seq_page_cost = 1
work_mem = 64MB
maintenance_work_mem = 1GB
max_parallel_workers_per_gather = 0
jit = off
CONFEOF
systemctl restart "postgresql@$PG_MAJOR-main"
for _ in $(seq 1 30); do pg_isready -q && break; sleep 1; done

echo "== database"
# a superuser role named after the login user, so psql works over the local
# socket with peer authentication and no password
sudo -u postgres psql -q -c "DO \$\$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = '$RUN_AS') THEN CREATE ROLE \"$RUN_AS\" SUPERUSER LOGIN; END IF; END \$\$;"
sudo -u postgres psql -q -tc "SELECT 1 FROM pg_database WHERE datname = 'skycell'" | grep -q 1 \
  || sudo -u postgres createdb -O "$RUN_AS" skycell
sudo -u postgres psql -q -d skycell -c "CREATE EXTENSION IF NOT EXISTS skycell" \
  -c "CREATE EXTENSION IF NOT EXISTS q3c" -c "CREATE EXTENSION IF NOT EXISTS pg_sphere"

echo "== regression suite"
chown -R "$RUN_AS" "$REPO/ext"
sudo -u "$RUN_AS" make -C "$REPO/ext" PG_CONFIG="$PG_CONFIG" installcheck PGUSER="$RUN_AS" \
  || { echo "installcheck FAILED -- see ext/regression.diffs" >&2; exit 1; }

echo "== versions"
sudo -u postgres psql -At -d skycell -c "SELECT version()" \
  -c "SELECT extname || ' ' || extversion FROM pg_extension WHERE extname IN ('skycell','q3c','pg_sphere') ORDER BY 1" \
  -c "SELECT name || ' = ' || setting FROM pg_settings WHERE name IN ('shared_buffers','effective_cache_size','random_page_cost','work_mem','maintenance_work_mem','max_parallel_workers_per_gather','jit') ORDER BY 1"
echo "PROVISION_DONE: now run vm/run_paper.sh as $RUN_AS (see vm/README.md)"
