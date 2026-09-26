-- ------------------------------------------------------------------
-- A real index on skyregion itself: a GiST opclass over && (region-region
-- INTERSECTS) and @> (region contains point), so `a.region && b.region` and
-- `f.region @> point(...)` / `point(...) <@ f.region` all work off an
-- ordinary `CREATE INDEX ON b USING gist (region)` with no side table and no
-- hand-written join -- unlike the MOC-ranges recipe above, which needs both,
-- or the point-in-footprint covering recipe, which needs the covering
-- precomputed per row.
--
-- Tuned, not just a correctness spike: a multi-cap key (up to 4 bounding
-- caps per region, see ext/src/gist_region.c's own file header) and an
-- R*-tree-style picksplit. Benchmarked against the MOC-ranges recipe and
-- pgSphere's native && in bench/22_region_gist.sql, and against the
-- point-in-footprint covering recipe in bench/23_region_contains.sql.
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
    OPERATOR 2 @> (skyregion, skypos),
    FUNCTION 1 skyregion_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skyregion_gist_union(internal, internal),
    FUNCTION 3 skyregion_gist_compress(internal),
    FUNCTION 4 skyregion_gist_decompress(internal),
    FUNCTION 5 skyregion_gist_penalty(internal, internal, internal),
    FUNCTION 6 skyregion_gist_picksplit(internal, internal),
    FUNCTION 7 skyregion_gist_same(bytea, bytea, internal),
    STORAGE bytea;

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL: indexes region-region && and region @> point directly (multi-cap bounding key, R*-tree-style picksplit). See ext/src/gist_region.c, bench/22_region_gist.sql, and bench/23_region_contains.sql before relying on this.';
