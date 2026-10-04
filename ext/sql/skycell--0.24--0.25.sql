\echo Use "ALTER EXTENSION skycell UPDATE TO '0.25'" to load this file. \quit

-- EXPERIMENTAL: a second box opclass for skyregion, skyregion_box4_gist_ops
-- (ext/src/gist_region_box4.c). Round fifty-seven confirmed pgSphere's own
-- scircle/spoly/spoint GiST key ("spherekey") is itself just an axis-
-- aligned 3D Cartesian box -- six float4s in a fixed-length 24-byte type,
-- no varlena header -- the same *shape* of key skyregion_box_gist_ops
-- already uses (six float8s in a bytea). This opclass clones pgSphere's
-- physical key layout exactly (float4, fixed-length, no varlena) while
-- keeping skycell's own unified skyregion type and the same picksplit/
-- consistent() logic as skyregion_box_gist_ops, to isolate whether
-- pgSphere's win is explained by key size/density (this change) or by
-- something else in its C code. See GIST_REGION_DESIGN.md's "Round
-- fifty-eight" for the measured result.
--
-- Not DEFAULT, not a replacement: select explicitly with
-- CREATE INDEX ... USING gist (col skyregion_box4_gist_ops).

CREATE TYPE skyregion_box4;

CREATE FUNCTION skyregion_box4_in(cstring) RETURNS skyregion_box4
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION skyregion_box4_out(skyregion_box4) RETURNS cstring
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE skyregion_box4 (
    INPUT = skyregion_box4_in, OUTPUT = skyregion_box4_out,
    INTERNALLENGTH = 24, ALIGNMENT = int4, STORAGE = plain
);
COMMENT ON TYPE skyregion_box4 IS
  'GiST-internal only: a fixed 24-byte axis-aligned 3D Cartesian box (six float4s, xmin/ymin/zmin/xmax/ymax/zmax), the key type for skyregion_box4_gist_ops. No textual representation -- never meant to be a column type. See ext/src/gist_region_box4.c.';

CREATE FUNCTION skyregion_box4_gist_consistent(internal, internal, int4, oid, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box4_gist_union(internal, internal) RETURNS skyregion_box4
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box4_gist_compress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box4_gist_decompress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box4_gist_penalty(internal, internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box4_gist_picksplit(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box4_gist_same(skyregion_box4, skyregion_box4, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS skyregion_box4_gist_ops
    FOR TYPE skyregion USING gist AS
    OPERATOR 1 && (skyregion, skyregion),
    OPERATOR 2 @> (skyregion, skypos),
    OPERATOR 3 @> (skyregion, skyregion),
    OPERATOR 4 <@ (skyregion, skyregion),
    FUNCTION 1 skyregion_box4_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skyregion_box4_gist_union(internal, internal),
    FUNCTION 3 skyregion_box4_gist_compress(internal),
    FUNCTION 4 skyregion_box4_gist_decompress(internal),
    FUNCTION 5 skyregion_box4_gist_penalty(internal, internal, internal),
    FUNCTION 6 skyregion_box4_gist_picksplit(internal, internal),
    FUNCTION 7 skyregion_box4_gist_same(skyregion_box4, skyregion_box4, internal),
    STORAGE skyregion_box4;

COMMENT ON OPERATOR CLASS skyregion_box4_gist_ops USING gist IS
  'EXPERIMENTAL, non-default: skyregion_box_gist_ops'' own box key (axis-aligned 3D Cartesian, min/max corner), re-keyed to pgSphere''s exact physical layout -- six float4s in a fixed 24-byte type with no varlena header, instead of six float8s in a bytea -- to isolate whether pgSphere''s win over skyregion_box_gist_ops is explained by key size/density or by something else. All bounds are rounded strictly outward from the double-precision exact box (never narrowed), so this remains a sound over-approximation like every other key in this extension. See ext/src/gist_region_box4.c and GIST_REGION_DESIGN.md''s "Round fifty-seven" and "Round fifty-eight" before relying on this for anything beyond experimentation.';
