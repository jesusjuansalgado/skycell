-- skycell 0.44 -> 0.45: skycell_moc_agg in parallel and in partial
-- aggregation (parallel workers, Citus shards): a combine step and its state
-- serialised between processes

CREATE FUNCTION skycell_moc_agg_combine(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C CALLED ON NULL INPUT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_agg_serialize(internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_moc_agg_deserialize(bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
ALTER FUNCTION skycell_moc_agg_trans(internal, integer, bigint) PARALLEL SAFE;
ALTER FUNCTION skycell_moc_agg_trans(internal, integer, bigint, text) PARALLEL SAFE;
ALTER FUNCTION skycell_moc_agg_final(internal) PARALLEL SAFE;

CREATE OR REPLACE AGGREGATE skycell_moc_agg(hpx_order integer, ipix bigint) (
  SFUNC = skycell_moc_agg_trans, STYPE = internal, FINALFUNC = skycell_moc_agg_final,
  COMBINEFUNC = skycell_moc_agg_combine, SERIALFUNC = skycell_moc_agg_serialize,
  DESERIALFUNC = skycell_moc_agg_deserialize, PARALLEL = SAFE);
CREATE OR REPLACE AGGREGATE skycell_moc_agg(hpx_order integer, ipix bigint, fmt text) (
  SFUNC = skycell_moc_agg_trans, STYPE = internal, FINALFUNC = skycell_moc_agg_final,
  COMBINEFUNC = skycell_moc_agg_combine, SERIALFUNC = skycell_moc_agg_serialize,
  DESERIALFUNC = skycell_moc_agg_deserialize, PARALLEL = SAFE);
