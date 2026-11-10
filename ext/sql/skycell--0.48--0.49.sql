-- skycell 0.48 -> 0.49: equality, binary I/O, a schema off the search path

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

