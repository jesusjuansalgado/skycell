/*
 * skycell.c -- PostgreSQL glue for the skycell prototype.
 *
 * Points are keyed by their order-29 HEALPix NESTED cell (int8) in an
 * ordinary B-tree.  Spatial predicates are written as plain boolean
 * functions:
 *
 *     WHERE skycell_cone(cell, ra, dec, ra0, dec0, radius)
 *
 * and a planner support function (SupportRequestSimplify) rewrites them
 * before path generation into
 *
 *     (cell BETWEEN lo1 AND hi1 OR cell BETWEEN lo2 AND hi2 ...)
 *     AND skycell_in_cone(ra, dec, ra0, dec0, radius, <selectivity>)
 *
 * The ranges come from a cost-based covering (cover.c) that uses the
 * ANALYZE histogram of the cell column as a sky density map.  Because the
 * index conditions are plain range predicates on a plain column, the planner
 * also gets real row estimates from the same histogram.
 *
 * When the cone parameters are not constants (joins, generic plans) the
 * rewrite emits a fixed number of range "slots" computed at run time per
 * outer row by skycell_cone_bound() -- the same trick Q3C uses -- but with a
 * density-aware covering.  skycell_cone_ranges() offers the alternative
 * LATERAL form with a variable number of ranges.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "catalog/namespace.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "common/hashfn.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "cover.h"
#include "skycell_internal.h"

PG_MODULE_MAGIC;

#define DEG2RAD (M_PI / 180.0)
#define MAX_SLOTS 16

/* GUCs */
static double skycell_range_cost = -1.0;
static double skycell_split_cost = 1.0;
static double skycell_max_area_ratio = 64.0;
static int	skycell_max_ranges = 64;
static int	skycell_max_steps = 4000;
int			skycell_join_slots = 4;	/* adql.c's non-constant skyregion branch shares this */
double		skycell_rewrite_max_waste = 100.0;	/* adql.c's region_support_simplify shares this */
static double skycell_rewrite_waste_scale_cap = 10.0;
static bool skycell_use_stats = true;
static bool skycell_cache_coverings = true;
static bool skycell_exact_cells = true;
static int	skycell_force_order = -1;
static int	skycell_probe_orders = 0;

void		_PG_init(void);

void
_PG_init(void)
{
	DefineCustomRealVariable("skycell.range_cost",
							 "Cost of one extra index range, in rows "
							 "(-1: derive it from the relation's statistics and "
							 "the planner's cost parameters).",
							 NULL, &skycell_range_cost, -1.0, -1.0, 1e9,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("skycell.split_cost",
							 "Cost of refining one covering cell, in rows.",
							 NULL, &skycell_split_cost, 1.0, 0.0, 1e9,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("skycell.max_area_ratio",
							 "Max area of a partially covered cell relative to the region (0 = unlimited).",
							 NULL, &skycell_max_area_ratio, 64.0, 0.0, 1e12,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("skycell.max_ranges",
							"Maximum index ranges for a constant region.",
							NULL, &skycell_max_ranges, 64, 1, 10000,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("skycell.max_steps",
							"Maximum cell classifications per covering.",
							NULL, &skycell_max_steps, 4000, 16, 10000000,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("skycell.join_slots",
							"Number of range slots emitted for non-constant regions (joins).",
							NULL, &skycell_join_slots, 4, 1, MAX_SLOTS,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("skycell.rewrite_max_waste",
							 "Decline the B-tree range rewrite for a constant region when the "
							 "covering's own cost model expects more than this many rows to be "
							 "fetched and then rejected by the exact test (cov.exp_rows * "
							 "(1 - sel)), scaled by how much of the relation is expected to be "
							 "cache-resident (see rewrite_waste_threshold(): this value applies "
							 "as-is when the whole relation fits in the shared buffer pool "
							 "(NBuffers), and scales up as it doesn't, since that same wasted "
							 "CPU work is cheap once I/O, not CPU, is the bottleneck -- measured "
							 "directly in GIST_REGION_DESIGN.md's 'Round forty-four', not "
							 "assumed). A covering's overshoot is cheap in absolute rows at small "
							 "radii and expensive at large ones when cache-resident; past this "
							 "many wasted rows, leaving the original <@/@> clause unrewritten lets "
							 "the planner cost a GiST-family index (skypos_spgist_ops, or an "
							 "experimental opclass) against it instead, if one exists -- see "
							 "GIST_REGION_DESIGN.md's 'Round forty-three' for the warm-cache "
							 "crossover this value was tuned from. Set this very high to recover "
							 "the always-rewrite behaviour every version before this one had.",
							 NULL, &skycell_rewrite_max_waste, 100.0, 0.0, 1e12,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("skycell.rewrite_waste_scale_cap",
							 "Upper bound, as a multiple of skycell.rewrite_max_waste, on how far "
							 "rewrite_waste_threshold() may scale the gate up for a relation far "
							 "bigger than the shared buffer pool. The scaling itself only prices "
							 "in the exact-test's wasted CPU rows getting cheaper as I/O comes to "
							 "dominate -- it has no term for the rewrite's own page count, which "
							 "GIST_REGION_DESIGN.md's 'Round forty-five' measured growing faster "
							 "than a GiST-family alternative's at the largest radii on a table "
							 "whose rows are not physically ordered by cell, even cold. Left "
							 "unbounded, a deployment with a small enough buffer pool relative to "
							 "the table could scale past that radius's own waste estimate and force "
							 "the rewrite exactly where this was measured to lose. This caps the "
							 "scaling before that point without touching small-to-mid radii, where "
							 "scaling up was measured correct even cold.",
							 NULL, &skycell_rewrite_waste_scale_cap, 10.0, 1.0, 1e12,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("skycell.use_stats",
							 "Use the ANALYZE histogram of the cell column as a density map.",
							 NULL, &skycell_use_stats, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("skycell.cache_coverings",
							 "Memoise coverings per backend (cleared when statistics change).",
							 NULL, &skycell_cache_coverings, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("skycell.exact_cells",
							 "Classify cells against a cone by their own corner geometry "
							 "(off: the provable max_pixrad cap, looser but cheaper to justify).",
							 NULL, &skycell_exact_cells, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("skycell.probe_orders",
							"Orders past the closed-form choice to score on the covering "
							"they actually produce (0, the default, disables: see the "
							"comment in cover.c -- the gain the forced-order measurements "
							"show is not yet captured by this).",
							NULL, &skycell_probe_orders, 0, 0, 8,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("skycell.force_order",
							"Diagnostics only: cover cones at this HEALPix order instead of "
							"the one the cost model chooses (-1 = let the model choose).",
							NULL, &skycell_force_order, -1, -1, SC_MAX_ORDER,
							PGC_USERSET, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("skycell");
}

/*
 * The price of one index range, in rows, from the relation's own statistics
 * and the planner's cost parameters.
 *
 * cost(s) in cover.h is in rows, so range_cost converts "one more B-tree range
 * scan" into "the number of false-positive rows it is worth avoiding".  Both
 * sides are things PostgreSQL already costs:
 *
 *   one more range  = a B-tree descent.  btcostestimate charges
 *                     ceil(log2(N)) * 50 * cpu_operator_cost for the
 *                     comparisons and (H + 1) * 50 * cpu_operator_cost for the
 *                     pages of the descent.
 *   one false row   = its index entry, its heap tuple, the exact predicate,
 *                     and the heap page amortised over the rows that share it
 *                     (reltuples / relpages of this very relation).
 *
 * The page term is what makes this per-relation rather than per-installation:
 * a narrow catalogue row packs 120 to a page and amortises it away, while a
 * wide observation row packs a handful and makes every false positive cost
 * real page traffic, so the same query should be covered more finely there.
 *
 * random_page_cost is used unblended, which is the pessimistic end (it assumes
 * the page is not resident).  That errs towards finer coverings; the cost
 * curve is flat near its minimum, so the error is small either way.
 */
static double
auto_range_cost(const sc_density *d)
{
	double		n = (d && d->ntotal > 1) ? d->ntotal : 1e6;
	double		rpp = (d && d->relpages > 0 && d->ntotal > 0)
		? d->ntotal / d->relpages : 100.0;
	double		height = fmax(1.0, ceil(log(n) / log(300.0)));	/* btree fanout ~300 */
	double		descent = (ceil(log(n) / log(2.0)) + height + 1.0)
		* 50.0 * cpu_operator_cost;
	double		per_row = cpu_tuple_cost + cpu_index_tuple_cost
		+ 3.0 * cpu_operator_cost				/* the exact predicate */
		+ random_page_cost / fmax(rpp, 1.0);

	if (!(per_row > 0))
		return 30.0;
	return fmin(1e6, fmax(1.0, descent / per_row));
}

/*
 * skycell.rewrite_max_waste scaled by how much of the relation PostgreSQL's
 * own planner already expects to find in cache -- GIST_REGION_DESIGN.md's
 * "Round forty-four" measured directly (fresh, restart-isolated probes,
 * not guessed) that the rewrite's exact-test waste this GUC bounds is
 * expensive only when it costs CPU with nothing to show for it, i.e. when
 * the pages it touches are already cache-resident; the exact same waste is
 * cheap -- cheaper than the alternative GiST-family descent it would
 * otherwise fall back to -- when the pages are not resident, because CPU is
 * negligible next to a real disk fetch. skycell's own GUC was tuned from
 * warm-cache measurements alone (a fully cache-resident table), so it is
 * scaled up here in proportion to how far the relation's own size exceeds
 * the server's actual cache capacity.
 *
 * That capacity is read from NBuffers -- the real, already-allocated size of
 * the shared buffer pool (shared_buffers converted to pages) -- rather than
 * from effective_cache_size. effective_cache_size is not a measurement of
 * anything: it is a standalone GUC the admin may set to any value (commonly
 * left at its build-in default, or sized for a machine the server no longer
 * runs on), with nothing tying it to the memory PostgreSQL actually holds.
 * NBuffers is that memory. The tradeoff is that NBuffers, unlike
 * effective_cache_size, counts only shared_buffers and not the OS page
 * cache behind it, so this reads as more conservative (a relation is
 * treated as exceeding cache sooner) whenever the OS cache is doing real
 * work the shared buffer pool alone would not reflect -- the same direction
 * Round forty-four already measured as the safer one to err towards: it
 * found skycell's rewrite winning most broadly exactly where cache pressure
 * is underestimated (cold, I/O-bound reads), and losing only in the fully
 * warm case this scaling leaves untouched (cache_frac = 1).
 *
 * This is deliberately the same level of approximation as auto_range_cost()
 * above (a simple ratio, not a reproduction of PostgreSQL's own, considerably
 * more elaborate formula): cache_frac = 1 when the whole relation is
 * expected to fit in the shared buffer pool (no change from the tuned
 * default), falling toward 0 as the relation grows far past it (the
 * threshold scaling up as a relation's reads become reliably I/O-bound) --
 * not a precise cost-model derivation, an explicit choice to stay as
 * legible as this file's other heuristics.
 *
 * The scaling is capped, not unbounded, at skycell.rewrite_waste_scale_cap
 * times the base threshold (cache_frac floored at 1/cap rather than at a
 * value close to zero). The scaling's whole justification is that the
 * waste it bounds -- wasted CPU rows from the exact test -- gets cheaper
 * as I/O comes to dominate; it says nothing about the rewrite's *own*
 * expected page count, which GIST_REGION_DESIGN.md's "Round forty-five"
 * measured growing faster than a GiST-family alternative's at the largest
 * radii specifically, on a relation whose rows are not physically ordered
 * by cell (an ordinary, unclustered table -- not a special case). Past
 * some point a low cache_frac stops meaning "the waste is cheap" and
 * starts meaning "the whole rewrite, waste included, is expensive, for a
 * reason this ratio was never modelling" -- the cap keeps the scaling in
 * the regime "Round forty-four" and "Round forty-five" both measured it
 * correct in (small-to-mid radii, even cold) without reaching into the
 * regime "Round forty-five" measured it wrong in (the largest radii, an
 * unclustered table, even cold).
 */
double
rewrite_waste_threshold(const sc_density *d)
{
	double		relpages = (d && d->relpages > 0) ? d->relpages : 0;
	double		cache_frac = (relpages > 0)
		? fmin(1.0, (double) NBuffers / relpages) : 1.0;
	double		min_frac = 1.0 / fmax(skycell_rewrite_waste_scale_cap, 1.0);

	return skycell_rewrite_max_waste / fmax(cache_frac, min_frac);
}

void
current_params(sc_cover_params *p, int max_ranges, const sc_density *d)
{
	sc_cover_params_default(p);
	p->range_cost = (skycell_range_cost < 0)
		? auto_range_cost(d) : skycell_range_cost;
	p->split_cost = skycell_split_cost;
	p->max_area_ratio = skycell_max_area_ratio;
	p->max_ranges = max_ranges;
	p->max_steps = skycell_max_steps;
	p->force_order = skycell_force_order;
	sc_exact_cells = skycell_exact_cells ? 1 : 0;
	sc_order_probe = skycell_probe_orders;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

void
check_err(const char *err)
{
	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("skycell: %s", err)));
}

/* float8[] {ra1, dec1, ra2, dec2, ...} -> polygon region */
static void
poly_from_array(ArrayType *arr, sc_region *r)
{
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	double	   *ra,
			   *dec;

	if (ARR_ELEMTYPE(arr) != FLOAT8OID)
		elog(ERROR, "skycell: polygon must be a float8[]");
	deconstruct_array(arr, FLOAT8OID, sizeof(float8), true, TYPALIGN_DOUBLE, &elems, &nulls, &n);
	if (n % 2 != 0)
		check_err("polygon array must contain ra,dec pairs");
	ra = palloc(sizeof(double) * n / 2);
	dec = palloc(sizeof(double) * n / 2);
	for (int i = 0; i < n / 2; i++)
	{
		if (nulls[2 * i] || nulls[2 * i + 1])
			check_err("polygon vertices must not be NULL");
		ra[i] = DatumGetFloat8(elems[2 * i]);
		dec[i] = DatumGetFloat8(elems[2 * i + 1]);
	}
	check_err(sc_region_poly(r, n / 2, ra, dec));
}

/* ------------------------------------------------------------------ */
/* statistics cache                                                    */
/* ------------------------------------------------------------------ */

/*
 * Coverings are a deterministic function of the cone, the density model and
 * the cost settings, and computing one is the largest part of what this
 * extension adds to planning.  A TAP service plans the same cone many times
 * -- the same object, the same survey field, a client paging -- so the
 * result is memoised per backend and thrown away with the statistics it was
 * computed from.
 */
typedef struct cover_key
{
	double		ra0,
				dec0,
				radius,
				range_cost,
				split_cost,
				area_ratio;
	Oid			statrel;		/* whose density map this used */
	int32		max_ranges;
	int32		max_steps;
	int32		force_order;	/* diagnostics; a covering computed at a forced
								 * order must not be handed to a query that did
								 * not ask for one */
	int32		probe_orders;	/* likewise: probing changes which order wins */
} cover_key;

typedef struct cover_entry
{
	cover_key	key;			/* must be first */
	int			n;
	sc_range   *r;
	double		area;
	double		exp_rows;
} cover_entry;

static HTAB *cover_cache = NULL;

#define SKYCELL_COVER_CACHE_MAX 2048


/*
 * Loading the density model costs more than computing the covering: the
 * histogram is deconstructed from pg_statistic on every plan, and finding
 * the expression index means opening every index of the relation.  Both are
 * cached per backend and dropped whole whenever the statistics, the
 * relation's row count or any relcache entry changes -- ANALYZE, DDL, or a
 * new index all invalidate, and a stale density map would silently plan
 * coverings for data that moved.
 */
typedef struct dens_key
{
	Oid			statrel;		/* the table, or the expression index */
	AttrNumber	attnum;
} dens_key;

typedef struct dens_entry
{
	dens_key	key;			/* must be first */
	double		ntotal;
	int			nbounds;
	int64	   *bounds;
	double		relpages;
	int			nmap;			/* multi-order count map, if one was built */
	int64	   *map_lo;
	int64	   *map_hi;
	double	   *map_n;
	double		map_total;
} dens_entry;

typedef struct idx_entry
{
	Oid			relid;			/* must be first: the table */
	Oid			indexoid;		/* the matching expression index, or InvalidOid */
	Node	   *expr;			/* its first key expression (varno 1) */
	bool		uses_cell_ops;	/* that index's opclass is skycell_cell_ops */
} idx_entry;

static MemoryContext skycell_cache_cxt = NULL;
static HTAB *dens_cache = NULL;
static HTAB *idx_cache = NULL;

static void
skycell_cache_flush(Datum arg, int cacheid, uint32 hashvalue)
{
	dens_cache = NULL;
	idx_cache = NULL;
	cover_cache = NULL;
	if (skycell_cache_cxt)
		MemoryContextReset(skycell_cache_cxt);
}

static void
skycell_cache_flush_rel(Datum arg, Oid relid)
{
	skycell_cache_flush(arg, 0, 0);
}

static void
skycell_cache_init(void)
{
	HASHCTL		ctl;

	if (skycell_cache_cxt == NULL)
	{
		skycell_cache_cxt = AllocSetContextCreate(TopMemoryContext,
												  "skycell statistics cache",
												  ALLOCSET_SMALL_SIZES);
		CacheRegisterSyscacheCallback(STATRELATTINH, skycell_cache_flush, (Datum) 0);
		CacheRegisterSyscacheCallback(RELOID, skycell_cache_flush, (Datum) 0);
		CacheRegisterRelcacheCallback(skycell_cache_flush_rel, (Datum) 0);
	}
	if (dens_cache == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(dens_key);
		ctl.entrysize = sizeof(dens_entry);
		ctl.hcxt = skycell_cache_cxt;
		dens_cache = hash_create("skycell density", 8, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if (idx_cache == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(idx_entry);
		ctl.hcxt = skycell_cache_cxt;
		idx_cache = hash_create("skycell cell index", 8, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if (cover_cache == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(cover_key);
		ctl.entrysize = sizeof(cover_entry);
		ctl.hcxt = skycell_cache_cxt;
		cover_cache = hash_create("skycell coverings", 64, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
}

/* sc_cover_compute() through the memo; the ranges stay owned by the cache */
void
cover_cached(const sc_region *reg, const sc_density *d, const sc_cover_params *p,
			 Oid statrel, double ra0, double dec0, double radius, sc_cover *out)
{
	cover_key	key;
	cover_entry *entry;
	bool		found;

	if (!skycell_cache_coverings)
	{
		sc_cover_compute(reg, d, p, out);
		return;
	}
	skycell_cache_init();
	if (hash_get_num_entries(cover_cache) >= SKYCELL_COVER_CACHE_MAX)
		skycell_cache_flush((Datum) 0, 0, 0);	/* simplest eviction: start over */
	skycell_cache_init();

	memset(&key, 0, sizeof(key));	/* padding must be zero: hashed as bytes */
	key.ra0 = ra0;
	key.dec0 = dec0;
	key.radius = radius;
	key.range_cost = p->range_cost;
	key.split_cost = p->split_cost;
	key.area_ratio = p->max_area_ratio;
	key.statrel = statrel;
	key.max_ranges = p->max_ranges;
	key.max_steps = p->max_steps;
	key.force_order = p->force_order;
	key.probe_orders = sc_order_probe;

	entry = (cover_entry *) hash_search(cover_cache, &key, HASH_ENTER, &found);
	if (!found)
	{
		sc_cover	fresh;
		MemoryContext old = MemoryContextSwitchTo(skycell_cache_cxt);

		sc_cover_compute(reg, d, p, &fresh);
		MemoryContextSwitchTo(old);
		entry->n = fresh.n;
		entry->r = fresh.r;
		entry->area = fresh.area;
		entry->exp_rows = fresh.exp_rows;
	}
	memset(out, 0, sizeof(*out));
	out->n = entry->n;
	out->r = entry->r;
	out->area = entry->area;
	out->exp_rows = entry->exp_rows;
}


/*
 * Density model: reltuples of the table plus the ANALYZE histogram of
 * column attnum of statrel -- the table itself for a plain column, or an
 * expression index on skycell_ang2cell(ra, dec), whose statistics ANALYZE
 * keeps under the index.  Bounds are palloc'd in the current context.
 */
static void
load_density_from(Oid relid, Oid statrel, AttrNumber attnum, sc_density *d)
{
	HeapTuple	tp;

	d->ntotal = 0;
	d->relpages = 0;
	d->nbounds = 0;
	d->bounds = NULL;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class rd = (Form_pg_class) GETSTRUCT(tp);

		d->ntotal = rd->reltuples > 0 ? rd->reltuples : 0;
		d->relpages = rd->relpages > 0 ? rd->relpages : 0;
		ReleaseSysCache(tp);
	}
	if (!skycell_use_stats)
		return;

	for (int inh = 0; inh <= 1; inh++)
	{
		tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(statrel),
							 Int16GetDatum(attnum), BoolGetDatum(inh));
		if (HeapTupleIsValid(tp))
		{
			AttStatsSlot sslot;
			float4		nullfrac = ((Form_pg_statistic) GETSTRUCT(tp))->stanullfrac;

			if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_HISTOGRAM,
								 InvalidOid, ATTSTATSSLOT_VALUES))
			{
				if (sslot.nvalues >= 2 && sslot.valuetype == INT8OID)
				{
					int64	   *b = palloc(sizeof(int64) * sslot.nvalues);

					for (int i = 0; i < sslot.nvalues; i++)
						b[i] = DatumGetInt64(sslot.values[i]);
					d->bounds = b;
					d->nbounds = sslot.nvalues;
				}
				free_attstatsslot(&sslot);
			}
			d->ntotal *= (1.0 - nullfrac);
			ReleaseSysCache(tp);
			return;
		}
	}
}

/*
 * load_density_from() through the cache.  The bounds are copied into the
 * caller's context: the covering reads them while the planner is free to
 * open relations, and an invalidation in between would otherwise free them
 * under it.
 */
/*
 * Read the multi-order count map for (statrel, attnum), if
 * skycell_density_build() has made one.  Called once per relation per backend,
 * inside the cache context, and dropped with the rest of the cache when
 * statistics or the relation change.
 *
 * The map is stored as NUNIQ cells, disjoint by construction; they are sorted
 * here by the low end of their order-29 interval so that sc_density_rows() can
 * binary search.  The table is found through the search path: a deployment
 * that hides the extension's schema simply gets the histogram instead.
 */
typedef struct map_cell
{
	int64		lo,
				hi;
	double		n;
} map_cell;

static int
map_cell_cmp(const void *a, const void *b)
{
	int64		x = ((const map_cell *) a)->lo,
				y = ((const map_cell *) b)->lo;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void
load_density_map(Oid statrel, AttrNumber attnum, dens_entry *e)
{
	Oid			maprel;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	ScanKeyData skey[2];
	TupleDesc	td;
	map_cell   *cells = NULL;
	int			cap = 0,
				n = 0;

	e->nmap = 0;
	e->map_lo = e->map_hi = NULL;
	e->map_n = NULL;
	e->map_total = 0;

	maprel = RelnameGetRelid("skycell_density_map");
	if (!OidIsValid(maprel))
		return;

	rel = table_open(maprel, AccessShareLock);
	td = RelationGetDescr(rel);
	if (td->natts < 4)
	{
		table_close(rel, AccessShareLock);
		return;
	}
	ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(statrel));
	ScanKeyInit(&skey[1], 2, BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(attnum));
	scan = table_beginscan(rel, GetActiveSnapshot(), 2, skey);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		isnull;
		Datum		dn = heap_getattr(tup, 3, td, &isnull);
		int64		nuniq,
					cnt,
					pix;
		int			order;

		if (isnull)
			continue;
		nuniq = DatumGetInt64(dn);
		cnt = DatumGetInt64(heap_getattr(tup, 4, td, &isnull));
		if (isnull || nuniq < 4 || cnt <= 0)
			continue;
		order = sc_nuniq_decode(nuniq, &pix);
		if (order < 0 || order > SC_MAX_ORDER || pix < 0)
			continue;
		if (n == cap)
		{
			cap = cap ? cap * 2 : 1024;
			cells = cells ? repalloc(cells, sizeof(map_cell) * cap)
				: palloc(sizeof(map_cell) * cap);
		}
		cells[n].lo = sc_pix_lo(order, pix);
		cells[n].hi = sc_pix_hi(order, pix);
		cells[n].n = (double) cnt;
		e->map_total += (double) cnt;
		n++;
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	if (n <= 0)
		return;
	qsort(cells, n, sizeof(map_cell), map_cell_cmp);
	e->map_lo = palloc(sizeof(int64) * n);
	e->map_hi = palloc(sizeof(int64) * n);
	e->map_n = palloc(sizeof(double) * n);
	for (int i = 0; i < n; i++)
	{
		e->map_lo[i] = cells[i].lo;
		e->map_hi[i] = cells[i].hi;
		e->map_n[i] = cells[i].n;
	}
	pfree(cells);
	e->nmap = n;
}

static void
load_density_cached(Oid relid, Oid statrel, AttrNumber attnum, sc_density *d)
{
	dens_key	key = {statrel, attnum};
	dens_entry *entry;
	bool		found;

	skycell_cache_init();
	entry = (dens_entry *) hash_search(dens_cache, &key, HASH_ENTER, &found);
	if (!found)
	{
		sc_density	fresh;
		MemoryContext old = MemoryContextSwitchTo(skycell_cache_cxt);

		/* load in the cache context so the bounds survive the query */
		load_density_from(relid, statrel, attnum, &fresh);
		MemoryContextSwitchTo(old);
		entry->ntotal = fresh.ntotal;
		entry->nbounds = fresh.nbounds;
		entry->bounds = (int64 *) fresh.bounds;
		entry->relpages = fresh.relpages;
		old = MemoryContextSwitchTo(skycell_cache_cxt);
		load_density_map(statrel, attnum, entry);
		MemoryContextSwitchTo(old);
	}
	d->ntotal = entry->ntotal;
	d->relpages = entry->relpages;
	d->nbounds = entry->nbounds;
	d->nmap = entry->nmap;
	d->map_lo = entry->map_lo;
	d->map_hi = entry->map_hi;
	d->map_n = entry->map_n;
	d->map_total = entry->map_total;
	d->bounds = NULL;
	if (entry->nbounds > 0)
	{
		int64	   *copy = palloc(sizeof(int64) * entry->nbounds);

		memcpy(copy, entry->bounds, sizeof(int64) * entry->nbounds);
		d->bounds = copy;
	}
}

static void
load_density(Oid relid, AttrNumber attnum, sc_density *d)
{
	load_density_cached(relid, relid, attnum, d);
}

/*
 * OID of skycell's own btree operator family (skycell--0.7.sql), whose
 * operators (#<, #<=, #=, #>=, #>) carry skycell_cellsel instead of the
 * stock int8 estimators.  InvalidOid before 0.7 or if the name was changed;
 * every caller treats that as "fall back to the stock operators".
 */
Oid
cell_ops_opfamily(void)
{
	List	   *name = list_make1(makeString("skycell_cell_ops"));

	return get_opfamily_oid(BTREE_AM_OID, name, true);
}

/*
 * The cell argument is an expression over one base relation, e.g.
 * skycell_ang2cell(s_ra, s_dec): use the statistics of an expression index
 * whose first column is that same expression.  Returns false if none.
 * *uses_cell_ops reports whether the matching index's opclass is
 * skycell_cell_ops, so callers building non-constant range predicates (a
 * cross-match) can emit its operators and get a real selectivity estimate
 * instead of the stock DEFAULT_INEQ_SEL fallback -- see skycell_cellsel().
 */
bool
density_for_expr(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
				  bool *uses_cell_ops)
{
	List	   *vars = pull_var_clause(arg, 0);
	ListCell   *lc;
	Index		varno = 0;
	RangeTblEntry *rte;
	Relation	rel;
	List	   *indexes;
	Node	   *key;
	Oid			matched = InvalidOid;
	Oid			cellops_oid;
	bool		found = false;
	bool		matched_uses_cell_ops = false;

	*uses_cell_ops = false;
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varlevelsup != 0 || (varno != 0 && v->varno != varno))
			return false;
		varno = v->varno;
	}
	if (varno == 0 || varno > list_length(root->parse->rtable))
		return false;
	rte = rt_fetch(varno, root->parse->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;

	/* index expressions are stored with varno 1 */
	key = copyObject(arg);
	ChangeVarNodes(key, varno, 1, 0);

	/* the index this expression was matched to last time, if still valid */
	skycell_cache_init();
	{
		idx_entry  *cached = (idx_entry *) hash_search(idx_cache, &rte->relid,
													   HASH_FIND, NULL);

		if (cached != NULL && equal(cached->expr, key))
		{
			if (!OidIsValid(cached->indexoid))
				return false;
			load_density_cached(rte->relid, cached->indexoid, 1, d);
			*statrel = cached->indexoid;
			*uses_cell_ops = cached->uses_cell_ops;
			return true;
		}
	}

	/*
	 * Several indexes can match the same expression (a plain one plus a
	 * skycell_cell_ops one added alongside it, per the README).  Scan all of
	 * them and prefer a cell_ops match over a plain one -- picking whichever
	 * comes first in RelationGetIndexList() would otherwise pick the plain
	 * index whenever it happens to sort first, silently discarding the
	 * selectivity fix cell_ops exists for.
	 */
	cellops_oid = cell_ops_opfamily();
	rel = table_open(rte->relid, NoLock);	/* locked by the parser */
	indexes = RelationGetIndexList(rel);
	foreach(lc, indexes)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	irel = index_open(indexoid, AccessShareLock);

		if (irel->rd_index->indnatts >= 1 && irel->rd_index->indkey.values[0] == 0)
		{
			List	   *exprs = RelationGetIndexExpressions(irel);

			if (exprs != NIL && equal(linitial(exprs), key))
			{
				bool		this_uses_cell_ops = OidIsValid(cellops_oid) &&
					irel->rd_opfamily[0] == cellops_oid;

				if (!found || (this_uses_cell_ops && !matched_uses_cell_ops))
				{
					load_density_cached(rte->relid, indexoid, 1, d);
					found = true;
					matched = indexoid;
					*statrel = indexoid;
					matched_uses_cell_ops = this_uses_cell_ops;
				}
			}
		}
		index_close(irel, AccessShareLock);
		if (matched_uses_cell_ops)
			break;					/* nothing beats a cell_ops match */
	}
	list_free(indexes);
	table_close(rel, NoLock);
	*uses_cell_ops = matched_uses_cell_ops;

	/* remember the outcome, including "this relation has no cell index" */
	{
		idx_entry  *entry;
		bool		hit;
		MemoryContext old;

		skycell_cache_init();
		entry = (idx_entry *) hash_search(idx_cache, &rte->relid, HASH_ENTER, &hit);
		old = MemoryContextSwitchTo(skycell_cache_cxt);
		entry->indexoid = matched;
		entry->expr = copyObject(key);
		entry->uses_cell_ops = matched_uses_cell_ops;
		MemoryContextSwitchTo(old);
	}
	return found;
}

/*
 * The region-area analogue of density_for_expr(), for the region-region
 * operator selectivity fix (adql.c's region_angle_est(), "Round forty-
 * eight"): given a region expression (in practice almost always a bare
 * column), looks for a plain btree expression index on area(<that same
 * expression>) and, if ANALYZE has run on it, returns a representative
 * value from its histogram -- in whatever unit the SQL area() function
 * itself returns (square degrees: skycell_area() multiplies
 * sc_region.area, in steradians, by RAD2DEG twice), not the steradians
 * every other caller of sc_region.area in this file works in. This
 * function's contract is "whatever area() would return for this
 * expression," matching the index a user would actually write
 * (CREATE INDEX ... (area(region_col))), not an internal-only unit
 * nothing else here uses; converting back to steradians is the caller's
 * job (region_angle_est() does it), not this function's.
 *
 * Deliberately a separate, simpler function rather than a generalisation
 * of density_for_expr(): that function's statistics are a *point*-density
 * model (a histogram of cell-id boundaries, optionally refined by
 * skycell_density_build()'s own multi-order count map) -- meaningless for
 * a plain distribution of float8 area values, which is all ANALYZE ever
 * needs to have built for an ordinary expression index, no custom
 * density map involved. No caching across calls within a plan either
 * (density_for_expr()'s idx_cache is specific to its own cell-expression
 * lookups) -- this runs only for region-region clauses, not the
 * potentially-hot point-rewrite path, so the lookup cost was not worth
 * the extra bookkeeping to avoid.
 *
 * The histogram's middle bound (its median -- see the comment at the
 * actual lookup below for why not the mean) stands in for the "typical"
 * area. Not a rigorous single-number summary of a whole distribution
 * (it ignores most-common-values entirely, and ANALYZE's own histogram is
 * itself a sample, not the true population), a deliberate approximation
 * in keeping with this file's other selectivity heuristics: legibility
 * over precision for a correction whose whole job is "don't assume the
 * other side has zero area," not a load-bearing statistic.
 */
bool
typical_region_area(Oid selfid, PlannerInfo *root, Node *region_expr, double *area)
{
	List	   *vars = pull_var_clause(region_expr, 0);
	ListCell   *lc;
	Index		varno = 0;
	RangeTblEntry *rte;
	Relation	rel;
	List	   *indexes;
	Node	   *probe;
	Oid			regiontype;
	Oid			one[1];
	bool		found = false;

	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varlevelsup != 0 || (varno != 0 && v->varno != varno))
			return false;
		varno = v->varno;
	}
	if (varno == 0 || varno > list_length(root->parse->rtable))
		return false;
	rte = rt_fetch(varno, root->parse->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;

	regiontype = exprType(region_expr);
	one[0] = regiontype;
	probe = (Node *) makeFuncExpr(lookup_sibling_func(selfid, "area", 1, one),
								  FLOAT8OID, list_make1(copyObject(region_expr)),
								  InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	/* index expressions are stored with varno 1 */
	ChangeVarNodes(probe, varno, 1, 0);

	rel = table_open(rte->relid, NoLock);	/* locked by the parser */
	indexes = RelationGetIndexList(rel);
	foreach(lc, indexes)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	irel = index_open(indexoid, AccessShareLock);

		if (irel->rd_rel->relam == BTREE_AM_OID &&
			irel->rd_index->indnatts >= 1 && irel->rd_index->indkey.values[0] == 0)
		{
			List	   *exprs = RelationGetIndexExpressions(irel);

			if (exprs != NIL && equal(linitial(exprs), probe))
			{
				for (int inh = 0; inh <= 1 && !found; inh++)
				{
					HeapTuple	tp = SearchSysCache3(STATRELATTINH,
													 ObjectIdGetDatum(indexoid),
													 Int16GetDatum(1), BoolGetDatum(inh));

					if (HeapTupleIsValid(tp))
					{
						AttStatsSlot sslot;

						if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_HISTOGRAM,
											 InvalidOid, ATTSTATSSLOT_VALUES))
						{
							if (sslot.nvalues >= 2 && sslot.valuetype == FLOAT8OID)
							{
								/*
								 * The histogram's own bounds are equal-
								 * frequency quantile boundaries, so the
								 * middle one is the distribution's real
								 * median -- robust to a skewed or
								 * multi-modal mix of region sizes (a
								 * catalogue with both small source
								 * footprints and a few huge survey tiles,
								 * say) in a way a plain average of all the
								 * bounds is not: that average is pulled
								 * toward whichever tail has the most
								 * extreme values, not whichever sizes most
								 * rows actually have.
								 */
								*area = DatumGetFloat8(sslot.values[sslot.nvalues / 2]);
								found = true;
							}
							free_attstatsslot(&sslot);
						}
						ReleaseSysCache(tp);
					}
				}
			}
		}
		index_close(irel, AccessShareLock);
		if (found)
			break;
	}
	list_free(indexes);
	table_close(rel, NoLock);
	return found;
}

/*
 * "Which of my regions contain this point" is the opposite direction from
 * density_for_expr's own case (a point-catalog index answering many
 * points against one region): here it's the *region* column that needs an
 * index, over many candidate rows against one (or a few, per-row-joined)
 * points. skycell_region_moc(region, max_cells, max_order) already gives
 * every row a small array of covering cells (see skycell--0.11.sql's "MOC-
 * in-a-B-tree" comment block); a plain GIN index on that expression
 * indexes the array-overlap test the manual side-table recipe there
 * computes by hand, so this is that same recipe, minus the side table.
 *
 * On success, *moc_expr is the actual indexed expression (copied, varno
 * changed from the index's own convention back to rg's real range table
 * entry, ready to embed in the caller's rewritten query -- it must match
 * the index's stored expression exactly, or the planner won't recognise
 * it as indexable), and *max_order is the max_order argument that
 * expression's skycell_region_moc() call was actually built with.
 *
 * That max_order is used as an exact, safe upper bound on the ancestor
 * lookup this enables (skycell_ancestors(point_cell, 0, max_order)): no
 * row's covering can use a finer cell than the index's own max_order
 * allows, so capping there cannot miss a match. The *lower* end is left
 * at 0 deliberately, not narrowed to whatever coarsest order the data
 * happens to use: that would depend on which regions actually exist in
 * the table, which only a real scan (or a statistics sample, which is not
 * guaranteed complete) can answer, and guessing wrong there risks a false
 * negative -- silently dropping a genuine match -- with no recheck able
 * to catch it, unlike a spurious candidate. A user who knows their own
 * corpus never needs cells coarser than some order can already get that
 * narrower range by hand, by passing a smaller max_order to
 * skycell_region_moc() when creating the index; this function only ever
 * reads back whatever was actually asked for.
 */
bool
gin_moc_index_for_region(PlannerInfo *root, Node *rg, Node **moc_expr, int *max_order)
{
	List	   *vars = pull_var_clause(rg, 0);
	ListCell   *lc;
	Index		varno = 0;
	RangeTblEntry *rte;
	Relation	rel;
	List	   *indexes;
	Node	   *key;
	bool		found = false;

	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varlevelsup != 0 || (varno != 0 && v->varno != varno))
			return false;
		varno = v->varno;
	}
	if (varno == 0 || varno > list_length(root->parse->rtable))
		return false;
	rte = rt_fetch(varno, root->parse->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;

	/* index expressions are stored with varno 1, same convention as
	 * density_for_expr's own key above */
	key = copyObject(rg);
	ChangeVarNodes(key, varno, 1, 0);

	rel = table_open(rte->relid, NoLock);	/* locked by the parser */
	indexes = RelationGetIndexList(rel);
	foreach(lc, indexes)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	irel = index_open(indexoid, AccessShareLock);

		if (irel->rd_rel->relam == GIN_AM_OID &&
			irel->rd_index->indnatts >= 1 && irel->rd_index->indkey.values[0] == 0)
		{
			List	   *exprs = RelationGetIndexExpressions(irel);

			if (exprs != NIL && IsA(linitial(exprs), FuncExpr))
			{
				FuncExpr   *fe = (FuncExpr *) linitial(exprs);
				char	   *name = get_func_name(fe->funcid);

				if (name != NULL && strcmp(name, "skycell_region_moc") == 0 &&
					list_length(fe->args) == 3 && equal(linitial(fe->args), key))
				{
					Node	   *mo = (Node *) lthird(fe->args);

					if (IsA(mo, Const) && !((Const *) mo)->constisnull)
					{
						Node	   *out = copyObject((Node *) fe);

						ChangeVarNodes(out, 1, varno, 0);
						*moc_expr = out;
						*max_order = DatumGetInt32(((Const *) mo)->constvalue);
						found = true;
					}
				}
			}
		}
		index_close(irel, AccessShareLock);
		if (found)
			break;
	}
	list_free(indexes);
	table_close(rel, NoLock);
	return found;
}

/*
 * A plain int8[] && int8[] OpExpr -- the operator the GIN opclass built
 * into every PostgreSQL install already indexes. Resolved with oper(),
 * not OpernameGetOprid(): the catalog entry is the polymorphic anyarray
 * && anyarray, not one specific to int8[], and only oper()'s parser-grade
 * matching (the same logic "a && b" in a query goes through) recognises
 * int8[] as a valid anyarray argument -- an exact-match OID lookup on
 * (int8[], int8[]) finds nothing and this returns NULL.
 */
Expr *
array_overlap_expr(Node *left, Expr *right)
{
	Operator	tup = oper(NULL, list_make1(makeString("&&")),
						  INT8ARRAYOID, INT8ARRAYOID, true, -1);
	Oid			opno;
	Expr	   *e;

	if (tup == NULL)
		return NULL;
	opno = oprid(tup);
	ReleaseSysCache(tup);
	if (!OidIsValid(opno))
		return NULL;
	e = make_opclause(opno, BOOLOID, false, (Expr *) copyObject(left), right,
					  InvalidOid, InvalidOid);
	set_opfuncid((OpExpr *) e);
	return e;
}

/*
 * density of the relation the cell argument comes from.  *uses_cell_ops
 * reports whether the matching index (if any) uses skycell_cell_ops -- see
 * density_for_expr().
 */
void
density_for_var(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
				 bool *uses_cell_ops)
{
	d->ntotal = 0;
	d->nbounds = 0;
	d->bounds = NULL;
	*statrel = InvalidOid;
	*uses_cell_ops = false;

	if (root && root->parse && !IsA(arg, Var) && !IsA(arg, Const))
	{
		density_for_expr(root, arg, d, statrel, uses_cell_ops);
		return;
	}
	if (root && root->parse && IsA(arg, Var))
	{
		Var		   *var = (Var *) arg;

		if (var->varlevelsup == 0 && var->varattno > 0 &&
			var->varno >= 1 && var->varno <= list_length(root->parse->rtable))
		{
			RangeTblEntry *rte = rt_fetch(var->varno, root->parse->rtable);

			if (rte->rtekind == RTE_RELATION)
			{
				load_density(rte->relid, var->varattno, d);
				*statrel = rte->relid;
			}
		}
	}
}

Oid
lookup_sibling_func(Oid funcid, const char *name, int nargs, const Oid *argtypes)
{
	char	   *nsp = get_namespace_name(get_func_namespace(funcid));

	return LookupFuncName(list_make2(makeString(nsp), makeString(pstrdup(name))),
						  nargs, argtypes, false);
}

Const *
int8_const(int64 v)
{
	return makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
					 Int64GetDatum(v), false, true);
}

Const *
float8_const(double v)
{
	return makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
					 Float8GetDatum(v), false, true);
}

Const *
int4_const(int32 v)
{
	return makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
					 Int32GetDatum(v), false, true);
}

static Expr *
int8_cmp_family(int strategy, Node *left, Expr *right, Oid opfamily)
{
	Oid			opno = get_opfamily_member(opfamily, INT8OID, INT8OID, strategy);
	Expr	   *e = make_opclause(opno, BOOLOID, false, (Expr *) copyObject(left),
								  right, InvalidOid, InvalidOid);

	set_opfuncid((OpExpr *) e);
	return e;
}

Expr *
int8_cmp(int strategy, Node *left, Expr *right)
{
	return int8_cmp_family(strategy, left, right, INTEGER_BTREE_FAM_OID);
}

/* cell >= lo AND cell <= hi */
Expr *
range_arm_family(Node *cell, Expr *lo, Expr *hi, Oid opfamily)
{
	return makeBoolExpr(AND_EXPR,
						list_make2(int8_cmp_family(BTGreaterEqualStrategyNumber, cell, lo, opfamily),
								   int8_cmp_family(BTLessEqualStrategyNumber, cell, hi, opfamily)),
						-1);
}

Expr *
range_arm(Node *cell, Expr *lo, Expr *hi)
{
	return range_arm_family(cell, lo, hi, INTEGER_BTREE_FAM_OID);
}

static bool
all_const(List *args, int from, int to, bool *anynull)
{
	*anynull = false;
	for (int i = from; i <= to; i++)
	{
		Node	   *a = list_nth(args, i);

		if (!IsA(a, Const))
			return false;
		if (((Const *) a)->constisnull)
			*anynull = true;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* planner support: skycell_cone / skycell_poly -> ranges + exact     */
/* ------------------------------------------------------------------ */

Node *
ranges_and_exact(sc_cover *cov, Node *cell, Expr *exact, bool uses_cell_ops)
{
	List	   *arms = NIL;
	Oid			opfamily = uses_cell_ops ? cell_ops_opfamily() : INTEGER_BTREE_FAM_OID;

	if (cov->n == 0)
		return (Node *) makeBoolConst(false, false);

	for (int i = 0; i < cov->n; i++)
		arms = lappend(arms, range_arm_family(cell,
									   (Expr *) int8_const(cov->r[i].lo),
									   (Expr *) int8_const(cov->r[i].hi),
									   opfamily));
	if (cov->n == 1)
		return (Node *) makeBoolExpr(AND_EXPR,
									 list_concat(((BoolExpr *) linitial(arms))->args,
												 list_make1(exact)),
									 -1);
	return (Node *) makeBoolExpr(AND_EXPR,
								 list_make2(makeBoolExpr(OR_EXPR, arms, -1), exact),
								 -1);
}

static Node *
simplify_cone(PlannerInfo *root, FuncExpr *fexpr)
{
	List	   *args = fexpr->args;
	Node	   *cell = linitial(args);
	bool		anynull;
	Oid			exact_types[6] = {FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID};
	Oid			exact_oid = lookup_sibling_func(fexpr->funcid, "skycell_in_cone", 6, exact_types);
	sc_density	dens;
	Oid			dens_statrel;
	bool		cell_uses_cell_ops;

	density_for_var(root, cell, &dens, &dens_statrel, &cell_uses_cell_ops);

	if (all_const(args, 3, 5, &anynull))
	{
		sc_region	reg;
		sc_cover	cov;
		sc_cover_params p;
		double		sel;
		Expr	   *exact;

		if (anynull)
			return (Node *) makeBoolConst(false, true);

		check_err(sc_region_cone(&reg,
								 DatumGetFloat8(((Const *) list_nth(args, 3))->constvalue),
								 DatumGetFloat8(((Const *) list_nth(args, 4))->constvalue),
								 DatumGetFloat8(((Const *) list_nth(args, 5))->constvalue)));
		current_params(&p, skycell_max_ranges, &dens);
		cover_cached(&reg, &dens, &p, dens_statrel,
					 DatumGetFloat8(((Const *) list_nth(args, 3))->constvalue),
					 DatumGetFloat8(((Const *) list_nth(args, 4))->constvalue),
					 DatumGetFloat8(((Const *) list_nth(args, 5))->constvalue), &cov);

		sel = (cov.area > 0) ? fmin(1.0, reg.area / cov.area) : 0.0;
		exact = (Expr *) makeFuncExpr(exact_oid, BOOLOID,
									  list_make5(copyObject(list_nth(args, 1)),
												 copyObject(list_nth(args, 2)),
												 copyObject(list_nth(args, 3)),
												 copyObject(list_nth(args, 4)),
												 copyObject(list_nth(args, 5))),
									  InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
		((FuncExpr *) exact)->args = lappend(((FuncExpr *) exact)->args, float8_const(sel));
		return ranges_and_exact(&cov, cell, exact, cell_uses_cell_ops);
	}
	else
	{
		/* run-time slots, one covering per distinct (ra0, dec0, radius) */
		Oid			bound_types[7] = {FLOAT8OID, FLOAT8OID, FLOAT8OID, INT4OID, INT4OID, FLOAT8OID, INT8ARRAYOID};
		Oid			bound_oid = lookup_sibling_func(fexpr->funcid, "skycell_cone_bound", 7, bound_types);
		int			k = skycell_join_slots;
		Oid			arm_opfamily = cell_uses_cell_ops
			? cell_ops_opfamily() : INTEGER_BTREE_FAM_OID;
		Const	   *hist;
		List	   *arms = NIL;
		Datum	   *hd = palloc(sizeof(Datum) * Max(dens.nbounds, 1));
		FuncExpr   *exact;

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
											 list_make5(copyObject(list_nth(args, 3)),
														copyObject(list_nth(args, 4)),
														copyObject(list_nth(args, 5)),
														int4_const(2 * s + j),
														int4_const(k)),
											 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
			for (int j = 0; j < 2; j++)
				((FuncExpr *) b[j])->args = lappend(lappend(((FuncExpr *) b[j])->args,
															float8_const(dens.ntotal)),
													copyObject(hist));
			arms = lappend(arms, range_arm_family(cell, b[0], b[1], arm_opfamily));
		}
		exact = makeFuncExpr(exact_oid, BOOLOID,
							 list_make5(copyObject(list_nth(args, 1)),
										copyObject(list_nth(args, 2)),
										copyObject(list_nth(args, 3)),
										copyObject(list_nth(args, 4)),
										copyObject(list_nth(args, 5))),
							 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
		exact->args = lappend(exact->args, float8_const(-1.0));
		return (Node *) makeBoolExpr(AND_EXPR,
									 list_make2(k == 1 ? linitial(arms) : makeBoolExpr(OR_EXPR, arms, -1),
												exact),
									 -1);
	}
}

static Node *
simplify_poly(PlannerInfo *root, FuncExpr *fexpr)
{
	List	   *args = fexpr->args;
	Node	   *cell = linitial(args);
	Node	   *poly = lfourth(args);
	sc_density	dens;
	Oid			dens_statrel;
	Oid			exact_types[4] = {FLOAT8OID, FLOAT8OID, FLOAT8ARRAYOID, FLOAT8OID};
	bool		uses_cell_ops;

	density_for_var(root, cell, &dens, &dens_statrel, &uses_cell_ops);

	if (IsA(poly, Const))
	{
		sc_region	reg;
		sc_cover	cov;
		sc_cover_params p;
		double		sel;
		FuncExpr   *exact;

		if (((Const *) poly)->constisnull)
			return (Node *) makeBoolConst(false, true);

		poly_from_array(DatumGetArrayTypeP(((Const *) poly)->constvalue), &reg);
		current_params(&p, skycell_max_ranges, &dens);
		sc_cover_compute(&reg, &dens, &p, &cov);
		sel = (cov.area > 0) ? fmin(1.0, reg.area / cov.area) : 0.0;

		exact = makeFuncExpr(lookup_sibling_func(fexpr->funcid, "skycell_in_poly", 4, exact_types),
							 BOOLOID,
							 list_make4(copyObject(lsecond(args)), copyObject(lthird(args)),
										copyObject(poly), float8_const(sel)),
							 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
		return ranges_and_exact(&cov, cell, (Expr *) exact, uses_cell_ops);
	}
	else
	{
		/*
		 * Run-time slots, one covering per distinct polygon row -- the same
		 * mechanism simplify_cone's non-constant branch uses for a
		 * cross-match cone, except the covering is keyed on the polygon's
		 * own bytes (skycell_poly_bound) rather than three scalars, since a
		 * polygon has no fixed-arity description to pass around directly.
		 *
		 * skycell_cellsel (the skycell_cell_ops estimator) only recognises
		 * skycell_cone_bound's radius argument, so unlike the cone case
		 * these range quals fall back to the stock DEFAULT_INEQ_SEL even
		 * against a skycell_cell_ops index -- there is no equivalent
		 * "radius" to recover a polygon's area from a non-constant, per-row
		 * shape. The ranges are still index-backed and correct either way;
		 * only the row-count estimate that costs the plan is coarser.
		 */
		Oid			bound_types[5] = {FLOAT8ARRAYOID, INT4OID, INT4OID, FLOAT8OID, INT8ARRAYOID};
		Oid			bound_oid = lookup_sibling_func(fexpr->funcid, "skycell_poly_bound", 5, bound_types);
		int			k = skycell_join_slots;
		Oid			arm_opfamily = uses_cell_ops
			? cell_ops_opfamily() : INTEGER_BTREE_FAM_OID;
		Const	   *hist;
		List	   *arms = NIL;
		Datum	   *hd = palloc(sizeof(Datum) * Max(dens.nbounds, 1));
		FuncExpr   *exact;

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
											 list_make3(copyObject(poly),
														int4_const(2 * s + j),
														int4_const(k)),
											 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
			for (int j = 0; j < 2; j++)
				((FuncExpr *) b[j])->args = lappend(lappend(((FuncExpr *) b[j])->args,
															float8_const(dens.ntotal)),
													copyObject(hist));
			arms = lappend(arms, range_arm_family(cell, b[0], b[1], arm_opfamily));
		}
		exact = makeFuncExpr(lookup_sibling_func(fexpr->funcid, "skycell_in_poly", 4, exact_types),
							 BOOLOID,
							 list_make4(copyObject(lsecond(args)), copyObject(lthird(args)),
										copyObject(poly), float8_const(-1.0)),
							 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
		return (Node *) makeBoolExpr(AND_EXPR,
									 list_make2(k == 1 ? linitial(arms) : makeBoolExpr(OR_EXPR, arms, -1),
												exact),
									 -1);
	}
}

/*
 * skycell_poly_join(ra, dec, poly): the Q3C-poly-query-shaped spelling for a
 * cross-match whose polygon comes from another table's row. Like
 * simplify_cone5 for skycell_join/skycell_radial_query, it synthesises the
 * index expression from the first two arguments and hands the four-argument
 * form to simplify_poly.
 */
static Node *
simplify_poly3(PlannerInfo *root, FuncExpr *fexpr)
{
	Oid			a2c_types[2] = {FLOAT8OID, FLOAT8OID};
	Oid			a2c_oid = lookup_sibling_func(fexpr->funcid, "skycell_ang2cell",
											  2, a2c_types);
	FuncExpr   *four = copyObject(fexpr);
	Expr	   *cell;

	cell = (Expr *) makeFuncExpr(a2c_oid, INT8OID,
								 list_make2(copyObject(linitial(fexpr->args)),
											copyObject(lsecond(fexpr->args))),
								 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

	four->args = lcons(cell, four->args);
	return simplify_poly(root, four);
}

/*
 * skycell_join(ra1, dec1, ra2, dec2, radius) and
 * skycell_radial_query(ra, dec, ra0, dec0, radius): the Q3C-shaped spellings.
 * They do not carry the index expression, so synthesise it from the first two
 * arguments and hand the six-argument form to simplify_cone.  The planner then
 * matches skycell_ang2cell(ra, dec) against an expression index exactly as it
 * does when the caller writes it out; with no such index the rewrite still
 * yields a correct sequential plan, which is what q3c does too.
 */
static Node *
simplify_cone5(PlannerInfo *root, FuncExpr *fexpr)
{
	Oid			a2c_types[2] = {FLOAT8OID, FLOAT8OID};
	Oid			a2c_oid = lookup_sibling_func(fexpr->funcid, "skycell_ang2cell",
											  2, a2c_types);
	FuncExpr   *six = copyObject(fexpr);
	Expr	   *cell;

	cell = (Expr *) makeFuncExpr(a2c_oid, INT8OID,
								 list_make2(copyObject(linitial(fexpr->args)),
											copyObject(lsecond(fexpr->args))),
								 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

	six->args = lcons(cell, six->args);
	return simplify_cone(root, six);
}

PG_FUNCTION_INFO_V1(skycell_support);
Datum
skycell_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		int			nargs = list_length(req->fcall->args);

		if (nargs == 6)
			PG_RETURN_POINTER(simplify_cone(req->root, req->fcall));
		if (nargs == 3)
			PG_RETURN_POINTER(simplify_poly3(req->root, req->fcall));
		if (nargs == 5)
			PG_RETURN_POINTER(simplify_cone5(req->root, req->fcall));
		if (nargs == 4)
			PG_RETURN_POINTER(simplify_poly(req->root, req->fcall));
	}
	PG_RETURN_POINTER(NULL);
}

/*
 * Selectivity of the exact test.  The rewrite passes area(region) /
 * area(covering) as the last argument, so that sel(ranges) * sel(exact)
 * approximates the true fraction.  For run-time slots (-1) fall back to the
 * uniform-sky fraction when the radius is known.
 */
PG_FUNCTION_INFO_V1(skycell_exact_support);
Datum
skycell_exact_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSelectivity))
	{
		SupportRequestSelectivity *req = (SupportRequestSelectivity *) rawreq;
		Node	   *last = llast(req->args);
		double		s = -1;

		if (IsA(last, Const) && !((Const *) last)->constisnull)
			s = DatumGetFloat8(((Const *) last)->constvalue);
		if (s < 0 && list_length(req->args) == 6 && IsA(list_nth(req->args, 4), Const) &&
			!((Const *) list_nth(req->args, 4))->constisnull)
		{
			double		r = DatumGetFloat8(((Const *) list_nth(req->args, 4))->constvalue) * DEG2RAD;

			s = pow(sin(fmin(fmax(r, 0), M_PI) / 2.0), 2);	/* cap area / 4pi */
		}
		if (s < 0)
			s = 1e-4;
		req->selectivity = fmin(1.0, fmax(s, 1e-12));
		PG_RETURN_POINTER(req);
	}
	PG_RETURN_POINTER(NULL);
}

/*
 * Restriction selectivity for the cell-range quals.
 *
 * A covering becomes `cell >= lo AND cell <= hi`.  When lo and hi are constants
 * the stock estimator reads the histogram and does well.  When they are not --
 * a cross-match, where the bounds come from the probe row -- PostgreSQL has
 * nothing to go on and returns DEFAULT_INEQ_SEL (1/3) for each side, so a pair
 * of them estimates 1/9 of the table per probe.  On a 20-million-row relation
 * that is 2.3 million rows where the truth is about one, the parameterised
 * index path is costed out of existence, and the planner picks a sequential
 * scan that we measured at more than 90 s against 145 ms for the path it
 * rejected.
 *
 * The radius is recoverable in the cone shapes we emit, so the covering's
 * sky fraction can be estimated instead of guessed:
 *
 *   join form     bound is skycell_cone_bound(ra, dec, radius, ...), radius Const
 *   LATERAL form  bound is a Var of a function scan over
 *                 skycell_cone_ranges(ra, dec, radius), radius Const
 *
 * Each of the two quals returns the square root of the cap fraction divided
 * by the slot count, so that their product is the fraction of the sphere one
 * slot's arm is expected to cover -- the join form OR's *nslots* such arms
 * together (one per range of the covering), each getting its own pair of
 * quals, so charging every arm the *whole* cone's fraction would overstate
 * the OR's combined selectivity by about nslots (Postgres combines OR'd
 * clauses by independence: 1-(1-s)^n =~ n*s for small s).  The LATERAL form
 * has no slot count -- each row from skycell_cone_ranges() is its own join,
 * not one arm of a fixed disjunction -- so it keeps the whole fraction.
 *
 * That still ignores the covering's overshoot, so it is an under-estimate of
 * the rows scanned by a factor of the area ratio (1.1 at a degree, up to
 * ~100 at an arcsecond); it is wrong in the direction that favours the index
 * path, which is the direction the measurement says is right, and it is
 * five to six orders of magnitude closer than the default it replaces.
 *
 * skycell_poly_bound/skycell_region_bound (non-constant polygon/region
 * cross-matches) have no such scalar to recover: the shape is a per-row
 * argument, and its area isn't known until it's actually evaluated. Still
 * recognising the call -- for nslots, and to tell skycell_cellsel this is a
 * bound it understands rather than an arbitrary expression -- matters even
 * without a real fraction: measured with EXPLAIN ANALYZE, leaving it
 * unrecognised (falling through to DEFAULT_INEQ_SEL below) inflates a
 * probe's estimated cost by about four orders of magnitude, which reliably
 * pushes small, sub-millisecond-per-probe cross-matches over jit_above_cost
 * and pays for JIT compilation nothing here is big enough to earn back --
 * roughly doubling wall-clock time in one measured case. Returning 0 (never
 * a legal radius) tells the caller "recognised, size unknown" so it can use
 * a flat, deliberately small fallback instead.
 */
static double
radius_from_bound(PlannerInfo *root, Node *arg, int *nslots_out)
{
	*nslots_out = 1;
	if (arg && IsA(arg, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) arg;
		char	   *name = get_func_name(f->funcid);

		/* skycell_cone_bound(ra0, dec0, radius, i, nslots, ntotal, hist) */
		if (list_length(f->args) == 7 && IsA(list_nth(f->args, 2), Const) &&
			!((Const *) list_nth(f->args, 2))->constisnull)
		{
			Node	   *nslots_arg = list_nth(f->args, 4);

			if (IsA(nslots_arg, Const) && !((Const *) nslots_arg)->constisnull)
				*nslots_out = Max(1, DatumGetInt32(((Const *) nslots_arg)->constvalue));
			return DatumGetFloat8(((Const *) list_nth(f->args, 2))->constvalue);
		}

		/* skycell_poly_bound(poly, i, nslots, ntotal, hist) / skycell_region_bound(region, i, nslots, ntotal, hist) */
		if (list_length(f->args) == 5 && name != NULL &&
			(strcmp(name, "skycell_poly_bound") == 0 || strcmp(name, "skycell_region_bound") == 0))
		{
			Node	   *nslots_arg = list_nth(f->args, 2);

			if (IsA(nslots_arg, Const) && !((Const *) nslots_arg)->constisnull)
				*nslots_out = Max(1, DatumGetInt32(((Const *) nslots_arg)->constvalue));
			return 0.0;			/* recognised, but no size to estimate from */
		}
	}
	if (arg && IsA(arg, Var) && root && root->parse)
	{
		Var		   *v = (Var *) arg;

		if (v->varlevelsup == 0 && v->varno >= 1 &&
			v->varno <= list_length(root->parse->rtable))
		{
			RangeTblEntry *rte = rt_fetch(v->varno, root->parse->rtable);

			if (rte->rtekind == RTE_FUNCTION && rte->functions)
			{
				RangeTblFunction *rtf = (RangeTblFunction *) linitial(rte->functions);

				if (rtf->funcexpr && IsA(rtf->funcexpr, FuncExpr))
				{
					FuncExpr   *f = (FuncExpr *) rtf->funcexpr;

					/* skycell_cone_ranges(ra0, dec0, radius [, tbl, col]) */
					if (list_length(f->args) >= 3 && IsA(list_nth(f->args, 2), Const) &&
						!((Const *) list_nth(f->args, 2))->constisnull)
						return DatumGetFloat8(((Const *) list_nth(f->args, 2))->constvalue);
				}
			}
		}
	}
	return -1;
}

PG_FUNCTION_INFO_V1(skycell_cellsel);
Datum
skycell_cellsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	double		sel = 0.3333333333333333;	/* DEFAULT_INEQ_SEL */
	double		r = -1;
	int			nslots = 1;

	if (list_length(args) == 2)
	{
		Node	   *other = (Node *) lsecond(args);

		if (IsA(other, Const))
		{
			/* a constant cone: the histogram knows better than we do */
			PG_RETURN_FLOAT8((float8) sel);
		}
		r = radius_from_bound(root, other, &nslots);
	}

	if (r > 0)
	{
		double		rad = fmin(fmax(r * DEG2RAD, 0), M_PI);
		double		cap = pow(sin(rad / 2.0), 2);	/* cap area / 4pi */

		/* nslots arms share the cone's fraction; see the comment above */
		sel = sqrt(fmax(cap, 1e-14) / (double) nslots);
	}
	else if (r == 0)
	{
		/*
		 * A recognised skycell_poly_bound/skycell_region_bound: no radius to
		 * compute a real fraction from, so assume the same small, unknown
		 * footprint skycell_exact_support assumes for the identical
		 * situation (see its own comment), split across nslots arms the
		 * same way a cone's cap fraction is above.
		 */
		sel = sqrt(1e-4 / (double) nslots);
	}
	PG_RETURN_FLOAT8((float8) fmin(1.0, fmax(sel, 1e-8)));
}

/* ------------------------------------------------------------------ */
/* the user-facing predicates (used as-is when not rewritten)         */
/* ------------------------------------------------------------------ */

typedef struct cone_cache
{
	double		dec0,
				radius,
				cosdec0,
				thr;			/* sin^2(radius/2), <0 => empty, >1 => all */
} cone_cache;

static inline bool
in_cone(FunctionCallInfo fcinfo, double ra, double dec, double ra0, double dec0, double radius)
{
	cone_cache *cc = (cone_cache *) fcinfo->flinfo->fn_extra;
	double		sdd,
				sda;

	if (cc == NULL)
	{
		cc = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, sizeof(cone_cache));
		cc->dec0 = NAN;
		fcinfo->flinfo->fn_extra = cc;
	}
	if (cc->dec0 != dec0 || cc->radius != radius)
	{
		cc->dec0 = dec0;
		cc->radius = radius;
		cc->cosdec0 = cos(dec0 * DEG2RAD);
		cc->thr = radius < 0 ? -1.0 : (radius >= 180.0 ? 2.0 : pow(sin(radius * DEG2RAD / 2.0), 2));
	}
	/* haversine */
	sdd = sin((dec - dec0) * DEG2RAD / 2.0);
	sda = sin((ra - ra0) * DEG2RAD / 2.0);
	return sdd * sdd + cos(dec * DEG2RAD) * cc->cosdec0 * sda * sda <= cc->thr;
}

PG_FUNCTION_INFO_V1(skycell_in_cone);
Datum
skycell_in_cone(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(in_cone(fcinfo, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1),
						   PG_GETARG_FLOAT8(2), PG_GETARG_FLOAT8(3), PG_GETARG_FLOAT8(4)));
}

/* skycell_cone(cell, ra, dec, ra0, dec0, radius): exact when not rewritten */
PG_FUNCTION_INFO_V1(skycell_cone);
Datum
skycell_cone(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(in_cone(fcinfo, PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2),
						   PG_GETARG_FLOAT8(3), PG_GETARG_FLOAT8(4), PG_GETARG_FLOAT8(5)));
}

typedef struct poly_cache
{
	int			nbytes;
	char	   *data;
	sc_region	reg;
} poly_cache;

static bool
in_poly(FunctionCallInfo fcinfo, double ra, double dec, ArrayType *arr)
{
	poly_cache *pc = (poly_cache *) fcinfo->flinfo->fn_extra;
	int			nbytes = VARSIZE(arr);

	if (pc == NULL || pc->nbytes != nbytes || memcmp(pc->data, arr, nbytes) != 0)
	{
		MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		if (pc == NULL)
			pc = palloc0(sizeof(poly_cache));
		else
		{
			pfree(pc->data);
			sc_region_free(&pc->reg);
		}
		pc->nbytes = nbytes;
		pc->data = palloc(nbytes);
		memcpy(pc->data, arr, nbytes);
		poly_from_array(arr, &pc->reg);
		fcinfo->flinfo->fn_extra = pc;
		MemoryContextSwitchTo(old);
	}
	return sc_region_contains(&pc->reg, sc_radec2vec(ra, dec));
}

PG_FUNCTION_INFO_V1(skycell_in_poly);
Datum
skycell_in_poly(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(in_poly(fcinfo, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1),
						   PG_GETARG_ARRAYTYPE_P(2)));
}

PG_FUNCTION_INFO_V1(skycell_poly);
Datum
skycell_poly(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(in_poly(fcinfo, PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2),
						   PG_GETARG_ARRAYTYPE_P(3)));
}

/* ------------------------------------------------------------------ */
/* run-time range slots (joins / non-constant cones)                  */
/* ------------------------------------------------------------------ */

typedef struct hist_cache
{
	ArrayType  *key;			/* the Const array this was parsed from */
	uint32		hash;
	int			n;
	int64	   *b;
} hist_cache;

static struct
{
	bool		valid;
	double		ra0,
				dec0,
				radius,
				ntotal,
				range_cost,
				split_cost,
				area_ratio;
	uint32		hhash;
	int			nslots;
	int64		lo[MAX_SLOTS],
				hi[MAX_SLOTS];
}			slot_cache;

PG_FUNCTION_INFO_V1(skycell_cone_bound);
Datum
skycell_cone_bound(PG_FUNCTION_ARGS)
{
	double		ra0 = PG_GETARG_FLOAT8(0),
				dec0 = PG_GETARG_FLOAT8(1),
				radius = PG_GETARG_FLOAT8(2);
	int32		i = PG_GETARG_INT32(3),
				nslots = PG_GETARG_INT32(4);
	double		ntotal = PG_GETARG_FLOAT8(5);
	ArrayType  *harr = PG_GETARG_ARRAYTYPE_P(6);
	hist_cache *hc = (hist_cache *) fcinfo->flinfo->fn_extra;

	if (nslots < 1 || nslots > MAX_SLOTS || i < 0 || i >= 2 * nslots)
		elog(ERROR, "skycell: bad slot %d/%d", i, nslots);

	if (hc == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
		Datum	   *elems;
		bool	   *nulls;

		hc = palloc0(sizeof(hist_cache));
		deconstruct_array(harr, INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE, &elems, &nulls, &hc->n);
		hc->b = palloc(sizeof(int64) * Max(hc->n, 1));
		for (int j = 0; j < hc->n; j++)
			hc->b[j] = DatumGetInt64(elems[j]);
		hc->hash = hash_bytes((const unsigned char *) hc->b, sizeof(int64) * hc->n) ^ (uint32) hc->n;
		fcinfo->flinfo->fn_extra = hc;
		MemoryContextSwitchTo(old);
	}

	if (!slot_cache.valid || slot_cache.ra0 != ra0 || slot_cache.dec0 != dec0 ||
		slot_cache.radius != radius || slot_cache.ntotal != ntotal ||
		slot_cache.hhash != hc->hash || slot_cache.nslots != nslots ||
		slot_cache.range_cost != skycell_range_cost ||
		slot_cache.split_cost != skycell_split_cost ||
		slot_cache.area_ratio != skycell_max_area_ratio)
	{
		sc_region	reg;
		sc_cover	cov;
		sc_cover_params p;
		sc_density	d = {ntotal, hc->n, hc->b};
		int			s;

		slot_cache.valid = false;
		check_err(sc_region_cone(&reg, ra0, dec0, radius));
		current_params(&p, nslots, &d);
		sc_cover_compute(&reg, &d, &p, &cov);
		for (s = 0; s < cov.n && s < nslots; s++)
		{
			slot_cache.lo[s] = cov.r[s].lo;
			slot_cache.hi[s] = cov.r[s].hi;
		}
		for (; s < nslots; s++)
		{
			slot_cache.lo[s] = 1;	/* empty range: never matches */
			slot_cache.hi[s] = 0;
		}
		sc_cover_free(&cov);
		slot_cache.ra0 = ra0;
		slot_cache.dec0 = dec0;
		slot_cache.radius = radius;
		slot_cache.ntotal = ntotal;
		slot_cache.hhash = hc->hash;
		slot_cache.nslots = nslots;
		slot_cache.range_cost = skycell_range_cost;
		slot_cache.split_cost = skycell_split_cost;
		slot_cache.area_ratio = skycell_max_area_ratio;
		slot_cache.valid = true;
	}
	PG_RETURN_INT64((i % 2 == 0) ? slot_cache.lo[i / 2] : slot_cache.hi[i / 2]);
}

/*
 * The polygon analogue of skycell_cone_bound(): a polygon has no fixed-arity
 * description to compare cheaply like (ra0, dec0, radius), so the cache key
 * is a hash of the array's own bytes instead of three scalar fields.
 */
static struct
{
	bool		valid;
	uint32		polyhash,
				hhash;
	double		ntotal,
				range_cost,
				split_cost,
				area_ratio;
	int			nslots;
	int64		lo[MAX_SLOTS],
				hi[MAX_SLOTS];
}			poly_slot_cache;

PG_FUNCTION_INFO_V1(skycell_poly_bound);
Datum
skycell_poly_bound(PG_FUNCTION_ARGS)
{
	ArrayType  *polyarr = PG_GETARG_ARRAYTYPE_P(0);
	int32		i = PG_GETARG_INT32(1),
				nslots = PG_GETARG_INT32(2);
	double		ntotal = PG_GETARG_FLOAT8(3);
	ArrayType  *harr = PG_GETARG_ARRAYTYPE_P(4);
	hist_cache *hc = (hist_cache *) fcinfo->flinfo->fn_extra;
	uint32		polyhash;

	if (nslots < 1 || nslots > MAX_SLOTS || i < 0 || i >= 2 * nslots)
		elog(ERROR, "skycell: bad slot %d/%d", i, nslots);

	if (hc == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
		Datum	   *elems;
		bool	   *nulls;

		hc = palloc0(sizeof(hist_cache));
		deconstruct_array(harr, INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE, &elems, &nulls, &hc->n);
		hc->b = palloc(sizeof(int64) * Max(hc->n, 1));
		for (int j = 0; j < hc->n; j++)
			hc->b[j] = DatumGetInt64(elems[j]);
		hc->hash = hash_bytes((const unsigned char *) hc->b, sizeof(int64) * hc->n) ^ (uint32) hc->n;
		fcinfo->flinfo->fn_extra = hc;
		MemoryContextSwitchTo(old);
	}

	polyhash = hash_bytes((const unsigned char *) polyarr, VARSIZE(polyarr));

	if (!poly_slot_cache.valid || poly_slot_cache.polyhash != polyhash ||
		poly_slot_cache.ntotal != ntotal ||
		poly_slot_cache.hhash != hc->hash || poly_slot_cache.nslots != nslots ||
		poly_slot_cache.range_cost != skycell_range_cost ||
		poly_slot_cache.split_cost != skycell_split_cost ||
		poly_slot_cache.area_ratio != skycell_max_area_ratio)
	{
		sc_region	reg;
		sc_cover	cov;
		sc_cover_params p;
		sc_density	d = {ntotal, hc->n, hc->b};
		int			s;

		poly_slot_cache.valid = false;
		poly_from_array(polyarr, &reg);
		current_params(&p, nslots, &d);
		sc_cover_compute(&reg, &d, &p, &cov);
		for (s = 0; s < cov.n && s < nslots; s++)
		{
			poly_slot_cache.lo[s] = cov.r[s].lo;
			poly_slot_cache.hi[s] = cov.r[s].hi;
		}
		for (; s < nslots; s++)
		{
			poly_slot_cache.lo[s] = 1;	/* empty range: never matches */
			poly_slot_cache.hi[s] = 0;
		}
		sc_cover_free(&cov);
		sc_region_free(&reg);
		poly_slot_cache.polyhash = polyhash;
		poly_slot_cache.ntotal = ntotal;
		poly_slot_cache.hhash = hc->hash;
		poly_slot_cache.nslots = nslots;
		poly_slot_cache.range_cost = skycell_range_cost;
		poly_slot_cache.split_cost = skycell_split_cost;
		poly_slot_cache.area_ratio = skycell_max_area_ratio;
		poly_slot_cache.valid = true;
	}
	PG_RETURN_INT64((i % 2 == 0) ? poly_slot_cache.lo[i / 2] : poly_slot_cache.hi[i / 2]);
}

/*
 * The generic-region analogue of skycell_poly_bound(): a skyregion column can
 * hold either kind (its own tag says which -- adql.c's skycell_region_from_
 * datum() dispatches on it), so this covers a non-constant CIRCLE-or-POLYGON
 * column with one function instead of needing a region_support equivalent
 * for each kind. Same hash-of-bytes caching as skycell_poly_bound, for the
 * same reason: a skyregion has no fixed-arity description either.
 */
static struct
{
	bool		valid;
	uint32		reghash,
				hhash;
	double		ntotal,
				range_cost,
				split_cost,
				area_ratio;
	int			nslots;
	int64		lo[MAX_SLOTS],
				hi[MAX_SLOTS];
}			region_slot_cache;

PG_FUNCTION_INFO_V1(skycell_region_bound);
Datum
skycell_region_bound(PG_FUNCTION_ARGS)
{
	struct varlena *regarr = PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	int32		i = PG_GETARG_INT32(1),
				nslots = PG_GETARG_INT32(2);
	double		ntotal = PG_GETARG_FLOAT8(3);
	ArrayType  *harr = PG_GETARG_ARRAYTYPE_P(4);
	hist_cache *hc = (hist_cache *) fcinfo->flinfo->fn_extra;
	uint32		reghash;

	if (nslots < 1 || nslots > MAX_SLOTS || i < 0 || i >= 2 * nslots)
		elog(ERROR, "skycell: bad slot %d/%d", i, nslots);

	if (hc == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
		Datum	   *elems;
		bool	   *nulls;

		hc = palloc0(sizeof(hist_cache));
		deconstruct_array(harr, INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE, &elems, &nulls, &hc->n);
		hc->b = palloc(sizeof(int64) * Max(hc->n, 1));
		for (int j = 0; j < hc->n; j++)
			hc->b[j] = DatumGetInt64(elems[j]);
		hc->hash = hash_bytes((const unsigned char *) hc->b, sizeof(int64) * hc->n) ^ (uint32) hc->n;
		fcinfo->flinfo->fn_extra = hc;
		MemoryContextSwitchTo(old);
	}

	reghash = hash_bytes((const unsigned char *) regarr, VARSIZE(regarr));

	if (!region_slot_cache.valid || region_slot_cache.reghash != reghash ||
		region_slot_cache.ntotal != ntotal ||
		region_slot_cache.hhash != hc->hash || region_slot_cache.nslots != nslots ||
		region_slot_cache.range_cost != skycell_range_cost ||
		region_slot_cache.split_cost != skycell_split_cost ||
		region_slot_cache.area_ratio != skycell_max_area_ratio)
	{
		sc_region	reg;
		sc_cover	cov;
		sc_cover_params p;
		sc_density	d = {ntotal, hc->n, hc->b};
		int			s;

		region_slot_cache.valid = false;
		skycell_region_from_datum(PointerGetDatum(regarr), &reg);
		current_params(&p, nslots, &d);
		sc_cover_compute(&reg, &d, &p, &cov);
		for (s = 0; s < cov.n && s < nslots; s++)
		{
			region_slot_cache.lo[s] = cov.r[s].lo;
			region_slot_cache.hi[s] = cov.r[s].hi;
		}
		for (; s < nslots; s++)
		{
			region_slot_cache.lo[s] = 1;	/* empty range: never matches */
			region_slot_cache.hi[s] = 0;
		}
		sc_cover_free(&cov);
		sc_region_free(&reg);
		region_slot_cache.reghash = reghash;
		region_slot_cache.ntotal = ntotal;
		region_slot_cache.hhash = hc->hash;
		region_slot_cache.nslots = nslots;
		region_slot_cache.range_cost = skycell_range_cost;
		region_slot_cache.split_cost = skycell_split_cost;
		region_slot_cache.area_ratio = skycell_max_area_ratio;
		region_slot_cache.valid = true;
	}
	PG_RETURN_INT64((i % 2 == 0) ? region_slot_cache.lo[i / 2] : region_slot_cache.hi[i / 2]);
}

/* ------------------------------------------------------------------ */
/* explicit coverings                                                  */
/* ------------------------------------------------------------------ */

typedef struct rel_density_cache
{
	Oid			relid;
	AttrNumber	attnum;
	sc_density	d;
} rel_density_cache;

/* density for (tbl regclass, col name) arguments at positions ai, ai+1 */
static sc_density *
density_from_args(FunctionCallInfo fcinfo, int ai)
{
	rel_density_cache *rc = (rel_density_cache *) fcinfo->flinfo->fn_extra;
	Oid			relid;
	AttrNumber	attnum;

	if (PG_NARGS() <= ai || PG_ARGISNULL(ai))
		return NULL;
	relid = PG_GETARG_OID(ai);
	attnum = get_attnum(relid, (PG_NARGS() > ai + 1 && !PG_ARGISNULL(ai + 1))
						? NameStr(*PG_GETARG_NAME(ai + 1)) : "cell");
	if (attnum == InvalidAttrNumber)
		ereport(ERROR, (errmsg("skycell: column not found in %s", get_rel_name(relid))));

	if (rc == NULL || rc->relid != relid || rc->attnum != attnum)
	{
		MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		rc = palloc0(sizeof(rel_density_cache));
		rc->relid = relid;
		rc->attnum = attnum;
		load_density(relid, attnum, &rc->d);
		fcinfo->flinfo->fn_extra = rc;
		MemoryContextSwitchTo(old);
	}
	return &rc->d;
}

static void
emit_ranges(FunctionCallInfo fcinfo, sc_cover *cov)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	for (int i = 0; i < cov->n; i++)
	{
		Datum		v[2] = {Int64GetDatum(cov->r[i].lo), Int64GetDatum(cov->r[i].hi)};
		bool		n[2] = {false, false};

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, n);
	}
}

/* skycell_cone_ranges(ra0, dec0, radius, tbl regclass, col name) -> (lo, hi) */
PG_FUNCTION_INFO_V1(skycell_cone_ranges);
Datum
skycell_cone_ranges(PG_FUNCTION_ARGS)
{
	sc_region	reg;
	sc_cover	cov;
	sc_cover_params p;
	const sc_density *dens;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
	{
		InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
		return (Datum) 0;
	}
	check_err(sc_region_cone(&reg, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2)));
	dens = density_from_args(fcinfo, 3);
	current_params(&p, skycell_max_ranges, dens);
	sc_cover_compute(&reg, dens, &p, &cov);
	emit_ranges(fcinfo, &cov);
	sc_cover_free(&cov);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(skycell_poly_ranges);
Datum
skycell_poly_ranges(PG_FUNCTION_ARGS)
{
	sc_region	reg;
	sc_cover	cov;
	sc_cover_params p;
	const sc_density *dens;

	if (PG_ARGISNULL(0))
	{
		InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
		return (Datum) 0;
	}
	poly_from_array(PG_GETARG_ARRAYTYPE_P(0), &reg);
	dens = density_from_args(fcinfo, 1);
	current_params(&p, skycell_max_ranges, dens);
	sc_cover_compute(&reg, dens, &p, &cov);
	emit_ranges(fcinfo, &cov);
	sc_cover_free(&cov);
	return (Datum) 0;
}

/*
 * skycell_cover_info(ra0, dec0, radius, tbl, col)
 *   -> (nranges, steps, deepest, exp_rows, area_ratio)
 */
PG_FUNCTION_INFO_V1(skycell_cover_info);
Datum
skycell_cover_info(PG_FUNCTION_ARGS)
{
	sc_region	reg;
	sc_cover	cov;
	sc_cover_params p;
	const sc_density *dens;
	TupleDesc	td;
	Datum		v[7];
	bool		n[7] = {0};

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_NULL();
	check_err(sc_region_cone(&reg, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2)));
	dens = density_from_args(fcinfo, 3);
	current_params(&p, skycell_max_ranges, dens);
	sc_cover_compute(&reg, dens, &p, &cov);
	if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	v[0] = Int32GetDatum(cov.n);
	v[1] = Int32GetDatum(cov.steps);
	v[2] = Int32GetDatum(cov.deepest);
	v[3] = Float8GetDatum(cov.exp_rows);
	v[4] = Float8GetDatum(reg.area > 0 ? cov.area / reg.area : 0);
	v[5] = Float8GetDatum(cov.rho);
	v[6] = Int32GetDatum(cov.order);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(td), v, n)));
}

/*
 * skycell_range_cost(tbl, col) -> the price of one index range, in rows, that
 * the cost model would use for this relation.  Exposed so that the derivation
 * can be checked against a measured sweep rather than trusted.
 */
PG_FUNCTION_INFO_V1(skycell_range_cost_for);
Datum
skycell_range_cost_for(PG_FUNCTION_ARGS)
{
	const sc_density *d = density_from_args(fcinfo, 0);
	sc_cover_params p;

	current_params(&p, skycell_max_ranges, d);
	PG_RETURN_FLOAT8(p.range_cost);
}

/* ------------------------------------------------------------------ */
/* keys, distances, MOC helpers                                        */
/* ------------------------------------------------------------------ */

static void
check_dec(double dec)
{
	if (!(dec >= -90.0 && dec <= 90.0))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("skycell: declination %g out of range [-90, 90]", dec)));
}

PG_FUNCTION_INFO_V1(skycell_ang2cell);
Datum
skycell_ang2cell(PG_FUNCTION_ARGS)
{
	double		ra = PG_GETARG_FLOAT8(0),
				dec = PG_GETARG_FLOAT8(1);

	check_dec(dec);
	if (!isfinite(ra))
		ereport(ERROR, (errmsg("skycell: RA must be finite")));
	PG_RETURN_INT64(sc_ang2pix(SC_MAX_ORDER, ra, dec));
}

PG_FUNCTION_INFO_V1(skycell_ang2pix);
Datum
skycell_ang2pix(PG_FUNCTION_ARGS)
{
	int32		order = PG_GETARG_INT32(0);
	double		ra = PG_GETARG_FLOAT8(1),
				dec = PG_GETARG_FLOAT8(2);

	if (order < 0 || order > SC_MAX_ORDER)
		ereport(ERROR, (errmsg("skycell: order must be within [0, 29]")));
	check_dec(dec);
	PG_RETURN_INT64(sc_ang2pix(order, ra, dec));
}

/* angular distance in degrees */
PG_FUNCTION_INFO_V1(skycell_dist);
Datum
skycell_dist(PG_FUNCTION_ARGS)
{
	sc_vec3		a = sc_radec2vec(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1));
	sc_vec3		b = sc_radec2vec(PG_GETARG_FLOAT8(2), PG_GETARG_FLOAT8(3));

	PG_RETURN_FLOAT8(sc_angle(a, b) / DEG2RAD);
}

/*
 * NUNIQ of the cell's ancestors at orders min_order..max_order (the order-29
 * cell itself is order 29).  Restricting the range to the orders present in
 * a MOC table keeps the ANY() lookup to a handful of B-tree probes.
 */
PG_FUNCTION_INFO_V1(skycell_ancestors);
Datum
skycell_ancestors(PG_FUNCTION_ARGS)
{
	int64		cell = PG_GETARG_INT64(0);
	int32		lo = PG_GETARG_INT32(1),
				hi = PG_GETARG_INT32(2);
	Datum		d[SC_MAX_ORDER + 1];
	int			n = 0;

	if (cell < 0 || cell >= SC_NPIX29)
		ereport(ERROR, (errmsg("skycell: cell out of range")));
	lo = Max(lo, 0);
	hi = Min(hi, SC_MAX_ORDER);
	for (int k = lo; k <= hi; k++)
		d[n++] = Int64GetDatum(sc_nuniq(k, cell >> (2 * (SC_MAX_ORDER - k))));
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(d, n, INT8OID));
}

PG_FUNCTION_INFO_V1(skycell_nuniq_order);
Datum
skycell_nuniq_order(PG_FUNCTION_ARGS)
{
	int64		pix;

	PG_RETURN_INT32(sc_nuniq_decode(PG_GETARG_INT64(0), &pix));
}

/* first / last order-29 cell of a NUNIQ cell */
PG_FUNCTION_INFO_V1(skycell_nuniq_lo);
Datum
skycell_nuniq_lo(PG_FUNCTION_ARGS)
{
	int64		pix;
	int			order = sc_nuniq_decode(PG_GETARG_INT64(0), &pix);

	PG_RETURN_INT64(sc_pix_lo(order, pix));
}

PG_FUNCTION_INFO_V1(skycell_nuniq_hi);
Datum
skycell_nuniq_hi(PG_FUNCTION_ARGS)
{
	int64		pix;
	int			order = sc_nuniq_decode(PG_GETARG_INT64(0), &pix);

	PG_RETURN_INT64(sc_pix_hi(order, pix));
}

/*
 * Multi-order coverage (NUNIQ list) with at most max_cells cells: refine the
 * partial cell with the most outside area first (S2 RegionCoverer style).
 * Used to index stored regions (footprints) in a plain B-tree.
 */
ArrayType *
moc_for_region(const sc_region *r, int max_cells, int max_order)
{
	typedef struct
	{
		int64		pix;
		int			order;
		double		pot;
	} mc;
	mc		   *heap = palloc(sizeof(mc) * 64);
	int			hn = 0,
				hcap = 64;
	Datum	   *out = palloc(sizeof(Datum) * 64);
	int			on = 0,
				ocap = 64,
				ncells = 0;

#define OUT_ADD(o, p) do { if (on == ocap) { ocap *= 2; out = repalloc(out, sizeof(Datum) * ocap); } \
		out[on++] = Int64GetDatum(sc_nuniq((o), (p))); } while (0)
#define HEAP_ADD(o, p, pt) do { if (hn == hcap) { hcap *= 2; heap = repalloc(heap, sizeof(mc) * hcap); } \
		heap[hn].pix = (p); heap[hn].order = (o); heap[hn].pot = (pt); hn++; } while (0)

	for (int f = 0; f < 12; f++)
	{
		double		fo;
		sc_class	c = sc_region_classify(r, 0, f, &fo);

		if (c == SC_OUT)
			continue;
		ncells++;
		if (c == SC_IN)
			OUT_ADD(0, f);
		else
			HEAP_ADD(0, f, fo * 4.0 * M_PI / 12.0);
	}

	while (hn > 0)
	{
		int			best = 0;
		mc			P;
		sc_class	cls[4];
		double		fo[4];
		int			nkept = 0;

		/* linear scan is fine: coverings are small */
		for (int i = 1; i < hn; i++)
			if (heap[i].pot > heap[best].pot)
				best = i;
		P = heap[best];
		heap[best] = heap[--hn];

		/*
		 * Refine only while cells are not negligibly small next to the
		 * region; otherwise boundary cells whose siblings are all OUT keep
		 * splitting "for free" down to order 29, and every point lookup then
		 * has to probe that many ancestor orders.
		 */
		if (P.order < max_order &&
			4.0 * M_PI / (double) ((int64) 12 << (2 * (P.order + 1))) >= r->area / (4.0 * max_cells))
		{
			for (int c = 0; c < 4; c++)
			{
				cls[c] = sc_region_classify(r, P.order + 1, 4 * P.pix + c, &fo[c]);
				if (cls[c] != SC_OUT)
					nkept++;
			}
			if (nkept == 0)
			{
				ncells--;
				continue;
			}
			if (ncells - 1 + nkept <= max_cells)
			{
				double		a = 4.0 * M_PI / (double) ((int64) 12 << (2 * (P.order + 1)));

				ncells += nkept - 1;
				for (int c = 0; c < 4; c++)
				{
					if (cls[c] == SC_IN)
						OUT_ADD(P.order + 1, 4 * P.pix + c);
					else if (cls[c] == SC_PARTIAL)
						HEAP_ADD(P.order + 1, 4 * P.pix + c, fo[c] * a);
				}
				continue;
			}
		}
		OUT_ADD(P.order, P.pix);
	}
	return construct_array_builtin(out, on, INT8OID);
}

PG_FUNCTION_INFO_V1(skycell_cone_moc);
Datum
skycell_cone_moc(PG_FUNCTION_ARGS)
{
	sc_region	reg;

	check_err(sc_region_cone(&reg, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2)));
	PG_RETURN_ARRAYTYPE_P(moc_for_region(&reg, Max(PG_GETARG_INT32(3), 4),
										 Min(Max(PG_GETARG_INT32(4), 0), SC_MAX_ORDER)));
}

PG_FUNCTION_INFO_V1(skycell_poly_moc);
Datum
skycell_poly_moc(PG_FUNCTION_ARGS)
{
	sc_region	reg;

	poly_from_array(PG_GETARG_ARRAYTYPE_P(0), &reg);
	PG_RETURN_ARRAYTYPE_P(moc_for_region(&reg, Max(PG_GETARG_INT32(1), 4),
										 Min(Max(PG_GETARG_INT32(2), 0), SC_MAX_ORDER)));
}

/*
 * skyregion covers both cone and polygon (its own "kind" tag, dispatched by
 * skycell_region_from_datum -- see adql.c), so one column can hold circles
 * and polygons together; this is that same covering for whichever one a
 * given row holds, without the caller branching on kind itself the way
 * skycell_cone_moc/skycell_poly_moc otherwise require.
 */
PG_FUNCTION_INFO_V1(skycell_region_moc);
Datum
skycell_region_moc(PG_FUNCTION_ARGS)
{
	sc_region	reg;

	skycell_region_from_datum(PG_GETARG_DATUM(0), &reg);
	PG_RETURN_ARRAYTYPE_P(moc_for_region(&reg, Max(PG_GETARG_INT32(1), 4),
										 Min(Max(PG_GETARG_INT32(2), 0), SC_MAX_ORDER)));
}
