# 5. ADQL, TAP services, and moving from Q3C or pgSphere

skycell defines ADQL's geometry under ADQL's own names, so a TAP service can
pass a query's geometry to PostgreSQL as written. This tutorial covers what a
service needs, what the IVOA user-defined functions add, and how to move a
database from Q3C or pgSphere. Every block runs as written, in order, on an
empty database.

```sql
CREATE SCHEMA sky;
CREATE EXTENSION skycell SCHEMA sky;
ALTER DATABASE skycell_tutorial SET search_path = sky, public, pg_catalog;
ALTER DATABASE skycell_tutorial SET session_preload_libraries = 'skycell';
```

```sql
SELECT setseed(0.5);
CREATE TABLE gaia_source AS
SELECT i AS source_id, 360 * random() AS ra, degrees(asin(2 * random() - 1)) AS dec,
       5 * random() AS parallax, 20 * (random() - 0.5) AS pmra, 20 * (random() - 0.5) AS pmdec,
       10 + 10 * random() AS phot_g_mean_mag
FROM generate_series(1, 200000) i;
CREATE INDEX gaia_source_cell ON gaia_source (skycell_ang2cell(ra, dec));
ALTER INDEX gaia_source_cell ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE gaia_source;
```

## What a TAP service needs

1. **The names to resolve.** The schema and `search_path` of the set-up block
   above: ADQL's `POINT`, `CIRCLE`, `BOX` and `POLYGON` would otherwise be
   PostgreSQL's geometric functions. Set it for the role or database the
   service connects as.
2. **ADQL's geometry passed through.** `CONTAINS(...) = 1`,
   `INTERSECTS(...) = 1`, `DISTANCE(...) < r` and `ORDER BY DISTANCE(...)` are
   indexed as ADQL writes them. The rest of ADQL is the service's own job:
   `TOP n` becomes `LIMIT n`, and so on.
3. **An index per positional table** (tutorials [1](01-source-catalogue.md)
   and [2](02-regions-and-footprints.md)), and `ANALYZE` after loads.

The queries of the Gaia archive's own examples run as written:

```sql
-- a cone search, written with the point inside a circle round each row
EXPLAIN (COSTS OFF)
SELECT source_id FROM gaia_source
WHERE 1 = CONTAINS(POINT('ICRS', 81.28, -69.78), CIRCLE('ICRS', ra, dec, 0.5));
-- expect: Custom Scan (SkycellCone) on gaia_source
-- SELECT TOP 5 ... ORDER BY DISTANCE
SELECT source_id, DISTANCE(POINT('ICRS', ra, dec), POINT('ICRS', 266.4, -29.0)) AS dist
FROM gaia_source ORDER BY dist LIMIT 5;
```

Written either way round, a cone is planned as the search round its fixed
point; with pgSphere and Q3C, the reversed form is a full table scan.

For services that translate ADQL themselves, `adql_geometry/` in the
repository has reference translators (Python and Java) from ADQL geometry to
skycell's SQL, and to pgSphere plus Q3C, so the two can be compared on a
service's own queries.

## The IVOA user-defined functions

The positional functions of the IVOA catalogue of ADQL user-defined
functions are included under its names, so ADQL written for DaCHS or the Gaia
archives runs unchanged: epoch propagation, and HEALPix indices.

```sql
SELECT ivo_epoch_prop_pos(266.4, -29.0, 5.2, 120.0, -80.0, NULL, 2016.0, 2026.0) AS moved,
       ivo_healpix_index(10, 266.4, -29.0) AS hpx,
       ivo_healpix_center(10, ivo_healpix_index(10, 266.4, -29.0)) AS hpx_centre;
```

Proper motions are in mas/yr, parallax in mas, radial velocity in km/s, epochs
in years. An index cannot know where a star has moved, so a search at another
epoch is an indexed cone widened by the largest motion of interest, with the
exact test on the propagated position:

```sql
EXPLAIN (COSTS OFF)
SELECT source_id FROM gaia_source
WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 269.45, 4.69, 0.05 + 0.006))
  AND 1 = CONTAINS(ivo_epoch_prop_pos(ra, dec, parallax, pmra, pmdec, NULL, 2016, 2026),
                   CIRCLE('ICRS', 269.45, 4.69, 0.05));
-- expect: Custom Scan (SkycellCone) on gaia_source
```

(0.006° is 21.6″, the motion of 2″/yr over ten years;
`skycell_pm_margin(pmra, pmdec, years)` computes such a margin.)

## From Q3C

Q3C's spellings have skycell twins with the same arguments, and an index on
`skycell_ang2cell(ra, dec)` replaces the one on `q3c_ang2ipix(ra, dec)`:

| Q3C | skycell |
|---|---|
| `CREATE INDEX ON t (q3c_ang2ipix(ra, dec))` | `CREATE INDEX ON t (skycell_ang2cell(ra, dec))` |
| `q3c_radial_query(ra, dec, ra0, dec0, r)` | `skycell_radial_query(ra, dec, ra0, dec0, r)` |
| `q3c_join(ra1, dec1, ra2, dec2, r)` | `skycell_join(ra1, dec1, ra2, dec2, r)` |
| `q3c_poly_query(ra, dec, poly)` | `skycell_poly_join(ra, dec, poly)` |
| `q3c_dist(ra1, dec1, ra2, dec2)` | `skycell_dist(ra1, dec1, ra2, dec2)` |

```sql
CREATE TABLE upload AS SELECT row_number() OVER () AS id, ra + 0.2 / 3600 AS ra, dec
FROM gaia_source TABLESAMPLE SYSTEM (1);
ANALYZE upload;
EXPLAIN (COSTS OFF)
SELECT u.id, g.source_id FROM upload u JOIN gaia_source g
  ON skycell_join(g.ra, g.dec, u.ra, u.dec, 1.0 / 3600);
-- expect: Ranges: per outer row
```

The two extensions can live in one database while queries move over; both
indexes are B-trees on the same columns, and Q3C's can be dropped once nothing
uses it. What Q3C does not have, skycell adds on the same index: `ORDER BY`
distance with a `LIMIT`, ADQL's spellings, polygons that need not be convex,
composite regions and MOCs.

## From pgSphere

pgSphere's types map onto skycell's; an index on a pgSphere `spoint` column
becomes the cell index on `ra` and `dec` (or on `skycell_cell(pos)` for a
`skypos` column), and a GiST on a region column becomes skycell's GiST on a
`skyregion` column:

| pgSphere | skycell |
|---|---|
| `spoint(radians(ra), radians(dec))` | `point(ra, dec)` (degrees) |
| `scircle(spoint(...), radians(r))` | `circle(ra, dec, r)` |
| `spoly '{(...), ...}'` | `polygon(ra1, dec1, ...)` or STC-S `REGION('POLYGON ICRS ...')` |
| `pos <@ scircle(...)` | `point(ra, dec) <@ circle(...)`, or ADQL `CONTAINS(...) = 1` |
| `pos <-> spoint(...)` | `DISTANCE(POINT(ra, dec), POINT(...))`, in degrees |
| `ORDER BY pos <-> spoint(...) LIMIT k` | `ORDER BY DISTANCE(...) LIMIT k` |
| `spoly && spoly` | `skyregion && skyregion`, or ADQL `INTERSECTS(...) = 1` |
| `smoc` | `skyregion` holding a MOC, exact to its cells |
| GiST on `spoint` | B-tree on `skycell_ang2cell(ra, dec)` |
| GiST on `spoly` / `scircle` | GiST on the `skyregion` column |

A pgSphere polygon column is converted through its vertices, which pgSphere
prints in radians by default (`set_sphere_output('RAD')`); with pgSphere still
installed, for instance:

<!-- check: skip -->
```sql
ALTER TABLE obscore ADD COLUMN region skyregion;
UPDATE obscore SET region = polygon(VARIADIC (
  SELECT array_agg(degrees(v) ORDER BY ord)
  FROM unnest(regexp_split_to_array(trim(both '{}()' FROM s_region::text), '\)\s*,\s*\(|\s*,\s*')
              ::float8[]) WITH ORDINALITY AS u(v, ord)));
CREATE INDEX ON obscore USING gist (region);
```

Text STC-S, which most `s_region` columns hold, converts directly with
`REGION()` ([tutorial 2](02-regions-and-footprints.md#a-footprint-table)).
