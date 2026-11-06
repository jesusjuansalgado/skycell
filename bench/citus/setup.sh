#!/bin/bash
# A local Citus cluster (a coordinator and two workers) with skycell, for
# checking skycell on distributed tables against the same table on one node.
#
#   bench/citus/setup.sh [PG_BIN] [BASE]
#
# Needs Citus built and installed for the same PostgreSQL (13.0.3 was used:
#   git clone --depth 1 --branch v13.0.3 https://github.com/citusdata/citus
#   ./configure PG_CONFIG=... --without-libcurl && make && make install
# with libzstd-dev, liblz4-dev and libkrb5-dev present), and skycell
# installed (make install in ext/).  Ports 5601-5603, sockets in /tmp; run as
# a user that can become postgres.
set -euo pipefail
PG_BIN=${1:-/usr/lib/postgresql/16/bin}
BASE=${2:-/home/user/runlogs/citus}

mkdir -p "$BASE"
chown postgres "$BASE"
for n in c:5601 w1:5602 w2:5603; do
	d=${n%%:*}
	p=${n##*:}
	if [ ! -d "$BASE/$d" ]; then
		su postgres -s /bin/bash -c "$PG_BIN/initdb -D $BASE/$d -A trust >/dev/null"
		echo "shared_preload_libraries = 'citus'" >> "$BASE/$d/postgresql.conf"
	fi
	su postgres -s /bin/bash -c "$PG_BIN/pg_ctl -D $BASE/$d -o '-p $p -k /tmp' -l $BASE/$d.log -w start" >/dev/null || true
	psql -X -h /tmp -p "$p" -U postgres -d postgres -qc "CREATE EXTENSION IF NOT EXISTS citus"
done

psql -X -h /tmp -p 5601 -U postgres -d postgres -v ON_ERROR_STOP=1 <<'EOF'
SELECT citus_set_coordinator_host('localhost', 5601);
SELECT citus_add_node('localhost', 5602) WHERE NOT EXISTS (SELECT 1 FROM pg_dist_node WHERE nodeport = 5602);
SELECT citus_add_node('localhost', 5603) WHERE NOT EXISTS (SELECT 1 FROM pg_dist_node WHERE nodeport = 5603);
CREATE EXTENSION IF NOT EXISTS skycell;   -- Citus creates it on the workers too

-- the same 200,000-row Fibonacci-sphere catalogue, on one node and over 8 shards
DROP TABLE IF EXISTS cat_local, cat_dist, probe_local, probe_ref, probe_dist;
CREATE TABLE cat_local AS
SELECT i AS id, mod(i * 137.50776405003785, 360.0) AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 200000.0)) AS dec, (i % 20)::real AS mag
FROM generate_series(0, 199999) i;
CREATE INDEX ON cat_local (skycell_ang2cell(ra, dec));
ANALYZE cat_local;
CREATE TABLE cat_dist (id int, ra float8, dec float8, mag real);
SELECT create_distributed_table('cat_dist', 'id', shard_count := 8);
INSERT INTO cat_dist SELECT * FROM cat_local;
CREATE INDEX ON cat_dist (skycell_ang2cell(ra, dec));
ANALYZE cat_dist;
EOF
echo "cluster ready: psql -h /tmp -p 5601 -U postgres -f bench/citus/compare.sql"
