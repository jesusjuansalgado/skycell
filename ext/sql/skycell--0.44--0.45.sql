-- skycell 0.44 -> 0.45: skycell_moc_agg in parallel and in partial
-- aggregation.  Its state becomes a type of its own, skycell_mocstate, in
-- place of internal: parallel workers hand it to the leader, and a sharded
-- database (Citus) pushes the aggregate down to its shards and combines their
-- states, sent as text, on the coordinator.

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
