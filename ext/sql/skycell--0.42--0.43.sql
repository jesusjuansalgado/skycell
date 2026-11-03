\echo Use "ALTER EXTENSION skycell UPDATE TO '0.43'" to load this file. \quit

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
