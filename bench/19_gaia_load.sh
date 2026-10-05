#!/bin/bash
# Load the CSVs fetched by 19_gaia_real.sh into the real-Gaia corpora that
# 17_ablation.sql and 18_xmatch_sweep.sql read (REPRODUCING.md §3):
#
#   gaia_realu   10M  real DR3 positions, random_index < 10^7, heap in random_index
#                     order -- i.e. random on the sky (unclustered)
#   gaia_realc   10M  the same rows, heap in skycell cell order (clustered)
#   gaia_fields  1.6M complete DR3 in the 8 crowded fields, cell order
#
# Indexes and statistics as REPRODUCING.md §4. Needs skycell, q3c and pg_sphere
# installed in the target database; psql connection from the usual PG* env vars.
set -euo pipefail
DIR=${GAIA_DIR:-/tmp/gaia}
ls "$DIR"/allsky_*.csv >/dev/null

psql -v ON_ERROR_STOP=1 -q <<'SQL'
CREATE EXTENSION IF NOT EXISTS skycell;
CREATE EXTENSION IF NOT EXISTS q3c;
CREATE EXTENSION IF NOT EXISTS pg_sphere;
DROP TABLE IF EXISTS gaia_raw, gaia_raw_fields;
CREATE UNLOGGED TABLE gaia_raw (source_id bigint, random_index bigint, ra float8, dec float8);
CREATE UNLOGGED TABLE gaia_raw_fields (field text, source_id bigint, ra float8, dec float8);
SQL

for f in "$DIR"/allsky_*.csv; do
  psql -v ON_ERROR_STOP=1 -q -c "\\copy gaia_raw FROM '$f' WITH (FORMAT csv, HEADER true)"
done
for f in "$DIR"/field_*.csv; do
  [ -e "$f" ] || continue
  n=$(basename "$f" .csv); n=${n#field_}
  psql -v ON_ERROR_STOP=1 -q -c "\\copy gaia_raw_fields (source_id, ra, dec) FROM '$f' WITH (FORMAT csv, HEADER true)" \
       -c "UPDATE gaia_raw_fields SET field = '$n' WHERE field IS NULL"
done

# CREATE TABLE AS, not ADD COLUMN + UPDATE: the latter rewrites every row and was
# OOM-killed at 10M rows (REPRODUCING.md §4).
psql -v ON_ERROR_STOP=1 <<'SQL'
\timing on
DROP TABLE IF EXISTS gaia_realu, gaia_realc, gaia_fields;

CREATE TABLE gaia_realu AS
SELECT source_id, random_index, ra, dec, spoint(radians(ra), radians(dec)) AS pos
FROM gaia_raw ORDER BY random_index;

CREATE TABLE gaia_realc AS
SELECT * FROM gaia_realu ORDER BY skycell_ang2cell(ra, dec), source_id;

-- the 47 Tuc and off-cluster cones are close; keep each source once
CREATE TABLE gaia_fields AS
SELECT field, source_id, ra, dec, spoint(radians(ra), radians(dec)) AS pos
FROM (SELECT DISTINCT ON (source_id) * FROM gaia_raw_fields ORDER BY source_id, field) d
ORDER BY skycell_ang2cell(ra, dec), source_id;

DO $$
DECLARE t text;
BEGIN
  FOREACH t IN ARRAY ARRAY['gaia_realu', 'gaia_realc', 'gaia_fields'] LOOP
    EXECUTE format('CREATE INDEX %1$s_cell ON %1$s (skycell_ang2cell(ra, dec))', t);
    EXECUTE format('ALTER INDEX %1$s_cell ALTER COLUMN 1 SET STATISTICS 1000', t);
    EXECUTE format('CREATE INDEX %1$s_q3c ON %1$s (q3c_ang2ipix(ra, dec))', t);
    EXECUTE format('CREATE INDEX %1$s_gist ON %1$s USING gist (pos)', t);
  END LOOP;
END $$;

-- Not inside the DO block: there, ANALYZE ignored the statistics target just
-- set on the index and built a 100-bucket histogram instead of 1000.
ANALYZE gaia_realu;
ANALYZE gaia_realc;
ANALYZE gaia_fields;
SELECT tablename, array_length(histogram_bounds::text::text[], 1) - 1 AS buckets
FROM pg_stats WHERE tablename ~ '^gaia_(realu|realc|fields)_cell$' ORDER BY 1;

DROP TABLE gaia_raw, gaia_raw_fields;

SELECT 'gaia_realu' AS corpus, count(*) AS n FROM gaia_realu
UNION ALL SELECT 'gaia_realc', count(*) FROM gaia_realc
UNION ALL SELECT 'gaia_fields', count(*) FROM gaia_fields;
SELECT field, count(*) FROM gaia_fields GROUP BY field ORDER BY field;
SELECT relname, pg_size_pretty(pg_relation_size(oid)) FROM pg_class
WHERE relname ~ '^gaia_(realu|realc|fields)' ORDER BY relname;
SQL
echo GAIA_LOAD_DONE
