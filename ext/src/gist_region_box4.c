/*
 * gist_region_box4.c -- EXPERIMENTAL second box opclass for skyregion:
 * byte-for-byte the same key shape as gist_region_box.c's GistBox3D (axis-
 * aligned 3D Cartesian box, min/max corner), but stored as six `float`s in
 * a genuine fixed-length 24-byte GiST key type instead of six `double`s in
 * a `bytea`-wrapped varlena -- i.e. pgSphere's own `spherekey` layout
 * (confirmed via pageinspect/pg_opclass/pg_type in GIST_REGION_DESIGN.md's
 * "Round fifty-seven"), applied to this extension's own box opclass.
 *
 * WHY THIS EXISTS: the question raised after round fifty-seven was whether
 * pgSphere's win over skyregion_box_gist_ops is mostly explained by its
 * smaller, denser key (half the bytes, no varlena header, higher fanout per
 * page) rather than anything about *what* the key represents -- both
 * opclasses already store the same kind of thing, an axis-aligned box, so
 * the geometry is already identical; the only remaining physical gap is bit
 * width and storage format. This opclass isolates that one variable: same
 * struct layout, same picksplit heuristic, same consistent() logic as
 * gist_region_box.c, with only float-vs-double and fixed-length-vs-varlena
 * changed. If this closes most of the gap, the lesson is "cache/fanout",
 * not "geometry"; if it doesn't, something about pgSphere's actual
 * consistent()/picksplit() code (not just its key's bit width) is doing
 * the work.
 *
 * KEY: a new fixed-length (INTERNALLENGTH = 24, ALIGNMENT = int4, STORAGE =
 * plain) type, skyregion_box4 -- six `float`s (xmin,ymin,zmin,xmax,ymax,
 * zmax), no varlena header at all, exactly pgSphere's spherekey shape.
 * Like skypos/skyregion's own shell-type pattern, and like pgSphere's own
 * spherekey, this type is GiST-internal only: its in/out functions exist
 * solely so CREATE TYPE is legal and always error if actually called,
 * since no column of this type is ever meant to exist.
 *
 * SOUNDNESS: building a box in double precision and then naively casting
 * each bound to float can round *inward* (a double 0.7000001 might round
 * to a float that reads back as 0.69999999), which would silently shrink
 * the box below the true region and drop real matches -- the exact bug
 * class this project's "measured, not assumed" discipline exists to catch
 * before it reaches anyone relying on this opclass. Every bound is instead
 * rounded strictly outward: the minimum axis extent only ever moves down
 * (round_down_f4) and the maximum only ever moves up (round_up_f4), via one
 * nextafterf() step whenever a plain narrowing cast moved the value inward.
 * The query side gets the identical treatment (box4_cached_query), and
 * every comparison against a point promotes the (already outward-rounded)
 * float bounds back up to double rather than narrowing the point down --
 * so the float key is only ever a safe over-approximation, same contract
 * as the double-precision box and the multi-cap key before it.
 *
 * Not DEFAULT, not a replacement for skyregion_box_gist_ops: a bit-width
 * experiment, nothing more. Select explicitly: CREATE INDEX ... USING gist
 * (col skyregion_box4_gist_ops). See GIST_REGION_DESIGN.md's "Round
 * fifty-eight" for the measured result.
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

#define GIST_REGION_BOX4_STRATEGY_OVERLAP 1
#define GIST_REGION_BOX4_STRATEGY_CONTAINS_POINT 2
#define GIST_REGION_BOX4_STRATEGY_CONTAINS_REGION 3
#define GIST_REGION_BOX4_STRATEGY_CONTAINED_BY_REGION 4

typedef struct
{
	float		xmin,
				ymin,
				zmin,
				xmax,
				ymax,
				zmax;
}			GistBox3Df;

/*
 * This type is a GiST-internal key only, never meant to hold a real column
 * value -- skyregion_box4_in errors, same as any other internal GiST key
 * type (no literal of this type is ever written). skyregion_box4_out does
 * print the box, deliberately real rather than an error stub: pageinspect's
 * gist_page_items() calls it to render a leaf/internal key, and being able
 * to inspect this opclass's actual on-disk key is exactly the point of
 * building it this way -- round fifty-seven relied on the same thing being
 * true of pgSphere's own spherekey.
 */
PG_FUNCTION_INFO_V1(skyregion_box4_in);
Datum
skyregion_box4_in(PG_FUNCTION_ARGS)
{
	elog(ERROR, "skyregion_box4 has no textual representation");
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(skyregion_box4_out);
Datum
skyregion_box4_out(PG_FUNCTION_ARGS)
{
	GistBox3Df *b = (GistBox3Df *) PG_GETARG_POINTER(0);
	char	   *str = psprintf("(%g,%g,%g),(%g,%g,%g)",
								b->xmin, b->ymin, b->zmin,
								b->xmax, b->ymax, b->zmax);

	PG_RETURN_CSTRING(str);
}

/* round strictly outward: narrowing double -> float must never shrink the box */
static inline float
round_down_f4(double x)
{
	float		f = (float) x;

	if ((double) f > x)
		f = nextafterf(f, -HUGE_VALF);
	return f;
}

static inline float
round_up_f4(double x)
{
	float		f = (float) x;

	if ((double) f < x)
		f = nextafterf(f, HUGE_VALF);
	return f;
}

static void
axis_extent_f4(double ck, double cr, double sr, float *lo, float *hi)
{
	double		root = sqrt(fmax(0.0, 1.0 - ck * ck));
	double		bmax = ck * cr + root * sr;
	double		bmin = ck * cr - root * sr;

	*hi = (ck >= cr) ? 1.0f : round_up_f4(bmax);
	*lo = (-ck >= cr) ? -1.0f : round_down_f4(bmin);
}

/* cap -> box3d, exact bound in double precision, rounded outward to float */
static void
cap_to_box3df(sc_vec3 c, double r, GistBox3Df *out)
{
	double		cr,
				sr;

	if (r < 0)
	{
		/* empty region: an inverted box so union()/overlap behave like
		 * the empty set. */
		out->xmin = out->ymin = out->zmin = 1.0f;
		out->xmax = out->ymax = out->zmax = -1.0f;
		return;
	}
	if (r >= M_PI)
	{
		out->xmin = out->ymin = out->zmin = -1.0f;
		out->xmax = out->ymax = out->zmax = 1.0f;
		return;
	}
	cr = cos(r);
	sr = sin(r);
	axis_extent_f4(c.x, cr, sr, &out->xmin, &out->xmax);
	axis_extent_f4(c.y, cr, sr, &out->ymin, &out->ymax);
	axis_extent_f4(c.z, cr, sr, &out->zmin, &out->zmax);
}

static void
region_to_box3df(sc_region *r, GistBox3Df *out)
{
	sc_vec3		c;
	double		rad;

	sc_region_bounding_cap(r, &c, &rad);
	cap_to_box3df(c, rad, out);
}

static void
box3df_union(const GistBox3Df *a, const GistBox3Df *b, GistBox3Df *out)
{
	out->xmin = Min(a->xmin, b->xmin);
	out->ymin = Min(a->ymin, b->ymin);
	out->zmin = Min(a->zmin, b->zmin);
	out->xmax = Max(a->xmax, b->xmax);
	out->ymax = Max(a->ymax, b->ymax);
	out->zmax = Max(a->zmax, b->zmax);
}

static double
box3df_volume(const GistBox3Df *b)
{
	double		dx = Max(0.0, (double) b->xmax - (double) b->xmin);
	double		dy = Max(0.0, (double) b->ymax - (double) b->ymin);
	double		dz = Max(0.0, (double) b->zmax - (double) b->zmin);

	return dx * dy * dz;
}

static bool
box3df_overlaps(const GistBox3Df *a, const GistBox3Df *b)
{
	return a->xmin <= b->xmax && b->xmin <= a->xmax &&
		a->ymin <= b->ymax && b->ymin <= a->ymax &&
		a->zmin <= b->zmax && b->zmin <= a->zmax;
}

/* promote the (already outward-rounded) float bounds to double: never
 * narrow the already-sound box, and never round the point itself. */
static bool
box3df_contains_point(const GistBox3Df *b, sc_vec3 p)
{
	return p.x >= (double) b->xmin && p.x <= (double) b->xmax &&
		p.y >= (double) b->ymin && p.y <= (double) b->ymax &&
		p.z >= (double) b->zmin && p.z <= (double) b->zmax;
}

PG_FUNCTION_INFO_V1(skyregion_box4_gist_compress);
Datum
skyregion_box4_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		sc_region	r;
		GistBox3Df *b = (GistBox3Df *) palloc(sizeof(GistBox3Df));

		skycell_region_from_datum(entry->key, &r);
		region_to_box3df(&r, b);
		sc_region_free(&r);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(b), entry->rel, entry->page, entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

PG_FUNCTION_INFO_V1(skyregion_box4_gist_decompress);
Datum
skyregion_box4_gist_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(skyregion_box4_gist_union);
Datum
skyregion_box4_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	GistBox3Df	out,
				b;
	GistBox3Df *outp;

	memcpy(&out, DatumGetPointer(entryvec->vector[0].key), sizeof(GistBox3Df));
	for (int i = 1; i < entryvec->n; i++)
	{
		memcpy(&b, DatumGetPointer(entryvec->vector[i].key), sizeof(GistBox3Df));
		box3df_union(&out, &b, &out);
	}
	outp = (GistBox3Df *) palloc(sizeof(GistBox3Df));
	*outp = out;
	*sizep = sizeof(GistBox3Df);
	PG_RETURN_POINTER(outp);
}

PG_FUNCTION_INFO_V1(skyregion_box4_gist_penalty);
Datum
skyregion_box4_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	GistBox3Df *o = (GistBox3Df *) DatumGetPointer(origentry->key);
	GistBox3Df *n = (GistBox3Df *) DatumGetPointer(newentry->key);
	GistBox3Df	u;

	box3df_union(o, n, &u);
	*result = (float) (box3df_volume(&u) - box3df_volume(o));
	PG_RETURN_POINTER(result);
}

typedef struct
{
	double		key;
	OffsetNumber idx;
}			box4_sort_entry;

static int
box4_sort_cmp(const void *a, const void *b)
{
	double		ka = ((const box4_sort_entry *) a)->key;
	double		kb = ((const box4_sort_entry *) b)->key;

	return (ka > kb) - (ka < kb);
}

/* same median-split heuristic as gist_region_box.c's own picksplit --
 * deliberately unchanged, so this experiment isolates key width/layout,
 * not split quality. */
PG_FUNCTION_INFO_V1(skyregion_box4_gist_picksplit);
Datum
skyregion_box4_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber maxoff = (OffsetNumber) (entryvec->n - 1);
	int			n = maxoff - FirstOffsetNumber + 1;
	GistBox3Df *bx = (GistBox3Df *) palloc(sizeof(GistBox3Df) * (maxoff + 1));
	box4_sort_entry *sorted = (box4_sort_entry *) palloc(sizeof(box4_sort_entry) * n);
	int			bestAxis = 0;
	double		bestSpread = -1;
	int			mid = n / 2;
	GistBox3Df	lu,
				ru;
	GistBox3Df *outp;

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		memcpy(&bx[i], DatumGetPointer(entryvec->vector[i].key), sizeof(GistBox3Df));

	for (int axis = 0; axis < 3; axis++)
	{
		double		lo = HUGE_VAL,
					hi = -HUGE_VAL;

		for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		{
			double		c = (axis == 0) ? ((double) bx[i].xmin + bx[i].xmax) / 2.0
				: (axis == 1) ? ((double) bx[i].ymin + bx[i].ymax) / 2.0
				: ((double) bx[i].zmin + bx[i].zmax) / 2.0;

			lo = Min(lo, c);
			hi = Max(hi, c);
		}
		if (hi - lo > bestSpread)
		{
			bestSpread = hi - lo;
			bestAxis = axis;
		}
	}

	for (int k = 0; k < n; k++)
	{
		OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);
		double		c = (bestAxis == 0) ? ((double) bx[idx].xmin + bx[idx].xmax) / 2.0
			: (bestAxis == 1) ? ((double) bx[idx].ymin + bx[idx].ymax) / 2.0
			: ((double) bx[idx].zmin + bx[idx].zmax) / 2.0;

		sorted[k].key = c;
		sorted[k].idx = idx;
	}
	qsort(sorted, n, sizeof(box4_sort_entry), box4_sort_cmp);

	v->spl_left = (OffsetNumber *) palloc(sizeof(OffsetNumber) * n);
	v->spl_right = (OffsetNumber *) palloc(sizeof(OffsetNumber) * n);
	v->spl_nleft = v->spl_nright = 0;

	lu = bx[sorted[0].idx];
	v->spl_left[v->spl_nleft++] = sorted[0].idx;
	for (int k = 1; k < mid; k++)
	{
		box3df_union(&lu, &bx[sorted[k].idx], &lu);
		v->spl_left[v->spl_nleft++] = sorted[k].idx;
	}

	ru = bx[sorted[mid].idx];
	v->spl_right[v->spl_nright++] = sorted[mid].idx;
	for (int k = mid + 1; k < n; k++)
	{
		box3df_union(&ru, &bx[sorted[k].idx], &ru);
		v->spl_right[v->spl_nright++] = sorted[k].idx;
	}

	outp = (GistBox3Df *) palloc(sizeof(GistBox3Df));
	*outp = lu;
	v->spl_ldatum = PointerGetDatum(outp);
	outp = (GistBox3Df *) palloc(sizeof(GistBox3Df));
	*outp = ru;
	v->spl_rdatum = PointerGetDatum(outp);

	pfree(bx);
	pfree(sorted);
	PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(skyregion_box4_gist_same);
Datum
skyregion_box4_gist_same(PG_FUNCTION_ARGS)
{
	GistBox3Df *a = (GistBox3Df *) PG_GETARG_POINTER(0);
	GistBox3Df *b = (GistBox3Df *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = (memcmp(a, b, sizeof(GistBox3Df)) == 0);
	PG_RETURN_POINTER(result);
}

/*
 * Query-side box cache: same wasted-work shape and value-based cache key as
 * gist_region_box.c's own box_cached_query -- the query region is converted
 * to an (outward-rounded) float box once per distinct query value, not once
 * per index entry visited.
 */
typedef struct
{
	bytea	   *last_query;
	Size		last_query_size;
	GistBox3Df	qbox;
}			box4_query_cache;

static const GistBox3Df *
box4_cached_query(FunctionCallInfo fcinfo, Datum queryDatum)
{
	box4_query_cache *qcache = (box4_query_cache *) fcinfo->flinfo->fn_extra;
	bytea	   *qb = DatumGetByteaP(queryDatum);
	Size		qsz = VARSIZE(qb);

	if (qcache == NULL)
	{
		qcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(box4_query_cache));
		fcinfo->flinfo->fn_extra = qcache;
	}
	if (qcache->last_query == NULL || qcache->last_query_size != qsz ||
		memcmp(qcache->last_query, qb, qsz) != 0)
	{
		sc_region	qreg;

		skycell_region_from_datum(queryDatum, &qreg);
		region_to_box3df(&qreg, &qcache->qbox);
		sc_region_free(&qreg);

		if (qcache->last_query == NULL || qcache->last_query_size < qsz)
		{
			if (qcache->last_query != NULL)
				pfree(qcache->last_query);
			qcache->last_query = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, qsz);
		}
		memcpy(qcache->last_query, qb, qsz);
		qcache->last_query_size = qsz;
	}
	return &qcache->qbox;
}

PG_FUNCTION_INFO_V1(skyregion_box4_gist_consistent);
Datum
skyregion_box4_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		queryDatum = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	GistBox3Df *eb = (GistBox3Df *) DatumGetPointer(entry->key);
	bool		result;

	switch (strategy)
	{
		case GIST_REGION_BOX4_STRATEGY_OVERLAP:
		case GIST_REGION_BOX4_STRATEGY_CONTAINS_REGION:
		case GIST_REGION_BOX4_STRATEGY_CONTAINED_BY_REGION:
			result = box3df_overlaps(eb, box4_cached_query(fcinfo, queryDatum));
			*recheck = true;
			break;
		case GIST_REGION_BOX4_STRATEGY_CONTAINS_POINT:
			result = box3df_contains_point(eb, skycell_pos_from_datum(queryDatum));
			*recheck = true;
			break;
		default:
			elog(ERROR, "skyregion_box4_gist_consistent: unsupported strategy %d", strategy);
			result = false;
	}
	PG_RETURN_BOOL(result);
}
