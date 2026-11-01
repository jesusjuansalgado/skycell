\echo Use "ALTER EXTENSION skycell UPDATE TO '0.38'" to load this file. \quit

-- 0.38 adds composite regions: a skyregion may be the union, intersection or
-- difference of others, or the sky less one, with REGION() reading their STC-S.
-- Stored regions keep their format; the new kinds only add to it.
--
-- 0.38 also removes the three operator classes deprecated in 0.36:
-- skyregion_box_gist_ops (use the default region GiST,
-- skyregion_box4_gist_ops), skypos_spgist_ops and skypos_cap_gist_ops (use a
-- B-tree on skycell_cell(pos)).  An index built on one has to be dropped
-- first; this script stops, naming it, while one exists.

DO $$
DECLARE
  idx text;
BEGIN
  SELECT string_agg(format('%s (%s)', i.indexrelid::regclass, c.opcname), ', ')
    INTO idx
    FROM pg_index i
    JOIN pg_opclass c ON c.oid = ANY (i.indclass::oid[])
   WHERE c.opcname IN ('skyregion_box_gist_ops', 'skypos_spgist_ops', 'skypos_cap_gist_ops')
     AND c.opcnamespace = (SELECT extnamespace FROM pg_extension WHERE extname = 'skycell');
  IF idx IS NOT NULL THEN
    RAISE EXCEPTION 'skycell 0.38 removes the operator class these indexes use: %', idx
      USING HINT = 'Drop them, then run ALTER EXTENSION skycell UPDATE again. Use the default region GiST for a skyregion column and a B-tree on skycell_cell(pos) for a skypos column.';
  END IF;
END $$;

DROP OPERATOR CLASS skyregion_box_gist_ops USING gist;
DROP OPERATOR FAMILY skyregion_box_gist_ops USING gist;
DROP FUNCTION skyregion_box_gist_consistent(internal, internal, int4, oid, internal);
DROP FUNCTION skyregion_box_gist_union(internal, internal);
DROP FUNCTION skyregion_box_gist_compress(internal);
DROP FUNCTION skyregion_box_gist_decompress(internal);
DROP FUNCTION skyregion_box_gist_penalty(internal, internal, internal);
DROP FUNCTION skyregion_box_gist_picksplit(internal, internal);
DROP FUNCTION skyregion_box_gist_same(bytea, bytea, internal);

DROP OPERATOR CLASS skypos_spgist_ops USING spgist;
DROP OPERATOR FAMILY skypos_spgist_ops USING spgist;
DROP FUNCTION spg_healpix_config(internal, internal);
DROP FUNCTION spg_healpix_choose(internal, internal);
DROP FUNCTION spg_healpix_picksplit(internal, internal);
DROP FUNCTION spg_healpix_inner_consistent(internal, internal);
DROP FUNCTION spg_healpix_leaf_consistent(internal, internal);

DROP OPERATOR CLASS skypos_cap_gist_ops USING gist;
DROP OPERATOR FAMILY skypos_cap_gist_ops USING gist;
DROP FUNCTION skypos_cap_gist_compress(internal);
DROP FUNCTION skypos_cap_gist_decompress(internal);
DROP FUNCTION skypos_cap_gist_union(internal, internal);
DROP FUNCTION skypos_cap_gist_penalty(internal, internal, internal);
DROP FUNCTION skypos_cap_gist_picksplit(internal, internal);
DROP FUNCTION skypos_cap_gist_same(bytea, bytea, internal);
DROP FUNCTION skypos_cap_gist_consistent(internal, internal, int4, oid, internal);

-- ------------------------------------------------------------------
-- composite regions (0.38): STC-S Union, Intersection, Difference, Not
--
-- A skyregion may combine others, nested: "UNION ICRS (CIRCLE 10 20 1
-- POLYGON ...)", "INTERSECTION ICRS (...)", "DIFFERENCE ICRS (a b)" (a less
-- b) and "NOT ICRS (a)" (the sky less a).  A position in one is answered
-- exactly, and through the index: the planner covers the composite as it
-- covers a polygon.  Containment and overlap between regions are answered
-- from the operands where they decide it and by refining cells otherwise;
-- AREA and CENTROID are integrated over cells (exact for a NOT of a cone or
-- polygon).
-- ------------------------------------------------------------------

-- ADQL REGION: a region from its STC-S
CREATE FUNCTION region(stcs text) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_region_stcs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION region(text) IS
  'ADQL REGION: a region from its STC-S, CIRCLE, POLYGON, UNION, INTERSECTION, DIFFERENCE or NOT';

CREATE FUNCTION skycell_union(VARIADIC regions skyregion[]) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_union(skyregion[]) IS 'the union of two regions or more (STC-S Union)';

CREATE FUNCTION skycell_intersection(VARIADIC regions skyregion[]) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_intersection(skyregion[]) IS 'the intersection of two regions or more (STC-S Intersection)';

CREATE FUNCTION skycell_difference(a skyregion, b skyregion) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_difference(skyregion, skyregion) IS 'a less b (STC-S Difference)';

CREATE FUNCTION skycell_not(r skyregion) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_not(skyregion) IS 'the sky less r (STC-S Not)';

-- ADQL's INTERSECTS takes its point either side: INTERSECTS(CIRCLE(...), POINT(...))
CREATE FUNCTION intersects(r skyregion, p skypos) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_intersects_region_pos' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
ALTER FUNCTION intersects(skyregion, skypos) SUPPORT skycell_distance_support;
