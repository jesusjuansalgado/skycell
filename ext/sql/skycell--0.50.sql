\echo Use "CREATE EXTENSION skycell" to load this file. \quit

-- ------------------------------------------------------------------
-- keys and distances
-- ------------------------------------------------------------------

-- order-29 HEALPix NESTED cell of a position (the index key)
CREATE FUNCTION skycell_ang2cell(ra float8, "dec" float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_ang2pix("order" int, ra float8, "dec" float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- angular distance in degrees
CREATE FUNCTION skycell_dist(ra1 float8, dec1 float8, ra2 float8, dec2 float8) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- exact predicates (the "refine" step); last arg is a selectivity hint
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_exact_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION skycell_in_cone(ra float8, "dec" float8, ra0 float8, dec0 float8,
                                radius float8, sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_exact_support;

CREATE FUNCTION skycell_in_poly(ra float8, "dec" float8, poly float8[],
                                sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_exact_support;

-- ------------------------------------------------------------------
-- indexable predicates: rewritten by the planner support function into
-- B-tree range conditions on "cell" + the exact test
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- cell must be skycell_ang2cell(ra, dec)
CREATE FUNCTION skycell_cone(cell int8, ra float8, "dec" float8,
                             ra0 float8, dec0 float8, radius float8) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- poly = ARRAY[ra1, dec1, ra2, dec2, ...] (a simple polygon, degrees)
CREATE FUNCTION skycell_poly(cell int8, ra float8, "dec" float8, poly float8[]) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- run-time range slot i (lo = even, hi = odd) for non-constant cones (joins)
CREATE FUNCTION skycell_cone_bound(ra0 float8, dec0 float8, radius float8, i int,
                                   nslots int, ntotal float8, hist int8[]) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- run-time range slot i (lo = even, hi = odd) for non-constant polygons
-- (joins): the polygon analogue of skycell_cone_bound, keyed on the array's
-- own bytes rather than three scalars.
CREATE FUNCTION skycell_poly_bound(poly float8[], i int, nslots int, ntotal float8,
                                   hist int8[]) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- explicit coverings (LATERAL joins, inspection)
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_cone_ranges(ra0 float8, dec0 float8, radius float8,
                                    tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                    OUT lo int8, OUT hi int8) RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE ROWS 3;

CREATE FUNCTION skycell_poly_ranges(poly float8[], tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                    OUT lo int8, OUT hi int8) RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE ROWS 8;

CREATE FUNCTION skycell_cover_info(ra0 float8, dec0 float8, radius float8,
                                   tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                   OUT nranges int, OUT steps int, OUT deepest int,
                                   OUT exp_rows float8, OUT area_ratio float8,
                                   OUT rho float8, OUT chosen_order int) RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- regions as multi-order coverages (IVOA MOC NUNIQ), for indexing stored
-- footprints in a B-tree: a point's candidate regions are those with a
-- covering cell equal to one of the point's 30 ancestors.
--
-- This is a different problem from the cross-match join above: here it's the
-- *stored regions* being searched (which footprint contains this point?),
-- not a per-row region being matched against an indexed point column, so
-- there's no single outer row to compute one covering from -- the region
-- column itself needs the index.
--
--   CREATE TABLE fp_cells AS
--     SELECT fid, unnest(skycell_region_moc(s_region, 12)) AS nuniq FROM footprints;
--   CREATE INDEX ON fp_cells (nuniq);
--
--   SELECT DISTINCT f.* FROM points p
--   JOIN LATERAL (SELECT DISTINCT fid FROM fp_cells
--                 WHERE nuniq = ANY (skycell_ancestors(p.cell))) m ON true
--   JOIN footprints f ON f.fid = m.fid
--   WHERE skycell_pos_in_region(point(p.ra, p.dec), f.s_region);  -- refine: the MOC overshoots
--
-- Not something a query rewrite can do -- unlike every other recipe in this
-- file, it needs a side table and a join the original query doesn't have, so
-- it has no automatic form (planner support function or ADQL translator
-- alike): build fp_cells once, keep it in sync with footprints, and write the
-- join above by hand. See ext/test/sql/skycell.sql's "stored regions as MOCs
-- in a B-tree" for a complete, tested example.
-- ------------------------------------------------------------------

-- ------------------------------------------------------------------
-- region-region INTERSECTS (INTERSECTS(region, region), skyregion's own &&):
-- neither side is a point, so there is no per-row covering to compute and no
-- point-in-footprint MOC join above to reuse as-is -- skyregion has no GiST
-- opclass, so unlike pgSphere's scircle/spoly this is genuinely unindexed on
-- its own.
--
-- The same MOC decomposition still helps, applied to *both* sides instead of
-- one: skycell_region_moc_ranges() (below, next to skycell_region_moc's own
-- definition) breaks a region into a handful of NUNIQ cells and turns each
-- back into the native [lo,hi] cell-id range the cross-match coverings above
-- already use, as a ready int8range. Because a MOC is a strict hierarchical
-- partition, two regions can only truly overlap if one pair of their cell
-- ranges overlaps as plain integer intervals -- so explode each side into
-- (row_id, int8range), index one side with PostgreSQL's own built-in GiST
-- support for int8range (no custom opclass needed), and join on `&&`:
--
--   CREATE TABLE b_moc AS SELECT bid, rng FROM b, skycell_region_moc_ranges(b.s_region, 8);
--   CREATE INDEX ON b_moc USING gist (rng);
--
--   SELECT DISTINCT a.aid, b.bid FROM a
--   JOIN LATERAL skycell_region_moc_ranges(a.s_region, 8) am ON true
--   JOIN b_moc bm ON bm.rng && am.rng
--   JOIN b ON b.bid = bm.bid  -- refine: the MOC covering overshoots, same as the point case
--   WHERE intersects(a.s_region, b.s_region) = 1;
--
-- Like the point-in-footprint recipe above, this needs a side table and a
-- join no query rewrite can introduce, so it has no automatic form either --
-- build b_moc once, keep it in sync with b, and write the join by hand. It is
-- also an approximation twice over (the MOC covering, then the interval
-- overlap test), so the exact intersects() recheck at the end is required,
-- not optional, exactly as for point-in-footprint. See ext/test/sql/
-- skycell.sql's "region-region INTERSECTS via MOC ranges" for a complete,
-- tested example, and bench/21_region_overlap.sql for how this compares to
-- pgSphere's natively GiST-indexed && on scircle/spoly.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_ancestors(cell int8, min_order int DEFAULT 0, max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_order(nuniq int8) RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_lo(nuniq int8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_hi(nuniq int8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_cone_moc(ra0 float8, dec0 float8, radius float8,
                                 max_cells int DEFAULT 8, max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_poly_moc(poly float8[], max_cells int DEFAULT 8,
                                 max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;


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
CREATE FUNCTION skycell_region_has_pos_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_region_sel_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- the exact test the rewrite appends (last argument is a selectivity hint)
CREATE FUNCTION skycell_in_region(p skypos, r skyregion, sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_pos_in_region_sel' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 25 SUPPORT skycell_region_sel_support;

CREATE FUNCTION skycell_pos_in_region(p skypos, r skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 25 SUPPORT skycell_region_support;

-- run-time range slot i (lo = even, hi = odd) for a non-constant skyregion
-- column (joins): the generic-region analogue of skycell_cone_bound/
-- skycell_poly_bound, for <@ against a per-row region of either kind (a
-- cross-match whose CIRCLE-or-POLYGON comes from another table's row, e.g.
-- an archive's per-row s_region footprint).
CREATE FUNCTION skycell_region_bound(region skyregion, i int, nslots int, ntotal float8,
                                     hist int8[]) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_region_has_pos(r skyregion, p skypos) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 25 SUPPORT skycell_region_has_pos_support;
CREATE FUNCTION skycell_region_overlap(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_region_covers(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_region_covered_by(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Restriction selectivity for the raw, un-rewritten <@(skypos,skyregion)
-- and its commutator @>(skyregion,skypos) -- as a GiST index on a
-- skyregion column serves them, or a GiST or SP-GiST index on skypos did
-- (both removed in 0.38), with no B-tree rewrite in the picture to
-- hand off to skycell_in_region/skycell_region_sel_support above. A
-- function's own SUPPORT clause cannot fix this selectivity for an
-- operator-backed function (PostgreSQL's own documented rule: the support
-- function is never consulted for selectivity when that function backs an
-- operator), so this is the classic oprrest-shaped mechanism instead,
-- replacing contsel's flat, radius-blind default with the same
-- area(region)/4pi "uniform sky" estimate skycell_region_sel_support
-- already uses for the post-rewrite exact test. See
-- GIST_REGION_DESIGN.md's "Round forty-three" for the measured effect
-- (a real, structurally-explained bug: both this operator and skycell's
-- own SP-GiST opclass were costed from a flat ~0.1%-of-table estimate
-- regardless of actual query radius, which skews the planner away from a
-- plain Index Scan toward an unneeded Bitmap Scan at small radii).
CREATE FUNCTION skycell_pos_region_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;
CREATE FUNCTION skycell_region_pos_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;

-- The same bug, on the region-region operators: &&/@>/<@ between two
-- skyregion values were declared RESTRICT = areasel/contsel, PostgreSQL's
-- generic defaults, which cost the query region's own size out of the
-- estimate entirely. Same fix, same area(region)/4pi uniform-sky estimate
-- as skycell_pos_region_sel/skycell_region_pos_sel above, generalised to
-- whichever operand is a compile-time constant and the question that
-- ratio actually answers -- see ext/src/adql.c's region_area_sel() for
-- why @>/<@ only use it when the *container* side is the constant one,
-- and GIST_REGION_DESIGN.md for the region-size-statistics follow-up this
-- does not attempt (no analogue yet to skycell_pos_region_sel's own
-- density-aware second round).
CREATE FUNCTION skycell_region_overlap_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;
CREATE FUNCTION skycell_region_covers_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;
CREATE FUNCTION skycell_region_covered_by_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;

CREATE OPERATOR <@ (
    LEFTARG = skypos, RIGHTARG = skyregion,
    FUNCTION = skycell_pos_in_region, COMMUTATOR = @>,
    RESTRICT = skycell_pos_region_sel, JOIN = contjoinsel
);
CREATE OPERATOR @> (
    LEFTARG = skyregion, RIGHTARG = skypos,
    FUNCTION = skycell_region_has_pos, COMMUTATOR = <@,
    RESTRICT = skycell_region_pos_sel, JOIN = contjoinsel
);
CREATE OPERATOR && (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_overlap, COMMUTATOR = &&,
    RESTRICT = skycell_region_overlap_sel, JOIN = areajoinsel
);
CREATE OPERATOR @> (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_covers, COMMUTATOR = <@,
    RESTRICT = skycell_region_covers_sel, JOIN = contjoinsel
);
CREATE OPERATOR <@ (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_covered_by, COMMUTATOR = @>,
    RESTRICT = skycell_region_covered_by_sel, JOIN = contjoinsel
);
CREATE OPERATOR <-> (
    LEFTARG = skypos, RIGHTARG = skypos,
    FUNCTION = distance, COMMUTATOR = <->
);

-- ------------------------------------------------------------------
-- Astronomy: the IVOA UDF registry's names, so ADQL written for other
-- services runs here unchanged.
-- ------------------------------------------------------------------

-- Not STRICT: a NULL parallax, proper motion or radial velocity enters as
-- 0, a NULL position or epoch is an error (the IVOA catalogue of ADQL
-- user-defined functions, ivo_epoch_prop_pos).
CREATE FUNCTION ivo_epoch_prop(ra float8, "dec" float8, parallax float8,
                               pmra float8, pmdec float8, radial_velocity float8,
                               ref_epoch float8, out_epoch float8) RETURNS float8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE PARALLEL SAFE;
COMMENT ON FUNCTION ivo_epoch_prop(float8, float8, float8, float8, float8, float8, float8, float8)
IS 'six-parameter astrometric solution propagated to out_epoch: {ra, dec, parallax, pmra, pmdec, radial_velocity}';

CREATE FUNCTION ivo_epoch_prop_pos(ra float8, "dec" float8, parallax float8,
                                   pmra float8, pmdec float8, radial_velocity float8,
                                   ref_epoch float8, out_epoch float8) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION ivo_epoch_prop_pos(ra float8, "dec" float8, pmra float8, pmdec float8,
                                   ref_epoch float8, out_epoch float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'ivo_epoch_prop_pos_pm' LANGUAGE C IMMUTABLE PARALLEL SAFE;
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

-- the four corners of a cell: ra, dec, ra, dec, ... (for plotting a covering)
CREATE FUNCTION skycell_cell_corners("order" int, pix int8) RETURNS float8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- the price of one index range, in rows, that the cost model derives for a
-- relation (skycell.range_cost = -1, the default); see auto_range_cost()
CREATE FUNCTION skycell_range_cost(tbl regclass DEFAULT NULL, col name DEFAULT 'cell')
RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_range_cost_for' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- multi-order count map
--
-- ANALYZE's histogram cannot see a cluster smaller than one of its buckets,
-- and because it is equi-depth, raising the statistics target does not help:
-- a denser region gets narrower buckets, so interpolating inside one is scale
-- invariant.  This map counts the rows in each of a set of disjoint cells,
-- split until a cell holds few enough rows for interpolation inside it to be
-- harmless.  The covering reads it in preference to the histogram.
-- ------------------------------------------------------------------

CREATE TABLE skycell_density_map (
  statrel  oid    NOT NULL,
  attnum   int2   NOT NULL,
  nuniq    int8   NOT NULL,
  n        int8   NOT NULL,
  PRIMARY KEY (statrel, attnum, nuniq)
);
SELECT pg_catalog.pg_extension_config_dump('skycell_density_map', '');

CREATE FUNCTION skycell_density_build(tbl regclass, col name DEFAULT 'cell',
                                      rows_per_cell int DEFAULT 100,
                                      max_order int DEFAULT 13)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
  srel oid; att int2; k int; nleaf bigint; expr text; idx oid;
BEGIN
  IF rows_per_cell < 1 OR max_order < 1 OR max_order > 20 THEN
    RAISE EXCEPTION 'skycell: rows_per_cell must be >= 1 and max_order within [1, 20]';
  END IF;

  -- statistics live on the table for a plain column, on the index for an
  -- expression index: match what the planner support function looks up.
  SELECT a.attnum, tbl INTO att, srel
  FROM pg_attribute a WHERE a.attrelid = tbl AND a.attname = col AND NOT a.attisdropped;
  IF att IS NULL THEN
    SELECT i.indexrelid INTO idx FROM pg_index i
    WHERE i.indrelid = tbl AND pg_get_indexdef(i.indexrelid) LIKE '%skycell_ang2cell%'
    ORDER BY i.indexrelid LIMIT 1;
    IF idx IS NULL THEN
      RAISE EXCEPTION 'skycell: no column % and no skycell_ang2cell index on %', col, tbl;
    END IF;
    srel := idx; att := 1;
    expr := (SELECT pg_get_expr(i.indexprs, i.indrelid) FROM pg_index i WHERE i.indexrelid = idx);
  ELSE
    expr := quote_ident(col);
  END IF;

  DELETE FROM skycell_density_map m WHERE m.statrel = srel AND m.attnum = att;

  /*
   * Count at the finest order allowed, then roll up one order at a time: a
   * cell is a leaf when it holds at most rows_per_cell and its parent does
   * not, plus the cells at the finest order that are still too full to split
   * further.  A recursive CTE cannot do this (no aggregates in the recursive
   * term), so it is a loop over the orders.
   */
  CREATE TEMP TABLE sc_lev (ord int, pix int8, n int8) ON COMMIT DROP;
  EXECUTE format(
    'INSERT INTO sc_lev SELECT %s, (%s) >> (2 * (29 - %s)), count(*) FROM %s '
    'WHERE (%s) IS NOT NULL GROUP BY 2',
    max_order, expr, max_order, tbl::text, expr);

  FOR k IN REVERSE max_order .. 1 LOOP
    INSERT INTO sc_lev SELECT k - 1, pix >> 2, sum(n) FROM sc_lev WHERE ord = k GROUP BY 2;
    EXIT WHEN NOT FOUND;
  END LOOP;
  CREATE INDEX ON sc_lev (ord, pix);

  INSERT INTO skycell_density_map (statrel, attnum, nuniq, n)
  SELECT srel, att, (4::int8 << (2 * u.ord)) + u.pix, u.n
  FROM sc_lev u
  LEFT JOIN sc_lev p ON p.ord = u.ord - 1 AND p.pix = u.pix >> 2
  WHERE (u.n <= rows_per_cell AND (p.n IS NULL OR p.n > rows_per_cell))
     OR (u.ord = max_order AND u.n > rows_per_cell);

  GET DIAGNOSTICS nleaf = ROW_COUNT;
  DROP TABLE sc_lev;
  RETURN nleaf;
END $$;

CREATE FUNCTION skycell_density_drop(tbl regclass, col name DEFAULT 'cell') RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE n bigint; idx oid;
BEGIN
  DELETE FROM skycell_density_map m
  WHERE m.statrel = tbl OR m.statrel IN (SELECT indexrelid FROM pg_index WHERE indrelid = tbl);
  GET DIAGNOSTICS n = ROW_COUNT;
  RETURN n;
END $$;

-- ------------------------------------------------------------------
-- Q3C-shaped spellings
-- ------------------------------------------------------------------
-- Same predicate as skycell_cone, without the explicit index expression:
-- the planner support function synthesises skycell_ang2cell(ra, dec) from the
-- first two arguments, so migrating from Q3C is a prefix replacement.
--   q3c_join(a.ra, a.dec, b.ra, b.dec, r)  ->  skycell_join(...)
--   q3c_radial_query(ra, dec, ra0, dec0, r) -> skycell_radial_query(...)
-- Both need an index on skycell_ang2cell(ra, dec) to be rewritten to ranges,
-- exactly as q3c needs one on q3c_ang2ipix(ra, dec); without it the plan is a
-- correct sequential scan.

CREATE FUNCTION skycell_join(ra1 float8, dec1 float8, ra2 float8, dec2 float8,
                             radius float8) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_cone'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

CREATE FUNCTION skycell_radial_query(ra float8, "dec" float8, ra0 float8, dec0 float8,
                                     radius float8) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_cone'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- The polygon analogue of skycell_join/skycell_radial_query, for a
-- cross-match whose polygon comes from another table's row (a q3c_poly_query
-- has no such counterpart -- Q3C has no region type -- so this has no
-- existing spelling to match; ra/dec/poly is skycell_poly's own order,
-- without the leading cell argument).
CREATE FUNCTION skycell_poly_join(ra float8, "dec" float8, poly float8[]) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_poly'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- ------------------------------------------------------------------
-- Cell-range selectivity: a b-tree operator class for wide relations
-- ------------------------------------------------------------------
-- A covering becomes `cell >= lo AND cell <= hi`.  With lo and hi constant the
-- stock estimator reads the histogram and does well.  In a cross-match they are
-- not constant -- they come from the probe row -- and PostgreSQL has no
-- estimator for that, so each side gets DEFAULT_INEQ_SEL and the pair estimates
-- a ninth of the relation per probe.  On a 20-million-row ObsCore relation that
-- is 2.3 million rows where the truth is about one; the parameterised index
-- path is costed out of existence and a sequential scan is chosen instead.
-- Measured: >120 s for the plan the planner picks, 50 ms for the one it rejects.
--
-- Selectivity for an operator comes from pg_operator.oprrest, and int8's cannot
-- be changed, so the fix is our own operators carrying our own estimator, in
-- our own b-tree operator class.  This is opt-in: build the index with
-- skycell_cell_ops and write the range join with #>= and #<=.  Indexes built
-- the ordinary way keep working exactly as before.
--
--   CREATE INDEX t_cell ON t (skycell_ang2cell(ra, dec) skycell_cell_ops);
--
--   SELECT ... FROM probes p
--   CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, r) g
--   JOIN t ON skycell_ang2cell(t.ra, t.dec) #>= g.lo
--         AND skycell_ang2cell(t.ra, t.dec) #<= g.hi
--   WHERE skycell_in_cone(t.ra, t.dec, p.ra, p.dec, r);

CREATE FUNCTION skycell_cellsel(internal, oid, internal, integer) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE OPERATOR #< (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8lt,
  RESTRICT = skycell_cellsel, JOIN = scalarltjoinsel, COMMUTATOR = #>);
CREATE OPERATOR #> (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8gt,
  RESTRICT = skycell_cellsel, JOIN = scalargtjoinsel, COMMUTATOR = #<);
CREATE OPERATOR #<= (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8le,
  RESTRICT = skycell_cellsel, JOIN = scalarlejoinsel, COMMUTATOR = #>=);
CREATE OPERATOR #>= (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8ge,
  RESTRICT = skycell_cellsel, JOIN = scalargejoinsel, COMMUTATOR = #<=);
CREATE OPERATOR #= (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8eq,
  RESTRICT = eqsel, JOIN = eqjoinsel, COMMUTATOR = #=, HASHES, MERGES);

CREATE OPERATOR CLASS skycell_cell_ops FOR TYPE int8 USING btree AS
  OPERATOR 1 #<, OPERATOR 2 #<=, OPERATOR 3 #=, OPERATOR 4 #>=, OPERATOR 5 #>,
  FUNCTION 1 btint8cmp(int8, int8);
COMMENT ON OPERATOR CLASS skycell_cell_ops USING btree IS
  'Optional B-tree operator class for the int8 cell. Its operators (#<, #<=, #=, #>=, #>) estimate a range condition from the cell histogram, so the planner can cost a hand-written range join against skycell_cone_ranges(); write the join with #>= and #<=.';

-- ------------------------------------------------------------------
-- A single covering function for a skyregion value, whichever kind it
-- holds -- skyregion is one type for both circles and polygons (its own
-- "kind" tag), so one column can mix them; skycell_cone_moc/
-- skycell_poly_moc take raw arguments and require the caller to already
-- know which one a given row is, this dispatches on the stored value
-- itself the same way contains()/intersects()/area() already do.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_region_moc(region skyregion, max_cells int DEFAULT 8,
                                   max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- Convenience wrapper for the region-region INTERSECTS recipe above (see the
-- worked example next to skycell_region_moc's own comment block, near the top
-- of this file): turns a region straight into its MOC cells' native [lo,hi]
-- cell-id ranges as int8range values, ready for PostgreSQL's own built-in
-- GiST opclass -- skips the unnest(skycell_region_moc(...)) +
-- skycell_nuniq_lo/skycell_nuniq_hi + int8range(...) boilerplate every use of
-- that recipe would otherwise repeat, on both the side-table build and the
-- per-query side.
CREATE FUNCTION skycell_region_moc_ranges(region skyregion, max_cells int DEFAULT 8,
                                          max_order int DEFAULT 29,
                                          OUT nuniq int8, OUT rng int8range) RETURNS SETOF record
AS $$ SELECT n, int8range(skycell_nuniq_lo(n), skycell_nuniq_hi(n), '[]')
      FROM unnest(skycell_region_moc(region, max_cells, max_order)) AS n $$
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE ROWS 8;

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
--
-- NOT DEFAULT as of 0.21 -- skyregion_box_gist_ops (removed in 0.38) took
-- that over in that round (GIST_REGION_DESIGN.md's "Round fifty-three",
-- corrected by "Round fifty-four" below). Two genuinely separate effects
-- decide which opclass wins, not one radius number:
--
--   1. On a column holding one roughly uniform scale of region (the
--      realistic case for most catalogue footprint columns), the box
--      opclass wins decisively from sub-degree radii up through roughly
--      5-8 degrees -- a real crossover, re-measured directly, but much
--      higher than "Round fifty-three"'s own first estimate (~0.3-1
--      degree), which turned out to conflate this effect with the next
--      one. Past that crossover this opclass wins back, and keeps
--      winning at any larger uniform scale tried (confirmed to 60
--      degrees).
--   2. A column that mixes very different scales of region in the *same*
--      index (a few huge footprints alongside many small ones) shifts
--      the balance toward this opclass even for a small probe radius
--      that would otherwise favour the box opclass in isolation -- an
--      extreme outlier region distorts the box key's own bounding boxes
--      for everything that shares its subtree, in a way this opclass's
--      per-sub-cap structure tolerates better. Confirmed directly:
--      identical small-radius probes against an isolated small-only
--      corpus favoured the box opclass by ~1.6x; the same probes against
--      a corpus mixing that small population with a huge-radius one,
--      sharing one index, favoured this opclass by ~1.5x instead.
--
-- Select this opclass explicitly for a column you know holds wide-area
-- or very large regions, or a strongly mixed-scale population:
-- `CREATE INDEX ON t USING gist (region skyregion_gist_ops)`.
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
    FOR TYPE skyregion USING gist AS
    OPERATOR 1 && (skyregion, skyregion),
    OPERATOR 2 @> (skyregion, skypos),
    OPERATOR 3 @> (skyregion, skyregion),
    OPERATOR 4 <@ (skyregion, skyregion),
    FUNCTION 1 skyregion_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skyregion_gist_union(internal, internal),
    FUNCTION 3 skyregion_gist_compress(internal),
    FUNCTION 4 skyregion_gist_decompress(internal),
    FUNCTION 5 skyregion_gist_penalty(internal, internal, internal),
    FUNCTION 6 skyregion_gist_picksplit(internal, internal),
    FUNCTION 7 skyregion_gist_same(bytea, bytea, internal),
    STORAGE bytea;

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'GiST operator class for skyregion keyed by a few spherical caps per entry, exact for a circle at any radius. Indexes &&, @> and <@ between regions, and region @> point. Not the default: choose it for a column mixing very different region scales (survey tiles alongside catalogue footprints, say), where the default box key is loose around large circles.';







-- A second box opclass for skyregion, skyregion_box4_gist_ops (ext/src/
-- gist_region_box4.c) -- DEFAULT as of 0.26. Round fifty-seven confirmed
-- pgSphere's own scircle/spoly/spoint GiST key ("spherekey") is itself
-- just an axis-aligned 3D Cartesian box -- six float4s in a fixed-length
-- 24-byte type, no varlena header -- the same *shape* of key
-- skyregion_box_gist_ops already uses (six float8s in a bytea). This
-- opclass clones pgSphere's physical key layout exactly (float4, fixed-
-- length, no varlena) while keeping skycell's own unified skyregion type
-- and the same picksplit/consistent() logic as skyregion_box_gist_ops,
-- to isolate whether pgSphere's win is explained by key size/density
-- (this change) or by something else in its C code. Round fifty-eight
-- found it is: the key-width change alone recovers and reverses the
-- gap. Rounds fifty-nine and sixty confirmed the win holds on a mixed
-- circle/polygon corpus and on all four strategies on the real 50,000-
-- row fpr corpus. Round sixty-one then ran the same promotion scrutiny
-- box8 itself had (pole proximity, large radii, a mixed-scale column)
-- and found no regime where this opclass was worse than box8 -- hence
-- the promotion to DEFAULT here, with the same "skyregion_gist_ops
-- wins for a mixed-scale column" caveat box8 always carried, confirmed
-- directly for this opclass too. See GIST_REGION_DESIGN.md's "Round
-- fifty-seven" through "Round sixty-one" for the full history.
--
-- skyregion_box_gist_ops was removed in 0.38. skyregion_
-- gist_ops remains available, explicitly, for a column you know mixes
-- very different region scales:
-- CREATE INDEX ... USING gist (col skyregion_gist_ops).

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
  'Internal key type of skyregion_box4_gist_ops: an axis-aligned 3D box of six float4s, rounded outward. Not meant to be used as a column type.';

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
    DEFAULT FOR TYPE skyregion USING gist AS
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
  'Default GiST operator class for skyregion: an axis-aligned 3D box per entry, six float4s rounded outward so that the key always contains the region. Indexes &&, @> and <@ between regions, and region @> point. For a column mixing very different region scales, skyregion_gist_ops can be tighter.';

-- distance(p1, p2) < r, ADQL's other spelling of a cone or a cross-match,
-- is planned as the cone it stands for (ext/src/adql.c,
-- skycell_index_distances): the planner adds `p1 <@ circle(p2, r')` next to
-- the comparison, which keeps deciding. That happens in a planner hook, so
-- the library has to be loaded before the query's tables are looked at,
-- and a comparison of a float8 calls nothing of skycell's while it is
-- planned. This support function does nothing but get called -- loading
-- the library -- when the planner simplifies the distance() call.
CREATE FUNCTION skycell_distance_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
ALTER FUNCTION distance(skypos, skypos) SUPPORT skycell_distance_support;
ALTER FUNCTION distance(float8, float8, float8, float8) SUPPORT skycell_distance_support;
ALTER FUNCTION skycell_dist(float8, float8, float8, float8) SUPPORT skycell_distance_support;

-- A distance comparison the planner has given a cone (0.27) is estimated
-- twice: once by the cone, with skycell's own estimate, and once more as an
-- inequality on an expression, which PostgreSQL guesses at 1/3. So once the
-- cone is the custom scan's, the comparison becomes
-- skycell_distance_below(d, r, strict) -- d < r, or d <= r -- the same test
-- with the float type's ordering, whose support function estimates it as
-- always true. Planner-internal: written by hand it would be estimated
-- as always true too.
CREATE FUNCTION skycell_distance_below_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_distance_below(d float8, r float8, strict bool) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_distance_below_support;

-- ADQL's CONTAINS and INTERSECTS return an integer, so a query compares
-- them with 1: `contains(p, r) = 1`. Each runs exactly an operator's test,
-- so the planner now replaces that comparison with the operator itself
-- (ext/src/adql.c, adql_predicate_operator), which is indexed:
-- contains/intersects(skypos, skyregion) and contains(skyregion,
-- skyregion) become <@, intersects(skyregion, skyregion) becomes &&.
-- As for distance() in 0.27, the support function does nothing but load
-- the library while the call is planned, so the planner hook is in place
-- for a session's first query.
ALTER FUNCTION contains(skypos, skyregion) SUPPORT skycell_distance_support;
ALTER FUNCTION contains(skyregion, skyregion) SUPPORT skycell_distance_support;
ALTER FUNCTION intersects(skypos, skyregion) SUPPORT skycell_distance_support;
ALTER FUNCTION intersects(skyregion, skyregion) SUPPORT skycell_distance_support;

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

-- ------------------------------------------------------------------
-- 0.39: a constant composite region through the custom scan
--
-- A composite (UNION, INTERSECTION, DIFFERENCE, NOT) written as a top-level
-- term, CONTAINS(POINT, REGION('...')) = 1 or pos <@ region, is planned as
-- skycell_region_scan(cell, ra, dec, region): the custom scan reads its
-- covering's ranges in cell order, as for a cone, instead of an OR of range
-- conditions planned as a BitmapOr.  On its own the function is the exact
-- test.  Anywhere else (under an OR, in a target list) the composite keeps
-- the range rewrite.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_region_scan_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION skycell_region_scan(cell int8, ra float8, "dec" float8, r skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_region_scan_support;
COMMENT ON FUNCTION skycell_region_scan(int8, float8, float8, skyregion) IS
  'Planner-internal: a position (ra, dec, with its index cell) in a constant composite region, answered by the custom scan. Write CONTAINS(POINT(...), REGION(...)) = 1 or pos <@ region instead.';

-- ------------------------------------------------------------------
-- 0.40: partition pruning by a cone's cells
--
-- skycell_cone_cell_lo/hi(ra0, dec0, radius): the lowest and highest
-- order-29 cell a cone can hold.  Next to a cone on a catalogue partitioned
-- by cell range (by skycell_ang2cell(ra, dec), a stored cell column, or a
-- source_id that encodes the cell), the planner adds cell >= lo and
-- cell <= hi, from which PostgreSQL prunes the partitions the cone cannot
-- reach: at plan time for a constant cone, and at every rescan for a
-- cross-match's.  The custom scan leaves the two comparisons out, as the
-- cone implies them.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_cone_cell_lo(ra0 float8, dec0 float8, radius float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_cone_cell_lo(float8, float8, float8) IS
  'The lowest order-29 cell the cone (ra0, dec0, radius in degrees) can hold; with skycell_cone_cell_hi, an interval every cell of the cone lies in. The planner adds these next to a cone on a catalogue partitioned by cell range, for partition pruning.';

CREATE FUNCTION skycell_cone_cell_hi(ra0 float8, dec0 float8, radius float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_cone_cell_hi(float8, float8, float8) IS
  'The highest order-29 cell the cone (ra0, dec0, radius in degrees) can hold; see skycell_cone_cell_lo.';

-- ------------------------------------------------------------------
-- 0.41: estimates of the number of sources in a region
--
-- The rows a table's statistics expect in a region or a HEALPix cell, read
-- from the density model the planner uses (the ANALYZE histogram of the
-- table's cell index), without reading a row: for a zoomed-out sky view,
-- the count of sources in it and a coarse density map, in microseconds.  As
-- current as the last ANALYZE.  Scalar, so that ADQL can call them, on a
-- TAP service that declares them; the table is named by a string.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_count_estimate(tbl text, r skyregion) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_count_estimate(text, skyregion) IS
  'The rows the statistics of table tbl (its skycell cell index; a partitioned table''s partitions) expect in region r, as the planner estimates them; no row is read. ADQL: skycell_count_estimate(''schema.table'', CIRCLE(''ICRS'', ra, dec, radius)).';

CREATE FUNCTION skycell_count_estimate(tbl text, hpx_order integer, ipix bigint) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_count_estimate_cell' LANGUAGE C STABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_count_estimate(text, integer, bigint) IS
  'The rows the statistics of table tbl expect in the NESTED HEALPix cell ipix of order hpx_order (a HiPS tile); no row is read.';

CREATE FUNCTION skycell_density_estimate(tbl text, r skyregion, hpx_order integer,
                                         OUT ipix bigint, OUT estimate float8, OUT inside boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 100;
COMMENT ON FUNCTION skycell_density_estimate(text, skyregion, integer) IS
  'The HEALPix cells of order hpx_order (0-20) that region r meets, with the rows the statistics of table tbl expect in each and whether it lies wholly inside r; at most 200,000 cells. For SQL; ADQL calls skycell_count_estimate per cell.';

-- ------------------------------------------------------------------
-- 0.42: materialised multi-order density maps
--
-- skycell_density_view(tbl, target, max_order, min_order) makes target a
-- materialised view of the number of rows in every occupied HEALPix cell of
-- the table, at each order from min_order to max_order (HiPS orders), with
-- a unique index on (hpx_order, ipix).  REFRESH MATERIALIZED VIEW
-- [CONCURRENTLY] target rebuilds it, after loading rows.  ADQL reads it as a
-- table, a view's cells through skycell_cell_overlaps(order, ipix, region),
-- which the planner turns into ranges of ipix on that index.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_cell_overlaps(hpx_order integer, ipix bigint, r skyregion) RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_cell_overlaps(integer, bigint, skyregion) IS
  '1 if the NESTED HEALPix cell ipix of order hpx_order meets region r, else 0 (conservative: a cell the region misses by a hair may count). ADQL: 1 = skycell_cell_overlaps(hpx_order, ipix, CIRCLE(...)), indexed on (hpx_order, ipix) for a constant order and region.';

CREATE FUNCTION skycell_cell_meets_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION skycell_cell_meets(hpx_order integer, ipix bigint, r skyregion) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_cell_meets_support;
COMMENT ON FUNCTION skycell_cell_meets(integer, bigint, skyregion) IS
  'Planner-internal: skycell_cell_overlaps(...) = 1 as a boolean, which the planner rewrites into ranges of ipix for a constant order and region.';

CREATE FUNCTION skycell_density_view(tbl text, target text, max_order integer DEFAULT 8,
                                      min_order integer DEFAULT 0) RETURNS bigint
LANGUAGE plpgsql AS $fn$
DECLARE
  rel regclass := tbl::regclass;
  cellexpr text;
  parts text[] := parse_ident(target);
  qname text;
  iname text;
  nrows bigint;
  q text;
BEGIN
  IF min_order < 0 OR max_order > 20 OR min_order > max_order THEN
    RAISE EXCEPTION 'skycell: orders must satisfy 0 <= min_order <= max_order <= 20';
  END IF;
  -- the table's cell expression: its skycell index's (on the table, or on one of
  -- its partitions), else a stored cell column
  SELECT pg_get_expr(i.indexprs, i.indrelid) INTO cellexpr
  FROM pg_index i
  WHERE i.indrelid IN (SELECT rel UNION ALL SELECT inhrelid FROM pg_inherits WHERE inhparent = rel)
    AND i.indkey[0] = 0
    AND pg_get_expr(i.indexprs, i.indrelid) ~ '^([a-z_]+\.)?skycell_(ang2cell|cell)\('
  ORDER BY i.indrelid = rel DESC
  LIMIT 1;
  IF cellexpr IS NULL THEN
    IF EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = rel AND attname = 'cell' AND NOT attisdropped) THEN
      cellexpr := 'cell';
    ELSE
      RAISE EXCEPTION 'skycell: % has no cell index to count from', rel
        USING HINT = 'Index skycell_ang2cell(ra, dec) (or a stored cell column).';
    END IF;
  END IF;
  qname := array_to_string(ARRAY(SELECT quote_ident(p) FROM unnest(parts) p), '.');
  iname := quote_ident(parts[array_length(parts, 1)] || '_cell');
  EXECUTE format('DROP MATERIALIZED VIEW IF EXISTS %s', qname);
  -- the finest order counted from the table, each coarser one from the order
  -- below it (a quarter of its rows): one pass over the table
  q := format('f%s AS MATERIALIZED (SELECT (%s) >> %s AS ipix, count(*)::bigint AS n FROM %s WHERE (%s) IS NOT NULL GROUP BY 1)',
              max_order, cellexpr, 2 * (29 - max_order), rel, cellexpr);
  FOR o IN REVERSE max_order - 1 .. min_order LOOP
    q := q || format(', f%s AS MATERIALIZED (SELECT ipix >> 2 AS ipix, sum(n)::bigint AS n FROM f%s GROUP BY 1)', o, o + 1);
  END LOOP;
  EXECUTE format('CREATE MATERIALIZED VIEW %s AS WITH %s %s', qname, q,
                 (SELECT string_agg(format('SELECT %s AS hpx_order, ipix, n FROM f%s', o, o), ' UNION ALL ' ORDER BY o)
                  FROM generate_series(min_order, max_order) o));
  EXECUTE format('CREATE UNIQUE INDEX %s ON %s (hpx_order, ipix)', iname, qname);
  EXECUTE format('ANALYZE %s', qname);
  EXECUTE format($c$COMMENT ON MATERIALIZED VIEW %s IS %L$c$, qname,
    format('skycell density map of %s: rows per NESTED HEALPix cell, orders %s-%s. REFRESH MATERIALIZED VIEW [CONCURRENTLY] to rebuild it.',
           rel, min_order, max_order));
  EXECUTE format('SELECT count(*) FROM %s', qname) INTO nrows;
  RETURN nrows;
END
$fn$;
COMMENT ON FUNCTION skycell_density_view(text, text, integer, integer) IS
  'Make target a materialised view of the rows of table tbl per occupied NESTED HEALPix cell, orders min_order to max_order (columns hpx_order, ipix, n), indexed on (hpx_order, ipix); returns its row count. REFRESH MATERIALIZED VIEW [CONCURRENTLY] rebuilds it.';

-- ------------------------------------------------------------------
-- 0.43: MOC export, IVOA MOC 2.0 ASCII and JSON
--
-- The MOC of a region, of a table's coverage, or of the cells of a query, as
-- text that Aladin, the CDS MOC Server or MOCPy read: normalised (four
-- siblings are their parent, runs written a-b), orders ascending, an empty
-- max order written last to declare the depth.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_moc(r skyregion, max_order integer DEFAULT 10, fmt text DEFAULT 'ascii')
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc(skyregion, integer, text) IS
  'The MOC of region r at max_order (0-29): the cells it meets, as skycell_cell_overlaps counts them, those inside it whole at coarser orders; fmt ''ascii'' (IVOA MOC 2.0) or ''json''.';

CREATE FUNCTION skycell_moc_agg_trans(internal, integer, bigint) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT;
CREATE FUNCTION skycell_moc_agg_trans(internal, integer, bigint, text) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT;
CREATE FUNCTION skycell_moc_agg_final(internal) RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE AGGREGATE skycell_moc_agg(hpx_order integer, ipix bigint) (
  SFUNC = skycell_moc_agg_trans, STYPE = internal, FINALFUNC = skycell_moc_agg_final);
CREATE AGGREGATE skycell_moc_agg(hpx_order integer, ipix bigint, fmt text) (
  SFUNC = skycell_moc_agg_trans, STYPE = internal, FINALFUNC = skycell_moc_agg_final);
COMMENT ON AGGREGATE skycell_moc_agg(integer, bigint) IS
  'The MOC (IVOA MOC 2.0 ASCII) of the NESTED HEALPix cells ipix of one order hpx_order (0-12) over the rows aggregated, duplicates allowed: a coverage MOC. NULL over no rows.';
COMMENT ON AGGREGATE skycell_moc_agg(integer, bigint, text) IS
  'skycell_moc_agg(hpx_order, ipix) with fmt ''ascii'' or ''json''.';

CREATE FUNCTION skycell_coverage_moc(tbl text, max_order integer DEFAULT 10, fmt text DEFAULT 'ascii')
RETURNS text LANGUAGE plpgsql STABLE AS $fn$
DECLARE
  rel regclass := tbl::regclass;
  cellexpr text;
  res text;
BEGIN
  IF max_order < 0 OR max_order > 12 THEN
    RAISE EXCEPTION 'skycell: a coverage MOC''s order must be within [0, 12]';
  END IF;
  -- the table's cell expression, as skycell_density_view finds it
  SELECT pg_get_expr(i.indexprs, i.indrelid) INTO cellexpr
  FROM pg_index i
  WHERE i.indrelid IN (SELECT rel UNION ALL SELECT inhrelid FROM pg_inherits WHERE inhparent = rel)
    AND i.indkey[0] = 0
    AND pg_get_expr(i.indexprs, i.indrelid) ~ '^([a-z_]+\.)?skycell_(ang2cell|cell)\('
  ORDER BY i.indrelid = rel DESC
  LIMIT 1;
  IF cellexpr IS NULL THEN
    IF EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = rel AND attname = 'cell' AND NOT attisdropped) THEN
      cellexpr := 'cell';
    ELSE
      RAISE EXCEPTION 'skycell: % has no cell index to cover', rel
        USING HINT = 'Index skycell_ang2cell(ra, dec) (or a stored cell column).';
    END IF;
  END IF;
  EXECUTE format('SELECT skycell_moc_agg(%s, (%s) >> %s, %L) FROM %s',
                 max_order, cellexpr, 2 * (29 - max_order), fmt, rel) INTO res;
  -- an empty table: an empty MOC of that depth
  RETURN coalesce(res, CASE lower(fmt) WHEN 'json' THEN format('{"%s": []}', max_order)
                                       ELSE format('%s/', max_order) END);
END
$fn$;
COMMENT ON FUNCTION skycell_coverage_moc(text, integer, text) IS
  'The coverage MOC of table tbl at max_order (0-12): the cells of that order holding a row, from its skycell cell index; fmt ''ascii'' (IVOA MOC 2.0) or ''json''.';
CREATE FUNCTION skycell_moc_region(moc text) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_moc_region' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc_region(text) IS
  'An IVOA MOC, in its ASCII ("5/4965 4967 6/19771-19775") or JSON form, as a region: a point is in it when its order-29 HEALPix cell is. REGION(''MOC 5/4965 ...'') reads the ASCII form too.';

CREATE TYPE skycell_mocstate;
CREATE FUNCTION skycell_mocstate_in(cstring) RETURNS skycell_mocstate
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_mocstate_out(skycell_mocstate) RETURNS cstring
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE TYPE skycell_mocstate (
  INPUT = skycell_mocstate_in, OUTPUT = skycell_mocstate_out,
  INTERNALLENGTH = VARIABLE, ALIGNMENT = int4, STORAGE = extended);
COMMENT ON TYPE skycell_mocstate IS
  'The state of skycell_moc_agg: a bitmap of the cells of one order; as text, its order, its format and the runs of its cells.';

CREATE FUNCTION skycell_moc_agg_trans(skycell_mocstate, integer, bigint) RETURNS skycell_mocstate
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_agg_trans(skycell_mocstate, integer, bigint, text) RETURNS skycell_mocstate
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_agg_combine(skycell_mocstate, skycell_mocstate) RETURNS skycell_mocstate
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_agg_final(skycell_mocstate) RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C PARALLEL SAFE;

CREATE OR REPLACE AGGREGATE skycell_moc_agg(hpx_order integer, ipix bigint) (
  SFUNC = skycell_moc_agg_trans, STYPE = skycell_mocstate, FINALFUNC = skycell_moc_agg_final,
  COMBINEFUNC = skycell_moc_agg_combine, PARALLEL = SAFE);
CREATE OR REPLACE AGGREGATE skycell_moc_agg(hpx_order integer, ipix bigint, fmt text) (
  SFUNC = skycell_moc_agg_trans, STYPE = skycell_mocstate, FINALFUNC = skycell_moc_agg_final,
  COMBINEFUNC = skycell_moc_agg_combine, PARALLEL = SAFE);

DROP FUNCTION skycell_moc_agg_trans(internal, integer, bigint);
DROP FUNCTION skycell_moc_agg_trans(internal, integer, bigint, text);
DROP FUNCTION skycell_moc_agg_final(internal);
CREATE FUNCTION skycell_moc_region(r skyregion, max_order integer) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_moc_region_of' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc_region(skyregion, integer) IS
  'Any region as a MOC: the cells of max_order (0-29) it meets, as skycell_moc writes them; a MOC finer than max_order degraded to it.';

CREATE FUNCTION skycell_moc_union(a skyregion, b skyregion) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc_union(skyregion, skyregion) IS
  'The union of two MOCs, exact; at the finer depth of the two.';
CREATE FUNCTION skycell_moc_intersection(a skyregion, b skyregion) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc_intersection(skyregion, skyregion) IS
  'The intersection of two MOCs, exact; at the finer depth of the two.';
CREATE FUNCTION skycell_moc_difference(a skyregion, b skyregion) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc_difference(skyregion, skyregion) IS
  'MOC a less MOC b, exact; at the finer depth of the two.';
CREATE FUNCTION skycell_moc_complement(a skyregion) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc_complement(skyregion) IS
  'The rest of the sky outside MOC a, exact.';
CREATE FUNCTION skycell_moc_degrade(a skyregion, hpx_order integer) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc_degrade(skyregion, integer) IS
  'MOC a at a coarser order: every cell of that order it meets, whole (a superset).';

CREATE FUNCTION skycell_moc_union_agg_trans(internal, skyregion) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_union_agg_trans(internal, skyregion, integer) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_union_agg_combine(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_union_agg_serialize(internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_union_agg_deserialize(bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_union_agg_final(internal) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C PARALLEL SAFE;
CREATE AGGREGATE skycell_moc_union_agg(r skyregion) (
  SFUNC = skycell_moc_union_agg_trans, STYPE = internal, FINALFUNC = skycell_moc_union_agg_final,
  COMBINEFUNC = skycell_moc_union_agg_combine, SERIALFUNC = skycell_moc_union_agg_serialize,
  DESERIALFUNC = skycell_moc_union_agg_deserialize, PARALLEL = SAFE);
CREATE AGGREGATE skycell_moc_union_agg(r skyregion, max_order integer) (
  SFUNC = skycell_moc_union_agg_trans, STYPE = internal, FINALFUNC = skycell_moc_union_agg_final,
  COMBINEFUNC = skycell_moc_union_agg_combine, SERIALFUNC = skycell_moc_union_agg_serialize,
  DESERIALFUNC = skycell_moc_union_agg_deserialize, PARALLEL = SAFE);
COMMENT ON AGGREGATE skycell_moc_union_agg(skyregion) IS
  'The union of the MOCs aggregated, exact: the combined footprint of a set of observations. NULL over no rows.';
COMMENT ON AGGREGATE skycell_moc_union_agg(skyregion, integer) IS
  'The union of any regions aggregated, each as skycell_moc_region(r, max_order) makes it.';


-- With skycell in public, ADQL's bare POINT(ra, dec) is pg_catalog's
-- point(float8, float8), and CONTAINS(POINT(ra, dec), CIRCLE(...)) found no
-- function.  An implicit cast reads that point as (ra, dec) in degrees; its
-- support function plans the cast of point(ra, dec) as skycell's own
-- point(ra, dec), so the search stays indexed.
CREATE FUNCTION skycell_pos_from_point_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_pos_from_point(p point) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_pos_from_point_support;
COMMENT ON FUNCTION skycell_pos_from_point(point) IS
  'PostgreSQL''s point as a sky position: x the right ascension, y the declination, degrees.';
CREATE CAST (point AS skypos) WITH FUNCTION skycell_pos_from_point(point) AS IMPLICIT;

-- Partitioning by sky region: a HEALPix cell of order k is one range of the
-- order-29 ids the index holds, so a table partitioned by RANGE on the cell
-- can be cut into cells.  The id range of one, and every partition of an
-- order made in one call.
CREATE FUNCTION skycell_cell_range(hpx_order integer, ipix bigint, OUT lo bigint, OUT hi bigint)
LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE AS $$
BEGIN
  IF hpx_order < 0 OR hpx_order > 29 THEN
    RAISE EXCEPTION 'HEALPix order % is out of range (0-29)', hpx_order;
  END IF;
  IF ipix < 0 OR ipix >= 12 * (1::bigint << (2 * hpx_order)) THEN
    RAISE EXCEPTION 'cell % does not exist at order % (0 to %)', ipix, hpx_order,
      12 * (1::bigint << (2 * hpx_order)) - 1;
  END IF;
  lo := ipix << (2 * (29 - hpx_order));
  hi := ((ipix + 1) << (2 * (29 - hpx_order))) - 1;
END $$;
COMMENT ON FUNCTION skycell_cell_range(integer, bigint) IS
  'The first and last order-29 cell ids inside a NESTED HEALPix cell: a partition holding that cell is FOR VALUES FROM (lo) TO (hi + 1).';

CREATE FUNCTION skycell_create_partitions(parent regclass, hpx_order integer,
                                          with_default boolean DEFAULT true)
RETURNS integer LANGUAGE plpgsql AS $$
DECLARE
  strat "char";
  nkeys smallint;
  nsp name;
  base name;
  n integer := 0;
  shift integer;
BEGIN
  SELECT partstrat, partnatts INTO strat, nkeys FROM pg_partitioned_table WHERE partrelid = parent;
  IF NOT FOUND OR strat <> 'r' OR nkeys <> 1 THEN
    RAISE EXCEPTION '% is not partitioned by RANGE on one key', parent
      USING HINT = 'CREATE TABLE ... PARTITION BY RANGE (skycell_ang2cell(ra, dec)), or on a stored cell column.';
  END IF;
  IF hpx_order IS NULL OR hpx_order < 0 OR hpx_order > 4 THEN
    RAISE EXCEPTION 'partitions of HEALPix order % (0-4: 12 to 3072 partitions)', hpx_order;
  END IF;
  IF EXISTS (SELECT FROM pg_inherits WHERE inhparent = parent) THEN
    RAISE EXCEPTION '% already has partitions', parent;
  END IF;
  SELECT n2.nspname, c.relname INTO nsp, base
  FROM pg_class c JOIN pg_namespace n2 ON n2.oid = c.relnamespace WHERE c.oid = parent;
  shift := 2 * (29 - hpx_order);
  FOR p IN 0 .. 12 * (1 << (2 * hpx_order)) - 1 LOOP
    EXECUTE format('CREATE TABLE %I.%I PARTITION OF %s FOR VALUES FROM (%s) TO (%s)',
                   nsp, left(base, 50) || '_' || hpx_order || '_' || p, parent,
                   p::bigint << shift, (p::bigint + 1) << shift);
    n := n + 1;
  END LOOP;
  IF with_default THEN
    EXECUTE format('CREATE TABLE %I.%I PARTITION OF %s DEFAULT', nsp, left(base, 50) || '_default', parent);
    n := n + 1;
  END IF;
  RETURN n;
END $$;
COMMENT ON FUNCTION skycell_create_partitions(regclass, integer, boolean) IS
  'Cut a table partitioned by RANGE on the cell into the 12 * 4^order HEALPix cells of an order (0-4), one partition each, named <table>_<order>_<cell>, and a default partition for rows without a position (their cell is NULL). Returns the partitions made.';


-- skycell's schema need not be on the search_path: the functions written in
-- SQL and PL/pgSQL name skycell's own objects with it, so the extension is no
-- longer relocatable (CREATE EXTENSION skycell SCHEMA s still chooses it).

-- Equality, ordering and hashing, so that DISTINCT, GROUP BY, UNION and ORDER
-- BY work on positions and regions.  A position is ordered by right ascension
-- then declination; a region by its stored form.  The orders mean nothing on
-- the sky.
CREATE FUNCTION skypos_eq(skypos, skypos) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skypos_ne(skypos, skypos) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skypos_lt(skypos, skypos) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skypos_le(skypos, skypos) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skypos_gt(skypos, skypos) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skypos_ge(skypos, skypos) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skypos_cmp(skypos, skypos) RETURNS int4 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skypos_hash(skypos) RETURNS int4 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE OPERATOR = (LEFTARG = skypos, RIGHTARG = skypos, FUNCTION = skypos_eq, COMMUTATOR = =, NEGATOR = <>,
                   RESTRICT = eqsel, JOIN = eqjoinsel, HASHES, MERGES);
CREATE OPERATOR <> (LEFTARG = skypos, RIGHTARG = skypos, FUNCTION = skypos_ne, COMMUTATOR = <>, NEGATOR = =,
                    RESTRICT = neqsel, JOIN = neqjoinsel);
CREATE OPERATOR < (LEFTARG = skypos, RIGHTARG = skypos, FUNCTION = skypos_lt, COMMUTATOR = >, NEGATOR = >=,
                   RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
CREATE OPERATOR <= (LEFTARG = skypos, RIGHTARG = skypos, FUNCTION = skypos_le, COMMUTATOR = >=, NEGATOR = >,
                    RESTRICT = scalarlesel, JOIN = scalarlejoinsel);
CREATE OPERATOR > (LEFTARG = skypos, RIGHTARG = skypos, FUNCTION = skypos_gt, COMMUTATOR = <, NEGATOR = <=,
                   RESTRICT = scalargtsel, JOIN = scalargtjoinsel);
CREATE OPERATOR >= (LEFTARG = skypos, RIGHTARG = skypos, FUNCTION = skypos_ge, COMMUTATOR = <=, NEGATOR = <,
                    RESTRICT = scalargesel, JOIN = scalargejoinsel);
CREATE OPERATOR CLASS skypos_btree_ops DEFAULT FOR TYPE skypos USING btree AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 skypos_cmp(skypos, skypos);
CREATE OPERATOR CLASS skypos_hash_ops DEFAULT FOR TYPE skypos USING hash AS
  OPERATOR 1 =, FUNCTION 1 skypos_hash(skypos);

CREATE FUNCTION skyregion_eq(skyregion, skyregion) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skyregion_ne(skyregion, skyregion) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skyregion_lt(skyregion, skyregion) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skyregion_le(skyregion, skyregion) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skyregion_gt(skyregion, skyregion) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skyregion_ge(skyregion, skyregion) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skyregion_cmp(skyregion, skyregion) RETURNS int4 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION skyregion_hash(skyregion) RETURNS int4 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE OPERATOR = (LEFTARG = skyregion, RIGHTARG = skyregion, FUNCTION = skyregion_eq, COMMUTATOR = =, NEGATOR = <>,
                   RESTRICT = eqsel, JOIN = eqjoinsel, HASHES, MERGES);
CREATE OPERATOR <> (LEFTARG = skyregion, RIGHTARG = skyregion, FUNCTION = skyregion_ne, COMMUTATOR = <>, NEGATOR = =,
                    RESTRICT = neqsel, JOIN = neqjoinsel);
CREATE OPERATOR < (LEFTARG = skyregion, RIGHTARG = skyregion, FUNCTION = skyregion_lt, COMMUTATOR = >, NEGATOR = >=,
                   RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
CREATE OPERATOR <= (LEFTARG = skyregion, RIGHTARG = skyregion, FUNCTION = skyregion_le, COMMUTATOR = >=, NEGATOR = >,
                    RESTRICT = scalarlesel, JOIN = scalarlejoinsel);
CREATE OPERATOR > (LEFTARG = skyregion, RIGHTARG = skyregion, FUNCTION = skyregion_gt, COMMUTATOR = <, NEGATOR = <=,
                   RESTRICT = scalargtsel, JOIN = scalargtjoinsel);
CREATE OPERATOR >= (LEFTARG = skyregion, RIGHTARG = skyregion, FUNCTION = skyregion_ge, COMMUTATOR = <=, NEGATOR = <,
                    RESTRICT = scalargesel, JOIN = scalargejoinsel);
CREATE OPERATOR CLASS skyregion_btree_ops DEFAULT FOR TYPE skyregion USING btree AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 skyregion_cmp(skyregion, skyregion);
CREATE OPERATOR CLASS skyregion_hash_ops DEFAULT FOR TYPE skyregion USING hash AS
  OPERATOR 1 =, FUNCTION 1 skyregion_hash(skyregion);

-- Binary I/O: COPY ... (FORMAT binary), binary replication and drivers that
-- read results in binary.  A position is its two float8s; a region a format
-- byte and its exact STC-S.
CREATE FUNCTION skypos_send(skypos) RETURNS bytea AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skypos_recv(internal) RETURNS skypos AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skyregion_send(skyregion) RETURNS bytea AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skyregion_recv(internal) RETURNS skyregion AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
ALTER TYPE skypos SET (SEND = skypos_send, RECEIVE = skypos_recv);
ALTER TYPE skyregion SET (SEND = skyregion_send, RECEIVE = skyregion_recv);

-- A density map names the relation it describes as well as its OID, which a
-- dump and restore does not keep: the map is found by name after a restore.
ALTER TABLE skycell_density_map ADD COLUMN relname text;
UPDATE skycell_density_map m SET relname = format('%I.%I', n.nspname, c.relname)
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = m.statrel;

CREATE OR REPLACE FUNCTION skycell_density_build(tbl regclass, col name DEFAULT 'cell',
                                      rows_per_cell int DEFAULT 100,
                                      max_order int DEFAULT 13)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
  srel oid; att int2; k int; nleaf bigint; expr text; idx oid;
BEGIN
  IF rows_per_cell < 1 OR max_order < 1 OR max_order > 20 THEN
    RAISE EXCEPTION 'skycell: rows_per_cell must be >= 1 and max_order within [1, 20]';
  END IF;

  -- statistics live on the table for a plain column, on the index for an
  -- expression index: match what the planner support function looks up.
  SELECT a.attnum, tbl INTO att, srel
  FROM pg_attribute a WHERE a.attrelid = tbl AND a.attname = col AND NOT a.attisdropped;
  IF att IS NULL THEN
    SELECT i.indexrelid INTO idx FROM pg_index i
    WHERE i.indrelid = tbl AND pg_get_indexdef(i.indexrelid) LIKE '%skycell_ang2cell%'
    ORDER BY i.indexrelid LIMIT 1;
    IF idx IS NULL THEN
      RAISE EXCEPTION 'skycell: no column % and no skycell_ang2cell index on %', col, tbl;
    END IF;
    srel := idx; att := 1;
    expr := (SELECT pg_get_expr(i.indexprs, i.indrelid) FROM pg_index i WHERE i.indexrelid = idx);
  ELSE
    expr := quote_ident(col);
  END IF;

  DELETE FROM @extschema@.skycell_density_map m WHERE m.statrel = srel AND m.attnum = att;

  /*
   * Count at the finest order allowed, then roll up one order at a time: a
   * cell is a leaf when it holds at most rows_per_cell and its parent does
   * not, plus the cells at the finest order that are still too full to split
   * further.  A recursive CTE cannot do this (no aggregates in the recursive
   * term), so it is a loop over the orders.
   */
  CREATE TEMP TABLE sc_lev (ord int, pix int8, n int8) ON COMMIT DROP;
  EXECUTE format(
    'INSERT INTO sc_lev SELECT %s, (%s) >> (2 * (29 - %s)), count(*) FROM %s '
    'WHERE (%s) IS NOT NULL GROUP BY 2',
    max_order, expr, max_order, tbl::text, expr);

  FOR k IN REVERSE max_order .. 1 LOOP
    INSERT INTO sc_lev SELECT k - 1, pix >> 2, sum(n) FROM sc_lev WHERE ord = k GROUP BY 2;
    EXIT WHEN NOT FOUND;
  END LOOP;
  CREATE INDEX ON sc_lev (ord, pix);

  INSERT INTO @extschema@.skycell_density_map (statrel, attnum, nuniq, n, relname)
  SELECT srel, att, (4::int8 << (2 * u.ord)) + u.pix, u.n,
         (SELECT format('%I.%I', n.nspname, c.relname) FROM pg_class c
          JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = srel)
  FROM sc_lev u
  LEFT JOIN sc_lev p ON p.ord = u.ord - 1 AND p.pix = u.pix >> 2
  WHERE (u.n <= rows_per_cell AND (p.n IS NULL OR p.n > rows_per_cell))
     OR (u.ord = max_order AND u.n > rows_per_cell);

  GET DIAGNOSTICS nleaf = ROW_COUNT;
  DROP TABLE sc_lev;
  RETURN nleaf;
END $$;

CREATE OR REPLACE FUNCTION skycell_density_drop(tbl regclass, col name DEFAULT 'cell') RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE n bigint; idx oid;
BEGIN
  DELETE FROM @extschema@.skycell_density_map m
  WHERE m.statrel = tbl OR m.statrel IN (SELECT indexrelid FROM pg_index WHERE indrelid = tbl)
     OR m.relname IN (SELECT format('%I.%I', n.nspname, c.relname) FROM pg_class c
                      JOIN pg_namespace n ON n.oid = c.relnamespace
                      WHERE c.oid = tbl OR c.oid IN (SELECT indexrelid FROM pg_index WHERE indrelid = tbl));
  GET DIAGNOSTICS n = ROW_COUNT;
  RETURN n;
END $$;

CREATE OR REPLACE FUNCTION skycell_coverage_moc(tbl text, max_order integer DEFAULT 10, fmt text DEFAULT 'ascii')
RETURNS text LANGUAGE plpgsql STABLE AS $fn$
DECLARE
  rel regclass := tbl::regclass;
  cellexpr text;
  res text;
BEGIN
  IF max_order < 0 OR max_order > 12 THEN
    RAISE EXCEPTION 'skycell: a coverage MOC''s order must be within [0, 12]';
  END IF;
  -- the table's cell expression, as skycell_density_view finds it
  SELECT pg_get_expr(i.indexprs, i.indrelid) INTO cellexpr
  FROM pg_index i
  WHERE i.indrelid IN (SELECT rel UNION ALL SELECT inhrelid FROM pg_inherits WHERE inhparent = rel)
    AND i.indkey[0] = 0
    AND pg_get_expr(i.indexprs, i.indrelid) ~ '^((\w+|"[^"]+")\.)?skycell_(ang2cell|cell)\('
  ORDER BY i.indrelid = rel DESC
  LIMIT 1;
  IF cellexpr IS NULL THEN
    IF EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = rel AND attname = 'cell' AND NOT attisdropped) THEN
      cellexpr := 'cell';
    ELSE
      RAISE EXCEPTION 'skycell: % has no cell index to cover', rel
        USING HINT = 'Index skycell_ang2cell(ra, dec) (or a stored cell column).';
    END IF;
  END IF;
  EXECUTE format('SELECT @extschema@.skycell_moc_agg(%s, (%s) >> %s, %L) FROM %s',
                 max_order, cellexpr, 2 * (29 - max_order), fmt, rel) INTO res;
  -- an empty table: an empty MOC of that depth
  RETURN coalesce(res, CASE lower(fmt) WHEN 'json' THEN format('{"%s": []}', max_order)
                                       ELSE format('%s/', max_order) END);
END
$fn$;

CREATE OR REPLACE FUNCTION skycell_density_view(tbl text, target text, max_order integer DEFAULT 8,
                                      min_order integer DEFAULT 0) RETURNS bigint
LANGUAGE plpgsql AS $fn$
DECLARE
  rel regclass := tbl::regclass;
  cellexpr text;
  parts text[] := parse_ident(target);
  qname text;
  iname text;
  nrows bigint;
  q text;
BEGIN
  IF min_order < 0 OR max_order > 20 OR min_order > max_order THEN
    RAISE EXCEPTION 'skycell: orders must satisfy 0 <= min_order <= max_order <= 20';
  END IF;
  -- the table's cell expression: its skycell index's (on the table, or on one of
  -- its partitions), else a stored cell column
  SELECT pg_get_expr(i.indexprs, i.indrelid) INTO cellexpr
  FROM pg_index i
  WHERE i.indrelid IN (SELECT rel UNION ALL SELECT inhrelid FROM pg_inherits WHERE inhparent = rel)
    AND i.indkey[0] = 0
    AND pg_get_expr(i.indexprs, i.indrelid) ~ '^((\w+|"[^"]+")\.)?skycell_(ang2cell|cell)\('
  ORDER BY i.indrelid = rel DESC
  LIMIT 1;
  IF cellexpr IS NULL THEN
    IF EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = rel AND attname = 'cell' AND NOT attisdropped) THEN
      cellexpr := 'cell';
    ELSE
      RAISE EXCEPTION 'skycell: % has no cell index to count from', rel
        USING HINT = 'Index skycell_ang2cell(ra, dec) (or a stored cell column).';
    END IF;
  END IF;
  qname := array_to_string(ARRAY(SELECT quote_ident(p) FROM unnest(parts) p), '.');
  iname := quote_ident(parts[array_length(parts, 1)] || '_cell');
  EXECUTE format('DROP MATERIALIZED VIEW IF EXISTS %s', qname);
  -- the finest order counted from the table, each coarser one from the order
  -- below it (a quarter of its rows): one pass over the table
  q := format('f%s AS MATERIALIZED (SELECT (%s) >> %s AS ipix, count(*)::bigint AS n FROM %s WHERE (%s) IS NOT NULL GROUP BY 1)',
              max_order, cellexpr, 2 * (29 - max_order), rel, cellexpr);
  FOR o IN REVERSE max_order - 1 .. min_order LOOP
    q := q || format(', f%s AS MATERIALIZED (SELECT ipix >> 2 AS ipix, sum(n)::bigint AS n FROM f%s GROUP BY 1)', o, o + 1);
  END LOOP;
  EXECUTE format('CREATE MATERIALIZED VIEW %s AS WITH %s %s', qname, q,
                 (SELECT string_agg(format('SELECT %s AS hpx_order, ipix, n FROM f%s', o, o), ' UNION ALL ' ORDER BY o)
                  FROM generate_series(min_order, max_order) o));
  EXECUTE format('CREATE UNIQUE INDEX %s ON %s (hpx_order, ipix)', iname, qname);
  EXECUTE format('ANALYZE %s', qname);
  EXECUTE format($c$COMMENT ON MATERIALIZED VIEW %s IS %L$c$, qname,
    format('skycell density map of %s: rows per NESTED HEALPix cell, orders %s-%s. REFRESH MATERIALIZED VIEW [CONCURRENTLY] to rebuild it.',
           rel, min_order, max_order));
  EXECUTE format('SELECT count(*) FROM %s', qname) INTO nrows;
  RETURN nrows;
END
$fn$;

CREATE OR REPLACE FUNCTION skycell_region_moc_ranges(region skyregion, max_cells int DEFAULT 8,
                                          max_order int DEFAULT 29,
                                          OUT nuniq int8, OUT rng int8range) RETURNS SETOF record
AS $$ SELECT n, int8range(@extschema@.skycell_nuniq_lo(n), @extschema@.skycell_nuniq_hi(n), '[]')
      FROM unnest(@extschema@.skycell_region_moc(region, max_cells, max_order)) AS n $$
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE ROWS 8;


-- A security-barrier view (or a view's security_barrier option) keeps a
-- condition out of the view unless every function in it that reads the
-- view's columns is LEAKPROOF: it says nothing of its arguments but its
-- result, raising no error that depends on them.  These do not: a position,
-- a region and the coordinates are tested as they are, and a stored region
-- that no longer decodes (only a circle of NaN radius stored before 0.56 can
-- be one) is empty rather than an error.  So a search through such a view
-- reaches the index of the table under it.
--
-- Not LEAKPROOF, by design: skycell_ang2cell() and ADQL's POINT(), which
-- reject impossible coordinates (an error that names the value), and the
-- functions taking a polygon as an array, which reject malformed arrays.  A
-- search on ra and dec columns through a security barrier is written with
-- skycell_radial_query() or skycell_join(), or on a skypos column.
ALTER FUNCTION coord1(skypos) LEAKPROOF;
ALTER FUNCTION coord2(skypos) LEAKPROOF;
ALTER FUNCTION distance(skypos, skypos) LEAKPROOF;
ALTER FUNCTION distance(float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_dist(float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_cell(skypos) LEAKPROOF;
ALTER FUNCTION skycell_in_cone(float8, float8, float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_cone(int8, float8, float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_join(float8, float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_radial_query(float8, float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_distance_below(float8, float8, bool) LEAKPROOF;
ALTER FUNCTION contains(skypos, skyregion) LEAKPROOF;
ALTER FUNCTION contains(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION intersects(skypos, skyregion) LEAKPROOF;
ALTER FUNCTION intersects(skyregion, skypos) LEAKPROOF;
ALTER FUNCTION intersects(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_pos_in_region(skypos, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_region_has_pos(skyregion, skypos) LEAKPROOF;
ALTER FUNCTION skycell_in_region(skypos, skyregion, float8) LEAKPROOF;
ALTER FUNCTION skycell_region_overlap(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_region_covers(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_region_covered_by(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_region_scan(int8, float8, float8, skyregion) LEAKPROOF;
