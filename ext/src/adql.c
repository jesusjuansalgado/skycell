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

#include <math.h>

#include "access/stratnum.h"
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

PG_FUNCTION_INFO_V1(skyregion_in);
Datum
skyregion_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	char	   *s = str;
	double		vals[512];
	int			n = 0;
	bool		cone;

	while (*s == ' ')
		s++;
	if (pg_strncasecmp(s, "CIRCLE", 6) == 0)
	{
		cone = true;
		s += 6;
	}
	else if (pg_strncasecmp(s, "POLYGON", 7) == 0)
	{
		cone = false;
		s += 7;
	}
	else
		ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("invalid input syntax for type skyregion: \"%s\"", str),
						errhint("Write CIRCLE(ra, dec, radius) or POLYGON(ra1, dec1, ...), in degrees.")));
	while (*s == ' ')
		s++;
	if (*s++ != '(')
		ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("invalid input syntax for type skyregion: \"%s\"", str)));
	for (;;)
	{
		char	   *end;

		while (*s == ' ' || *s == ',')
			s++;
		if (*s == ')' || *s == '\0')
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
		appendStringInfo(&buf, "CIRCLE(%.15g,%.15g,%.15g)", r->v[0], r->v[1], r->v[2]);
	else
	{
		int			nv = SKYREGION_NVERT(r);

		appendStringInfoString(&buf, "POLYGON(");
		for (int i = 0; i < nv; i++)
			appendStringInfo(&buf, "%s%.15g,%.15g", i ? "," : "", r->v[2 * i], r->v[2 * i + 1]);
		appendStringInfoChar(&buf, ')');
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
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(d, 2, FLOAT8OID));
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
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(d, 8, FLOAT8OID));
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

PG_FUNCTION_INFO_V1(skycell_region_support);
Datum
skycell_region_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		FuncExpr   *fexpr = req->fcall;
		Node	   *pt = linitial(fexpr->args);
		Node	   *rg = lsecond(fexpr->args);
		Node	   *cell;
		sc_region	reg;
		sc_cover	cov;
		sc_cover_params p;
		sc_density	dens;
		Oid			statrel = InvalidOid;
		double		sel;
		Oid			exact_types[3];
		FuncExpr   *exact;
		double		ra0 = 0,
					dec0 = 0,
					radius = -1;

		if (!IsA(rg, Const) || req->root == NULL)
			PG_RETURN_POINTER(NULL);
		if (((Const *) rg)->constisnull)
			PG_RETURN_POINTER(makeBoolConst(false, true));

		cell = cell_expr_for_point(fexpr->funcid, pt);
		if (cell == NULL)
			PG_RETURN_POINTER(NULL);

		/* only worth rewriting if an index answers that expression */
		if (!density_for_expr(req->root, cell, &dens, &statrel))
			PG_RETURN_POINTER(NULL);

		skycell_region_from_datum(((Const *) rg)->constvalue, &reg);
		current_params(&p, 64);
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

		exact_types[0] = exprType(pt);
		exact_types[1] = exprType(rg);
		exact_types[2] = FLOAT8OID;
		exact = makeFuncExpr(lookup_sibling_func(fexpr->funcid, "skycell_in_region", 3, exact_types),
							 BOOLOID,
							 list_make3(copyObject(pt), copyObject(rg), float8_const(sel)),
							 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
		PG_RETURN_POINTER(ranges_and_exact(&cov, cell, (Expr *) exact));
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
