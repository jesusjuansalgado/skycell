/*
 * adql.c -- the ADQL-shaped surface of skycell.
 *
 * ADQL (IVOA, the query language every TAP service speaks) writes sky
 * geometry as POINT, CIRCLE, BOX, POLYGON, CONTAINS, INTERSECTS, DISTANCE,
 * AREA.  This file gives skycell two types -- skypos and skyregion -- and
 * functions of exactly those names, so a translator emits the standard's own
 * spelling instead of an extension's dialect:
 *
 *     1 = CONTAINS(POINT('ICRS', s_ra, s_dec), CIRCLE('ICRS', 10, 20, 0.1))
 *   ->  1 = contains(point('ICRS', s_ra, s_dec), circle('ICRS', 10, 20, 0.1))
 *
 * The operator spelling of the same predicate, `point(...) <@ circle(...)`,
 * is the one the planner can answer from an index: its support function
 * rewrites it into B-tree ranges over the cell id plus an exact test, the
 * same machinery skycell_cone() uses, except that the cell expression is
 * *found* (from the relation's indexes) rather than spelled out by the
 * caller.  Both spellings return the same rows.
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/stratnum.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "cover.h"
#include "skycell_internal.h"

#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

/* ------------------------------------------------------------------ */
/* the types                                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
	double		ra;				/* degrees */
	double		dec;			/* degrees */
} SkyPos;

#define SKY_CONE 1
#define SKY_POLY 2

typedef struct
{
	int32		vl_len_;
	int32		kind;
	/* cone: ra, dec, radius (degrees); polygon: ra, dec per vertex */
	double		v[FLEXIBLE_ARRAY_MEMBER];
} SkyRegion;

#define DatumGetSkyPos(x)	((SkyPos *) DatumGetPointer(x))
#define PG_GETARG_SKYPOS(n) DatumGetSkyPos(PG_GETARG_DATUM(n))
#define DatumGetSkyRegion(x) ((SkyRegion *) PG_DETOAST_DATUM(x))
#define PG_GETARG_SKYREGION(n) DatumGetSkyRegion(PG_GETARG_DATUM(n))
#define SKYREGION_NVERT(r) ((VARSIZE(r) - offsetof(SkyRegion, v)) / (2 * sizeof(double)))

static SkyPos *
skypos_make(double ra, double dec)
{
	SkyPos	   *p;

	if (!(dec >= -90.0 && dec <= 90.0))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("declination %g out of range [-90, 90]", dec)));
	if (!isfinite(ra))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("right ascension must be finite")));
	p = (SkyPos *) palloc(sizeof(SkyPos));
	p->ra = ra - floor(ra / 360.0) * 360.0;
	p->dec = dec;
	return p;
}

static SkyRegion *
region_cone(double ra, double dec, double radius)
{
	Size		sz = offsetof(SkyRegion, v) + 3 * sizeof(double);
	SkyRegion  *r = (SkyRegion *) palloc0(sz);
	SkyPos	   *c = skypos_make(ra, dec);

	SET_VARSIZE(r, sz);
	r->kind = SKY_CONE;
	r->v[0] = c->ra;
	r->v[1] = c->dec;
	r->v[2] = radius;
	return r;
}

static SkyRegion *
region_poly(int nv, const double *coords)
{
	Size		sz = offsetof(SkyRegion, v) + 2 * nv * sizeof(double);
	SkyRegion  *r;
	sc_region	probe;

	if (nv < 3)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("a polygon needs at least 3 vertices")));
	r = (SkyRegion *) palloc0(sz);
	SET_VARSIZE(r, sz);
	r->kind = SKY_POLY;
	for (int i = 0; i < 2 * nv; i++)
		r->v[i] = coords[i];

	/* refuse a polygon the geometry cannot handle, at construction */
	{
		double	   *ra = palloc(sizeof(double) * nv),
				   *dec = palloc(sizeof(double) * nv);

		for (int i = 0; i < nv; i++)
		{
			ra[i] = coords[2 * i];
			dec[i] = coords[2 * i + 1];
		}
		check_err(sc_region_poly(&probe, nv, ra, dec));
		sc_region_free(&probe);
		pfree(ra);
		pfree(dec);
	}
	return r;
}

/* a skypos datum as the covering code understands it: a unit vector */
sc_vec3
skycell_pos_from_datum(Datum d)
{
	SkyPos	   *p = DatumGetSkyPos(d);

	return sc_radec2vec(p->ra, p->dec);
}

/* a skyregion datum as the covering code understands it */
void
skycell_region_from_datum(Datum d, sc_region *out)
{
	SkyRegion  *r = DatumGetSkyRegion(d);

	if (r->kind == SKY_CONE)
		check_err(sc_region_cone(out, r->v[0], r->v[1], r->v[2]));
	else
	{
		int			nv = SKYREGION_NVERT(r);
		double	   *ra = palloc(sizeof(double) * nv),
				   *dec = palloc(sizeof(double) * nv);

		for (int i = 0; i < nv; i++)
		{
			ra[i] = r->v[2 * i];
			dec[i] = r->v[2 * i + 1];
		}
		check_err(sc_region_poly(out, nv, ra, dec));
	}
}

/* ------------------------------------------------------------------ */
/* input and output                                                    */
/* ------------------------------------------------------------------ */

PG_FUNCTION_INFO_V1(skypos_in);
Datum
skypos_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	double		ra,
				dec;
	char		junk;

	if (sscanf(str, " ( %lf , %lf ) %c", &ra, &dec, &junk) == 2 ||
		sscanf(str, " %lf , %lf %c", &ra, &dec, &junk) == 2 ||
		sscanf(str, " %lf %lf %c", &ra, &dec, &junk) == 2)
		PG_RETURN_POINTER(skypos_make(ra, dec));
	ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("invalid input syntax for type skypos: \"%s\"", str),
					errhint("Write a position in degrees, for example (266.4, -28.9).")));
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(skypos_out);
Datum
skypos_out(PG_FUNCTION_ARGS)
{
	SkyPos	   *p = PG_GETARG_SKYPOS(0);

	PG_RETURN_CSTRING(psprintf("(%.15g,%.15g)", p->ra, p->dec));
}

/*
 * skyregion's text form is STC-S (IVOA Space-Time Coordinate string), the
 * same syntax ObsCore's own s_region column carries -- "CIRCLE ICRS ra dec
 * radius" / "POLYGON ICRS ra1 dec1 ra2 dec2 ...", whitespace-separated, no
 * commas or parentheses. That is purely a text-representation choice: every
 * operator, predicate and covering function works on the parsed sc_region
 * this produces, never on the text, so a table already storing STC-S
 * s_region values casts straight into skyregion with no reshaping, and a
 * skyregion column reads back as the STC-S the table stored (a TAP service
 * emits ADQL's own function-call spelling instead, via circle()/polygon()
 * and CONTAINS/INTERSECTS -- this text form is for when the value itself
 * needs to be a string, e.g. loading or re-exporting s_region as-is).
 *
 * The frame token must be ICRS, matching every other coordsys argument in
 * this extension; a small number of further whitespace-separated STC-S
 * tokens (flavor, reference position -- e.g. "CIRCLE ICRS TOPOCENTER ...")
 * are accepted and skipped, since real archives sometimes carry them, but
 * are not otherwise interpreted.
 */
static const char *
stcs_skip_ws(const char *s)
{
	while (isspace((unsigned char) *s))
		s++;
	return s;
}

/* match a keyword at s, case-insensitively, on a token boundary; NULL if no match */
static const char *
stcs_match_word(const char *s, const char *word)
{
	size_t		len = strlen(word);

	if (pg_strncasecmp(s, word, len) != 0)
		return NULL;
	if (isalnum((unsigned char) s[len]))
		return NULL;			/* "CIRCLET..." is not the keyword CIRCLE */
	return stcs_skip_ws(s + len);
}

PG_FUNCTION_INFO_V1(skyregion_in);
Datum
skyregion_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	const char *s = stcs_skip_ws(str);
	const char *t;
	double		vals[512];
	int			n = 0;
	bool		cone;

	if ((t = stcs_match_word(s, "CIRCLE")) != NULL)
		cone = true;
	else if ((t = stcs_match_word(s, "POLYGON")) != NULL)
		cone = false;
	else
		ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("invalid input syntax for type skyregion: \"%s\"", str),
						errhint("Write STC-S: \"CIRCLE ICRS ra dec radius\" or "
								"\"POLYGON ICRS ra1 dec1 ra2 dec2 ...\", degrees.")));
	s = t;

	if ((t = stcs_match_word(s, "ICRS")) == NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("invalid input syntax for type skyregion: \"%s\"", str),
						errhint("The coordinate frame must be ICRS; convert with "
								"gal2icrs() or ecl2icrs() first.")));
	s = t;

	/* skip any further non-numeric STC-S tokens (flavor, refpos, ...) */
	for (;;)
	{
		char	   *end;

		strtod(s, &end);
		if (end != s)
			break;				/* a number starts here: done skipping */
		if (!isalpha((unsigned char) *s))
			break;				/* not a token either: let the loop below report it */
		while (isalnum((unsigned char) *s))
			s++;
		s = stcs_skip_ws(s);
	}

	for (;;)
	{
		char	   *end;

		s = stcs_skip_ws(s);
		if (*s == '\0')
			break;
		if (n >= (int) (sizeof(vals) / sizeof(vals[0])))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("too many coordinates in skyregion")));
		vals[n] = strtod(s, &end);
		if (end == s)
			ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("invalid input syntax for type skyregion: \"%s\"", str)));
		n++;
		s = end;
	}
	if (cone)
	{
		if (n != 3)
			ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("CIRCLE takes ra, dec and radius")));
		PG_RETURN_POINTER(region_cone(vals[0], vals[1], vals[2]));
	}
	if (n < 6 || n % 2 != 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("POLYGON takes at least three ra, dec pairs")));
	PG_RETURN_POINTER(region_poly(n / 2, vals));
}

PG_FUNCTION_INFO_V1(skyregion_out);
Datum
skyregion_out(PG_FUNCTION_ARGS)
{
	SkyRegion  *r = PG_GETARG_SKYREGION(0);
	StringInfoData buf;

	initStringInfo(&buf);
	if (r->kind == SKY_CONE)
		appendStringInfo(&buf, "CIRCLE ICRS %.15g %.15g %.15g", r->v[0], r->v[1], r->v[2]);
	else
	{
		int			nv = SKYREGION_NVERT(r);

		appendStringInfoString(&buf, "POLYGON ICRS");
		for (int i = 0; i < nv; i++)
			appendStringInfo(&buf, " %.15g %.15g", r->v[2 * i], r->v[2 * i + 1]);
	}
	PG_RETURN_CSTRING(buf.data);
}

/* ------------------------------------------------------------------ */
/* ADQL constructors                                                   */
/* ------------------------------------------------------------------ */

/* ADQL's coordinate system argument: accepted, and required to be ICRS */
static void
check_coordsys(text *cs)
{
	char	   *s = text_to_cstring(cs);

	if (*s != '\0' && pg_strcasecmp(s, "ICRS") != 0 && pg_strcasecmp(s, "J2000") != 0)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("coordinate system \"%s\" is not supported", s),
						errhint("skycell works in ICRS; convert with gal2icrs() or ecl2icrs().")));
	pfree(s);
}

PG_FUNCTION_INFO_V1(skycell_point);
Datum
skycell_point(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(skypos_make(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1)));
}

PG_FUNCTION_INFO_V1(skycell_point_cs);
Datum
skycell_point_cs(PG_FUNCTION_ARGS)
{
	check_coordsys(PG_GETARG_TEXT_PP(0));
	PG_RETURN_POINTER(skypos_make(PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2)));
}

PG_FUNCTION_INFO_V1(skycell_circle);
Datum
skycell_circle(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(region_cone(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1),
								  PG_GETARG_FLOAT8(2)));
}

PG_FUNCTION_INFO_V1(skycell_circle_cs);
Datum
skycell_circle_cs(PG_FUNCTION_ARGS)
{
	check_coordsys(PG_GETARG_TEXT_PP(0));
	PG_RETURN_POINTER(region_cone(PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2),
								  PG_GETARG_FLOAT8(3)));
}

/* ADQL BOX: centre, width and height in degrees, as a four-corner polygon */
static SkyRegion *
box_region(double ra, double dec, double w, double h)
{
	double		hw = w / 2,
				hh = h / 2,
				d1 = dec - hh,
				d2 = dec + hh,
				coords[8];
	double		s1,
				s2;

	if (w <= 0 || h <= 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("BOX needs a positive width and height")));
	if (d1 <= -90 || d2 >= 90)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("BOX reaches a pole; use a POLYGON or a CIRCLE instead")));
	/* the width is an angle on the sky, so it opens out in right ascension */
	s1 = hw / cos(d1 * DEG2RAD);
	s2 = hw / cos(d2 * DEG2RAD);
	if (s1 >= 90 || s2 >= 90)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("BOX is too wide at this declination")));
	coords[0] = ra - s1;
	coords[1] = d1;
	coords[2] = ra + s1;
	coords[3] = d1;
	coords[4] = ra + s2;
	coords[5] = d2;
	coords[6] = ra - s2;
	coords[7] = d2;
	return region_poly(4, coords);
}

PG_FUNCTION_INFO_V1(skycell_box);
Datum
skycell_box(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(box_region(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1),
								 PG_GETARG_FLOAT8(2), PG_GETARG_FLOAT8(3)));
}

PG_FUNCTION_INFO_V1(skycell_box_cs);
Datum
skycell_box_cs(PG_FUNCTION_ARGS)
{
	check_coordsys(PG_GETARG_TEXT_PP(0));
	PG_RETURN_POINTER(box_region(PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2),
								 PG_GETARG_FLOAT8(3), PG_GETARG_FLOAT8(4)));
}

static SkyRegion *
polygon_from_array(ArrayType *arr)
{
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	double	   *coords;
	SkyRegion  *r;

	if (ARR_ELEMTYPE(arr) != FLOAT8OID)
		elog(ERROR, "polygon coordinates must be double precision");
	deconstruct_array(arr, FLOAT8OID, sizeof(float8), true, TYPALIGN_DOUBLE,
					  &elems, &nulls, &n);
	if (n < 6 || n % 2 != 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("POLYGON takes at least three ra, dec pairs")));
	coords = palloc(sizeof(double) * n);
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							errmsg("polygon coordinates must not be NULL")));
		coords[i] = DatumGetFloat8(elems[i]);
	}
	r = region_poly(n / 2, coords);
	pfree(coords);
	return r;
}

PG_FUNCTION_INFO_V1(skycell_polygon);
Datum
skycell_polygon(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(polygon_from_array(PG_GETARG_ARRAYTYPE_P(0)));
}

PG_FUNCTION_INFO_V1(skycell_polygon_cs);
Datum
skycell_polygon_cs(PG_FUNCTION_ARGS)
{
	check_coordsys(PG_GETARG_TEXT_PP(0));
	PG_RETURN_POINTER(polygon_from_array(PG_GETARG_ARRAYTYPE_P(1)));
}

/* ------------------------------------------------------------------ */
/* predicates and measures                                             */
/* ------------------------------------------------------------------ */

static sc_vec3
pos_vec(SkyPos *p)
{
	return sc_radec2vec(p->ra, p->dec);
}

static bool
pos_in_region(SkyPos *p, Datum region)
{
	sc_region	reg;
	bool		res;

	skycell_region_from_datum(region, &reg);
	res = sc_region_contains(&reg, pos_vec(p)) != 0;
	sc_region_free(&reg);
	return res;
}

PG_FUNCTION_INFO_V1(skycell_pos_in_region);
Datum
skycell_pos_in_region(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(pos_in_region(PG_GETARG_SKYPOS(0), PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(skycell_region_has_pos);
Datum
skycell_region_has_pos(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(pos_in_region(PG_GETARG_SKYPOS(1), PG_GETARG_DATUM(0)));
}

/* the exact test the rewrite appends; the last argument is a selectivity hint */
PG_FUNCTION_INFO_V1(skycell_pos_in_region_sel);
Datum
skycell_pos_in_region_sel(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(pos_in_region(PG_GETARG_SKYPOS(0), PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(skycell_contains);
Datum
skycell_contains(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(pos_in_region(PG_GETARG_SKYPOS(0), PG_GETARG_DATUM(1)) ? 1 : 0);
}

static int
region_region(PG_FUNCTION_ARGS, bool contains)
{
	sc_region	a,
				b;
	int			res;

	skycell_region_from_datum(PG_GETARG_DATUM(0), &a);
	skycell_region_from_datum(PG_GETARG_DATUM(1), &b);
	res = contains ? sc_region_contains_region(&a, &b) : sc_region_overlaps(&a, &b);
	sc_region_free(&a);
	sc_region_free(&b);
	return res ? 1 : 0;
}

PG_FUNCTION_INFO_V1(skycell_contains_region);
Datum
skycell_contains_region(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(region_region(fcinfo, true));
}

PG_FUNCTION_INFO_V1(skycell_intersects);
Datum
skycell_intersects(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(region_region(fcinfo, false));
}

PG_FUNCTION_INFO_V1(skycell_intersects_pos);
Datum
skycell_intersects_pos(PG_FUNCTION_ARGS)
{
	/* a point has no area: intersecting one is being contained in it */
	PG_RETURN_INT32(pos_in_region(PG_GETARG_SKYPOS(0), PG_GETARG_DATUM(1)) ? 1 : 0);
}

PG_FUNCTION_INFO_V1(skycell_region_overlap);
Datum
skycell_region_overlap(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(region_region(fcinfo, false) != 0);
}

PG_FUNCTION_INFO_V1(skycell_region_covers);
Datum
skycell_region_covers(PG_FUNCTION_ARGS)
{
	/* @>(a, b) must mean "a contains b" -- every point of b is in a --
	 * the reverse of region_region(fcinfo, true)'s own ADQL CONTAINS(a, b)
	 * convention ("every point of a is in b"), which skycell_contains_region
	 * above correctly reuses unchanged. Swap the arguments going into
	 * sc_region_contains_region instead of reusing region_region() as-is:
	 * @>(a, b) was calling region_region(fcinfo, true), i.e.
	 * sc_region_contains_region(a, b) = "a is inside b", backwards from
	 * what @> and its own documentation have always said it computes.
	 * Caught because it was never actually indexed until this file's own
	 * new GiST strategy sent someone looking for the exact test it
	 * reuses; the only regression test for this function,
	 * skycell_region_covers(p, p), is symmetric and could never catch a
	 * reversed argument order. */
	sc_region	a,
				b;
	int			res;

	skycell_region_from_datum(PG_GETARG_DATUM(0), &a);
	skycell_region_from_datum(PG_GETARG_DATUM(1), &b);
	res = sc_region_contains_region(&b, &a);
	sc_region_free(&a);
	sc_region_free(&b);
	PG_RETURN_BOOL(res != 0);
}

PG_FUNCTION_INFO_V1(skycell_region_covered_by);
Datum
skycell_region_covered_by(PG_FUNCTION_ARGS)
{
	/* <@(a, b) must mean "a is contained by b" -- every point of a is in b
	 * -- exactly region_region(fcinfo, true)'s own ADQL CONTAINS(a, b)
	 * convention, the same one skycell_contains_region above already uses
	 * correctly. Unlike skycell_region_covers (@>), this needs no argument
	 * swap: it can reuse region_region() directly. */
	PG_RETURN_BOOL(region_region(fcinfo, true) != 0);
}

PG_FUNCTION_INFO_V1(skycell_distance);
Datum
skycell_distance(PG_FUNCTION_ARGS)
{
	SkyPos	   *a = PG_GETARG_SKYPOS(0),
			   *b = PG_GETARG_SKYPOS(1);

	PG_RETURN_FLOAT8(sc_angle(pos_vec(a), pos_vec(b)) * RAD2DEG);
}

PG_FUNCTION_INFO_V1(skycell_area);
Datum
skycell_area(PG_FUNCTION_ARGS)
{
	sc_region	r;
	double		a;

	skycell_region_from_datum(PG_GETARG_DATUM(0), &r);
	a = r.area * RAD2DEG * RAD2DEG;
	sc_region_free(&r);
	PG_RETURN_FLOAT8(a);
}

PG_FUNCTION_INFO_V1(skycell_coord1);
Datum
skycell_coord1(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(PG_GETARG_SKYPOS(0)->ra);
}

PG_FUNCTION_INFO_V1(skycell_coord2);
Datum
skycell_coord2(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(PG_GETARG_SKYPOS(0)->dec);
}

PG_FUNCTION_INFO_V1(skycell_centroid);
Datum
skycell_centroid(PG_FUNCTION_ARGS)
{
	sc_region	r;
	sc_vec3		c;
	double		ra,
				dec;

	skycell_region_from_datum(PG_GETARG_DATUM(0), &r);
	c = sc_region_centroid(&r);
	sc_region_free(&r);
	dec = asin(fmax(-1.0, fmin(1.0, c.z))) * RAD2DEG;
	ra = atan2(c.y, c.x) * RAD2DEG;
	PG_RETURN_POINTER(skypos_make(ra, dec));
}

/* the index key of a position: the order-29 HEALPix cell */
PG_FUNCTION_INFO_V1(skycell_cell);
Datum
skycell_cell(PG_FUNCTION_ARGS)
{
	SkyPos	   *p = PG_GETARG_SKYPOS(0);

	PG_RETURN_INT64(sc_ang2pix(SC_MAX_ORDER, p->ra, p->dec));
}

PG_FUNCTION_INFO_V1(skycell_pos_ra_dec);
Datum
skycell_pos_ra_dec(PG_FUNCTION_ARGS)
{
	SkyPos	   *p = PG_GETARG_SKYPOS(0);
	Datum		d[2];

	d[0] = Float8GetDatum(p->ra);
	d[1] = Float8GetDatum(p->dec);
	PG_RETURN_ARRAYTYPE_P(construct_array(d, 2, FLOAT8OID, sizeof(float8), true, TYPALIGN_DOUBLE));
}

/* the four corners of a cell, as ra, dec, ra, dec, ... (degrees) */
PG_FUNCTION_INFO_V1(skycell_cell_corners);
Datum
skycell_cell_corners(PG_FUNCTION_ARGS)
{
	int32		order = PG_GETARG_INT32(0);
	int64		pix = PG_GETARG_INT64(1);
	sc_vec3		c[4];
	Datum		d[8];

	if (order < 0 || order > SC_MAX_ORDER)
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("order must be within [0, %d]", SC_MAX_ORDER)));
	if (pix < 0 || pix >= ((int64) 12 << (2 * order)))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("cell index out of range for order %d", order)));
	sc_pix_corners(order, pix, c);
	for (int i = 0; i < 4; i++)
	{
		d[2 * i] = Float8GetDatum(atan2(c[i].y, c[i].x) * RAD2DEG);
		d[2 * i + 1] = Float8GetDatum(asin(fmax(-1.0, fmin(1.0, c[i].z))) * RAD2DEG);
	}
	PG_RETURN_ARRAYTYPE_P(construct_array(d, 8, FLOAT8OID, sizeof(float8), true, TYPALIGN_DOUBLE));
}

/* ------------------------------------------------------------------ */
/* making `point(...) <@ region` an index scan                         */
/* ------------------------------------------------------------------ */

/*
 * The cell expression this predicate could be answered from, given the
 * position argument: skycell_cell(pos) for a stored skypos, or
 * skycell_ang2cell(ra, dec) for a point() built from two columns.  Unlike
 * skycell_cone(), the caller never writes it -- the expression is built here
 * and kept only if the relation actually carries an index on it.
 */
static Node *
cell_expr_for_point(Oid selfid, Node *pt)
{
	Oid			argtypes[2] = {FLOAT8OID, FLOAT8OID};

	if (IsA(pt, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) pt;
		char	   *name = get_func_name(f->funcid);
		List	   *args = f->args;

		if (name != NULL && (strcmp(name, "point") == 0 ||
							 strcmp(name, "skycell_point") == 0))
		{
			/* point('ICRS', ra, dec) and point(ra, dec) */
			if (list_length(args) == 3)
				args = list_delete_first(list_copy(args));
			if (list_length(args) == 2)
				return (Node *) makeFuncExpr(lookup_sibling_func(selfid, "skycell_ang2cell", 2, argtypes),
											 INT8OID, list_copy(args),
											 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
		}
		return NULL;
	}
	if (IsA(pt, Var))
	{
		Oid			postype = exprType(pt);
		Oid			one[1] = {postype};

		return (Node *) makeFuncExpr(lookup_sibling_func(selfid, "skycell_cell", 1, one),
									 INT8OID, list_make1(copyObject(pt)),
									 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	}
	return NULL;
}

/*
 * The actual rewrite, shared by both argument orders: "point is in region"
 * and "region contains point" are the same boolean regardless of which side
 * of the operator wrote it, so once pt/rg are identified (by whichever
 * caller below extracted them from its own operator's argument order), the
 * rest of the rewrite -- the covering, the exact test, the range arms -- is
 * completely order-independent.  funcid is only used to resolve sibling
 * function names into the caller's own namespace (lookup_sibling_func), so
 * it does not need to be the same function whose args pt/rg came from.
 */
/*
 * "Which of my regions contain this point" (rg not constant, no index on
 * the point side for the classic rewrite below to use): the GIN-indexed
 * alternative to the region GiST opclass's own @>(region,point) strategy,
 * built from a plain `CREATE INDEX ... USING gin (skycell_region_moc(rg))`
 * -- see gin_moc_index_for_region()'s own comment in skycell.c for why
 * max_order comes from the index's own definition rather than a guess, and
 * GIST_REGION_DESIGN.md's "Round eight" for the numbers this rewrite is
 * for. Returns NULL (not an error) if no such index exists or the array
 * overlap operator can't be resolved, so the caller falls back exactly as
 * it always has.
 */
static Node *
gin_region_rewrite(Oid funcid, Node *pt, Node *rg, Node *cell,
					Node *moc_expr, int max_order)
{
	Oid			ancestors_types[3] = {INT8OID, INT4OID, INT4OID};
	Oid			exact_types[3];
	FuncExpr   *ancestors;
	Expr	   *overlap;
	FuncExpr   *exact;

	ancestors = makeFuncExpr(lookup_sibling_func(funcid, "skycell_ancestors", 3, ancestors_types),
							 INT8ARRAYOID,
							 list_make3(copyObject(cell), int4_const(0), int4_const(max_order)),
							 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	overlap = array_overlap_expr(moc_expr, (Expr *) ancestors);
	if (overlap == NULL)
		return NULL;

	exact_types[0] = exprType(pt);
	exact_types[1] = exprType(rg);
	exact_types[2] = FLOAT8OID;
	exact = makeFuncExpr(lookup_sibling_func(funcid, "skycell_in_region", 3, exact_types),
						 BOOLOID,
						 list_make3(copyObject(pt), copyObject(rg), float8_const(-1.0)),
						 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	return (Node *) makeBoolExpr(AND_EXPR, list_make2(overlap, (Expr *) exact), -1);
}

static Node *
region_support_simplify(SupportRequestSimplify *req, Oid funcid, Node *pt, Node *rg)
{
	Node	   *cell;
	sc_density	dens;
	Oid			statrel = InvalidOid;
	Oid			exact_types[3];
	bool		uses_cell_ops;

	if (req->root == NULL)
		return NULL;

	cell = cell_expr_for_point(funcid, pt);
	if (cell == NULL)
		return NULL;

	/* only worth rewriting if an index answers that expression */
	if (!density_for_expr(req->root, cell, &dens, &statrel, &uses_cell_ops))
	{
		Node	   *moc_expr;
		int			max_order;

		/* the classic rewrite below needs a point-side index either way;
		 * absent one, "many regions" (rg not constant) has its own
		 * possible index instead -- see gin_region_rewrite() above. A
		 * constant rg has no column to carry such an index, so this is
		 * skipped rather than attempted and failing every time. */
		if (!IsA(rg, Const) &&
			gin_moc_index_for_region(req->root, rg, &moc_expr, &max_order))
			return gin_region_rewrite(funcid, pt, rg, cell, moc_expr, max_order);
		return NULL;
	}

	exact_types[0] = exprType(pt);
	exact_types[1] = exprType(rg);
	exact_types[2] = FLOAT8OID;

	if (IsA(rg, Const))
	{
		sc_region	reg;
		sc_cover	cov;
		sc_cover_params p;
		double		sel;
		FuncExpr   *exact;
		double		ra0 = 0,
					dec0 = 0,
					radius = -1;

		if (((Const *) rg)->constisnull)
			return makeBoolConst(false, true);

		skycell_region_from_datum(((Const *) rg)->constvalue, &reg);
		current_params(&p, 64, &dens);
		if (reg.kind == SC_REGION_CONE)
		{
			ra0 = atan2(reg.center.y, reg.center.x) * RAD2DEG;
			dec0 = asin(fmax(-1.0, fmin(1.0, reg.center.z))) * RAD2DEG;
			radius = reg.radius * RAD2DEG;
			cover_cached(&reg, &dens, &p, statrel, ra0, dec0, radius, &cov);
		}
		else
			sc_cover_compute(&reg, &dens, &p, &cov);
		sel = (cov.area > 0) ? fmin(1.0, reg.area / cov.area) : 0.0;

		/*
		 * Decline the rewrite when the covering's own cost model expects
		 * too many rows to be fetched from the heap only to be rejected by
		 * the exact test below (skycell.rewrite_max_waste's own comment has
		 * the full story). Measured directly, not guessed: at small radii
		 * this waste is a handful of rows regardless of how poor the
		 * covering's *ratio* looks (sel near 0 but cov.exp_rows tiny), and
		 * the rewrite wins outright; past roughly a hundred wasted rows
		 * (when the table is expected to be cache-resident -- see
		 * rewrite_waste_threshold()'s own comment for why that qualifier
		 * matters and how the threshold scales when it isn't) it starts
		 * costing more in exact-test CPU than the ranges save in heap I/O,
		 * and a GiST-family index (when one exists) wins instead.
		 * Returning NULL here leaves the original <@/@> clause in place for
		 * the planner's normal cost-based index selection to consider --
		 * skycell_pos_region_sel/skycell_region_pos_sel give it a real,
		 * non-default selectivity estimate for that clause either way.
		 *
		 * cov is NOT freed here (unlike reg): for a cone, cover_cached()
		 * hands back entry->r, a pointer straight into its own long-lived
		 * cache entry (skycell_cache_cxt), not a caller-owned copy -- the
		 * existing success path below never frees cov either, for exactly
		 * that reason. sc_cover_free() on it would pfree the cache's own
		 * backing array out from under every future cache hit for this key,
		 * corrupting that memory context (found the hard way: a segfault
		 * several unrelated queries later, in whatever next allocation hit
		 * the corrupted free list -- see GIST_REGION_DESIGN.md).
		 */
		if (cov.exp_rows * (1.0 - sel) > rewrite_waste_threshold(&dens))
		{
			sc_region_free(&reg);
			return NULL;
		}

		exact = makeFuncExpr(lookup_sibling_func(funcid, "skycell_in_region", 3, exact_types),
							 BOOLOID,
							 list_make3(copyObject(pt), copyObject(rg), float8_const(sel)),
							 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
		return ranges_and_exact(&cov, cell, (Expr *) exact, uses_cell_ops);
	}
	else
	{
		/*
		 * Run-time slots, one covering per distinct region row -- the
		 * generic-region analogue of simplify_cone's/simplify_poly's own
		 * non-constant branch, via skycell_region_bound (which
		 * dispatches on the stored value's own kind tag, same as
		 * skycell_region_from_datum above). A cross-match against
		 * another table's per-row skyregion column (e.g. an archive's
		 * s_region footprint) reaches an index this way instead of the
		 * sequential scan it fell back to before.
		 */
		Oid			bound_types[5] = {InvalidOid, INT4OID, INT4OID, FLOAT8OID, INT8ARRAYOID};
		Oid			bound_oid;
		int			k = skycell_join_slots;
		Oid			arm_opfamily = uses_cell_ops
			? cell_ops_opfamily() : INTEGER_BTREE_FAM_OID;
		Const	   *hist;
		List	   *arms = NIL;
		Datum	   *hd = palloc(sizeof(Datum) * Max(dens.nbounds, 1));
		FuncExpr   *exact;

		bound_types[0] = exact_types[1];
		bound_oid = lookup_sibling_func(funcid, "skycell_region_bound", 5, bound_types);

		for (int i = 0; i < dens.nbounds; i++)
			hd[i] = Int64GetDatum(dens.bounds[i]);
		hist = makeConst(INT8ARRAYOID, -1, InvalidOid, -1,
						 PointerGetDatum(construct_array_builtin(hd, dens.nbounds, INT8OID)),
						 false, false);

		for (int s = 0; s < k; s++)
		{
			Expr	   *b[2];

			for (int j = 0; j < 2; j++)
				b[j] = (Expr *) makeFuncExpr(bound_oid, INT8OID,
											 list_make3(copyObject(rg),
														int4_const(2 * s + j),
														int4_const(k)),
											 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
			for (int j = 0; j < 2; j++)
				((FuncExpr *) b[j])->args = lappend(lappend(((FuncExpr *) b[j])->args,
															float8_const(dens.ntotal)),
													copyObject(hist));
			arms = lappend(arms, range_arm_family(cell, b[0], b[1], arm_opfamily));
		}
		exact = makeFuncExpr(lookup_sibling_func(funcid, "skycell_in_region", 3, exact_types),
							 BOOLOID,
							 list_make3(copyObject(pt), copyObject(rg), float8_const(-1.0)),
							 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
		return (Node *) makeBoolExpr(AND_EXPR,
									 list_make2(k == 1 ? linitial(arms) : makeBoolExpr(OR_EXPR, arms, -1),
												exact),
									 -1);
	}
}

/* skycell_pos_in_region(p skypos, r skyregion) -- backs <@(skypos,skyregion) */
PG_FUNCTION_INFO_V1(skycell_region_support);
Datum
skycell_region_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		Node	   *pt = linitial(req->fcall->args);
		Node	   *rg = lsecond(req->fcall->args);
		Node	   *result = region_support_simplify(req, req->fcall->funcid, pt, rg);

		if (result != NULL)
			PG_RETURN_POINTER(result);
	}
	PG_RETURN_POINTER(NULL);
}

/*
 * skycell_region_has_pos(r skyregion, p skypos) -- backs @>(skyregion,skypos),
 * <@'s commutator. Same rewrite, region and point simply swapped at the
 * call site before handing off to the shared implementation: this is the
 * entire fix for @> silently falling back to a sequential scan while <@,
 * the otherwise-identical reversed spelling, was already indexed.
 */
PG_FUNCTION_INFO_V1(skycell_region_has_pos_support);
Datum
skycell_region_has_pos_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		Node	   *rg = linitial(req->fcall->args);
		Node	   *pt = lsecond(req->fcall->args);
		Node	   *result = region_support_simplify(req, req->fcall->funcid, pt, rg);

		if (result != NULL)
			PG_RETURN_POINTER(result);
	}
	PG_RETURN_POINTER(NULL);
}

/* selectivity of the exact test: the hint the rewrite computed */
PG_FUNCTION_INFO_V1(skycell_region_sel_support);
Datum
skycell_region_sel_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSelectivity))
	{
		SupportRequestSelectivity *req = (SupportRequestSelectivity *) rawreq;
		Node	   *last = llast(req->args);
		double		s = -1;

		if (IsA(last, Const) && !((Const *) last)->constisnull)
			s = DatumGetFloat8(((Const *) last)->constvalue);
		if (s < 0 && list_length(req->args) >= 2 && IsA(lsecond(req->args), Const) &&
			!((Const *) lsecond(req->args))->constisnull)
		{
			sc_region	reg;

			skycell_region_from_datum(((Const *) lsecond(req->args))->constvalue, &reg);
			s = reg.area / (4.0 * M_PI);
			sc_region_free(&reg);
		}
		req->selectivity = fmin(1.0, fmax(s < 0 ? 1e-4 : s, 1e-12));
		PG_RETURN_POINTER(req);
	}
	PG_RETURN_POINTER(NULL);
}

/*
 * Restriction selectivity for the raw, un-rewritten <@(skypos,skyregion) and
 * @>(skyregion,skypos) operators -- the ones a GiST or SP-GiST index on
 * skypos indexes directly (skypos_spgist_ops, gist_point_cap.c's
 * experimental skypos_cap_gist_ops), with no B-tree rewrite in the picture
 * to hand off to skycell_in_region/skycell_region_sel_support above.
 *
 * Both operators are declared with `RESTRICT = contsel` (see their CREATE
 * OPERATOR), PostgreSQL's generic containment-operator fallback, which
 * returns a flat default (effectively ~0.001 of the table) regardless of
 * the actual query radius. A SupportRequestSelectivity handler on the
 * backing function cannot fix this -- per PostgreSQL's own documented rule
 * (nodes/supportnodes.h), a function's support function is never consulted
 * for selectivity when that function is invoked as an operator's
 * implementation, only the operator's own RESTRICT/JOIN estimator is. The
 * fix has to be a plain oprrest-shaped C function assigned via RESTRICT=,
 * the same mechanism contsel itself uses.
 *
 * This was found, not assumed: it explained a real, measured planner
 * misbehaviour -- cat_pos_capgist (58 actual matching rows at a given
 * probe) was costed at a flat 10000-row estimate, which pushed the planner
 * onto a Bitmap Index Scan + Bitmap Heap Scan (built for an estimated-large,
 * low-selectivity result) where pgSphere's own spoint<@scircle, selectivity-
 * aware, got a cheap plain Index Scan instead for the same 58-row result,
 * at a fraction of the wall-clock cost despite touching more buffer pages.
 * See GIST_REGION_DESIGN.md for the numbers.
 *
 * ROUND TWO -- DENSITY-AWARE: a first version of this fix used only
 * area(region)/4pi, a uniform-sky estimate. Measured against a real,
 * 60%-clustered corpus, it was a clear win for ordinary (sparse-sky)
 * queries but a real regression for queries landing in or near a genuine
 * density cluster: a uniform-sky estimate understates their true row count,
 * which flipped some medium-radius probes from a (correct, needed) Bitmap
 * Scan to a (wrong, slower) plain Index Scan. The fix for that is exactly
 * the machinery region_support_simplify() above already uses for the B-tree
 * rewrite's own cost model: cell_expr_for_point() builds the skycell_cell
 * (pos) expression a point column would be indexed by, density_for_expr()
 * finds that expression index's real ANALYZE histogram (or skycell's own
 * finer multi-order count map, when skycell_density_build() has made one)
 * if the table has one, and sc_cover_compute()/cover_cached() turn that
 * into cov.exp_rows -- an estimate that already reflects genuine clustering,
 * not an assumption of uniformity. reg.area/cov.area narrows that from
 * "rows in the covering" to "rows truly inside the region", the same ratio
 * the rewrite's own exact-test selectivity hint already uses. Falls back to
 * the round-one uniform-sky estimate when no such index exists on the
 * table (this opclass needs no such index itself to function -- only this
 * selectivity estimate benefits from one being present) or the region
 * isn't a compile-time constant.
 */
static double
region_pos_density_sel(Oid selfid, PlannerInfo *root, Node *pt, Node *rg)
{
	sc_region	reg;
	double		fallback;
	double		result;

	if (!IsA(rg, Const) || ((Const *) rg)->constisnull)
		return 1e-4;		/* non-constant or null region: no better guess */

	skycell_region_from_datum(((Const *) rg)->constvalue, &reg);
	fallback = fmin(1.0, fmax(reg.area / (4.0 * M_PI), 1e-12));
	result = fallback;

	if (root != NULL)
	{
		Node	   *cell = cell_expr_for_point(selfid, pt);

		if (cell != NULL)
		{
			sc_density	dens;
			Oid			statrel = InvalidOid;
			bool		uses_cell_ops;

			if (density_for_expr(root, cell, &dens, &statrel, &uses_cell_ops) &&
				dens.ntotal > 0)
			{
				sc_cover_params p;
				sc_cover	cov;
				double		est;

				current_params(&p, 64, &dens);
				if (reg.kind == SC_REGION_CONE)
				{
					double		ra0 = atan2(reg.center.y, reg.center.x) * RAD2DEG;
					double		dec0 = asin(fmax(-1.0, fmin(1.0, reg.center.z))) * RAD2DEG;
					double		radius = reg.radius * RAD2DEG;

					cover_cached(&reg, &dens, &p, statrel, ra0, dec0, radius, &cov);
				}
				else
					sc_cover_compute(&reg, &dens, &p, &cov);

				est = cov.exp_rows * ((cov.area > 0) ? fmin(1.0, reg.area / cov.area) : 0.0);
				result = fmin(1.0, fmax(est / dens.ntotal, 1e-12));
				/* cov not freed: see region_support_simplify's identical
				 * comment above -- cover_cached()'s cone branch aliases its
				 * own long-lived cache entry, not a caller-owned copy. */
			}
		}
	}
	sc_region_free(&reg);
	return result;
}

/* backs <@(skypos,skyregion): point is the left operand, region the right */
PG_FUNCTION_INFO_V1(skycell_pos_region_sel);
Datum
skycell_pos_region_sel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	List	   *args = (List *) PG_GETARG_POINTER(2);

	if (list_length(args) != 2)
		PG_RETURN_FLOAT8(1e-4);
	PG_RETURN_FLOAT8(region_pos_density_sel(fcinfo->flinfo->fn_oid, root,
											 (Node *) linitial(args), (Node *) lsecond(args)));
}

/* backs @>(skyregion,skypos): region is the left operand, point the right */
PG_FUNCTION_INFO_V1(skycell_region_pos_sel);
Datum
skycell_region_pos_sel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	List	   *args = (List *) PG_GETARG_POINTER(2);

	if (list_length(args) != 2)
		PG_RETURN_FLOAT8(1e-4);
	PG_RETURN_FLOAT8(region_pos_density_sel(fcinfo->flinfo->fn_oid, root,
											 (Node *) lsecond(args), (Node *) linitial(args)));
}

/*
 * Selectivity for the region-region operators (&&, @>, <@ between two
 * skyregion values) -- the same bug as skycell_pos_region_sel/
 * skycell_region_pos_sel above, on a different set of operators:
 * skycell_region_overlap/_covers/_covered_by are declared with
 * RESTRICT = areasel/contsel, PostgreSQL's generic, radius-blind
 * defaults, which cost a query region's own size out of the estimate
 * entirely. The fix is the same shape -- an oprrest-shaped RESTRICT
 * function, since none of these three backing functions has (or could
 * usefully have) a SUPPORT clause consulted for selectivity, same rule
 * as before.
 *
 * ROUND TWO -- region_angle_est() below replaces the original "Tier 1"
 * area(const)/4pi estimate (GIST_REGION_DESIGN.md's "Round forty-six")
 * with one that no longer assumes the *other* operand is point-like.
 * "Round forty-seven" measured directly why that assumption matters: on
 * a corpus whose own stored regions have large areas (round forty-two's
 * own large/huge radius bands, not the small-catalogue-footprint case
 * Tier 1 was scoped for), area(const)/4pi underestimated a 67-degree
 * probe's true overlap fraction by 2.2x (estimated 3685 rows, actual
 * 8085), which in turn led the planner into a severe, confirmed-by-
 * disabling-plain-index-scans regression (9,293 buffers via a plain
 * Index Scan against 319 via Bitmap Heap Scan on the *same* index for
 * the *same* query) -- not an inherent, unrelated PostgreSQL cost-model
 * blind spot as that round's own first-pass diagnosis said, but a direct
 * consequence of this selectivity estimate being wrong by over 2x.
 * Confirmed directly: feeding the planner a selectivity close to the
 * true value (by hand, for that diagnosis only) made it choose the
 * correct plan on its own.
 *
 * The fix, "Round forty-eight": typical_region_area() (skycell.c)
 * answered "what's a typical stored region's own area" with a single
 * number pulled from a `CREATE INDEX ... (area(region_col))` expression
 * index's ANALYZE histogram when one exists, converted to an angular
 * radius (area = 2*pi*(1-cos(theta)), inverted) and combined with the
 * other operand's own angle (summed for &&, subtracted for @>/<@). That
 * closed the regression it was built for, but round forty-eight's own
 * STATUS flagged a residual: a single summary number, however chosen,
 * represents a genuinely mixed-scale column poorly, and the pathology
 * it was fixing (a plain Index Scan narrowly, wrongly, beating a Bitmap
 * Heap Scan on the planner's own cost estimate) can still trigger
 * whenever that one number is off by enough -- which it will be for
 * *some* rows whenever the real distribution is wide.
 *
 * "Round forty-nine" removes the single-number step: region_area_
 * histogram() (skycell.c) now returns the *whole* histogram, and
 * node_angles() below converts every one of its bounds to an angle
 * instead of picking one. combine_angles_avg() then averages the
 * selectivity formula itself over the cross product of both operands'
 * angle sets -- exact under an equal-frequency histogram's own implicit
 * model (each bound stands for an equal share of the rows), not a
 * best-guess single point standing in for a population that may have no
 * single typical member at all. A constant operand's "distribution" is
 * just its own one exact angle, so this reduces to round forty-eight's
 * own formula whenever the non-constant side has no matching index
 * (folded in as a single angle of 0, point-like, exactly Tier 1's
 * original assumption) -- another strict generalisation, not a
 * replacement: identical output in every case round forty-eight already
 * got right, closer to the true distribution in the cases it didn't.
 *
 * && sums each pair of angles across both operands' sets (two circles
 * overlap roughly whenever their centres are within the sum of their
 * radii) and averages area(cap of that sum)/4pi over every pair.
 * @>/<@ need the *container* side to offer at least one angle (a
 * constant, or a column with an area() index) to compute anything --
 * falling back to the flat default otherwise, same rule as round forty-
 * eight. Once it does, the *contained* side's own angle set (if any)
 * shrinks the effective containing cap pair by pair, clamped at 0, and
 * averaged the same way -- including, as round forty-eight's own bonus
 * still holds, the "wrong direction" case (contained side constant,
 * container column has an area() index) now answering a real question
 * instead of shrugging at it.
 */
static bool
node_angles(Oid selfid, PlannerInfo *root, Node *n, double **angles, int *count)
{
	if (IsA(n, Const))
	{
		sc_region	reg;
		double	   *a;

		if (((Const *) n)->constisnull)
			return false;
		skycell_region_from_datum(((Const *) n)->constvalue, &reg);
		a = palloc(sizeof(double));
		a[0] = acos(fmax(-1.0, fmin(1.0, 1.0 - reg.area / (2.0 * M_PI))));
		sc_region_free(&reg);
		*angles = a;
		*count = 1;
		return true;
	}
	else
	{
		double	   *areas;		/* square degrees, region_area_histogram()'s
								 * own unit -- see that function's header
								 * comment for why not steradians */
		int			n_areas;

		if (!region_area_histogram(selfid, root, n, &areas, &n_areas))
			return false;
		for (int i = 0; i < n_areas; i++)
		{
			double		area_sr = areas[i] / (RAD2DEG * RAD2DEG);

			areas[i] = acos(fmax(-1.0, fmin(1.0, 1.0 - area_sr / (2.0 * M_PI))));
		}
		*angles = areas;		/* same buffer, now holding angles */
		*count = n_areas;
		return true;
	}
}

/* area(cap of this angular radius)/4pi, clamped to a sane selectivity range */
static double
cap_frac(double angle)
{
	angle = fmin(M_PI, fmax(0.0, angle));
	return fmin(1.0, fmax((1.0 - cos(angle)) / 2.0, 1e-12));
}

/*
 * cap_frac(a[i] +/- b[j]), averaged over every pair -- a's or b's own
 * angle set standing in for "unknown" (no constant, no area() index) is
 * a single 0 (point-like), not zero pairs, so an entirely-unknown
 * operand degrades to exactly the other operand's own area(.)/4pi
 * rather than to no estimate at all.
 */
static double
combine_angles_avg(double *a, int na, double *b, int nb, bool subtract)
{
	double		zero = 0.0;
	double		sum = 0;

	if (na == 0)
	{
		a = &zero;
		na = 1;
	}
	if (nb == 0)
	{
		b = &zero;
		nb = 1;
	}
	for (int i = 0; i < na; i++)
		for (int j = 0; j < nb; j++)
			sum += cap_frac(subtract ? (a[i] - b[j]) : (a[i] + b[j]));
	return sum / ((double) na * (double) nb);
}

/* backs &&(skyregion,skyregion): symmetric, average over both sides' angle sets */
PG_FUNCTION_INFO_V1(skycell_region_overlap_sel);
Datum
skycell_region_overlap_sel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	double	   *a = NULL,
			   *b = NULL;
	int			na = 0,
				nb = 0;
	bool		have_a,
				have_b;

	if (list_length(args) != 2)
		PG_RETURN_FLOAT8(1e-4);
	have_a = node_angles(fcinfo->flinfo->fn_oid, root, (Node *) linitial(args), &a, &na);
	have_b = node_angles(fcinfo->flinfo->fn_oid, root, (Node *) lsecond(args), &b, &nb);
	if (!have_a && !have_b)
		PG_RETURN_FLOAT8(1e-4);
	PG_RETURN_FLOAT8(combine_angles_avg(a, na, b, nb, false));
}

/* backs @>(skyregion,skyregion): LEFTARG is the container, RIGHTARG the contained */
PG_FUNCTION_INFO_V1(skycell_region_covers_sel);
Datum
skycell_region_covers_sel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	double	   *container = NULL,
			   *contained = NULL;
	int			ncontainer = 0,
				ncontained = 0;

	if (list_length(args) != 2)
		PG_RETURN_FLOAT8(1e-4);
	if (!node_angles(fcinfo->flinfo->fn_oid, root, (Node *) linitial(args), &container, &ncontainer))
		PG_RETURN_FLOAT8(1e-4);
	(void) node_angles(fcinfo->flinfo->fn_oid, root, (Node *) lsecond(args), &contained, &ncontained);
	PG_RETURN_FLOAT8(combine_angles_avg(container, ncontainer, contained, ncontained, true));
}

/* backs <@(skyregion,skyregion): RIGHTARG is the container, LEFTARG the contained */
PG_FUNCTION_INFO_V1(skycell_region_covered_by_sel);
Datum
skycell_region_covered_by_sel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	double	   *container = NULL,
			   *contained = NULL;
	int			ncontainer = 0,
				ncontained = 0;

	if (list_length(args) != 2)
		PG_RETURN_FLOAT8(1e-4);
	if (!node_angles(fcinfo->flinfo->fn_oid, root, (Node *) lsecond(args), &container, &ncontainer))
		PG_RETURN_FLOAT8(1e-4);
	(void) node_angles(fcinfo->flinfo->fn_oid, root, (Node *) linitial(args), &contained, &ncontained);
	PG_RETURN_FLOAT8(combine_angles_avg(container, ncontainer, contained, ncontained, true));
}
