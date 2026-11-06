CREATE TEMP TABLE q (name text, w text);
INSERT INTO q VALUES
 ('adql cone',        $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 2))$q$),
 ('skycell_cone',     $q$skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 120, 30, 2)$q$),
 ('distance',         $q$distance(ra, dec, 200, -40) < 1.5$q$),
 ('polygon',          $q$1 = CONTAINS(POINT('ICRS', ra, dec), POLYGON('ICRS', 200, -10, 215, -8, 208, 4))$q$),
 ('box',              $q$1 = CONTAINS(POINT('ICRS', ra, dec), BOX('ICRS', 10, 10, 4, 3))$q$),
 ('composite',        $q$1 = CONTAINS(POINT('ICRS', ra, dec), REGION('DIFFERENCE ICRS (CIRCLE 300 60 6 CIRCLE 300 60 3)'))$q$),
 ('moc',              $q$1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('3/100-110 5/2000-2010'))$q$),
 ('moc in composite', $q$1 = CONTAINS(POINT('ICRS', ra, dec), REGION('DIFFERENCE ICRS (MOC 2/25-26 CIRCLE 60 40 3)'))$q$),
 ('pole',             $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 0, 90, 5))$q$),
 ('cone and mag',     $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 5)) AND mag < 5$q$),
 ('cone or',          $q$(1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 1)) OR id = 7)$q$);
CREATE TEMP TABLE res (name text, local text, dist text, err text);
DO $do$
DECLARE r record; a text; b text; e text;
BEGIN
  FOR r IN SELECT * FROM q LOOP
    EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM cat_local WHERE %s', r.w) INTO a;
    BEGIN
      EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM cat_dist WHERE %s', r.w) INTO b; e := NULL;
    EXCEPTION WHEN OTHERS THEN b := NULL; e := SQLERRM;
    END;
    INSERT INTO res VALUES (r.name, a, b, e);
  END LOOP;
END $do$;
SELECT name, local, local = dist AS same, err FROM res;
