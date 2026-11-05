\pset pager off
-- plans for polygons and composites on a shard
EXPLAIN (COSTS OFF) SELECT id FROM cat_dist WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), POLYGON('ICRS', 200, -10, 215, -8, 208, 4));
EXPLAIN (COSTS OFF) SELECT id FROM cat_dist WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), REGION('DIFFERENCE ICRS (CIRCLE 300 60 6 CIRCLE 300 60 3)'));

-- probes: a small list of positions
CREATE TABLE probe_local AS SELECT id AS pid, ra + 0.0003 AS ra, dec FROM cat_local WHERE id % 997 = 0;
CREATE TABLE probe_ref AS SELECT * FROM probe_local;
SELECT create_reference_table('probe_ref');
CREATE TABLE probe_dist AS SELECT * FROM probe_local;
SELECT create_distributed_table('probe_dist', 'pid', shard_count := 8);
ANALYZE probe_local; ANALYZE probe_ref; ANALYZE probe_dist;

-- cross-match: local truth
SELECT count(*) || ':' || sum(c.id) AS local FROM probe_local p JOIN cat_local c ON 1 = CONTAINS(POINT('ICRS', c.ra, c.dec), CIRCLE('ICRS', p.ra, p.dec, 0.05));
-- against a reference (replicated) probe table
SELECT count(*) || ':' || sum(c.id) AS ref FROM probe_ref p JOIN cat_dist c ON 1 = CONTAINS(POINT('ICRS', c.ra, c.dec), CIRCLE('ICRS', p.ra, p.dec, 0.05));
EXPLAIN (COSTS OFF) SELECT c.id FROM probe_ref p JOIN cat_dist c ON 1 = CONTAINS(POINT('ICRS', c.ra, c.dec), CIRCLE('ICRS', p.ra, p.dec, 0.05));
SELECT count(*) || ':' || sum(c.id) AS ref_distance FROM probe_ref p JOIN cat_dist c ON distance(p.ra, p.dec, c.ra, c.dec) < 0.05;
-- against a distributed probe table (not co-located on any key)
SELECT count(*) || ':' || sum(c.id) AS dist FROM probe_dist p JOIN cat_dist c ON 1 = CONTAINS(POINT('ICRS', c.ra, c.dec), CIRCLE('ICRS', p.ra, p.dec, 0.05));
-- against a local coordinator table
SELECT count(*) || ':' || sum(c.id) AS local_probe FROM probe_local p JOIN cat_dist c ON 1 = CONTAINS(POINT('ICRS', c.ra, c.dec), CIRCLE('ICRS', p.ra, p.dec, 0.05));

-- density view and estimates of a cell
SELECT skycell_density_view('cat_dist', 'cat_dens', 4, 0);
SELECT skycell_count_estimate('cat_dist', 4, 100) AS cell_est_dist, skycell_count_estimate('cat_local', 4, 100) AS cell_est_local;
-- a skycell setting made on the coordinator: does it reach the shards?
SET skycell.custom_scan = off;
EXPLAIN (COSTS OFF) SELECT id FROM cat_dist WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 2));
RESET skycell.custom_scan;
-- a prepared statement with the centre as a parameter
PREPARE pc(float8, float8) AS SELECT count(*) FROM cat_dist WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', $1, $2, 2));
EXECUTE pc(120, 30); EXECUTE pc(120, 30); EXECUTE pc(120, 30); EXECUTE pc(120, 30); EXECUTE pc(120, 30); EXECUTE pc(120, 30); EXECUTE pc(120, 30);
