-- skycell 0.47 -> 0.48: what the region tests cost

-- A position tested against a stored region costs about a microsecond, and
-- a region's MOC cells (the GIN index's expression) tens of microseconds:
-- at the default cost of one operator the planner preferred reading a
-- footprint table through, over its GIN index (0.4 ms against 9.5 ms on
-- 5000 footprints).  Costs in units of cpu_operator_cost.
ALTER FUNCTION skycell_in_region(skypos, skyregion, float8) COST 25;
ALTER FUNCTION skycell_pos_in_region(skypos, skyregion) COST 25;
ALTER FUNCTION skycell_region_has_pos(skyregion, skypos) COST 25;
ALTER FUNCTION skycell_region_moc(skyregion, int, int) COST 1000;
