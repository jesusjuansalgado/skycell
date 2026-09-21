-- skycell 0.4 -> 0.5: the multi-order count map (see skycell_density_build)
\echo Use "ALTER EXTENSION skycell UPDATE TO '0.5'" to load this file. \quit

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
