\echo Use "ALTER EXTENSION skycell UPDATE" to load this file. \quit

-- ------------------------------------------------------------------
-- The ADQL surface: two types and functions named as the standard names
-- them, so a TAP translator emits the standard's own spelling.
--
--   1 = CONTAINS(POINT('ICRS', s_ra, s_dec), CIRCLE('ICRS', 10, 20, 0.1))
--     -> 1 = contains(point('ICRS', s_ra, s_dec), circle('ICRS', 10, 20, 0.1))
--
-- The forms carrying ADQL's coordinate-system argument never collide with
-- PostgreSQL's built-in point/circle/box/polygon; the two-argument forms do,
-- so put this extension's schema before pg_catalog to use them unqualified:
--   SET search_path = skycell, public, pg_catalog;
-- ------------------------------------------------------------------

CREATE TYPE skypos;

CREATE FUNCTION skypos_in(cstring) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skypos_out(skypos) RETURNS cstring
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE skypos (
    INPUT = skypos_in, OUTPUT = skypos_out,
    INTERNALLENGTH = 16, ALIGNMENT = double, STORAGE = plain
);
COMMENT ON TYPE skypos IS 'a position on the sky: (ra, dec) in degrees, ICRS';

CREATE TYPE skyregion;

CREATE FUNCTION skyregion_in(cstring) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skyregion_out(skyregion) RETURNS cstring
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE skyregion (
    INPUT = skyregion_in, OUTPUT = skyregion_out,
    INTERNALLENGTH = VARIABLE, ALIGNMENT = double, STORAGE = extended
);
COMMENT ON TYPE skyregion IS 'a region on the sky: CIRCLE(ra, dec, radius) or POLYGON(ra1, dec1, ...), degrees, ICRS';

-- constructors ------------------------------------------------------

CREATE FUNCTION point(ra float8, "dec" float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'skycell_point' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION point(coordsys text, ra float8, "dec" float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'skycell_point_cs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION circle(ra float8, "dec" float8, radius float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_circle' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION circle(coordsys text, ra float8, "dec" float8, radius float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_circle_cs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION box(ra float8, "dec" float8, width float8, height float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_box' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION box(coordsys text, ra float8, "dec" float8, width float8, height float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_box_cs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION polygon(VARIADIC coords float8[]) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_polygon' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION polygon(coordsys text, VARIADIC coords float8[]) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_polygon_cs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- PostgreSQL has its own point(float8, float8); the ADQL form with the
-- coordinate system never collides, and these aliases let a deployment that
-- cannot reorder search_path write the two-argument ones unambiguously.
CREATE FUNCTION skycell_point(ra float8, "dec" float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'skycell_point' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_circle(ra float8, "dec" float8, radius float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_circle' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_box(ra float8, "dec" float8, width float8, height float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_box' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_polygon(VARIADIC coords float8[]) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_polygon' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- predicates and measures -------------------------------------------

CREATE FUNCTION contains(p skypos, r skyregion) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_contains' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION contains(a skyregion, b skyregion) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_contains_region' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION intersects(p skypos, r skyregion) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_intersects_pos' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION intersects(a skyregion, b skyregion) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_intersects' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION distance(a skypos, b skypos) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION distance(ra1 float8, dec1 float8, ra2 float8, dec2 float8) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_dist' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION area(r skyregion) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_area' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION coord1(p skypos) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_coord1' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION coord2(p skypos) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_coord2' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION centroid(r skyregion) RETURNS skypos
AS 'MODULE_PATHNAME', 'skycell_centroid' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- the index key of a position, for CREATE INDEX ... (skycell_cell(pos))
CREATE FUNCTION skycell_cell(p skypos) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- operators: the indexable spelling ---------------------------------

CREATE FUNCTION skycell_region_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_region_sel_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- the exact test the rewrite appends (last argument is a selectivity hint)
CREATE FUNCTION skycell_in_region(p skypos, r skyregion, sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_pos_in_region_sel' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_region_sel_support;

CREATE FUNCTION skycell_pos_in_region(p skypos, r skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_region_support;
CREATE FUNCTION skycell_region_has_pos(r skyregion, p skypos) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_region_overlap(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_region_covers(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <@ (
    LEFTARG = skypos, RIGHTARG = skyregion,
    FUNCTION = skycell_pos_in_region, COMMUTATOR = @>,
    RESTRICT = contsel, JOIN = contjoinsel
);
CREATE OPERATOR @> (
    LEFTARG = skyregion, RIGHTARG = skypos,
    FUNCTION = skycell_region_has_pos, COMMUTATOR = <@,
    RESTRICT = contsel, JOIN = contjoinsel
);
CREATE OPERATOR && (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_overlap, COMMUTATOR = &&,
    RESTRICT = areasel, JOIN = areajoinsel
);
CREATE OPERATOR @> (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_covers,
    RESTRICT = contsel, JOIN = contjoinsel
);
CREATE OPERATOR <-> (
    LEFTARG = skypos, RIGHTARG = skypos,
    FUNCTION = distance, COMMUTATOR = <->
);

-- ------------------------------------------------------------------
-- Astronomy: the IVOA UDF registry's names, so ADQL written for other
-- services runs here unchanged.
-- ------------------------------------------------------------------

CREATE FUNCTION ivo_epoch_prop(ra float8, "dec" float8, parallax float8,
                               pmra float8, pmdec float8, radial_velocity float8,
                               ref_epoch float8, out_epoch float8) RETURNS float8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION ivo_epoch_prop(float8, float8, float8, float8, float8, float8, float8, float8)
IS 'six-parameter astrometric solution propagated to out_epoch: {ra, dec, parallax, pmra, pmdec, radial_velocity}';

CREATE FUNCTION ivo_epoch_prop_pos(ra float8, "dec" float8, parallax float8,
                                   pmra float8, pmdec float8, radial_velocity float8,
                                   ref_epoch float8, out_epoch float8) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ivo_epoch_prop_pos(ra float8, "dec" float8, pmra float8, pmdec float8,
                                   ref_epoch float8, out_epoch float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'ivo_epoch_prop_pos_pm' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ivo_apply_pm(ra float8, "dec" float8, pmra float8, pmdec float8,
                             epdiff float8) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- how far a proper motion can carry a source in dt years (degrees)
CREATE FUNCTION skycell_pm_margin(pmra float8, pmdec float8, dt float8) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION icrs2gal(p skypos) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION gal2icrs(p skypos) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION icrs2ecl(p skypos) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ecl2icrs(p skypos) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ivo_healpix_index("order" int, ra float8, "dec" float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ivo_healpix_index("order" int, p skypos) RETURNS int8
AS 'MODULE_PATHNAME', 'ivo_healpix_index_pos' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ivo_healpix_center("order" int, hpxindex int8) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
