/*
 * gist_region.c -- a GiST opclass for skyregion, indexing region-region
 * INTERSECTS (&&) directly.
 *
 * skyregion has no per-row covering the way a point column does (skycell_
 * cone_bound and friends): both sides of a region-region test are extended
 * shapes, so there is no single outer row to build a covering from.  The
 * MOC-ranges recipe (skycell_region_moc_ranges, see skycell--0.9.sql) works
 * around that with a hand-maintained side table.  This file instead gives
 * skyregion a real index: a GiST opclass whose key is a cheap, fixed-size
 * bounding spherical cap (unit-vector centre + angular radius), the same
 * kind of bound PostGIS's box2df or pg_sphere's spherekey use for their own
 * geometry types.  A cap is:
 *
 *   - exact for a cone (it already is one: sc_region_bounding_cap just
 *     returns its own centre and radius);
 *   - a cheap over-approximation for a polygon (centroid + farthest vertex,
 *     via the existing sc_region_centroid -- not the minimal enclosing cap,
 *     but valid, and reuses machinery cover.c already has for other
 *     purposes).
 *
 * Two caps overlap iff the angle between their centres is at most the sum
 * of their radii -- exactly sc_region_overlaps's own cone-cone formula,
 * lifted to bound *any* region kind uniformly.  That makes the cap test a
 * correct (if lossy) pruning condition for &&: every genuinely overlapping
 * pair of regions has overlapping bounding caps, so a leaf whose cap does
 * not overlap the query's cap can be skipped outright, and a leaf whose cap
 * does overlap needs the real intersects() check (GiST's "recheck") to rule
 * out false positives from the two shapes overlapping in bounding-cap space
 * but not in reality.
 *
 * Support functions: standard GiST (compress/decompress/union/penalty/
 * picksplit/same/consistent), storage type plain bytea -- a packed
 * {cx,cy,cz,radius} struct, no custom SQL type needed.  Modelled on
 * pg_sphere's own spherekey convention (verified compatible with this
 * PostgreSQL version: pg_sphere's GiST indexes are exercised throughout this
 * project's own benchmarks), not on PostgreSQL's textbook point/box
 * example, since a unit-sphere cap has no direct textbook analogue.
 *
 * STATUS: a correctness-first spike, not yet a tuned index. picksplit uses
 * a simple max-distance-seed linear split (R-tree "linear split", not the
 * R*-tree-style split PostgreSQL's own box opclass uses) -- correct, but
 * likely to build a lower-quality tree than a production opclass would.
 * cap_union's near-antipodal case (centres almost pi radians apart) is
 * approximated rather than handled exactly; skyregion itself already
 * forbids a cone larger than a hemisphere and a polygon spanning one, so a
 * single region's own cap cannot be near-antipodal internally, but a union
 * of two widely-separated small regions' caps could approach it. Meant to
 * answer "is this approach even worth pursuing further", not to be merged
 * as-is.
 *
 * picksplit's loop bounds below are OffsetNumber/FirstOffsetNumber, not
 * plain 0-based indices, on purpose: an earlier version treated
 * entryvec->vector[0] as a real entry there (it is not -- that slot is
 * reserved, unlike in union(), which really is 0-based) and crashed the
 * server building an index over as few as ~160 rows, the first row count
 * that forces a page split. See GIST_REGION_DESIGN.md for how that was
 * diagnosed; a correctness test too small to force a split will not catch a
 * regression here.
 */
#include "postgres.h"

#include <math.h>
#include <string.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "cover.h"
#include "healpix.h"
#include "skycell_internal.h"

#define GIST_REGION_STRATEGY_OVERLAP 1	/* && (skyregion, skyregion) */

typedef struct
{
	double		cx,
				cy,
				cz;
	double		radius;			/* radians; negative marks the empty cap */
} GistCap;

/*
 * VARDATA(out) is only 4-byte aligned (right after a short varlena header),
 * not the 8-byte alignment a GistCap's doubles want -- a direct struct
 * pointer dereference there can fault under vectorised load/store codegen.
 * memcpy sidesteps the alignment requirement entirely.
 */
static bytea *
cap_to_bytea(sc_vec3 c, double radius)
{
	Size		sz = VARHDRSZ + sizeof(GistCap);
	bytea	   *out = (bytea *) palloc(sz);
	GistCap		k;

	SET_VARSIZE(out, sz);
	k.cx = c.x;
	k.cy = c.y;
	k.cz = c.z;
	k.radius = radius;
	memcpy(VARDATA(out), &k, sizeof(GistCap));
	return out;
}

static void
bytea_to_cap(bytea *b, sc_vec3 *c, double *radius)
{
	GistCap		k;

	/*
	 * VARDATA_ANY, not VARDATA: an index tuple GiST hands back here may have
	 * been repacked with a 1-byte varlena header (it easily fits under the
	 * short-header limit), and VARDATA alone assumes the 4-byte form we
	 * ourselves palloc'd it with -- reading through the wrong offset there
	 * is exactly the kind of thing that segfaults deep in a page split.
	 */
	memcpy(&k, VARDATA_ANY(b), sizeof(GistCap));
	c->x = k.cx;
	c->y = k.cy;
	c->z = k.cz;
	*radius = k.radius;
}

/*
 * Smallest cap (not necessarily minimal, but valid and cheap) covering both
 * inputs.  d + r1 + r2 < pi is assumed for the general branch: a single
 * region's own cap cannot violate that (skyregion forbids a hemisphere-or-
 * larger cone and a polygon spanning one), but repeated unions of far-apart
 * small regions could in principle approach it -- clamped rather than
 * exactly handled, a known limitation of this spike (see file header).
 */
static void
cap_union2(sc_vec3 c1, double r1, sc_vec3 c2, double r2, sc_vec3 *co, double *ro)
{
	double		d;
	double		theta,
				sinD,
				a,
				b;

	if (r1 < 0)
	{
		*co = c2;
		*ro = r2;
		return;
	}
	if (r2 < 0)
	{
		*co = c1;
		*ro = r1;
		return;
	}

	d = sc_angle(c1, c2);
	if (d + r2 <= r1)
	{
		*co = c1;
		*ro = r1;
		return;
	}
	if (d + r1 <= r2)
	{
		*co = c2;
		*ro = r2;
		return;
	}

	*ro = fmin(M_PI, (d + r1 + r2) / 2.0);
	theta = *ro - r1;			/* angular distance from c1 towards c2 */
	sinD = sin(d);
	if (sinD < 1e-9)
	{
		/* c1 and c2 (near-)coincident but neither cap contains the other:
		 * cannot happen with finite r1,r2 unless d is also ~0, which the
		 * containment checks above already caught -- fall back to c1. */
		*co = c1;
		return;
	}
	a = sin(d - theta) / sinD;
	b = sin(theta) / sinD;
	co->x = a * c1.x + b * c2.x;
	co->y = a * c1.y + b * c2.y;
	co->z = a * c1.z + b * c2.z;
	{
		double		n = sqrt(sc_dot(*co, *co));

		if (n > 1e-15)
		{
			co->x /= n;
			co->y /= n;
			co->z /= n;
		}
		else
			*co = c1;			/* antipodal degenerate case: give up gracefully */
	}
}

/* monotonic in the cap's true area (2*pi*(1-cos(r))); the constant does not
 * matter since penalty only compares differences. */
static double
cap_area_proxy(double radius)
{
	if (radius < 0)
		return 0.0;
	return 1.0 - cos(fmin(radius, M_PI));
}

static bool
cap_overlaps(sc_vec3 c1, double r1, sc_vec3 c2, double r2)
{
	if (r1 < 0 || r2 < 0)
		return false;
	return sc_angle(c1, c2) <= r1 + r2;
}

PG_FUNCTION_INFO_V1(skyregion_gist_compress);
Datum
skyregion_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		sc_region	r;
		sc_vec3		c;
		double		radius;
		bytea	   *k;

		skycell_region_from_datum(entry->key, &r);
		sc_region_bounding_cap(&r, &c, &radius);
		sc_region_free(&r);
		k = cap_to_bytea(c, radius);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(k), entry->rel, entry->page, entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

PG_FUNCTION_INFO_V1(skyregion_gist_decompress);
Datum
skyregion_gist_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(skyregion_gist_union);
Datum
skyregion_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	sc_vec3		c;
	double		radius;
	bytea	   *out;

	bytea_to_cap(DatumGetByteaP(entryvec->vector[0].key), &c, &radius);
	for (int i = 1; i < entryvec->n; i++)
	{
		sc_vec3		c2;
		double		r2;

		bytea_to_cap(DatumGetByteaP(entryvec->vector[i].key), &c2, &r2);
		cap_union2(c, radius, c2, r2, &c, &radius);
	}
	out = cap_to_bytea(c, radius);
	*sizep = VARSIZE(out);
	PG_RETURN_POINTER(out);
}

PG_FUNCTION_INFO_V1(skyregion_gist_penalty);
Datum
skyregion_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	sc_vec3		c1,
				c2,
				cu;
	double		r1,
				r2,
				ru;

	bytea_to_cap(DatumGetByteaP(origentry->key), &c1, &r1);
	bytea_to_cap(DatumGetByteaP(newentry->key), &c2, &r2);
	cap_union2(c1, r1, c2, r2, &cu, &ru);
	*result = (float) (cap_area_proxy(ru) - cap_area_proxy(r1));
	PG_RETURN_POINTER(result);
}

/*
 * Linear split (Guttman's original, not the R*-tree quadratic/greedy split
 * PostgreSQL's own box opclass uses): pick the two entries whose caps are
 * farthest apart as seeds, then assign every other entry to whichever
 * seed's group needs the smaller penalty to absorb it, breaking ties by
 * putting entries in the smaller group so a split cannot degenerate into
 * "everything on one side". Correct for any assignment (union is computed
 * from whatever ends up in each side), just not necessarily a *good* split
 * -- see the file header.
 */
PG_FUNCTION_INFO_V1(skyregion_gist_picksplit);
Datum
skyregion_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	/* real entries are entryvec->vector[FirstOffsetNumber..maxoff]: index 0
	 * is a reserved slot, not a real key (a GiST convention shared with the
	 * OffsetNumber-addressed spl_left/spl_right arrays below, which is why
	 * the loop indices below are used as OffsetNumbers directly, with no
	 * +1/-1 conversion). */
	OffsetNumber maxoff = (OffsetNumber) (entryvec->n - 1);
	sc_vec3    *c = palloc(sizeof(sc_vec3) * (maxoff + 1));
	double	   *r = palloc(sizeof(double) * (maxoff + 1));
	OffsetNumber seed1 = FirstOffsetNumber,
				seed2 = FirstOffsetNumber + 1;
	double		worst = -1;
	sc_vec3		lc,
				rc;
	double		lr,
				rr;

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		bytea_to_cap(DatumGetByteaP(entryvec->vector[i].key), &c[i], &r[i]);

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		for (OffsetNumber j = i + 1; j <= maxoff; j++)
		{
			double		d = sc_angle(c[i], c[j]) + r[i] + r[j];

			if (d > worst)
			{
				worst = d;
				seed1 = i;
				seed2 = j;
			}
		}

	v->spl_left = palloc(sizeof(OffsetNumber) * maxoff);
	v->spl_right = palloc(sizeof(OffsetNumber) * maxoff);
	v->spl_nleft = v->spl_nright = 0;
	lc = c[seed1];
	lr = r[seed1];
	rc = c[seed2];
	rr = r[seed2];

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
	{
		sc_vec3		ulc,
					urc;
		double		ulr,
					urr;
		double		pl,
					pr;
		bool		goLeft;

		if (i == seed1)
		{
			v->spl_left[v->spl_nleft++] = i;
			continue;
		}
		if (i == seed2)
		{
			v->spl_right[v->spl_nright++] = i;
			continue;
		}

		cap_union2(lc, lr, c[i], r[i], &ulc, &ulr);
		cap_union2(rc, rr, c[i], r[i], &urc, &urr);
		pl = cap_area_proxy(ulr) - cap_area_proxy(lr);
		pr = cap_area_proxy(urr) - cap_area_proxy(rr);

		if (pl < pr)
			goLeft = true;
		else if (pr < pl)
			goLeft = false;
		else
			goLeft = (v->spl_nleft <= v->spl_nright);

		/* keep either side from absorbing everything */
		if (v->spl_nleft >= maxoff - 1)
			goLeft = false;
		else if (v->spl_nright >= maxoff - 1)
			goLeft = true;

		if (goLeft)
		{
			v->spl_left[v->spl_nleft++] = i;
			lc = ulc;
			lr = ulr;
		}
		else
		{
			v->spl_right[v->spl_nright++] = i;
			rc = urc;
			rr = urr;
		}
	}

	v->spl_ldatum = PointerGetDatum(cap_to_bytea(lc, lr));
	v->spl_rdatum = PointerGetDatum(cap_to_bytea(rc, rr));
	pfree(c);
	pfree(r);
	PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(skyregion_gist_same);
Datum
skyregion_gist_same(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_P(0);
	bytea	   *b = PG_GETARG_BYTEA_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	sc_vec3		ca,
				cb;
	double		ra,
				rb;

	bytea_to_cap(a, &ca, &ra);
	bytea_to_cap(b, &cb, &rb);
	*result = (fabs(ra - rb) < 1e-12 && sc_angle(ca, cb) < 1e-12);
	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(skyregion_gist_consistent);
Datum
skyregion_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		queryDatum = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	sc_vec3		ec,
				qc;
	double		er,
				qr;
	sc_region	qreg;
	bool		result;

	bytea_to_cap(DatumGetByteaP(entry->key), &ec, &er);

	switch (strategy)
	{
		case GIST_REGION_STRATEGY_OVERLAP:
			skycell_region_from_datum(queryDatum, &qreg);
			sc_region_bounding_cap(&qreg, &qc, &qr);
			sc_region_free(&qreg);
			result = cap_overlaps(ec, er, qc, qr);
			*recheck = true;	/* the cap test is lossy either way */
			break;
		default:
			elog(ERROR, "skyregion_gist_consistent: unsupported strategy %d", strategy);
			result = false;	/* unreachable */
	}
	PG_RETURN_BOOL(result);
}
