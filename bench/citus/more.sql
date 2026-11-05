\pset pager off
EXPLAIN (COSTS OFF) SELECT id FROM cat_dist WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 2));
EXPLAIN (COSTS OFF) SELECT id FROM cat_dist WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('3/100-110'));

-- nearest neighbours: ORDER BY distance LIMIT k
SELECT (SELECT array_agg(id) FROM (SELECT id FROM cat_local ORDER BY distance(ra, dec, 120, 30) LIMIT 10) s)
     = (SELECT array_agg(id) FROM (SELECT id FROM cat_dist  ORDER BY distance(ra, dec, 120, 30) LIMIT 10) s) AS nearest_same;
EXPLAIN (COSTS OFF) SELECT id FROM cat_dist ORDER BY distance(ra, dec, 120, 30) LIMIT 10;

-- coverage: the aggregate, and the function that runs it
SELECT (SELECT skycell_moc_agg(6, skycell_ang2cell(ra, dec) >> 46) FROM cat_local)
     = (SELECT skycell_moc_agg(6, skycell_ang2cell(ra, dec) >> 46) FROM cat_dist) AS agg_same;
EXPLAIN (COSTS OFF) SELECT skycell_moc_agg(6, skycell_ang2cell(ra, dec) >> 46) FROM cat_dist;
SELECT skycell_coverage_moc('cat_local', 6) = skycell_coverage_moc('cat_dist', 6) AS coverage_same;

-- estimates read from statistics (the coordinator holds none for a distributed table)
SELECT skycell_count_estimate('cat_local', CIRCLE('ICRS', 120, 30, 10)) AS est_local,
       skycell_count_estimate('cat_dist', CIRCLE('ICRS', 120, 30, 10)) AS est_dist;
