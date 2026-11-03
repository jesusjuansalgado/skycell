\echo Use "ALTER EXTENSION skycell UPDATE TO '0.42'" to load this file. \quit

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
