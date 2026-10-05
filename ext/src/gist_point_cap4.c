/*
 * gist_point_cap4.c -- EXPERIMENTAL variant of gist_point_cap.c: shrinks
 * ONLY the internal-node cap (28 bytes, double centre + float radius) to
 * 16 bytes (float4 centre + float4 radius), leaving the leaf key exactly
 * as gist_point_cap.c already has it (24 bytes, double centre, exact,
 * recheck=false).
 *
 * WHY THIS IS A DIFFERENT EXPERIMENT FROM THE ONE gist_point_cap.c's OWN
 * "ROUND TWO" ALREADY REVERTED: that round floated the LEAF centre -- the
 * one field the exact, recheck=false leaf test depends on -- and lost
 * 3-5x in wall-clock because every leaf candidate then needed recheck=true,
 * a cost that scales with result rows, not pages touched. This file never
 * touches the leaf shape at all; it only shrinks the INTERNAL cap, which
 * already feeds nothing but a lossy, already-recheck=true pruning test
 * (cap_overlaps() against the query's own bounding cap) in the unmodified
 * file too. Internal-node size is what determines GiST fan-out during a
 * tree descent -- the exact lever GIST_REGION_DESIGN.md's "Round fifty-
 * eight" found closing (and reversing) pgSphere's advantage over
 * skyregion_box_gist_ops -- so this asks the natural follow-up question
 * for *points*: does shrinking just that lever, without paying round
 * two's leaf-exactness cost, narrow or close the sub-10-30-arcmin
 * crossover "Round forty-three" found against pgSphere's own spoint GiST?
 *
 * SOUNDNESS: cap_overlaps() is a necessary-condition pruning test -- the
 * internal cap must never be smaller than the true union it stands for,
 * or a real match could be pruned away (a false negative, not just an
 * extra candidate). Rounding a cap's centre to float4 moves it by a tiny
 * but nonzero angle; the stored radius is inflated by strictly more than
 * that angle (plus the already-necessary double-to-float rounding of the
 * radius itself, rounded up) before being narrowed to float4, so the
 * float4 cap (float_centre, float_radius) always contains the exact
 * double-precision cap (centre, radius) it was built from. See
 * shrink_cap_f4() below for the exact bound.
 *
 * Everything else -- compress()/picksplit()/same()/the query-side cache/
 * consistent()'s leaf-vs-internal branch -- is copied unmodified from
 * gist_point_cap.c; only cap_to_bytea()'s internal-tuple branch and
 * bytea_to_cap()'s matching decode differ.
 *
 * STATUS: correctness-verified (0 mismatches against both gist_point_cap.c
 * and pgSphere's native spoint GiST, every radius tested). The result
 * doesn't repeat "Round fifty-eight"'s box4 win, and the gap is not closed:
 * reproduced on two independently reseeded 60-probes-per-radius runs
 * (after finding and fixing three real benchmark-methodology bugs, not
 * just the opclass itself -- see GIST_REGION_DESIGN.md's "Round sixty-
 * two"), buffers came out statistically indistinguishable from
 * gist_point_cap.c's own at every radius (unlike box4's clean, reproducible
 * reduction for regions), wall-clock was genuinely noisy and unresolved,
 * and pgSphere still won decisively (1.4-2.7x) at every radius from 1
 * arcsecond through 30 arcmin in both runs, unchanged from "Round forty-
 * three". Structural, not a tuning gap: box4 shrank every region key,
 * leaf included, because region leaves were never exact in the first
 * place; here leaf exactness is load-bearing, so only the minority
 * internal-entry population could shrink -- far less surface area for the
 * same lever to act on. Not wired into any versioned SQL file; currently
 * registered ad hoc (CREATE FUNCTION ... AS '$libdir/skycell'; CREATE
 * OPERATOR CLASS ...) against `paper_bench`, left in place (not cleaned
 * up, unlike this round's own scratch tables) for anyone who wants to
 * keep poking at it.
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

#define SKYPOS_CAP4_STRATEGY_CONTAINED_BY_REGION 1

typedef struct
{
	double		cx,
				cy,
				cz;
	double		radius;			/* radians; 0 for an exact point, negative = empty */
}			GistCap;

static inline GistCap
cap_make(sc_vec3 c, double r)
{
	GistCap		k;

	k.cx = c.x;
	k.cy = c.y;
	k.cz = c.z;
	k.radius = r;
	return k;
}

static inline sc_vec3
cap_center(GistCap k)
{
	sc_vec3		c;

	c.x = k.cx;
	c.y = k.cy;
	c.z = k.cz;
	return c;
}

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
	theta = *ro - r1;
	sinD = sin(d);
	if (sinD < 1e-9)
	{
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
			*co = c1;
	}
}

static inline GistCap
cap_union_caps(GistCap a, GistCap b)
{
	sc_vec3		c;
	double		r;

	cap_union2(cap_center(a), a.radius, cap_center(b), b.radius, &c, &r);
	return cap_make(c, r);
}

static double
cap_area_proxy(double radius)
{
	if (radius < 0)
		return 0.0;
	return 1.0 - cos(fmin(radius, M_PI));
}

static bool
cap_overlaps(GistCap a, GistCap b)
{
	double		sum;

	if (a.radius < 0 || b.radius < 0)
		return false;
	sum = a.radius + b.radius;
	if (sum >= M_PI)
		return true;
	return sc_dot(cap_center(a), cap_center(b)) >= cos(sum);
}

static double
cap_overlap_amount(GistCap a, GistCap b)
{
	if (a.radius < 0 || b.radius < 0)
		return 0.0;
	return fmax(0.0, a.radius + b.radius - sc_angle(cap_center(a), cap_center(b)));
}

/* leaf: unchanged from gist_point_cap.c -- double centre, exact, no radius
 * stored (always 0) */
typedef struct
{
	double		cx,
				cy,
				cz;
}			GistCapLeaf;

/* internal: float4 centre + float4 radius, 16 bytes, no padding needed
 * (four 4-byte fields, naturally 4-byte aligned) */
typedef struct
{
	float		cx,
				cy,
				cz,
				radius;
}			GistCapInternalF4;

/*
 * Round a double-precision cap to a float4 cap that provably still
 * contains it: round the centre to the nearest float4 per axis (small,
 * bounded error), then inflate the radius by strictly more than the
 * resulting centre displacement (sc_angle between the exact and rounded
 * centres, renormalised) plus the radius's own double-to-float rounding,
 * and round that sum up (nextafterf toward +inf) rather than to nearest.
 * A radius that would exceed pi is simply clamped to pi (any two points
 * are within pi of each other -- same convention cap_overlaps() uses).
 */
static void
shrink_cap_f4(GistCap in, GistCapInternalF4 *out)
{
	sc_vec3		c = cap_center(in);
	sc_vec3		cf;
	double		n;
	double		slack;
	float		rf;

	out->cx = (float) c.x;
	out->cy = (float) c.y;
	out->cz = (float) c.z;

	/* renormalise the rounded centre before measuring its drift -- an
	 * unnormalised vector would make sc_angle()'s own acos/atan2 math
	 * meaningless */
	cf.x = out->cx;
	cf.y = out->cy;
	cf.z = out->cz;
	n = sqrt(sc_dot(cf, cf));
	if (n > 1e-15)
	{
		cf.x /= n;
		cf.y /= n;
		cf.z /= n;
	}
	else
		cf = c;

	slack = sc_angle(c, cf);
	if (in.radius < 0)
	{
		out->radius = -1.0f;
		return;
	}

	rf = (float) (in.radius + slack);
	/* guarantee rf (as a double) >= in.radius + slack, not just "close" */
	while ((double) rf < in.radius + slack)
		rf = nextafterf(rf, HUGE_VALF);
	if (rf > (float) M_PI || !(rf <= (float) M_PI))
		rf = (float) M_PI;
	out->radius = rf;
}

static bytea *
cap_to_bytea(const GistCap *c, bool is_leaf)
{
	if (is_leaf)
	{
		Size		sz = VARHDRSZ + sizeof(GistCapLeaf);
		bytea	   *out = (bytea *) palloc(sz);
		GistCapLeaf l;

		Assert(c->radius == 0.0);
		l.cx = c->cx;
		l.cy = c->cy;
		l.cz = c->cz;
		SET_VARSIZE(out, sz);
		memcpy(VARDATA(out), &l, sizeof(GistCapLeaf));
		return out;
	}
	else
	{
		Size		sz = VARHDRSZ + sizeof(GistCapInternalF4);
		bytea	   *out = (bytea *) palloc(sz);
		GistCapInternalF4 n;

		shrink_cap_f4(*c, &n);
		SET_VARSIZE(out, sz);
		memcpy(VARDATA(out), &n, sizeof(GistCapInternalF4));
		return out;
	}
}

static void
bytea_to_cap(bytea *b, GistCap *out)
{
	Size		sz = VARSIZE_ANY_EXHDR(b);

	if (sz == sizeof(GistCapLeaf))
	{
		GistCapLeaf l;

		memcpy(&l, VARDATA_ANY(b), sizeof(GistCapLeaf));
		out->cx = l.cx;
		out->cy = l.cy;
		out->cz = l.cz;
		out->radius = 0.0;
	}
	else
	{
		GistCapInternalF4 n;

		Assert(sz == sizeof(GistCapInternalF4));
		memcpy(&n, VARDATA_ANY(b), sizeof(GistCapInternalF4));
		out->cx = n.cx;
		out->cy = n.cy;
		out->cz = n.cz;
		out->radius = n.radius;
	}
}

PG_FUNCTION_INFO_V1(skypos_cap4_gist_compress);
Datum
skypos_cap4_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		sc_vec3		p = skycell_pos_from_datum(entry->key);
		GistCap		k = cap_make(p, 0.0);
		bytea	   *b = cap_to_bytea(&k, true);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(b), entry->rel, entry->page, entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

PG_FUNCTION_INFO_V1(skypos_cap4_gist_decompress);
Datum
skypos_cap4_gist_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(skypos_cap4_gist_union);
Datum
skypos_cap4_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	GistCap		out,
				c;
	bytea	   *outb;

	bytea_to_cap(DatumGetByteaP(entryvec->vector[0].key), &out);
	for (int i = 1; i < entryvec->n; i++)
	{
		bytea_to_cap(DatumGetByteaP(entryvec->vector[i].key), &c);
		out = cap_union_caps(out, c);
	}
	outb = cap_to_bytea(&out, false);
	*sizep = VARSIZE(outb);
	PG_RETURN_POINTER(outb);
}

PG_FUNCTION_INFO_V1(skypos_cap4_gist_penalty);
Datum
skypos_cap4_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	GistCap		o,
				n,
				u;

	bytea_to_cap(DatumGetByteaP(origentry->key), &o);
	bytea_to_cap(DatumGetByteaP(newentry->key), &n);
	u = cap_union_caps(o, n);
	*result = (float) (cap_area_proxy(u.radius) - cap_area_proxy(o.radius));
	PG_RETURN_POINTER(result);
}

typedef struct
{
	double		key;
	OffsetNumber idx;
}			cap_sort_entry;

static int
cap_sort_cmp(const void *a, const void *b)
{
	double		ka = ((const cap_sort_entry *) a)->key;
	double		kb = ((const cap_sort_entry *) b)->key;

	return (ka > kb) - (ka < kb);
}

PG_FUNCTION_INFO_V1(skypos_cap4_gist_picksplit);
Datum
skypos_cap4_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber maxoff = (OffsetNumber) (entryvec->n - 1);
	int			n = maxoff - FirstOffsetNumber + 1;
	GistCap    *caps = (GistCap *) palloc(sizeof(GistCap) * (maxoff + 1));
	int			minfill = Max(1, n * 3 / 10);
	cap_sort_entry *sorted = (cap_sort_entry *) palloc(sizeof(cap_sort_entry) * n);
	GistCap    *fwd = (GistCap *) palloc(sizeof(GistCap) * n);
	GistCap    *bwd = (GistCap *) palloc(sizeof(GistCap) * n);
	int			bestAxis = 0;
	double		bestAxisMargin = HUGE_VAL;

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		bytea_to_cap(DatumGetByteaP(entryvec->vector[i].key), &caps[i]);

	for (int axis = 0; axis < 3; axis++)
	{
		double		marginSum = 0;

		for (int k = 0; k < n; k++)
		{
			OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);
			sc_vec3		c = cap_center(caps[idx]);

			sorted[k].idx = idx;
			sorted[k].key = (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;
		}
		qsort(sorted, n, sizeof(cap_sort_entry), cap_sort_cmp);

		fwd[0] = caps[sorted[0].idx];
		for (int k = 1; k < n; k++)
			fwd[k] = cap_union_caps(fwd[k - 1], caps[sorted[k].idx]);
		bwd[n - 1] = caps[sorted[n - 1].idx];
		for (int k = n - 2; k >= 0; k--)
			bwd[k] = cap_union_caps(bwd[k + 1], caps[sorted[k].idx]);

		for (int m = minfill; m <= n - minfill; m++)
			marginSum += cap_area_proxy(fwd[m - 1].radius) + cap_area_proxy(bwd[m].radius);

		if (marginSum < bestAxisMargin)
		{
			bestAxisMargin = marginSum;
			bestAxis = axis;
		}
	}

	for (int k = 0; k < n; k++)
	{
		OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);
		sc_vec3		c = cap_center(caps[idx]);

		sorted[k].idx = idx;
		sorted[k].key = (bestAxis == 0) ? c.x : (bestAxis == 1) ? c.y : c.z;
	}
	qsort(sorted, n, sizeof(cap_sort_entry), cap_sort_cmp);
	fwd[0] = caps[sorted[0].idx];
	for (int k = 1; k < n; k++)
		fwd[k] = cap_union_caps(fwd[k - 1], caps[sorted[k].idx]);
	bwd[n - 1] = caps[sorted[n - 1].idx];
	for (int k = n - 2; k >= 0; k--)
		bwd[k] = cap_union_caps(bwd[k + 1], caps[sorted[k].idx]);

	{
		int			bestM = minfill;
		double		bestOverlap = HUGE_VAL;
		double		bestArea = HUGE_VAL;

		for (int m = minfill; m <= n - minfill; m++)
		{
			double		overlap = cap_overlap_amount(fwd[m - 1], bwd[m]);
			double		area = cap_area_proxy(fwd[m - 1].radius) + cap_area_proxy(bwd[m].radius);

			if (overlap < bestOverlap || (overlap == bestOverlap && area < bestArea))
			{
				bestOverlap = overlap;
				bestArea = area;
				bestM = m;
			}
		}

		v->spl_left = (OffsetNumber *) palloc(sizeof(OffsetNumber) * bestM);
		v->spl_right = (OffsetNumber *) palloc(sizeof(OffsetNumber) * (n - bestM));
		v->spl_nleft = v->spl_nright = 0;
		for (int k = 0; k < bestM; k++)
			v->spl_left[v->spl_nleft++] = sorted[k].idx;
		for (int k = bestM; k < n; k++)
			v->spl_right[v->spl_nright++] = sorted[k].idx;

		v->spl_ldatum = PointerGetDatum(cap_to_bytea(&fwd[bestM - 1], false));
		v->spl_rdatum = PointerGetDatum(cap_to_bytea(&bwd[bestM], false));
	}

	pfree(caps);
	pfree(sorted);
	pfree(fwd);
	pfree(bwd);
	PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(skypos_cap4_gist_same);
Datum
skypos_cap4_gist_same(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_P(0);
	bytea	   *b = PG_GETARG_BYTEA_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	GistCap		ca,
				cb;

	bytea_to_cap(a, &ca);
	bytea_to_cap(b, &cb);
	*result = (memcmp(&ca, &cb, sizeof(GistCap)) == 0);
	PG_RETURN_POINTER(result);
}

typedef struct
{
	bytea	   *last_query;
	Size		last_query_size;
	sc_region	reg;
	GistCap		bound;
}			cap_query_cache;

static void
cap_cached_query(FunctionCallInfo fcinfo, Datum queryDatum, sc_region **reg_out, GistCap *bound_out)
{
	cap_query_cache *qcache = (cap_query_cache *) fcinfo->flinfo->fn_extra;
	bytea	   *qb = DatumGetByteaP(queryDatum);
	Size		qsz = VARSIZE(qb);

	if (qcache == NULL)
	{
		qcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(cap_query_cache));
		fcinfo->flinfo->fn_extra = qcache;
	}
	if (qcache->last_query == NULL || qcache->last_query_size != qsz ||
		memcmp(qcache->last_query, qb, qsz) != 0)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
		sc_vec3		c;
		double		r;

		if (qcache->last_query != NULL)
			sc_region_free(&qcache->reg);
		skycell_region_from_datum(queryDatum, &qcache->reg);
		MemoryContextSwitchTo(oldcxt);

		sc_region_bounding_cap(&qcache->reg, &c, &r);
		qcache->bound = cap_make(c, r);

		if (qcache->last_query == NULL || qcache->last_query_size < qsz)
		{
			if (qcache->last_query != NULL)
				pfree(qcache->last_query);
			qcache->last_query = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, qsz);
		}
		memcpy(qcache->last_query, qb, qsz);
		qcache->last_query_size = qsz;
	}
	*reg_out = &qcache->reg;
	*bound_out = qcache->bound;
}

PG_FUNCTION_INFO_V1(skypos_cap4_gist_consistent);
Datum
skypos_cap4_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		queryDatum = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	GistCap		ek;
	sc_region  *qreg;
	GistCap		qbound;
	bool		result;

	if (strategy != SKYPOS_CAP4_STRATEGY_CONTAINED_BY_REGION)
		elog(ERROR, "skypos_cap4_gist_consistent: unsupported strategy %d", strategy);

	bytea_to_cap(DatumGetByteaP(entry->key), &ek);
	cap_cached_query(fcinfo, queryDatum, &qreg, &qbound);

	if (GIST_LEAF(entry))
	{
		result = sc_region_contains(qreg, cap_center(ek)) != 0;
		*recheck = false;
	}
	else
	{
		result = cap_overlaps(ek, qbound);
		*recheck = true;
	}
	PG_RETURN_BOOL(result);
}
