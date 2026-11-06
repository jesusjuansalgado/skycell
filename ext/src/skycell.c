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
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace_d.h"
#include "utils/regproc.h"
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
#include "executor/spi.h"
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
#include "port/pg_bitutils.h"
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
static double skycell_probe_split_cost = 1.0;

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
							 "Decline the B-tree range rewrite of a constant region when the "
							 "covering is expected to fetch more than this many rows that the "
							 "exact test then rejects, if a GiST or SP-GiST index on the "
							 "position can answer the query instead.",
							 "The threshold is scaled up for a relation larger than "
							 "shared_buffers, where wasted rows cost less relative to I/O "
							 "(bounded by skycell.rewrite_waste_scale_cap). Without such an "
							 "index the rewrite is always kept.",
							 &skycell_rewrite_max_waste, 100.0, 0.0, 1e12,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("skycell.rewrite_waste_scale_cap",
							 "Upper bound, as a multiple of skycell.rewrite_max_waste, on how "
							 "far that threshold is scaled up for a relation larger than "
							 "shared_buffers.",
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
							"Experimental: also score this many HEALPix orders past the cost "
							"model's choice on the covering each produces, and keep the "
							"cheapest (0 = off).",
							"It can lower execution time, but the extra coverings are "
							"computed at plan time for every query, which usually costs more "
							"than it saves.",
							&skycell_probe_orders, 0, 0, 8,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("skycell.probe_split_cost",
							 "Cost, in rows, charged per cell examined by the order probe "
							 "(skycell.probe_orders); no effect while that is 0.",
							 NULL, &skycell_probe_split_cost, 1.0, 0.0, 1e9,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("skycell.force_order",
							"Diagnostics only: cover cones at this HEALPix order instead of "
							"the one the cost model chooses (-1 = let the model choose).",
							NULL, &skycell_force_order, -1, -1, SC_MAX_ORDER,
							PGC_USERSET, 0, NULL, NULL, NULL);
	cone_scan_init();
	MarkGUCPrefixReserved("skycell");
}

/*
 * The fraction of the relation the shared buffer pool can hold: 1 when it
 * fits, falling as it outgrows NBuffers.  Read from NBuffers rather than
 * effective_cache_size for the reasons rewrite_waste_threshold() gives below.
 */
static double
cache_fraction(const sc_density *d)
{
	double		relpages = (d && d->relpages > 0) ? d->relpages : 0;

	return (relpages > 0) ? fmin(1.0, (double) NBuffers / relpages) : 1.0;
}

/*
 * The price of one index range, in rows, when the custom scan (cone_scan.c)
 * answers the cone, from the relation's own statistics and the planner's cost
 * parameters.
 *
 * cost(s) in cover.h is in rows, so range_cost converts "one more B-tree range
 * scan" into "the number of false-positive rows it is worth avoiding".  Both
 * sides are things PostgreSQL already costs:
 *
 *   one more range  = a B-tree descent, as btcostestimate charges it:
 *                     ceil(log2(N)) * cpu_operator_cost for the comparisons
 *                     and (H + 1) * 50 * cpu_operator_cost for the pages
 *                     descended through -- plus two pages read: the leaf the
 *                     descent lands on and the heap page the new range starts
 *                     on, where the scan stops running sequentially.
 *   one false row   = its index entry, its heap tuple, the exact predicate,
 *                     and the heap page amortised over the rows that share it
 *                     (reltuples / relpages of this very relation).
 *
 * The page term is what makes this per-relation rather than per-installation:
 * a narrow catalogue row packs 120 to a page and amortises it away, while a
 * wide observation row packs a handful and makes every false positive cost
 * real page traffic, so the same query should be covered more finely there.
 *
 * A page is priced by where it is expected to come from.  One that must be
 * read costs random_page_cost; one already in shared buffers costs what
 * btcostestimate charges for touching an index page, 50 * cpu_operator_cost.
 * cache_fraction() blends the two.  On the warm 10M-row catalogue this comes
 * out at ~34 rows (~87 for a relation far larger than the buffer pool), where
 * auto_range_cost() below gives ~110; measured in-trial against it, the
 * custom scan is 3-5% faster on the catalogue warm, as fast or faster cold,
 * and up to 11% faster on the ObsCore-shaped relation.
 */
static double
scan_range_cost(const sc_density *d)
{
	double		n = (d && d->ntotal > 1) ? d->ntotal : 1e6;
	double		rpp = (d && d->relpages > 0 && d->ntotal > 0)
		? d->ntotal / d->relpages : 100.0;
	double		height = fmax(1.0, ceil(log(n) / log(300.0)));	/* btree fanout ~300 */
	double		cached = cache_fraction(d);
	double		page = cached * 50.0 * cpu_operator_cost + (1.0 - cached) * random_page_cost;
	double		range = ceil(log(n) / log(2.0)) * cpu_operator_cost
		+ (height + 1.0) * 50.0 * cpu_operator_cost
		+ 2.0 * page;
	double		per_row = cpu_tuple_cost + cpu_index_tuple_cost
		+ 3.0 * cpu_operator_cost				/* the exact predicate */
		+ page / fmax(rpp, 1.0);

	if (!(per_row > 0))
		return 30.0;
	return fmin(1e6, fmax(1.0, range / per_row));
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
 *
 * This is the price the range rewrite uses, and it is knowingly high: the
 * comparisons term is charged at 50x where btcostestimate charges 1x, and
 * every page at random_page_cost.  For the rewrite that over-pricing stands
 * in for a cost it does not model -- each range also becomes an OR arm that
 * PostgreSQL's planner builds a path for and estimates, about 10 us apiece on
 * the paper's catalogue -- and measured in-trial, pricing the rewrite's ranges
 * like the custom scan's (scan_range_cost() above) made it up to 5% slower
 * warm.  The custom scan has no per-range planning and uses that price.
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
	double		cache_frac = cache_fraction(d);
	double		min_frac = 1.0 / fmax(skycell_rewrite_waste_scale_cap, 1.0);

	return skycell_rewrite_max_waste / fmax(cache_frac, min_frac);
}

/* current_params() with the custom scan's range price (scan_range_cost()) */
static void
current_params_scan(sc_cover_params *p, int max_ranges, const sc_density *d)
{
	current_params(p, max_ranges, d);
	if (skycell_range_cost < 0)
		p->range_cost = scan_range_cost(d);
}

void
current_params(sc_cover_params *p, int max_ranges, const sc_density *d)
{
	sc_cover_params_default(p);
	p->range_cost = (skycell_range_cost < 0)
		? auto_range_cost(d) : skycell_range_cost;
	p->split_cost = skycell_split_cost;
	p->probe_split_cost = skycell_probe_split_cost;
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
	check_err(sc_region_poly(r, n / 2, ra, dec, true));
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
				probe_split_cost,
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

	/*
	 * With no such index: a stored column known to hold expr and indexed by
	 * a B-tree (stored_cell_column()), or 0; and whether that index is
	 * skycell_cell_ops.
	 */
	AttrNumber	stored;
	bool		stored_cell_ops;
} idx_entry;

static MemoryContext skycell_cache_cxt = NULL;

/*
 * The coverings live in a context of their own, so that evicting them does not
 * take the density model with them: cover_cached() evicts while its caller
 * holds an sc_density whose map points into skycell_cache_cxt, and a reload
 * of the histogram and map costs more than the coverings it would save.
 */
static MemoryContext skycell_cover_cxt = NULL;
static HTAB *dens_cache = NULL;
static HTAB *idx_cache = NULL;

/*
 * A flush frees what the cache handed out, so it is counted: a caller that
 * keeps an sc_density across anything that can process invalidations -- a
 * lock, a catalog lookup, a function on the outer side of a join -- checks
 * skycell_density_generation() before using it again, and reloads if the
 * count moved.  Those are the cross-match scan, which holds its density for
 * every outer row, and a function's fn_extra.
 */
static uint64 skycell_cache_generation = 0;

uint64
skycell_density_generation(void)
{
	return skycell_cache_generation;
}

static void
skycell_cache_flush(Datum arg, int cacheid, uint32 hashvalue)
{
	skycell_cache_generation++;
	dens_cache = NULL;
	idx_cache = NULL;
	cover_cache = NULL;
	if (skycell_cache_cxt)
		MemoryContextReset(skycell_cache_cxt);
	if (skycell_cover_cxt)
		MemoryContextReset(skycell_cover_cxt);
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
	if (skycell_cover_cxt == NULL)
		skycell_cover_cxt = AllocSetContextCreate(TopMemoryContext,
												  "skycell covering cache",
												  ALLOCSET_DEFAULT_SIZES);
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
		ctl.hcxt = skycell_cover_cxt;
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
	{
		/* simplest eviction: start over, the coverings only */
		cover_cache = NULL;
		MemoryContextReset(skycell_cover_cxt);
		skycell_cache_init();
	}

	memset(&key, 0, sizeof(key));	/* padding must be zero: hashed as bytes */
	key.ra0 = ra0;
	key.dec0 = dec0;
	key.radius = radius;
	key.range_cost = p->range_cost;
	key.split_cost = p->split_cost;
	key.probe_split_cost = p->probe_split_cost;
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
		MemoryContext old = MemoryContextSwitchTo(skycell_cover_cxt);

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
	dens_key	key;
	dens_entry *entry;
	dens_entry	fresh;			/* an uncached load, when the cache moved */

	memset(&key, 0, sizeof(key));	/* padding must be zero: hashed as bytes */
	key.statrel = statrel;
	key.attnum = attnum;
	skycell_cache_init();
	entry = (dens_entry *) hash_search(dens_cache, &key, HASH_FIND, NULL);
	if (entry == NULL)
	{
		/*
		 * Loading looks up the catalogs and opens the map, and either can
		 * process an invalidation, which flushes the cache.  So the load goes
		 * into a context of its own, and joins the cache only if no flush
		 * happened meanwhile; otherwise it serves this caller alone and is
		 * freed with the caller's context.
		 */
		sc_density	fd;
		uint64		generation = skycell_cache_generation;
		MemoryContext loadcxt = AllocSetContextCreate(CurrentMemoryContext,
													  "skycell density load",
													  ALLOCSET_SMALL_SIZES);
		MemoryContext old = MemoryContextSwitchTo(loadcxt);

		memset(&fresh, 0, sizeof(fresh));
		fresh.key = key;
		load_density_from(relid, statrel, attnum, &fd);
		fresh.ntotal = fd.ntotal;
		fresh.nbounds = fd.nbounds;
		fresh.bounds = (int64 *) fd.bounds;
		fresh.relpages = fd.relpages;
		load_density_map(statrel, attnum, &fresh);
		MemoryContextSwitchTo(old);

		entry = &fresh;
		if (generation == skycell_cache_generation)
		{
			MemoryContextSetParent(loadcxt, skycell_cache_cxt);
			entry = (dens_entry *) hash_search(dens_cache, &key, HASH_ENTER, NULL);
			*entry = fresh;
		}
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
 * For cone_scan.c's run-time coverings (a cross-match cone, one per outer
 * row): the backend's cached density for a relation's cell statistics, and
 * the custom scan's covering parameters.
 */
void
skycell_load_density(Oid relid, Oid statrel, AttrNumber attnum, sc_density *d)
{
	load_density_cached(relid, statrel, attnum, d);
}

void
skycell_scan_params(sc_cover_params *p, const sc_density *d)
{
	current_params_scan(p, skycell_max_ranges, d);
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
/*
 * A column of rel that holds the cell expression key (varno 1), for a table
 * that stores the cell instead of indexing the expression (README, setup 2).
 * The planner has to be told what the column was computed from, in either of
 * two ways PostgreSQL itself enforces:
 *
 *   - a stored generated column, GENERATED ALWAYS AS (key) STORED;
 *   - a validated, enforced CHECK (column = key) on a NOT NULL column.
 *     NOT NULL as well because a CHECK passes on NULL: a row whose cell were
 *     NULL beside a valid position would be in no range of the index.
 *
 * The column must be int8 and the first key of a B-tree index without a
 * predicate.  0 when there is none; *cell_ops says whether that index is
 * skycell_cell_ops.
 */
static bool
attr_not_null(TupleDesc desc, AttrNumber att)
{
#if PG_VERSION_NUM >= 180000
	/* 18's NOT NULL can be NOT VALID; only a validated one says anything */
	return TupleDescCompactAttr(desc, att - 1)->attnullability == ATTNULLABLE_VALID;
#else
	return TupleDescAttr(desc, att - 1)->attnotnull;
#endif
}

/* column = key or key = column, as int8's equality, else 0 */
static AttrNumber
check_column_equal(Node *e, Node *key)
{
	OpExpr	   *op = (OpExpr *) e;
	Node	   *a,
			   *b;

	if (!IsA(e, OpExpr) || list_length(op->args) != 2 ||
		(OidIsValid(op->opfuncid) ? op->opfuncid : get_opcode(op->opno)) != F_INT8EQ)
		return 0;
	a = linitial(op->args);
	b = lsecond(op->args);
	if (IsA(a, Var) && ((Var *) a)->varno == 1 && ((Var *) a)->varlevelsup == 0 && equal(b, key))
		return ((Var *) a)->varattno;
	if (IsA(b, Var) && ((Var *) b)->varno == 1 && ((Var *) b)->varlevelsup == 0 && equal(a, key))
		return ((Var *) b)->varattno;
	return 0;
}

static AttrNumber
stored_cell_column(Relation rel, List *indexes, Node *key, bool *cell_ops)
{
	TupleDesc	desc = RelationGetDescr(rel);
	TupleConstr *constr = desc->constr;
	Oid			cellops_oid = cell_ops_opfamily();
	ListCell   *lc;

	*cell_ops = false;
	if (constr == NULL)
		return 0;
	foreach(lc, indexes)
	{
		Relation	irel = index_open(lfirst_oid(lc), AccessShareLock);
		AttrNumber	att = irel->rd_index->indkey.values[0];
		bool		ok = false;

		if (irel->rd_rel->relam == BTREE_AM_OID && att > 0 &&
			heap_attisnull(irel->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			TupleDescAttr(desc, att - 1)->atttypid == INT8OID &&
			!TupleDescAttr(desc, att - 1)->attisdropped)
		{
			if (TupleDescAttr(desc, att - 1)->attgenerated == ATTRIBUTE_GENERATED_STORED)
			{
				for (int i = 0; i < constr->num_defval && !ok; i++)
					if (constr->defval[i].adnum == att)
						ok = equal(stringToNode(constr->defval[i].adbin), key);
			}
			else if (attr_not_null(desc, att))
			{
				for (int i = 0; i < constr->num_check && !ok; i++)
				{
					ConstrCheck *cc = &constr->check[i];

#if PG_VERSION_NUM >= 180000
					if (!cc->ccenforced)
						continue;
#endif
					ok = cc->ccvalid && check_column_equal(stringToNode(cc->ccbin), key) == att;
				}
			}
		}
		if (ok)
			*cell_ops = OidIsValid(cellops_oid) && irel->rd_opfamily[0] == cellops_oid;
		index_close(irel, AccessShareLock);
		if (ok)
			return att;
	}
	return 0;
}

typedef struct
{
	List	   *vars;
	int			flags;
} local_vars_ctx;

static bool
local_vars_walker(Node *node, local_vars_ctx *cx)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == 0)
			cx->vars = lappend(cx->vars, node);
		return false;
	}
	if (IsA(node, Aggref) || IsA(node, GroupingFunc))
	{
		Index		up = IsA(node, Aggref) ? ((Aggref *) node)->agglevelsup
			: ((GroupingFunc *) node)->agglevelsup;

		if (up > 0)
			return false;
		if (cx->flags & PVC_INCLUDE_AGGREGATES)
		{
			cx->vars = lappend(cx->vars, node);
			return false;
		}
	}
	else if (IsA(node, WindowFunc))
	{
		if (cx->flags & PVC_INCLUDE_WINDOWFUNCS)
		{
			cx->vars = lappend(cx->vars, node);
			return false;
		}
	}
	else if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup > 0)
			return false;
		if (cx->flags & PVC_INCLUDE_PLACEHOLDERS)
		{
			cx->vars = lappend(cx->vars, node);
			return false;
		}
	}
	return expression_tree_walker(node, local_vars_walker, (void *) cx);
}

List *
sc_pull_local_vars(Node *node, int flags)
{
	local_vars_ctx cx = {NIL, flags};

	local_vars_walker(node, &cx);
	return cx.vars;
}

bool
density_for_expr(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
				  bool *uses_cell_ops)
{
	List	   *vars = sc_pull_local_vars(arg, 0);
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
	AttrNumber	stored_att = 0;
	bool		stored_ops = false;

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
	{
		bool		scops = false;
		AttrNumber	stored = found ? 0 : stored_cell_column(rel, indexes, key, &scops);

		list_free(indexes);
		table_close(rel, NoLock);
		*uses_cell_ops = matched_uses_cell_ops;
		/* remembered below with the rest of the outcome */
		stored_att = stored;
		stored_ops = scops;
	}

	/*
	 * Loaded only now: opening the next index takes a lock, which can process
	 * an invalidation and free a density loaded in the loop.
	 */
	if (found)
		load_density_cached(rte->relid, matched, 1, d);

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
		entry->stored = stored_att;
		entry->stored_cell_ops = stored_ops;
		MemoryContextSwitchTo(old);
	}
	return found;
}

/*
 * density_for_expr() for a cell expression a query synthesised from its
 * position -- skycell_ang2cell(ra, dec), skycell_cell(pos) -- that also takes
 * a table storing the cell: when no index holds the expression but a stored
 * column is known to hold it (stored_cell_column()), *cell becomes that
 * column, and the density is its statistics'.  Callers then build their
 * conditions and paths on *cell, which an index answers either way.
 */
bool
skycell_cell_density(PlannerInfo *root, Node **cell, sc_density *d, Oid *statrel,
					 bool *uses_cell_ops)
{
	List	   *vars;
	Index		varno = 0;
	ListCell   *lc;
	RangeTblEntry *rte;
	Node	   *key;
	idx_entry  *entry;
	AttrNumber	att;
	bool		ops;

	memset(d, 0, sizeof(*d));
	*statrel = InvalidOid;
	*uses_cell_ops = false;
	if (root == NULL || root->parse == NULL)
		return false;
	if (density_for_expr(root, *cell, d, statrel, uses_cell_ops))
		return true;
	vars = sc_pull_local_vars(*cell, 0);
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
	key = copyObject(*cell);
	ChangeVarNodes(key, varno, 1, 0);

	/* density_for_expr() has just looked, and remembered what it found */
	skycell_cache_init();
	entry = (idx_entry *) hash_search(idx_cache, &rte->relid, HASH_FIND, NULL);
	if (entry == NULL || !equal(entry->expr, key) || entry->stored == 0)
		return false;
	att = entry->stored;
	ops = entry->stored_cell_ops;
	*cell = (Node *) makeVar(varno, att, INT8OID, -1, InvalidOid, 0);
	load_density(rte->relid, att, d);
	*statrel = rte->relid;
	*uses_cell_ops = ops;
	return true;
}

/*
 * The region-area analogue of density_for_expr(), for the region-region
 * operator selectivity fix (adql.c's region_angle_est(), "Round forty-
 * eight"/"Round forty-nine"): given a region expression (in practice
 * almost always a bare column), looks for a plain btree expression index
 * on area(<that same expression>) and, if ANALYZE has run on it, returns
 * its histogram -- in whatever unit the SQL area() function itself
 * returns (square degrees: skycell_area() multiplies sc_region.area, in
 * steradians, by RAD2DEG twice), not the steradians every other caller
 * of sc_region.area in this file works in. This function's contract is
 * "whatever area() would return for this expression," matching the index
 * a user would actually write (CREATE INDEX ... (area(region_col))), not
 * an internal-only unit nothing else here uses; converting back to
 * steradians is the caller's job (region_angle_est() does it), not this
 * function's.
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
 * Returns the *whole* histogram, not a single "typical" value collapsed
 * out of it ("Round forty-nine" -- a single value, however chosen, is a
 * poor stand-in for the real distribution once a column mixes genuinely
 * different scales, e.g. both small source footprints and a few huge
 * survey tiles: whichever single number is picked, most rows are not
 * that size). region_angle_est()'s own callers average their selectivity
 * formula over every bound instead, which is exact for an equal-
 * frequency histogram's own implicit model of the distribution (each
 * bound represents an equal share of the rows) rather than a guess at
 * which single bound best represents all of them. Still only as good as
 * ANALYZE's histogram itself is -- a sample, not the true population,
 * and silent about most-common-values entirely -- but that is a
 * precision limit of the statistics available, not an approximation
 * this function adds on top of them.
 */
bool
region_area_histogram(Oid selfid, PlannerInfo *root, Node *region_expr,
					   double **areas, int *n)
{
	List	   *vars = sc_pull_local_vars(region_expr, 0);
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
								double	   *vals = palloc(sizeof(double) * sslot.nvalues);

								for (int i = 0; i < sslot.nvalues; i++)
									vals[i] = DatumGetFloat8(sslot.values[i]);
								*areas = vals;
								*n = sslot.nvalues;
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
	List	   *vars = sc_pull_local_vars(rg, 0);
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
	/*
	 * All of it: with no statistics found, the caller still covers against
	 * this as a uniform sky, and the covering reads the map fields too.
	 */
	memset(d, 0, sizeof(*d));
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
								 DatumGetFloat8(((Const *) list_nth(args, 5))->constvalue),
								 true));
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

		/*
		 * As a top-level term the polygon goes to the custom scan (0.43), as
		 * a constant ADQL polygon does: skycell_region_scan() over its
		 * covering in cell order, the rows tested from ra and dec.
		 */
		{
			FuncExpr   *scan = skycell_poly_region_scan(fexpr->funcid, cell, lsecond(args),
														lthird(args),
														DatumGetArrayTypeP(((Const *) poly)->constvalue));

			if (scan != NULL && region_scan_keep(root, fexpr, scan))
			{
				sc_region_free(&reg);
				return region_with_partition_bounds(root, scan);
			}
		}
		density_for_var(root, cell, &dens, &dens_statrel, &uses_cell_ops);
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
		Oid			arm_opfamily;
		Const	   *hist;
		List	   *arms = NIL;
		Datum	   *hd;
		FuncExpr   *exact;

		density_for_var(root, cell, &dens, &dens_statrel, &uses_cell_ops);
		arm_opfamily = uses_cell_ops ? cell_ops_opfamily() : INTEGER_BTREE_FAM_OID;
		hd = palloc(sizeof(Datum) * Max(dens.nbounds, 1));
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

/* the synthesised cell expression, or the stored column that holds it */
static Expr *
stored_cell_or(PlannerInfo *root, Expr *cell)
{
	Node	   *c = (Node *) cell;
	sc_density	d;
	Oid			statrel;
	bool		ops;

	(void) skycell_cell_density(root, &c, &d, &statrel, &ops);
	return (Expr *) c;
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
	cell = stored_cell_or(root, cell);

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
 * yields a correct sequential plan, which is what q3c does too.  When the
 * custom scan can take the cone (cone_scan_keep_as()), the six-argument
 * skycell_cone is returned unrewritten instead, for its path.
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
	cell = stored_cell_or(root, cell);

	six->args = lcons(cell, six->args);

	/* left for the custom scan as the skycell_cone it stands for, if it can take it */
	{
		Oid			cone_types[6] = {INT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID};
		FuncExpr   *cone6 = makeFuncExpr(lookup_sibling_func(fexpr->funcid, "skycell_cone",
															 6, cone_types),
										 BOOLOID, six->args, InvalidOid, InvalidOid,
										 COERCE_EXPLICIT_CALL);

		if (cone_scan_keep_as(root, fexpr, cone6))
			return cone_with_partition_bounds(root, cone6);
	}
	return simplify_cone(root, six);
}

/*
 * The covering of a constant skycell_cone(cell, ra, dec, ra0, dec0, radius)
 * for the custom scan, through the same density model and memo the rewrite
 * uses but the custom scan's range price (scan_range_cost()).  False when the
 * cone's parameters are not all non-null constants.  The ranges are owned by
 * the memo (or the current context when it is off): copy them before anything
 * else can flush it.
 */
bool
skycell_const_cone_cover(PlannerInfo *root, List *args, sc_region *reg,
						 sc_cover *cov, sc_density *dens)
{
	bool		anynull;
	Oid			statrel;
	bool		uses_cell_ops;
	sc_cover_params p;
	double		ra0,
				dec0,
				radius;

	if (list_length(args) != 6 || !all_const(args, 3, 5, &anynull) || anynull)
		return false;
	ra0 = DatumGetFloat8(((Const *) list_nth(args, 3))->constvalue);
	dec0 = DatumGetFloat8(((Const *) list_nth(args, 4))->constvalue);
	radius = DatumGetFloat8(((Const *) list_nth(args, 5))->constvalue);

	density_for_var(root, linitial(args), dens, &statrel, &uses_cell_ops);
	check_err(sc_region_cone(reg, ra0, dec0, radius, true));
	current_params_scan(&p, skycell_max_ranges, dens);
	cover_cached(reg, dens, &p, statrel, ra0, dec0, radius, cov);
	return true;
}

/*
 * Selectivity of a constant cone left unrewritten for the custom scan: the
 * rows the covering expects, scaled by area(cone) / area(covering) -- the same
 * product the rewrite's range quals and exact-test selectivity make.
 */
static double
cone_selectivity(PlannerInfo *root, List *args)
{
	sc_region	reg;
	sc_cover	cov;
	sc_density	dens;
	double		r = DatumGetFloat8(((Const *) list_nth(args, 5))->constvalue) * DEG2RAD;

	if (skycell_const_cone_cover(root, args, &reg, &cov, &dens) &&
		dens.ntotal > 0 && cov.area > 0)
		return cov.exp_rows * fmin(1.0, reg.area / cov.area) / dens.ntotal;
	return pow(sin(fmin(fmax(r, 0), M_PI) / 2.0), 2);	/* cap area / 4pi */
}

/*
 * A constant composite region answered by the custom scan
 * (skycell_region_scan(cell, ra, dec, region), cone_scan.c): its covering,
 * computed once per planning however many times selectivity and path ask.
 * Composites are not keyed by a centre and radius as cones are, so they get
 * a small cache of their own, matched on the region's bytes.
 */
#define REGION_COVER_CACHE 16

typedef struct
{
	Oid			statrel;
	sc_cover_params params;
	Size		len;
	char	   *bytes;
	sc_cover	cov;
} region_cover_entry;

static MemoryContext region_cover_cxt = NULL;
static region_cover_entry region_cover_cache[REGION_COVER_CACHE];
static int	region_cover_n = 0;

static bool
params_equal(const sc_cover_params *a, const sc_cover_params *b)
{
	return a->max_order == b->max_order && a->max_ranges == b->max_ranges &&
		a->range_cost == b->range_cost && a->split_cost == b->split_cost &&
		a->max_steps == b->max_steps && a->probe_split_cost == b->probe_split_cost &&
		a->max_area_ratio == b->max_area_ratio && a->use_seed == b->use_seed &&
		a->direct == b->direct && a->force_order == b->force_order;
}

bool
skycell_const_region_cover(PlannerInfo *root, List *args, sc_region *reg,
						   sc_cover *cov, sc_density *dens)
{
	Const	   *rc;
	bytea	   *b;
	Size		len;
	Oid			statrel;
	bool		uses_cell_ops;
	sc_cover_params p;
	region_cover_entry *e;

	if (list_length(args) != 4 || !IsA(llast(args), Const) || ((Const *) llast(args))->constisnull)
		return false;
	rc = (Const *) llast(args);
	b = DatumGetByteaPP(rc->constvalue);
	len = VARSIZE_ANY(b);

	density_for_var(root, linitial(args), dens, &statrel, &uses_cell_ops);
	skycell_region_from_datum(rc->constvalue, reg);
	current_params_scan(&p, skycell_max_ranges, dens);

	for (int i = 0; i < region_cover_n; i++)
	{
		e = &region_cover_cache[i];
		if (e->statrel == statrel && e->len == len && params_equal(&e->params, &p) &&
			memcmp(e->bytes, b, len) == 0)
		{
			*cov = e->cov;		/* the ranges stay owned by the cache */
			return true;
		}
	}
	if (region_cover_cxt == NULL)
		region_cover_cxt = AllocSetContextCreate(TopMemoryContext, "skycell region coverings",
												 ALLOCSET_DEFAULT_SIZES);
	if (region_cover_n == REGION_COVER_CACHE)
	{
		MemoryContextReset(region_cover_cxt);	/* start over */
		region_cover_n = 0;
	}
	{
		MemoryContext old = MemoryContextSwitchTo(region_cover_cxt);

		e = &region_cover_cache[region_cover_n++];
		e->statrel = statrel;
		e->params = p;
		e->len = len;
		e->bytes = palloc(len);
		memcpy(e->bytes, b, len);
		sc_cover_compute(reg, dens, &p, &e->cov);
		MemoryContextSwitchTo(old);
	}
	*cov = e->cov;
	return true;
}

/*
 * selectivity of skycell_region_scan(): as a cone's, from its covering.  And
 * the custom scan for a skycell_region_scan() that arrives written, not
 * rewritten here from CONTAINS: a Citus worker receives its shard's query
 * from the coordinator after the coordinator's rewrite (0.45).
 */
PG_FUNCTION_INFO_V1(skycell_region_scan_support);
Datum
skycell_region_scan_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;

		if (req->root != NULL)
			(void) region_scan_keep(req->root, req->fcall, req->fcall);
		PG_RETURN_POINTER(NULL);	/* the call stays as it is */
	}

	if (IsA(rawreq, SupportRequestSelectivity))
	{
		SupportRequestSelectivity *req = (SupportRequestSelectivity *) rawreq;
		sc_region	reg;
		sc_cover	cov;
		sc_density	dens;

		if (!req->is_join && req->root != NULL &&
			skycell_const_region_cover(req->root, req->args, &reg, &cov, &dens) &&
			dens.ntotal > 0)
		{
			/* an empty covering (an empty composite) holds no row */
			req->selectivity = cov.area <= 0 ? 1e-12
				: fmin(1.0, fmax(sc_region_expected_rows(&reg, &dens, &cov)
								 / dens.ntotal, 1e-12));
			sc_region_free(&reg);
			PG_RETURN_POINTER(req);
		}
	}
	PG_RETURN_POINTER(NULL);
}

PG_FUNCTION_INFO_V1(skycell_support);
Datum
skycell_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSelectivity))
	{
		SupportRequestSelectivity *req = (SupportRequestSelectivity *) rawreq;
		bool		anynull;

		if (!req->is_join && req->root != NULL && list_length(req->args) == 6 &&
			all_const(req->args, 3, 5, &anynull) && !anynull)
		{
			req->selectivity = fmin(1.0, fmax(cone_selectivity(req->root, req->args), 1e-12));
			PG_RETURN_POINTER(req);
		}
		/* a cross-match cone the custom scan answers per outer row */
		if (req->root != NULL && list_length(req->args) == 6)
		{
			double		sel = cone_join_selectivity(req->root, req->args);

			if (sel >= 0)
			{
				req->selectivity = fmin(1.0, fmax(sel, 1e-12));
				PG_RETURN_POINTER(req);
			}
		}
		PG_RETURN_POINTER(NULL);
	}

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		int			nargs = list_length(req->fcall->args);

		if (nargs == 6)
		{
			/* already given its bounds: see SKYCELL_BOUNDED_LOCATION */
			if (skycell_is_bounded(req->fcall->args))
				PG_RETURN_POINTER(NULL);
			/* left as is for the custom scan (cone_scan.c), if it can take it */
			if (cone_scan_keep(req->root, req->fcall))
			{
				/* with partition bounds (0.44), or left as is */
				Node	   *n = cone_with_partition_bounds(req->root, copyObject(req->fcall));

				PG_RETURN_POINTER(IsA(n, BoolExpr) ? n : NULL);
			}
			PG_RETURN_POINTER(simplify_cone(req->root, req->fcall));
		}
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
				probe_split_cost,
				area_ratio;
	uint32		hhash;
	int			nslots;
	int32		probe_orders;
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
		slot_cache.probe_split_cost != skycell_probe_split_cost ||
		slot_cache.probe_orders != skycell_probe_orders ||
		slot_cache.area_ratio != skycell_max_area_ratio)
	{
		sc_region	reg;
		sc_cover	cov;
		sc_cover_params p;
		sc_density	d = {ntotal, hc->n, hc->b};
		int			s;

		slot_cache.valid = false;
		check_err(sc_region_cone(&reg, ra0, dec0, radius, true));
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
		slot_cache.probe_split_cost = skycell_probe_split_cost;
		slot_cache.probe_orders = skycell_probe_orders;
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
				probe_split_cost,
				area_ratio;
	int			nslots;
	int32		probe_orders;
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
		poly_slot_cache.probe_split_cost != skycell_probe_split_cost ||
		poly_slot_cache.probe_orders != skycell_probe_orders ||
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
		poly_slot_cache.probe_split_cost = skycell_probe_split_cost;
		poly_slot_cache.probe_orders = skycell_probe_orders;
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
				probe_split_cost,
				area_ratio;
	int			nslots;
	int32		probe_orders;
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
		region_slot_cache.probe_split_cost != skycell_probe_split_cost ||
		region_slot_cache.probe_orders != skycell_probe_orders ||
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
		region_slot_cache.probe_split_cost = skycell_probe_split_cost;
		region_slot_cache.probe_orders = skycell_probe_orders;
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
	uint64		generation;		/* skycell_cache_generation it was loaded at */
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

	if (rc == NULL || rc->relid != relid || rc->attnum != attnum ||
		rc->generation != skycell_cache_generation)
	{
		MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		if (rc == NULL)
			rc = palloc0(sizeof(rel_density_cache));
		else if (rc->d.bounds)
			pfree((void *) rc->d.bounds);	/* load_density() copied them here */
		rc->relid = relid;
		rc->attnum = attnum;
		/* before the load: a flush during it must make the next call reload */
		rc->generation = skycell_cache_generation;
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
	check_err(sc_region_cone(&reg, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2), true));
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
	check_err(sc_region_cone(&reg, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2), true));
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

/*
 * angular distance in degrees.  One of the two points is usually the same on
 * every call -- the centre a query orders or filters by -- so the vector of
 * each argument pair is kept with the call site, and converted again only
 * when the pair changes: the same arithmetic, so the same result.
 */
typedef struct dist_point_cache
{
	bool		valid[2];
	double		ra[2],
				dec[2];
	sc_vec3		v[2];
} dist_point_cache;

static sc_vec3
dist_point(dist_point_cache *c, int i, double ra, double dec)
{
	if (c == NULL)
		return sc_radec2vec(ra, dec);
	/* compared as bits: -0.0 is not 0.0 to sin(), and NaN is never equal */
	if (!c->valid[i] || memcmp(&c->ra[i], &ra, sizeof(double)) != 0 ||
		memcmp(&c->dec[i], &dec, sizeof(double)) != 0)
	{
		c->ra[i] = ra;
		c->dec[i] = dec;
		c->v[i] = sc_radec2vec(ra, dec);
		c->valid[i] = true;
	}
	return c->v[i];
}

PG_FUNCTION_INFO_V1(skycell_dist);
Datum
skycell_dist(PG_FUNCTION_ARGS)
{
	dist_point_cache *c = fcinfo->flinfo ? (dist_point_cache *) fcinfo->flinfo->fn_extra : NULL;
	sc_vec3		a,
				b;

	if (c == NULL && fcinfo->flinfo != NULL)
		c = fcinfo->flinfo->fn_extra = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
															  sizeof(dist_point_cache));
	a = dist_point(c, 0, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1));
	b = dist_point(c, 1, PG_GETARG_FLOAT8(2), PG_GETARG_FLOAT8(3));
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

	check_err(sc_region_cone(&reg, PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2), true));
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

/* ------------------------------------------------------------------ */
/* estimates of the number of rows in a region (0.46)                  */
/* ------------------------------------------------------------------ */

/*
 * The density model the planner reads for a table: its index on
 * skycell_ang2cell(ra, dec) or skycell_cell(pos) (a skycell_cell_ops one
 * preferred, as density_for_expr() does), else a stored column named cell.
 * False when it has neither.
 */
static bool
table_cell_density(Oid relid, sc_density *d)
{
	Relation	rel = table_open(relid, AccessShareLock);
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	Oid			best = InvalidOid;
	bool		best_ops = false;
	Oid			cellops = cell_ops_opfamily();
	AttrNumber	att;

	foreach(lc, indexes)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	irel = index_open(indexoid, AccessShareLock);

		if (irel->rd_index->indnatts >= 1 && irel->rd_index->indkey.values[0] == 0)
		{
			List	   *exprs = RelationGetIndexExpressions(irel);
			Node	   *e = exprs ? linitial(exprs) : NULL;

			if (e != NULL && IsA(e, FuncExpr))
			{
				char	   *name = get_func_name(((FuncExpr *) e)->funcid);
				bool		ops = OidIsValid(cellops) && irel->rd_opfamily[0] == cellops;

				if (name != NULL && (strcmp(name, "skycell_ang2cell") == 0 ||
									 strcmp(name, "skycell_cell") == 0) &&
					(!OidIsValid(best) || (ops && !best_ops)))
				{
					best = indexoid;
					best_ops = ops;
				}
			}
		}
		index_close(irel, AccessShareLock);
	}
	list_free(indexes);
	table_close(rel, AccessShareLock);
	if (OidIsValid(best))
	{
		load_density_cached(relid, best, 1, d);
		return true;
	}
	att = get_attnum(relid, "cell");
	if (att != InvalidAttrNumber)
	{
		load_density(relid, att, d);
		return true;
	}
	return false;
}

/*
 * The table a name gives, as regclass reads it (search_path, quoting), and
 * the relations whose statistics hold its rows: itself, or a partitioned
 * table's leaf partitions.
 */
static List *
estimate_relations(text *name)
{
	char	   *s = text_to_cstring(name);
	Oid			relid = DatumGetObjectId(DirectFunctionCall1(regclassin, CStringGetDatum(s)));
	List	   *out = NIL;
	ListCell   *lc;

	if (get_rel_relkind(relid) != RELKIND_PARTITIONED_TABLE)
		return list_make1_oid(relid);
	foreach(lc, find_all_inheritors(relid, AccessShareLock, NULL))
	{
		Oid			r = lfirst_oid(lc);

		if (get_rel_relkind(r) == RELKIND_RELATION || get_rel_relkind(r) == RELKIND_MATVIEW)
			out = lappend_oid(out, r);
	}
	return out;
}

/*
 * A Citus distributed table (0.45): its rows, and so its statistics, are in
 * shards on the workers, and the coordinator holds none.  The estimates are
 * then made on every shard, by the same function, and summed.  True when
 * relid is such a table (Citus installed and the table in pg_dist_partition).
 */
static bool
is_citus_distributed(Oid relid)
{
	Oid			argt[1] = {OIDOID};
	Datum		argv[1] = {ObjectIdGetDatum(relid)};
	bool		found;

	if (!OidIsValid(get_relname_relid("pg_dist_partition", PG_CATALOG_NAMESPACE)))
		return false;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "skycell: SPI_connect failed");
	if (SPI_execute_with_args("SELECT 1 FROM pg_catalog.pg_dist_partition WHERE logicalrelid = $1",
							  1, argt, argv, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "skycell: could not read pg_dist_partition");
	found = SPI_processed > 0;
	SPI_finish();
	return found;
}

/* the relation tbl names, without its partitions */
static Oid
estimate_relid(text *name)
{
	return DatumGetObjectId(DirectFunctionCall1(regclassin, CStringGetDatum(text_to_cstring(name))));
}

/*
 * The sum over a distributed table's shards of the float8 that shard_sql
 * (with %s for the shard's name, which run_command_on_shards() fills in)
 * returns; an error on any shard is raised here.
 */
static double
citus_shard_sum(Oid relid, const char *shard_sql)
{
	Oid			argt[2] = {REGCLASSOID, TEXTOID};
	Datum		argv[2] = {ObjectIdGetDatum(relid), CStringGetTextDatum(shard_sql)};
	double		total = 0;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "skycell: SPI_connect failed");
	if (SPI_execute_with_args("SELECT success, result FROM run_command_on_shards($1, $2)",
							  2, argt, argv, NULL, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "skycell: run_command_on_shards failed");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		bool		ok = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull));
		char	   *res = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2);

		if (!ok)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							errmsg("skycell: estimate on a shard of %s failed: %s",
								   get_rel_name(relid), res ? res : "")));
		if (res != NULL && *res != '\0')
			total += strtod(res, NULL);
	}
	SPI_finish();
	return total;
}

/* a skyregion as an SQL literal, for a shard's query */
static char *
region_literal(Datum r)
{
	return quote_literal_cstr(DatumGetCString(DirectFunctionCall1(skyregion_out, r)));
}

static void
no_density(Oid relid)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("skycell: %s has no cell index to estimate from", get_rel_name(relid)),
			 errhint("Index skycell_ang2cell(ra, dec) (or a stored cell column) and ANALYZE the table.")));
}

/*
 * The rows the density model puts inside a region: the region's cells,
 * refined from the twelve base cells where its edge crosses them, each cell
 * inside counted whole, until a cell is a 16th of the region across or holds
 * under half a row -- then counted whole if its centre is inside.  So a
 * coarse covering where the table is sparse (a partition holding half the
 * sky, the hole of an annulus) does not dilute the count, as scaling a
 * covering's rows by its area would.  At most 20,000 cells.
 */
#define ESTIMATE_MAX_CELLS 20000

static double
region_rows(const sc_region *reg, const sc_density *d)
{
	struct
	{
		int			o;
		int64		pix;
	}			stack[12 + 3 * (SC_MAX_ORDER + 1)];
	int			top = 0,
				steps = 0,
				maxo;
	double		total = 0,
				rad;
	sc_vec3		c;

	sc_region_bounding_cap(reg, &c, &rad);
	maxo = (int) floor(log2(1.0233267079464885 / fmax(fmin(rad, M_PI) / 16.0, 1e-12)));
	maxo = Max(0, Min(maxo, SC_MAX_ORDER));
	for (int f = 11; f >= 0; f--)
	{
		stack[top].o = 0;
		stack[top++].pix = f;
	}
	while (top > 0)
	{
		int			o = stack[--top].o;
		int64		pix = stack[top].pix;
		double		fo,
					rows;
		sc_class	cls = sc_region_classify(reg, o, pix, &fo);

		steps++;
		if (cls == SC_OUT)
			continue;
		rows = sc_density_rows(d, sc_pix_lo(o, pix), sc_pix_hi(o, pix));
		if (cls == SC_IN)
			total += rows;
		else if (rows < 0.5 || o >= maxo || steps + top + 4 > ESTIMATE_MAX_CELLS)
		{
			if (sc_region_contains(reg, sc_pix2vec(o, pix)))
				total += rows;
		}
		else
			for (int ch = 3; ch >= 0; ch--)
			{
				stack[top].o = o + 1;
				stack[top++].pix = 4 * pix + ch;
			}
	}
	return total;
}

/*
 * skycell_count_estimate(tbl, region): the rows the table's statistics expect
 * in the region (region_rows() above).  No row is read: well under a
 * millisecond, at the resolution of the histogram (about ntotal / statistics
 * target rows per bucket), and as current as the last ANALYZE.
 */
PG_FUNCTION_INFO_V1(skycell_count_estimate);
Datum
skycell_count_estimate(PG_FUNCTION_ARGS)
{
	List	   *rels;
	sc_region	reg;
	double		total = 0;
	ListCell   *lc;
	Oid			relid = estimate_relid(PG_GETARG_TEXT_PP(0));

	if (is_citus_distributed(relid))
	{
		const char *nsp = quote_identifier(get_namespace_name(get_func_namespace(fcinfo->flinfo->fn_oid)));

		PG_RETURN_FLOAT8(citus_shard_sum(relid,
										 psprintf("SELECT %s.skycell_count_estimate('%%s', %s::%s.skyregion)",
												  nsp, region_literal(PG_GETARG_DATUM(1)), nsp)));
	}
	rels = estimate_relations(PG_GETARG_TEXT_PP(0));

	skycell_region_from_datum(PG_GETARG_DATUM(1), &reg);
	foreach(lc, rels)
	{
		sc_density	d;

		if (!table_cell_density(lfirst_oid(lc), &d))
			no_density(lfirst_oid(lc));
		if (d.ntotal > 0 && reg.area > 0)
			total += region_rows(&reg, &d);
	}
	sc_region_free(&reg);
	PG_RETURN_FLOAT8(total);
}

/* skycell_count_estimate(tbl, hpx_order, ipix): the same for one HEALPix cell */
PG_FUNCTION_INFO_V1(skycell_count_estimate_cell);
Datum
skycell_count_estimate_cell(PG_FUNCTION_ARGS)
{
	List	   *rels;
	int32		order = PG_GETARG_INT32(1);
	int64		ipix = PG_GETARG_INT64(2);
	double		total = 0;
	ListCell   *lc;
	Oid			relid = estimate_relid(PG_GETARG_TEXT_PP(0));

	if (order < 0 || order > SC_MAX_ORDER)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("skycell: HEALPix order must be within [0, %d]", SC_MAX_ORDER)));
	if (ipix < 0 || ipix >= 12 * ((int64) 1 << (2 * order)))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("skycell: no HEALPix cell %lld at order %d", (long long) ipix, order)));
	if (is_citus_distributed(relid))
	{
		const char *nsp = quote_identifier(get_namespace_name(get_func_namespace(fcinfo->flinfo->fn_oid)));

		PG_RETURN_FLOAT8(citus_shard_sum(relid,
										 psprintf("SELECT %s.skycell_count_estimate('%%s', %d, " INT64_FORMAT ")",
												  nsp, order, ipix)));
	}
	rels = estimate_relations(PG_GETARG_TEXT_PP(0));
	foreach(lc, rels)
	{
		sc_density	d;

		if (!table_cell_density(lfirst_oid(lc), &d))
			no_density(lfirst_oid(lc));
		if (d.ntotal > 0)
			total += sc_density_rows(&d, sc_pix_lo(order, ipix), sc_pix_hi(order, ipix));
	}
	PG_RETURN_FLOAT8(total);
}

/*
 * skycell_density_estimate(tbl, region, hpx_order) -> (ipix, estimate,
 * inside): the cells of that order the region meets, each with the rows the
 * statistics expect in it (the whole cell's) and whether it lies wholly inside
 * the region; in cell order.  At most 200,000 cells.
 */
#define DENSITY_MAX_CELLS 200000

/*
 * skycell_density_estimate() of a distributed table: each shard's cells, as
 * JSON, their estimates summed over the shards.  Every shard lists the same
 * cells (those the region meets), so whether one is inside agrees.
 */
static void
density_estimate_shards(FunctionCallInfo fcinfo, Oid relid, Datum region, int order)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const char *nsp = quote_identifier(get_namespace_name(get_func_namespace(fcinfo->flinfo->fn_oid)));
	char	   *shard_sql = psprintf("SELECT coalesce(json_agg(json_build_array(ipix, estimate, inside)), '[]') "
									 "FROM %s.skycell_density_estimate('%%s', %s::%s.skyregion, %d)",
									 nsp, region_literal(region), nsp, order);
	Oid			argt[2] = {REGCLASSOID, TEXTOID};
	Datum		argv[2] = {ObjectIdGetDatum(relid), CStringGetTextDatum(shard_sql)};

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "skycell: SPI_connect failed");
	if (SPI_execute_with_args("SELECT success, result FROM run_command_on_shards($1, $2)",
							  2, argt, argv, NULL, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "skycell: run_command_on_shards failed");
	{
		uint64		nshards = SPI_processed;
		Datum	   *results = palloc(sizeof(Datum) * Max(nshards, 1));
		Oid			at[1] = {TEXTARRAYOID};
		Datum		av[1];

		for (uint64 i = 0; i < nshards; i++)
		{
			bool		isnull;
			char	   *res = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2);

			if (!DatumGetBool(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull)))
				ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								errmsg("skycell: estimate on a shard of %s failed: %s",
									   get_rel_name(relid), res ? res : "")));
			results[i] = CStringGetTextDatum(res ? res : "[]");
		}
		/* the shards' cells, summed */
		av[0] = PointerGetDatum(construct_array(results, (int) nshards, TEXTOID, -1, false, TYPALIGN_INT));
		if (SPI_execute_with_args("SELECT (e->>0)::int8, sum((e->>1)::float8), bool_and((e->>2)::bool) "
								  "FROM unnest($1) r, json_array_elements(r::json) e GROUP BY 1 ORDER BY 1",
								  1, at, av, NULL, true, 0) != SPI_OK_SELECT)
			elog(ERROR, "skycell: summing the shards' estimates failed");
	}
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		Datum		v[3];
		bool		nulls[3];

		for (int k = 0; k < 3; k++)
			v[k] = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, k + 1, &nulls[k]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
	}
	SPI_finish();
}

PG_FUNCTION_INFO_V1(skycell_density_estimate);
Datum
skycell_density_estimate(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *rels;
	int32		order = PG_GETARG_INT32(2);
	Oid			relid = estimate_relid(PG_GETARG_TEXT_PP(0));
	sc_region	reg;
	sc_density *ds;
	int			nd = 0,
				n = 0;
	ListCell   *lc;
	struct
	{
		int			o;
		int64		pix;
	}		   *stack;
	int			top = 0;

	if (order < 0 || order > 20)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("skycell: HEALPix order must be within [0, 20] here")));
	if (is_citus_distributed(relid))
	{
		density_estimate_shards(fcinfo, relid, PG_GETARG_DATUM(1), order);
		return (Datum) 0;
	}
	rels = estimate_relations(PG_GETARG_TEXT_PP(0));
	ds = palloc(sizeof(sc_density) * Max(list_length(rels), 1));
	foreach(lc, rels)
	{
		if (!table_cell_density(lfirst_oid(lc), &ds[nd]))
			no_density(lfirst_oid(lc));
		nd++;
	}
	skycell_region_from_datum(PG_GETARG_DATUM(1), &reg);
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	stack = palloc(sizeof(*stack) * (12 + 3 * (order + 1)));
	for (int f = 11; f >= 0; f--)
	{
		stack[top].o = 0;
		stack[top++].pix = f;
	}
	while (top > 0)
	{
		int			o = stack[--top].o;
		int64		pix = stack[top].pix;
		double		fo;
		sc_class	cls = sc_region_classify(&reg, o, pix, &fo);

		if (cls == SC_OUT)
			continue;
		if (o < order)
		{
			/* children last to first, so that they come off in cell order */
			for (int c = 3; c >= 0; c--)
			{
				stack[top].o = o + 1;
				stack[top++].pix = 4 * pix + c;
			}
			continue;
		}
		if (++n > DENSITY_MAX_CELLS)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("skycell: the region meets more than %d cells at order %d",
								   DENSITY_MAX_CELLS, order),
							errhint("Ask for a coarser order.")));
		{
			double		est = 0;
			Datum		v[3];
			bool		nulls[3] = {false, false, false};

			for (int i = 0; i < nd; i++)
				if (ds[i].ntotal > 0)
					est += sc_density_rows(&ds[i], sc_pix_lo(o, pix), sc_pix_hi(o, pix));
			v[0] = Int64GetDatum(pix);
			v[1] = Float8GetDatum(est);
			v[2] = BoolGetDatum(cls == SC_IN);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
		}
	}
	sc_region_free(&reg);
	return (Datum) 0;
}

/* ------------------------------------------------------------------ */
/* MOC export: IVOA MOC 2.0 ASCII and JSON (0.43)                      */
/* ------------------------------------------------------------------ */

/* a growable list of inclusive order-29 cell ranges */
typedef struct
{
	int64	   *r;				/* lo0, hi0, lo1, hi1, ... */
	int			n,
				cap;
} moc_ranges;

static void
moc_add(moc_ranges *m, int64 lo, int64 hi)
{
	if (m->n > 0 && m->r[2 * m->n - 1] + 1 >= lo)
	{
		if (hi > m->r[2 * m->n - 1])
			m->r[2 * m->n - 1] = hi;
		return;
	}
	if (m->n == m->cap)
	{
		m->cap = m->cap ? 2 * m->cap : 64;
		m->r = m->r ? repalloc(m->r, sizeof(int64) * 2 * m->cap) : palloc(sizeof(int64) * 2 * m->cap);
	}
	m->r[2 * m->n] = lo;
	m->r[2 * m->n + 1] = hi;
	m->n++;
}

static int
moc_fmt(text *t)
{
	char	   *f = text_to_cstring(t);

	if (pg_strcasecmp(f, "ascii") == 0)
		return 0;
	if (pg_strcasecmp(f, "json") == 0)
		return 1;
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("skycell: MOC format must be 'ascii' or 'json'")));
	return 0;
}

static void
moc_check_order(int32 order, int32 max)
{
	if (order < 0 || order > max)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("skycell: MOC order must be within [0, %d]", max)));
}

/*
 * Sorted, merged order-29 ranges (multiples of the max_order cell) as a
 * normalised MOC: each range cut into the largest aligned cells it holds,
 * so that four siblings are always their parent; then per order, ascending,
 * the cells in order, runs written a-b in ASCII.  An empty max_order is
 * written last ("10/", or "10": []) to declare the MOC's depth, as MOCPy and
 * the MOC 2.0 recommendation do.
 */
static char *moc_serialise_cstr(const moc_ranges *m, int max_order, int json);

static text *
moc_serialise(const moc_ranges *m, int max_order, int json)
{
	return cstring_to_text(moc_serialise_cstr(m, max_order, json));
}

/*
 * The MOC of n sorted, merged order-29 ranges (pairs lo, hi), for the text
 * form of a MOC region (adql.c); max_order no finer than its finest cell.
 */
char *
skycell_moc_cstring(const int64 *pairs, int n, int max_order, bool json)
{
	moc_ranges	m = {(int64 *) pairs, n, n};

	return moc_serialise_cstr(&m, max_order, json);
}

static char *
moc_serialise_cstr(const moc_ranges *m, int max_order, int json)
{
	StringInfoData *ord = palloc0(sizeof(StringInfoData) * (max_order + 1));
	int64	   *last = palloc(sizeof(int64) * (max_order + 1)),
			   *run = palloc(sizeof(int64) * (max_order + 1));
	bool	   *any = palloc0(sizeof(bool) * (max_order + 1));
	StringInfoData out;

	for (int o = 0; o <= max_order; o++)
		initStringInfo(&ord[o]);

	/* the ASCII run of cells pending at order o, written when it ends */
#define FLUSH_RUN(o) \
	do { \
		if (run[o] == last[o]) \
			appendStringInfo(&ord[o], "%s" INT64_FORMAT, ord[o].len ? " " : "", run[o]); \
		else \
			appendStringInfo(&ord[o], "%s" INT64_FORMAT "-" INT64_FORMAT, ord[o].len ? " " : "", run[o], last[o]); \
	} while (0)

	for (int i = 0; i < m->n; i++)
	{
		int64		lo = m->r[2 * i],
					end = m->r[2 * i + 1] + 1;

		while (lo < end)
		{
			int			j = 2 * SC_MAX_ORDER;	/* 4^29 order-29 cells: an order-0 cell */
			int			o;
			int64		pix;

			while (j > 0 && ((lo & (((int64) 1 << j) - 1)) != 0 || lo + ((int64) 1 << j) > end))
				j -= 2;
			o = SC_MAX_ORDER - j / 2;
			pix = lo >> j;
			if (json)
			{
				appendStringInfo(&ord[o], "%s" INT64_FORMAT, any[o] ? ", " : "", pix);
				any[o] = true;
			}
			else if (!any[o])
			{
				run[o] = last[o] = pix;
				any[o] = true;
			}
			else if (pix == last[o] + 1)
				last[o] = pix;
			else
			{
				FLUSH_RUN(o);
				run[o] = last[o] = pix;
			}
			lo += (int64) 1 << j;
		}
	}
	initStringInfo(&out);
	if (json)
		appendStringInfoChar(&out, '{');
	for (int o = 0; o <= max_order; o++)
	{
		if (!any[o] && o < max_order)
			continue;
		if (!json && any[o])
			FLUSH_RUN(o);
		if (json)
			appendStringInfo(&out, "%s\"%d\": [%s]", out.len > 1 ? ", " : "", o, ord[o].data);
		else
			appendStringInfo(&out, "%s%d/%s", out.len ? " " : "", o, ord[o].data);
	}
	if (json)
		appendStringInfoChar(&out, '}');
#undef FLUSH_RUN
	return out.data;
}

/*
 * skycell_moc(region, max_order, fmt): the MOC of the cells of max_order that
 * meet the region (as skycell_cell_overlaps() counts them: a cell the region
 * misses by less than 1/16 of its size may count, never one it meets left
 * out), cells inside it whole at a coarser order.
 */
/*
 * The cells of max_order a region meets, as skycell_cell_overlaps() counts
 * them, those inside it whole at coarser orders: sorted, merged order-29
 * ranges (pairs lo, hi), *n of them.
 */
static void
region_moc_ranges(const sc_region *reg, int max_order, moc_ranges *m)
{
	struct
	{
		int			o;
		int64		pix;
	}			stack[12 + 3 * (SC_MAX_ORDER + 1)];
	int			top = 0;

	for (int f = 11; f >= 0; f--)
	{
		stack[top].o = 0;
		stack[top++].pix = f;
	}
	while (top > 0)
	{
		int			o = stack[--top].o;
		int64		pix = stack[top].pix;
		double		fo;
		sc_class	c = sc_region_classify(reg, o, pix, &fo);

		CHECK_FOR_INTERRUPTS();
		if (c == SC_OUT)
			continue;
		if (c == SC_PARTIAL && o < max_order)
		{
			for (int ch = 3; ch >= 0; ch--)
			{
				stack[top].o = o + 1;
				stack[top++].pix = 4 * pix + ch;
			}
			continue;
		}
		if (c == SC_PARTIAL && !sc_region_cell_meets(reg, o, pix))
			continue;
		moc_add(m, sc_pix_lo(o, pix), sc_pix_hi(o, pix));
	}
}

int64 *
skycell_region_moc_pairs(const sc_region *reg, int max_order, int *n)
{
	moc_ranges	m = {0};

	region_moc_ranges(reg, max_order, &m);
	*n = m.n;
	return m.r;
}

PG_FUNCTION_INFO_V1(skycell_moc);
Datum
skycell_moc(PG_FUNCTION_ARGS)
{
	int32		max_order = PG_GETARG_INT32(1);
	int			json = moc_fmt(PG_GETARG_TEXT_PP(2));
	sc_region	reg;
	moc_ranges	m = {0};

	moc_check_order(max_order, SC_MAX_ORDER);
	skycell_region_from_datum(PG_GETARG_DATUM(0), &reg);
	region_moc_ranges(&reg, max_order, &m);
	sc_region_free(&reg);
	PG_RETURN_TEXT_P(moc_serialise(&m, max_order, json));
}

/*
 * skycell_moc_agg(hpx_order, ipix [, fmt]): the MOC of the cells of one order
 * aggregated, duplicates allowed (the coverage of the rows of a query: one
 * bit a cell of that order, 1.5 MB at order 10; at most order 12).
 *
 * Its state is a type of its own, skycell_mocstate, not internal (0.45): a
 * sharded database such as Citus pushes an aggregate down to its shards only
 * when the state can travel, as text, from each shard to the coordinator,
 * which then combines them; and PostgreSQL's parallel workers hand theirs to
 * the leader the same way.  In memory the state is the bitmap, changed in
 * place by the transition function (allowed when it is called as an
 * aggregate); as text it is the runs of set cells, which for a coverage is
 * far smaller than the bitmap.
 */
typedef struct
{
	int32		vl_len_;
	int32		order;
	int32		json;
	uint8		bits[FLEXIBLE_ARRAY_MEMBER];
} MocAggState;

#define MOC_AGG_MAX_ORDER 12
#define MOC_AGG_NCELLS(order) (12 * ((int64) 1 << (2 * (order))))

/*
 * The next set cell at or after i in a state's bitmap, or ncells; and the
 * first clear one at or after i.  Word at a time: a coverage at order 12 is
 * 200M bits, most of them in long runs of one value.
 */
static int64
moc_bits_next(const uint8 *bits, int64 ncells, int64 i, bool set)
{
	while (i < ncells)
	{
		if ((i & 63) == 0 && i + 64 <= ncells)
		{
			const uint8 *p = bits + (i >> 3);
			uint64		w = 0;

			/* cell i + k is bit k of the word, on any byte order */
			for (int k = 7; k >= 0; k--)
				w = (w << 8) | p[k];
			if (!set)
				w = ~w;
			if (w == 0)
			{
				i += 64;
				continue;
			}
			return i + pg_rightmost_one_pos64(w);
		}
		if (((bits[i >> 3] >> (i & 7)) & 1) == (set ? 1 : 0))
			return i;
		i++;
	}
	return ncells;
}

/* set cells [first, first + n) */
static void
moc_bits_set(uint8 *bits, int64 first, int64 n)
{
	int64		i = first,
				end = first + n;

	for (; i < end && (i & 7); i++)
		bits[i >> 3] |= (uint8) (1 << (i & 7));
	if (end - i >= 8)
	{
		memset(bits + (i >> 3), 0xFF, (end - i) >> 3);
		i += ((end - i) >> 3) << 3;
	}
	for (; i < end; i++)
		bits[i >> 3] |= (uint8) (1 << (i & 7));
}

/* an empty state of an order, in cxt */
static MocAggState *
moc_state_new(MemoryContext cxt, int order, int json)
{
	Size		sz = offsetof(MocAggState, bits) + (MOC_AGG_NCELLS(order) + 7) / 8;
	MocAggState *st = MemoryContextAllocZero(cxt, sz);

	SET_VARSIZE(st, sz);
	st->order = order;
	st->json = json;
	return st;
}

/* a state datum as it is, detoasted (it never is, being built in memory) */
static MocAggState *
moc_state_arg(FunctionCallInfo fcinfo, int n)
{
	MocAggState *st = (MocAggState *) PG_DETOAST_DATUM(PG_GETARG_DATUM(n));

	if (st->order < 0 || st->order > MOC_AGG_MAX_ORDER ||
		VARSIZE(st) != offsetof(MocAggState, bits) + (MOC_AGG_NCELLS(st->order) + 7) / 8)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("skycell: invalid skycell_moc_agg state")));
	return st;
}

/* text: "order json first+count first+count ..." */
PG_FUNCTION_INFO_V1(skycell_mocstate_out);
Datum
skycell_mocstate_out(PG_FUNCTION_ARGS)
{
	MocAggState *st = moc_state_arg(fcinfo, 0);
	int64		ncells = MOC_AGG_NCELLS(st->order);
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "%d %d", st->order, st->json);
	for (int64 i = moc_bits_next(st->bits, ncells, 0, true); i < ncells;)
	{
		int64		j = moc_bits_next(st->bits, ncells, i, false);

		appendStringInfo(&buf, " " INT64_FORMAT "+" INT64_FORMAT, i, j - i);
		i = moc_bits_next(st->bits, ncells, j, true);
	}
	PG_RETURN_CSTRING(buf.data);
}

PG_FUNCTION_INFO_V1(skycell_mocstate_in);
Datum
skycell_mocstate_in(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	char	   *end;
	long		order,
				json;
	MocAggState *st;
	int64		ncells;

	order = strtol(s, &end, 10);
	if (end == s || order < 0 || order > MOC_AGG_MAX_ORDER)
		goto bad;
	s = end;
	json = strtol(s, &end, 10);
	if (end == s || (json != 0 && json != 1))
		goto bad;
	s = end;
	st = moc_state_new(CurrentMemoryContext, (int) order, (int) json);
	ncells = MOC_AGG_NCELLS(order);
	for (;;)
	{
		long long	first,
					n;

		while (*s == ' ')
			s++;
		if (*s == '\0')
			break;
		errno = 0;
		first = strtoll(s, &end, 10);
		if (end == s || *end != '+' || errno != 0)
			goto bad;
		s = end + 1;
		n = strtoll(s, &end, 10);
		if (end == s || errno != 0 || first < 0 || n < 1 || first + n > ncells)
			goto bad;
		s = end;
		moc_bits_set(st->bits, first, n);
	}
	PG_RETURN_POINTER(st);
bad:
	ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("invalid input syntax for type skycell_mocstate")));
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(skycell_moc_agg_trans);
Datum
skycell_moc_agg_trans(PG_FUNCTION_ARGS)
{
	MemoryContext aggcxt;
	MocAggState *st;
	int32		order;
	int64		pix;

	if (!AggCheckCallContext(fcinfo, &aggcxt))
		elog(ERROR, "skycell_moc_agg_trans called outside an aggregate");
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
	{
		/* a row without a cell adds nothing; with no state yet, still none */
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	}
	order = PG_GETARG_INT32(1);
	pix = PG_GETARG_INT64(2);
	if (PG_ARGISNULL(0))
	{
		moc_check_order(order, MOC_AGG_MAX_ORDER);
		/* 25 MB at order 12 */
		st = moc_state_new(aggcxt, order,
						   (PG_NARGS() > 3 && !PG_ARGISNULL(3)) ? moc_fmt(PG_GETARG_TEXT_PP(3)) : 0);
	}
	else
	{
		/* in place: the aggregate's own state, in its own context */
		st = (MocAggState *) PG_GETARG_POINTER(0);
		if (order != st->order)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("skycell: skycell_moc_agg() takes one order")));
	}
	if (pix < 0 || pix >= MOC_AGG_NCELLS(order))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("skycell: no HEALPix cell " INT64_FORMAT " at order %d", pix, order)));
	st->bits[pix >> 3] |= (uint8) (1 << (pix & 7));
	PG_RETURN_POINTER(st);
}

/* partial states, of parallel workers or of shards: their cells ORed */
PG_FUNCTION_INFO_V1(skycell_moc_agg_combine);
Datum
skycell_moc_agg_combine(PG_FUNCTION_ARGS)
{
	MemoryContext aggcxt;
	MocAggState *a,
			   *b;
	int64		nb;

	if (!AggCheckCallContext(fcinfo, &aggcxt))
		elog(ERROR, "skycell_moc_agg_combine called outside an aggregate");
	if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	}
	b = moc_state_arg(fcinfo, 1);
	if (PG_ARGISNULL(0))
	{
		/* the first partial: a copy the aggregate's own context owns */
		a = moc_state_new(aggcxt, b->order, b->json);
		memcpy(a->bits, b->bits, (MOC_AGG_NCELLS(b->order) + 7) / 8);
		PG_RETURN_POINTER(a);
	}
	a = (MocAggState *) PG_GETARG_POINTER(0);
	if (a->order != b->order)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("skycell: skycell_moc_agg() takes one order")));
	nb = (MOC_AGG_NCELLS(a->order) + 7) / 8;
	{
		int64		i = 0;

		for (; i + 8 <= nb; i += 8)
		{
			uint64		x,
						y;

			memcpy(&x, a->bits + i, 8);
			memcpy(&y, b->bits + i, 8);
			x |= y;
			memcpy(a->bits + i, &x, 8);
		}
		for (; i < nb; i++)
			a->bits[i] |= b->bits[i];
	}
	a->json |= b->json;
	PG_RETURN_POINTER(a);
}

PG_FUNCTION_INFO_V1(skycell_moc_agg_final);
Datum
skycell_moc_agg_final(PG_FUNCTION_ARGS)
{
	MocAggState *st;
	moc_ranges	m = {0};
	int64		ncells;
	int			sh;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();		/* no row, no order: no MOC */
	st = moc_state_arg(fcinfo, 0);
	ncells = MOC_AGG_NCELLS(st->order);
	sh = 2 * (SC_MAX_ORDER - st->order);
	for (int64 i = moc_bits_next(st->bits, ncells, 0, true); i < ncells;)
	{
		int64		j = moc_bits_next(st->bits, ncells, i, false);

		moc_add(&m, i << sh, (j << sh) - 1);
		i = moc_bits_next(st->bits, ncells, j, true);
	}
	PG_RETURN_TEXT_P(moc_serialise(&m, st->order, st->json));
}
