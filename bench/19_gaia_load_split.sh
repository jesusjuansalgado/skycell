#!/bin/bash
# Load the all-sky CSVs fetched by 19_gaia_real.sh into one table *per method*,
# mirroring 02_build.sql's cat_cell / cat_sphere split, instead of the shared
# gaia_realc heap 19_gaia_load.sh builds:
#
#   gaia_real_cell    10M real DR3 positions, stored int8 cell column, plain
#                     B-tree on it, statistics target 1000
#   gaia_real_sphere  the same rows, spoint column, pgSphere GiST
#
# Both heaps in skycell cell order (02_build.sql's convention: every method
# gets the same heap locality).  Separate heaps are what make a cold-cache
# comparison possible: on a shared heap a centre probed by one method warms
# the other's pages (GIST_REGION_DESIGN.md round sixty-nine).
set -euo pipefail
DIR=${GAIA_DIR:-/tmp/gaia}
ls "$DIR"/allsky_*.csv >/dev/null

psql -v ON_ERROR_STOP=1 -q <<'SQL'
CREATE EXTENSION IF NOT EXISTS skycell;
CREATE EXTENSION IF NOT EXISTS pg_sphere;
DROP TABLE IF EXISTS gaia_split_raw;
CREATE UNLOGGED TABLE gaia_split_raw (source_id bigint, random_index bigint, ra float8, dec float8);
SQL
for f in "$DIR"/allsky_*.csv; do
  psql -v ON_ERROR_STOP=1 -q -c "\\copy gaia_split_raw FROM '$f' WITH (FORMAT csv, HEADER true)"
done

psql -v ON_ERROR_STOP=1 <<'SQL'
\timing on
DROP TABLE IF EXISTS gaia_real_cell, gaia_real_sphere;
CREATE TABLE gaia_real_cell AS
SELECT source_id, random_index, ra, dec, skycell_ang2cell(ra, dec) AS cell
FROM gaia_split_raw ORDER BY 5, source_id;
CREATE INDEX gaia_real_cell_idx ON gaia_real_cell (cell);
ALTER TABLE gaia_real_cell ALTER COLUMN cell SET STATISTICS 1000;

CREATE TABLE gaia_real_sphere AS
SELECT source_id, random_index, ra, dec, spoint(radians(ra), radians(dec)) AS pos
FROM gaia_split_raw ORDER BY skycell_ang2cell(ra, dec), source_id;
CREATE INDEX gaia_real_sphere_idx ON gaia_real_sphere USING gist (pos);
DROP TABLE gaia_split_raw;
SQL
# outside any DO block or function (see 19_gaia_load.sh)
psql -v ON_ERROR_STOP=1 -q -c "ANALYZE gaia_real_cell" -c "ANALYZE gaia_real_sphere" \
     -c "VACUUM (FREEZE) gaia_real_cell, gaia_real_sphere"
psql -v ON_ERROR_STOP=1 <<'SQL'
SELECT 'gaia_real_cell' AS t, count(*) FROM gaia_real_cell
UNION ALL SELECT 'gaia_real_sphere', count(*) FROM gaia_real_sphere;
SELECT array_length(histogram_bounds::text::text[], 1) - 1 AS cell_buckets
FROM pg_stats WHERE tablename = 'gaia_real_cell' AND attname = 'cell';
SELECT relname, pg_size_pretty(pg_relation_size(oid)) FROM pg_class
WHERE relname ~ '^gaia_real_(cell|sphere)' ORDER BY relname;
SQL
echo GAIA_SPLIT_DONE
