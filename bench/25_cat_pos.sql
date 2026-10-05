-- A skypos-typed, point-indexed view of the catalogue, with all three
-- point-indexing strategies registered on the same column at once:
-- skypos_spgist_ops (the shipped default), the plain btree expression
-- index skycell_cell(pos) that region_support_simplify()'s B-tree rewrite
-- needs, and skypos_cap_gist_ops (the experimental spherical-cap opclass).
-- Having all three present together is the point -- it's what lets the
-- adaptive gate (skycell.rewrite_max_waste/rewrite_waste_threshold()) and
-- PostgreSQL's own cost-based choice between SP-GiST and cap-GiST both be
-- exercised on the same table, the way GIST_REGION_DESIGN.md's rounds
-- forty-three through forty-five do.
--
-- Depends on `src` (01_data.sql): run that first. Ordered along the same
-- HEALPix curve as cat_sphere/cat_cell (02_build.sql) for comparable heap
-- locality.
--
-- skycell_point(ra, dec), not point(ra, dec): the unqualified two-argument
-- `point()` is ambiguous with PostgreSQL's own built-in geometric point
-- constructor and silently resolves to it instead of skycell's skypos one
-- if the search_path or argument types let it -- skycell_point() is the
-- unqualified alias precisely so this doesn't need a schema-qualified
-- call or a search_path dependency to get right.
--
-- The cap-GiST index is by far the slowest of the three to build here
-- (several minutes on 10M rows -- see GIST_REGION_DESIGN.md's "Round
-- forty-three": no bulk-load fast path, a generic buffered GiST build).
\set ON_ERROR_STOP 1

DROP TABLE IF EXISTS cat_pos CASCADE;
CREATE TABLE cat_pos AS
SELECT id, ra, dec, mag, skycell_point(ra, dec) AS pos
FROM src ORDER BY skycell_ang2cell(ra, dec);

CREATE INDEX cat_pos_spgist ON cat_pos USING spgist (pos);
CREATE INDEX cat_pos_cellexpr ON cat_pos (skycell_cell(pos));
CREATE INDEX cat_pos_capgist ON cat_pos USING gist (pos skypos_cap_gist_ops);
ANALYZE cat_pos;

SELECT relname, relpages, pg_size_pretty(pg_relation_size(relname::regclass))
FROM pg_class
WHERE relname IN ('cat_pos', 'cat_pos_spgist', 'cat_pos_cellexpr', 'cat_pos_capgist')
ORDER BY relname;
