-- ------------------------------------------------------------------
-- A real index on skyregion itself: a GiST opclass over && (region-region
-- INTERSECTS), so `a.region && b.region` with an ordinary
-- `CREATE INDEX ON b USING gist (region)` is index-backed with no side
-- table and no hand-written join -- unlike the MOC-ranges recipe above,
-- which needs both.
--
-- EXPERIMENTAL: this is a correctness-first spike (see ext/src/gist_
-- region.c's own file header for the exact caveats), not a tuned index.
-- picksplit is a simple linear split, not the R*-tree-style split
-- PostgreSQL's own box opclass uses, so tree quality -- and therefore query
-- performance -- has real headroom left unexplored. Benchmarked against the
-- MOC-ranges recipe and pgSphere's native && in bench/22_region_gist.sql.
-- ------------------------------------------------------------------

CREATE FUNCTION skyregion_gist_consistent(internal, internal, int4, oid, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_union(internal, internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_compress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_decompress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_penalty(internal, internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_picksplit(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_same(bytea, bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS skyregion_gist_ops
    DEFAULT FOR TYPE skyregion USING gist AS
    OPERATOR 1 && (skyregion, skyregion),
    FUNCTION 1 skyregion_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skyregion_gist_union(internal, internal),
    FUNCTION 3 skyregion_gist_compress(internal),
    FUNCTION 4 skyregion_gist_decompress(internal),
    FUNCTION 5 skyregion_gist_penalty(internal, internal, internal),
    FUNCTION 6 skyregion_gist_picksplit(internal, internal),
    FUNCTION 7 skyregion_gist_same(bytea, bytea, internal),
    STORAGE bytea;

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL spike: indexes region-region && directly (bounding-cap key, linear-split picksplit). See ext/src/gist_region.c and bench/22_region_gist.sql before relying on this.';
