#!/bin/bash
# A second database, real26, under skycell 0.26's library (sk026.so, built from
# skycell commit 4ea17ee, the 0.26 the results-pg18 real-corpus runs used) and
# catalogue (CREATE EXTENSION skycell VERSION '0.26').  Its gaia_realc,
# gaia_real_cell and gaia_real_sphere are built from real36's rows with
# 19_gaia_load.sh's and 19_gaia_load_split.sh's SQL, so the heaps are the same.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../server.sh"
S=${S:-/home/user/build}
L=/home/user/pg18/lib/postgresql/skycell.so
swap() { cp "$S/sk0$1.so" "$L.new" && mv -f "$L.new" "$L"; }
q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }
swap 26
q -d postgres -c "DROP DATABASE IF EXISTS real26" -c "CREATE DATABASE real26"
q -d real26 -c "CREATE EXTENSION skycell VERSION '0.26'" -c "CREATE EXTENSION q3c" -c "CREATE EXTENSION pg_sphere"
q -d real26 -c "CREATE UNLOGGED TABLE gaia_raw (source_id bigint, random_index bigint, ra float8, dec float8)"
q -d real36 -c "\\copy (SELECT source_id, random_index, ra, dec FROM gaia_realc) TO STDOUT" \
  | q -d real26 -c "\\copy gaia_raw FROM STDIN"
q -d real26 <<'SQL'
CREATE TABLE gaia_realu AS
SELECT source_id, random_index, ra, dec, spoint(radians(ra), radians(dec)) AS pos
FROM gaia_raw ORDER BY random_index;
CREATE TABLE gaia_realc AS
SELECT * FROM gaia_realu ORDER BY skycell_ang2cell(ra, dec), source_id;
DROP TABLE gaia_realu;
CREATE INDEX gaia_realc_cell ON gaia_realc (skycell_ang2cell(ra, dec));
ALTER INDEX gaia_realc_cell ALTER COLUMN 1 SET STATISTICS 1000;
CREATE INDEX gaia_realc_q3c ON gaia_realc (q3c_ang2ipix(ra, dec));
CREATE INDEX gaia_realc_gist ON gaia_realc USING gist (pos);
CREATE TABLE gaia_real_cell AS
SELECT source_id, random_index, ra, dec, skycell_ang2cell(ra, dec) AS cell
FROM gaia_raw ORDER BY 5, source_id;
CREATE INDEX gaia_real_cell_idx ON gaia_real_cell (cell);
ALTER TABLE gaia_real_cell ALTER COLUMN cell SET STATISTICS 1000;
CREATE TABLE gaia_real_sphere AS
SELECT source_id, random_index, ra, dec, spoint(radians(ra), radians(dec)) AS pos
FROM gaia_raw ORDER BY skycell_ang2cell(ra, dec), source_id;
CREATE INDEX gaia_real_sphere_idx ON gaia_real_sphere USING gist (pos);
DROP TABLE gaia_raw;
SQL
q -d real26 -c "ANALYZE gaia_realc" -c "ANALYZE gaia_real_cell" -c "ANALYZE gaia_real_sphere" \
  -c "VACUUM (FREEZE) gaia_real_cell, gaia_real_sphere"
# the heaps are the same in both databases: row order and size
for d in real36 real26; do
  for t in gaia_realc gaia_real_cell gaia_real_sphere; do
    echo "$d $t $(q -d $d -At -c "SELECT count(*), md5(string_agg(source_id::text, ',' ORDER BY ctid)), pg_relation_size('$t') FROM $t")"
  done
done | tee "$HERE/heaps.txt"
swap 36
