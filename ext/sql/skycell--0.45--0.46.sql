-- skycell 0.45 -> 0.46: MOC set operations

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
