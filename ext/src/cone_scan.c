/*
 * cone_scan.c -- a custom scan for constant cones (skycell.custom_scan).
 *
 * The rewrite in skycell.c turns skycell_cone(cell, ra, dec, ra0, dec0, r)
 * into an OR of `cell >= lo AND cell <= hi` arms plus the exact test, and
 * leaves the rest to PostgreSQL.  The planner then builds an index path per
 * arm, estimates every arm against the column's histogram and costs a
 * BitmapOr over them: measured at 10-12 us per range on the paper's
 * catalogue, more than skycell's own covering, and the whole of its warm
 * deficit against pgSphere at small radii.
 *
 * With skycell.custom_scan on (the default), a constant cone on a single base relation is
 * left as the opaque function call (skycell_support declines to rewrite it),
 * so the planner sees one restriction clause whose selectivity skycell
 * itself supplies.  set_rel_pathlist_hook then adds one CustomPath per such
 * clause that a B-tree index on the cell expression can serve, costed from
 * the covering; the executor walks the covering's ranges in cell order with
 * a plain index scan each when the heap follows the cell order, or collects
 * their TIDs into a bitmap and reads each heap page once when it does not --
 * the same choice PostgreSQL makes between an index and a bitmap scan, from
 * the same correlation statistic -- and rechecks rows with
 * skycell_in_cone(ra, dec, ...), which, unlike skycell_cone(), does not need
 * the cell expression evaluated per row.
 *
 * A cross-match cone (centre or radius from another relation's row) gets the
 * same node parameterized by that row, computing each row's covering on
 * rescan; a cone over parameters alone (a generic plan, a correlated
 * sub-select) is covered per rescan as well.  skycell_join and
 * skycell_radial_query take these paths as the skycell_cone they stand for.
 *
 * Limits: only a cone that is a top-level AND term of a WHERE or JOIN ON (one
 * under an OR keeps the rewrite, which a BitmapOr can serve); and before
 * PostgreSQL 18 the bitmap mode does not prefetch (18's table AM reads the
 * bitmap through a read stream, which does).
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "executor/tuptable.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_statistic.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"	/* ExplainProperty*() moved here in 18 */
#endif
#include "miscadmin.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/tidbitmap.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "port/atomics.h"
#include "access/parallel.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "utils/partcache.h"
#include "catalog/partition.h"
#include "rewrite/rewriteManip.h"
#include "parser/parse_func.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_opfamily_d.h"

#include "cover.h"
#include "skycell_internal.h"

static bool skycell_custom_scan = true;
static bool skycell_index_filter = true;
static bool skycell_nearest_scan = true;

/* the six-argument skycell_cone, as last seen by cone_scan_keep() */
static Oid	cone_funcid = InvalidOid;
static Oid	region_scan_funcid = InvalidOid;	/* skycell_region_scan() */

/* skycell_in_cone(float8 x 6), looked up once per cone_funcid */
static Oid	in_cone_funcid = InvalidOid;
static Oid	in_cone_for = InvalidOid;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

static Plan *cone_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					   List *tlist, List *clauses, List *custom_plans);
static Node *cone_create_state(CustomScan *cscan);
static void cone_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *cone_exec(CustomScanState *node);
static void cone_end(CustomScanState *node);
static void cone_rescan(CustomScanState *node);
static void cone_explain(CustomScanState *node, List *ancestors, ExplainState *es);
static Size cone_estimate_dsm(CustomScanState *node, ParallelContext *pcxt);
static void cone_initialize_dsm(CustomScanState *node, ParallelContext *pcxt, void *coordinate);
static void cone_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt, void *coordinate);
static void cone_initialize_worker(CustomScanState *node, shm_toc *toc, void *coordinate);

static void add_nearest_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte);
static Plan *nearest_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
						  List *tlist, List *clauses, List *custom_plans);
static Node *nearest_create_state(CustomScan *cscan);
static void nearest_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *nearest_exec(CustomScanState *node);
static void nearest_end(CustomScanState *node);
static void nearest_rescan(CustomScanState *node);
static void nearest_explain(CustomScanState *node, List *ancestors, ExplainState *es);

static const CustomPathMethods nearest_path_methods = {
	.CustomName = "SkycellNearest",
	.PlanCustomPath = nearest_plan,
};

static const CustomScanMethods nearest_scan_methods = {
	.CustomName = "SkycellNearest",
	.CreateCustomScanState = nearest_create_state,
};

static const CustomExecMethods nearest_exec_methods = {
	.CustomName = "SkycellNearest",
	.BeginCustomScan = nearest_begin,
	.ExecCustomScan = nearest_exec,
	.EndCustomScan = nearest_end,
	.ReScanCustomScan = nearest_rescan,
	.ExplainCustomScan = nearest_explain,
};

static const CustomPathMethods cone_path_methods = {
	.CustomName = "SkycellCone",
	.PlanCustomPath = cone_plan,
};

static const CustomScanMethods cone_scan_methods = {
	.CustomName = "SkycellCone",
	.CreateCustomScanState = cone_create_state,
};

static const CustomExecMethods cone_exec_methods = {
	.CustomName = "SkycellCone",
	.BeginCustomScan = cone_begin,
	.ExecCustomScan = cone_exec,
	.EndCustomScan = cone_end,
	.ReScanCustomScan = cone_rescan,
	.EstimateDSMCustomScan = cone_estimate_dsm,
	.InitializeDSMCustomScan = cone_initialize_dsm,
	.ReInitializeDSMCustomScan = cone_reinitialize_dsm,
	.InitializeWorkerCustomScan = cone_initialize_worker,
	.ExplainCustomScan = cone_explain,
};

typedef struct ConeScanState
{
	CustomScanState css;
	Oid			indexoid;
	Relation	index;
	IndexScanDesc iscan;
	int			nranges;
	const int64 *bounds;		/* lo0, hi0, lo1, hi1, ... */
	int			cur;			/* ordered: range being scanned, -1 before the first */
	ScanKeyData keys[2];
	bool		bitmap;			/* bitmap mode, else ordered */
	TableScanDesc hscan;		/* bitmap: heap scan over the TID bitmap */

	/*
	 * Parallel (a constant cone or composite under a Gather): the ranges are
	 * cut into chunks, the same in every process, which the leader and the
	 * workers claim from a shared counter, each scanning the chunks it
	 * claims.  Without the shared counter (no parallel context) the chunks
	 * are taken in turn, as a serial scan takes its ranges.
	 */
	bool		parallel;
	pg_atomic_uint32 *next;		/* the next chunk to claim, in shared memory */
	uint32		local_next;
	int			plan_nranges;	/* the covering's ranges, for EXPLAIN */
	TIDBitmap  *tbm;			/* bitmap: NULL until the first fetch */
#if PG_VERSION_NUM < 180000
	TBMIterator *it;
	TBMIterateResult *tbmres;	/* bitmap: page being returned, or NULL */
#endif

	/* a cross-match cone: the covering is computed per outer row */
	bool		runtime;
	bool		joined;			/* parameterized by an outer row */
	bool		need_cover;		/* on the first fetch after a (re)start */
	List	   *args;			/* ExprStates of ra0, dec0, radius */
	Oid			relid,
				statrel;
	AttrNumber	attnum;
	bool		have_dens;
	uint64		dens_generation;	/* skycell_density_generation() at the load */
	sc_density	dens;
	int64	   *rt_bounds;		/* bounds' storage, rt_cap ranges */
	int			rt_cap;

	/*
	 * The lowest and highest key in the index (0.44), read once, on the
	 * first covering: a covering is clipped to them, so a cone that falls
	 * outside the cells the relation holds -- another partition's part of
	 * the sky, under partitioning by cell or by source_id -- reads no index
	 * page.  kmin > kmax for an index with no key.
	 */
	bool		have_keyrange;
	int64		kmin,
				kmax;
	bool		keys_partial;	/* the keys span part of the sky: test cones */
	double		ncoverings,		/* for EXPLAIN ANALYZE: this process's own */
				nranges_total;

	/*
	 * The index filter: when the index also holds the cone's ra and dec
	 * columns (INCLUDE), the cone is tested on each index entry and only the
	 * entries inside it are fetched from the heap.  ira/idec are their index
	 * attribute numbers, 0 when the index holds neither.
	 */
	AttrNumber	ira,
				idec;
	bool		have_centre;
	double		cra,
				cosdec0,
				cdec,
				thr;
} ConeScanState;

/* ------------------------------------------------------------------ */
/* planning                                                           */
/* ------------------------------------------------------------------ */

/*
 * What the centre and radius (arguments 3-5) make of a cone over relation
 * varno: a constant cone (all three constants), a cross-match cone (none of
 * them reads varno, at least one reads another relation of this query level,
 * so the scan is parameterized by the outer row and computes its covering
 * per row), a run-time cone (no other relation, but parameters: a generic
 * plan, or a correlated sub-select's outer columns, so the covering is
 * computed per rescan), or neither -- left to the rewrite, as is anything
 * volatile, set-returning or containing a sub-select or an aggregate.
 */
typedef enum
{
	CONE_NONE,
	CONE_CONST,
	CONE_JOIN,
	CONE_RUNTIME
} ConeKind;

typedef struct
{
	Index		varno;
	bool		outer;			/* reads another relation */
	bool		param;			/* reads a parameter or an outer query's column */
} cone_arg_ctx;

static bool
cone_arg_unsafe(Node *node, cone_arg_ctx *c)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink) || IsA(node, SubPlan) || IsA(node, Aggref) ||
		IsA(node, WindowFunc) || IsA(node, GroupingFunc) || IsA(node, PlaceHolderVar))
		return true;
	if (IsA(node, Var))
	{
		Var		   *v = (Var *) node;

		if (v->varlevelsup == 0)
		{
			if (v->varno == c->varno)
				return true;
			c->outer = true;
		}
		else
			c->param = true;
		return false;
	}
	if (IsA(node, Param))
		c->param = true;
	return expression_tree_walker(node, cone_arg_unsafe, (void *) c);
}

static ConeKind
cone_kind(List *args, Index varno)
{
	cone_arg_ctx c = {varno, false, false};
	bool		all_const = true;

	for (int i = 3; i <= 5; i++)
	{
		Node	   *a = list_nth(args, i);

		if (IsA(a, Const))
		{
			if (((Const *) a)->constisnull)
				return CONE_NONE;
			continue;
		}
		all_const = false;
		if (cone_arg_unsafe(a, &c) || contain_volatile_functions(a) || expression_returns_set(a))
			return CONE_NONE;
	}
	if (all_const)
		return CONE_CONST;
	return c.outer ? CONE_JOIN : c.param ? CONE_RUNTIME : CONE_NONE;
}

/*
 * Does the qual contain fcall as one of its top-level AND terms, written as the
 * call or as an operator over it?  The call the
 * support function sees is a rebuilt copy whose arguments are already folded,
 * so the query's own terms are compared by value: an argument matches if it is
 * equal as written or once folded the same way.
 */
/*
 * The common unfolded argument is a literal under a one-argument cast, such
 * as the numeric literal 12.5 passed as float8.  Folding it is calling that
 * cast on the constant, which is what eval_const_expressions() does for an
 * immutable strict function, without the executor state evaluate_expr()
 * builds for every argument.  Returns false, leaving *v unset, for anything
 * else; then the caller folds the argument the general way.
 */
static bool
fold_cast_of_const(Node *x, Datum *v)
{
	FuncExpr   *f;
	Const	   *c;

	if (!IsA(x, FuncExpr))
		return false;
	f = (FuncExpr *) x;
	if (list_length(f->args) != 1 || f->funcretset || !IsA(linitial(f->args), Const))
		return false;
	c = (Const *) linitial(f->args);
	if (c->constisnull || func_volatile(f->funcid) != PROVOLATILE_IMMUTABLE ||
		!func_strict(f->funcid))
		return false;
	*v = OidFunctionCall1Coll(f->funcid, f->inputcollid, c->constvalue);
	return true;
}

static bool
call_matches(PlannerInfo *root, Oid funcid, List *args, FuncExpr *fcall)
{
	ListCell   *a,
			   *b;

	if (funcid != fcall->funcid ||
		list_length(args) != list_length(fcall->args))
		return false;
	forboth(a, args, b, fcall->args)
	{
		Node	   *x = (Node *) lfirst(a);
		Node	   *y = (Node *) lfirst(b);
		Datum		v;

		if (equal(x, y))
			continue;
		if (IsA(x, Var) || IsA(x, Const))
			return false;
		if (IsA(y, Const) && !((Const *) y)->constisnull &&
			exprType(x) == ((Const *) y)->consttype &&
			exprTypmod(x) == ((Const *) y)->consttypmod &&
			exprCollation(x) == ((Const *) y)->constcollid && fold_cast_of_const(x, &v))
		{
			Const	   *k = (Const *) y;

			if (!datumIsEqual(v, k->constvalue, k->constbyval, k->constlen))
				return false;
			continue;
		}
		if (!equal(eval_const_expressions(root, copyObject(x)), y))
			return false;
	}
	return true;
}

static bool
qual_has_term(PlannerInfo *root, Node *qual, FuncExpr *fcall)
{
	ListCell   *lc;

	if (qual == NULL)
		return false;
	if (IsA(qual, List))
	{
		foreach(lc, (List *) qual)
			if (qual_has_term(root, lfirst(lc), fcall))
				return true;
		return false;
	}
	if (is_andclause(qual))
		return qual_has_term(root, (Node *) ((BoolExpr *) qual)->args, fcall);
	if (IsA(qual, FuncExpr))
		return call_matches(root, ((FuncExpr *) qual)->funcid, ((FuncExpr *) qual)->args, fcall);
	/* an operator (the ADQL <@ and @>): the support function sees its function */
	if (IsA(qual, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) qual;

		return call_matches(root, OidIsValid(op->opfuncid) ? op->opfuncid : get_opcode(op->opno),
							op->args, fcall);
	}
	return false;
}

/* ...in any WHERE or JOIN ON of the join tree (pulled-up subqueries included) */
static bool
jointree_has_term(PlannerInfo *root, Node *jt, FuncExpr *fcall)
{
	ListCell   *lc;

	if (jt == NULL)
		return false;
	if (IsA(jt, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jt;

		if (qual_has_term(root, f->quals, fcall))
			return true;
		foreach(lc, f->fromlist)
			if (jointree_has_term(root, lfirst(lc), fcall))
				return true;
		return false;
	}
	if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;

		return qual_has_term(root, j->quals, fcall) ||
			jointree_has_term(root, j->larg, fcall) ||
			jointree_has_term(root, j->rarg, fcall);
	}
	return false;
}

/*
 * Called from skycell_support's SupportRequestSimplify: true to leave this
 * skycell_cone call unrewritten for the custom path.  Only a cone whose
 * parameters are constants, whose cell/ra/dec come from one base relation, and
 * which is a top-level AND term of a WHERE or JOIN ON: only there can it become
 * a restriction a scan answers.  A cross-match cone (centre or radius from
 * another relation, cone_kind()) is kept as well, for the parameterized path
 * of cone_set_rel_pathlist().  Under an OR, a NOT, a CASE or in a target list
 * it is rewritten as before, which a BitmapOr can still serve.  Whether an
 * index can serve it is only known once the planner has the relation's index
 * list, and a relation without one gets a sequential scan either way.
 */
/* ------------------------------------------------------------------ */
/* partition pruning by a cone's cells (0.44)                          */
/* ------------------------------------------------------------------ */

static void cone_cell_bounds_uncached(double ra0, double dec0, double radius,
									  int64 *lo, int64 *hi);

/*
 * skycell_cone_cell_lo/hi(ra0, dec0, radius): the lowest and highest order-29
 * cell the cone can hold (sc_cone_key_bounds()).  The planner compares a
 * catalogue partitioned by cell range with them, next to a kept cone, so that
 * PostgreSQL prunes the partitions a cone cannot reach: at plan time for a
 * constant cone, at every rescan for a cross-match's.  An invalid centre gives
 * the whole sky, leaving the error to the cone.
 */
static void
cone_cell_bounds(double ra0, double dec0, double radius, int64 *lo, int64 *hi)
{
	/* lo and hi are asked for in turn with the same cone: one seed for both */
	static struct
	{
		bool		valid;
		double		ra0,
					dec0,
					radius;
		int64		lo,
					hi;
	}			last;

	if (last.valid && last.ra0 == ra0 && last.dec0 == dec0 && last.radius == radius)
	{
		*lo = last.lo;
		*hi = last.hi;
		return;
	}
	cone_cell_bounds_uncached(ra0, dec0, radius, lo, hi);
	last.valid = true;
	last.ra0 = ra0;
	last.dec0 = dec0;
	last.radius = radius;
	last.lo = *lo;
	last.hi = *hi;
}

static void
cone_cell_bounds_uncached(double ra0, double dec0, double radius, int64 *lo, int64 *hi)
{
	sc_region	reg;

	if (isnan(radius) || sc_region_cone(&reg, ra0, dec0, radius, false) != NULL)
	{
		*lo = 0;
		*hi = 12 * ((int64) 1 << (2 * SC_MAX_ORDER)) - 1;
		return;
	}
	sc_cone_key_bounds(&reg, lo, hi);
}

PG_FUNCTION_INFO_V1(skycell_cone_cell_lo);
Datum
skycell_cone_cell_lo(PG_FUNCTION_ARGS)
{
	int64		lo,
				hi;

	cone_cell_bounds(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2), &lo, &hi);
	PG_RETURN_INT64(lo);
}

PG_FUNCTION_INFO_V1(skycell_cone_cell_hi);
Datum
skycell_cone_cell_hi(PG_FUNCTION_ARGS)
{
	int64		lo,
				hi;

	cone_cell_bounds(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2), &lo, &hi);
	PG_RETURN_INT64(hi);
}

static Oid
cone_bound_fn(Oid sibling, const char *name)
{
	Oid			t[3] = {FLOAT8OID, FLOAT8OID, FLOAT8OID};
	char	   *nsp = get_namespace_name(get_func_namespace(sibling));

	return LookupFuncName(list_make2(makeString(nsp), makeString(pstrdup(name))), 3, t, true);
}

/*
 * A kept cone (skycell_cone(cell, ra, dec, ra0, dec0, radius)) with, when its
 * relation is range-partitioned on exactly its cell expression, the two
 * comparisons cell >= skycell_cone_cell_lo(ra0, dec0, radius) and cell <=
 * skycell_cone_cell_hi(...): an AND that PostgreSQL's partition pruning
 * reads, and the custom scan drops again (cone_implies_clause()), as the cone
 * implies both.  Otherwise the cone alone.  Installed SQL older than 0.40 has
 * no bound functions and gets the cone alone too.
 */
/*
 * Whether cell's relation is range-partitioned on exactly cell (an int8
 * expression or column): then the partition key's >= and <= operators, for
 * comparisons PostgreSQL's partition pruning reads.
 */
static bool
partition_key_ops(PlannerInfo *root, Node *cell, Oid *geop, Oid *leop)
{
	List	   *vars = sc_pull_local_vars(cell, 0);
	Index		varno;
	RangeTblEntry *rte;
	Relation	rel;
	PartitionKey key;
	bool		match = false;

	*geop = *leop = InvalidOid;
	if (root == NULL || root->parse == NULL || vars == NIL || !IsA(linitial(vars), Var))
		return false;
	varno = ((Var *) linitial(vars))->varno;
	if (varno < 1 || varno > (Index) list_length(root->parse->rtable))
		return false;
	rte = rt_fetch(varno, root->parse->rtable);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_PARTITIONED_TABLE)
		return false;

	rel = table_open(rte->relid, NoLock);	/* locked by the parser */
	key = RelationGetPartitionKey(rel);
	if (key != NULL && key->strategy == PARTITION_STRATEGY_RANGE && key->partnatts == 1 &&
		key->parttypid[0] == INT8OID)
	{
		if (key->partattrs[0] != 0)
			match = IsA(cell, Var) && ((Var *) cell)->varno == (int) varno &&
				((Var *) cell)->varattno == key->partattrs[0];
		else
		{
			Node	   *e = copyObject(linitial(key->partexprs));

			ChangeVarNodes(e, 1, varno, 0);
			match = equal(e, cell);
		}
		if (match)
		{
			*geop = get_opfamily_member(key->partopfamily[0], INT8OID, INT8OID,
										BTGreaterEqualStrategyNumber);
			*leop = get_opfamily_member(key->partopfamily[0], INT8OID, INT8OID,
										BTLessEqualStrategyNumber);
		}
	}
	table_close(rel, NoLock);
	return match && OidIsValid(*geop) && OidIsValid(*leop);
}

/*
 * Shard keys (0.45).  A sharded database such as Citus distributes a
 * catalogue by one column and skips the shards a query cannot reach only on
 * key = ... or key = ANY(...).  Distributed by a coarse HEALPix cell, kept
 * true by a constraint the planner can read,
 *
 *     hpx5 bigint NOT NULL CHECK (hpx5 = skycell_ang2cell(ra, dec) >> 48)
 *
 * (48 = 2 * (29 - 5): the order-5 cell), a constant cone or region on the
 * table gets hpx5 = ANY('{the order-5 cells it meets}'), so each query goes
 * to the shards holding those cells.  The cells come from the same
 * classification as skycell_cell_overlaps(), which holds every cell the
 * region meets, so no row of the region is skipped.  Only a validated
 * constraint counts: it is what makes every row's key its own cell's.
 */
#define SHARD_KEY_MAX_CELLS 1024

/* skycell_ang2cell(ra, dec) >> shift, ra and dec as the query has them */
static bool
shard_key_expr(Node *e, Node *ra, Node *dec, int *order)
{
	OpExpr	   *shr;
	FuncExpr   *f;
	Const	   *k;
	char	   *name;
	int			shift;

	if (!IsA(e, OpExpr) || list_length(((OpExpr *) e)->args) != 2)
		return false;
	shr = (OpExpr *) e;
	set_opfuncid(shr);
	if (shr->opfuncid != F_INT8SHR)
		return false;
	f = (FuncExpr *) linitial(shr->args);
	k = (Const *) lsecond(shr->args);
	if (!IsA(f, FuncExpr) || list_length(f->args) != 2 || !IsA(k, Const) || k->constisnull)
		return false;
	name = get_func_name(f->funcid);
	if (name == NULL || strcmp(name, "skycell_ang2cell") != 0 ||
		!equal(linitial(f->args), ra) || !equal(lsecond(f->args), dec))
		return false;
	shift = DatumGetInt32(k->constvalue);
	if (shift < 0 || shift > 2 * SC_MAX_ORDER || shift % 2 != 0)
		return false;
	*order = SC_MAX_ORDER - shift / 2;
	return true;
}

/*
 * The shard key of the relation ra and dec read: its column and the order of
 * its cells, from a validated CHECK (key = skycell_ang2cell(ra, dec) >> s),
 * either way round.
 */
static bool
shard_key_for(PlannerInfo *root, Node *ra, Node *dec, Index *varno, AttrNumber *attnum,
			  int *order)
{
	List	   *vars = sc_pull_local_vars((Node *) list_make2(ra, dec), 0);
	RangeTblEntry *rte;
	Relation	rel;
	TupleConstr *constr;
	bool		found = false;

	if (root == NULL || root->parse == NULL || vars == NIL || !IsA(linitial(vars), Var))
		return false;
	*varno = ((Var *) linitial(vars))->varno;
	if (*varno < 1 || *varno > (Index) list_length(root->parse->rtable))
		return false;
	rte = rt_fetch(*varno, root->parse->rtable);
	if (rte->rtekind != RTE_RELATION ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_PARTITIONED_TABLE))
		return false;

	rel = table_open(rte->relid, NoLock);	/* locked by the parser */
	constr = RelationGetDescr(rel)->constr;
	for (int i = 0; constr != NULL && i < constr->num_check && !found; i++)
	{
		Node	   *e;
		OpExpr	   *eq;
		Node	   *a,
				   *b;

		if (!constr->check[i].ccvalid)
			continue;
		e = stringToNode(constr->check[i].ccbin);
		ChangeVarNodes(e, 1, *varno, 0);
		if (!IsA(e, OpExpr) || list_length(((OpExpr *) e)->args) != 2)
			continue;
		eq = (OpExpr *) e;
		set_opfuncid(eq);
		if (eq->opfuncid != F_INT8EQ)
			continue;
		a = linitial(eq->args);
		b = lsecond(eq->args);
		if (!IsA(a, Var))
		{
			Node	   *t = a;

			a = b;
			b = t;
		}
		if (IsA(a, Var) && ((Var *) a)->varno == (int) *varno &&
			((Var *) a)->vartype == INT8OID && shard_key_expr(b, ra, dec, order))
		{
			*attnum = ((Var *) a)->varattno;
			found = true;
		}
	}
	table_close(rel, NoLock);
	return found;
}

/*
 * The cells of order the region meets, as skycell_cell_overlaps() counts
 * them; -1 when there are more than max.
 */
static int
region_cells_at(const sc_region *reg, int order, Datum *out, int max)
{
	struct
	{
		int			o;
		int64		pix;
	}			stack[12 + 3 * (SC_MAX_ORDER + 1)];
	int			top = 0,
				n = 0;

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

		if (c == SC_OUT)
			continue;
		if (c == SC_PARTIAL && o < order)
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
		{
			int64		count = (int64) 1 << (2 * (order - o)),
						first = pix << (2 * (order - o));

			if (n + count > max)
				return -1;
			for (int64 i = 0; i < count; i++)
				out[n++] = Int64GetDatum(first + i);
		}
	}
	return n;
}

/* expr AND key = ANY(the cells of the key's order reg meets), when the relation has a key */
static Node *
with_shard_key(PlannerInfo *root, Node *expr, Node *ra, Node *dec, const sc_region *reg)
{
	Index		varno;
	AttrNumber	attnum;
	int			order,
				n;
	Datum	   *cells;
	ScalarArrayOpExpr *saop;
	Oid			eqop;

	if (!shard_key_for(root, ra, dec, &varno, &attnum, &order))
		return expr;
	eqop = get_opfamily_member(INTEGER_BTREE_FAM_OID, INT8OID, INT8OID, BTEqualStrategyNumber);
	if (!OidIsValid(eqop))
		return expr;
	cells = palloc(sizeof(Datum) * SHARD_KEY_MAX_CELLS);
	n = region_cells_at(reg, order, cells, SHARD_KEY_MAX_CELLS);
	if (n < 0)
		return expr;			/* most of the sky: most shards anyway */
	saop = makeNode(ScalarArrayOpExpr);
	saop->opno = eqop;
	saop->opfuncid = F_INT8EQ;
	saop->useOr = true;
	saop->inputcollid = InvalidOid;
	saop->args = list_make2(makeVar(varno, attnum, INT8OID, -1, InvalidOid, 0),
							makeConst(INT8ARRAYOID, -1, InvalidOid, -1,
									  PointerGetDatum(construct_array(cells, n, INT8OID, 8, true,
																	  TYPALIGN_DOUBLE)),
									  false, false));
	if (IsA(expr, BoolExpr) && ((BoolExpr *) expr)->boolop == AND_EXPR)
	{
		((BoolExpr *) expr)->args = lappend(((BoolExpr *) expr)->args, saop);
		return expr;
	}
	return (Node *) makeBoolExpr(AND_EXPR, list_make2(expr, saop), -1);
}

/* expr AND cell >= lo AND cell <= hi */
static Node *
with_cell_bounds(Node *expr, Node *cell, Oid geop, Oid leop, Expr *lo, Expr *hi)
{
	OpExpr	   *c1 = (OpExpr *) make_opclause(geop, BOOLOID, false, (Expr *) copyObject(cell), lo,
											  InvalidOid, InvalidOid);
	OpExpr	   *c2 = (OpExpr *) make_opclause(leop, BOOLOID, false, (Expr *) copyObject(cell), hi,
											  InvalidOid, InvalidOid);

	set_opfuncid(c1);
	set_opfuncid(c2);
	return (Node *) makeBoolExpr(AND_EXPR, list_make3(expr, c1, c2), -1);
}

/*
 * A kept cone (skycell_cone(cell, ra, dec, ra0, dec0, radius)) with, when its
 * relation is range-partitioned on exactly its cell expression, the two
 * comparisons cell >= skycell_cone_cell_lo(ra0, dec0, radius) and cell <=
 * skycell_cone_cell_hi(...): an AND that PostgreSQL's partition pruning
 * reads, and the custom scan drops again (cone_implies_clause()), as the cone
 * implies both.  Otherwise the cone alone.  Installed SQL older than 0.40 has
 * no bound functions and gets the cone alone too.
 */
static Node *cone_with_cell_bounds(PlannerInfo *root, FuncExpr *cone6);

Node *
cone_with_partition_bounds(PlannerInfo *root, FuncExpr *cone6)
{
	Node	   *res = cone_with_cell_bounds(root, cone6);
	List	   *args = cone6->args;
	Const	   *c[3] = {lfourth(args), list_nth(args, 4), list_nth(args, 5)};
	sc_region	reg;
	bool		consts = true;

	/* and, for a constant cone on a table with a shard key, the key's cells */
	for (int i = 0; i < 3; i++)
		if (!IsA(c[i], Const) || c[i]->constisnull)
			consts = false;
	if (consts && sc_region_cone(&reg, DatumGetFloat8(c[0]->constvalue),
								 DatumGetFloat8(c[1]->constvalue),
								 DatumGetFloat8(c[2]->constvalue), true) == NULL)
	{
		res = with_shard_key(root, res, lsecond(args), lthird(args), &reg);
		sc_region_free(&reg);
	}
	if (res != (Node *) cone6)
		skycell_mark_bounded(cone6);
	return res;
}

/* the argument that carries the mark: a node with a location of its own */
static int *
bounded_mark_slot(List *args)
{
	ListCell   *lc;

	foreach(lc, args)
	{
		Node	   *a = lfirst(lc);

		if (IsA(a, Const))
			return &((Const *) a)->location;
		if (IsA(a, Var))
			return &((Var *) a)->location;
		if (IsA(a, FuncExpr))
			return &((FuncExpr *) a)->location;
		if (IsA(a, Param))
			return &((Param *) a)->location;
	}
	return NULL;
}

void
skycell_mark_bounded(FuncExpr *cone6)
{
	int		   *slot;

	/* its own copy of the argument, so that no other expression is marked */
	cone6->args = copyObject(cone6->args);
	slot = bounded_mark_slot(cone6->args);
	if (slot != NULL)
		*slot = SKYCELL_BOUNDED_LOCATION;
}

bool
skycell_is_bounded(List *cone_args)
{
	int		   *slot = bounded_mark_slot(cone_args);

	return slot != NULL && *slot == SKYCELL_BOUNDED_LOCATION;
}

static Node *
cone_with_cell_bounds(PlannerInfo *root, FuncExpr *cone6)
{
	List	   *args = cone6->args;
	Node	   *cell = linitial(args);
	Oid			geop,
				leop,
				lofn,
				hifn;

	if (!partition_key_ops(root, cell, &geop, &leop))
		return (Node *) cone6;
	lofn = cone_bound_fn(cone6->funcid, "skycell_cone_cell_lo");
	hifn = cone_bound_fn(cone6->funcid, "skycell_cone_cell_hi");
	if (!OidIsValid(lofn) || !OidIsValid(hifn))
		return (Node *) cone6;
	return with_cell_bounds((Node *) cone6, cell, geop, leop,
							(Expr *) makeFuncExpr(lofn, INT8OID, list_copy_tail(copyObject(args), 3),
												  InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL),
							(Expr *) makeFuncExpr(hifn, INT8OID, list_copy_tail(copyObject(args), 3),
												  InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL));
}

/*
 * The interval of cells a constant region (a polygon or a composite) can
 * hold: its bounding cap's, as a cone's (0.45).  The whole sky for a region
 * whose cap is (a NOT, say).  A MOC's are its own first and last cells.
 */
static void
region_cell_bounds(Datum region, int64 *lo, int64 *hi)
{
	sc_region	reg,
				cap;
	sc_vec3		c;
	double		rad;

	*lo = 0;
	*hi = 12 * ((int64) 1 << (2 * SC_MAX_ORDER)) - 1;
	skycell_region_from_datum_lite(region, &reg);
	if (SC_REGION_IS_MOC(&reg))
	{
		/* a MOC's own first and last cells (an empty one holds none) */
		*lo = reg.nmoc > 0 ? reg.mlo[0] : 1;
		*hi = reg.nmoc > 0 ? reg.mhi[reg.nmoc - 1] : 0;
		sc_region_free(&reg);
		return;
	}
	sc_region_bounding_cap(&reg, &c, &rad);
	sc_region_free(&reg);
	if (!(rad < M_PI) || isnan(rad))
		return;
	if (sc_region_cone(&cap, atan2(c.y, c.x) * (180.0 / M_PI),
					   asin(fmax(-1.0, fmin(1.0, c.z))) * (180.0 / M_PI),
					   rad * (180.0 / M_PI) * (1.0 + 1e-9) + 1e-12, false) != NULL)
		return;
	sc_cone_key_bounds(&cap, lo, hi);
}

/*
 * A kept skycell_region_scan(cell, ra, dec, region) with, on a catalogue
 * range-partitioned on its cell, cell >= lo and cell <= hi for the region's
 * interval: constants, so PostgreSQL prunes at plan time.  The scan drops
 * them again (region_implies_clause()).
 */
Node *
region_with_partition_bounds(PlannerInfo *root, FuncExpr *scan)
{
	Node	   *cell = linitial(scan->args);
	Const	   *rg = (Const *) lfourth(scan->args);
	Node	   *res = (Node *) scan;
	Oid			geop,
				leop;
	int64		lo,
				hi;
	sc_region	reg;

	if (!IsA(rg, Const) || rg->constisnull)
		return res;
	if (partition_key_ops(root, cell, &geop, &leop))
	{
		region_cell_bounds(rg->constvalue, &lo, &hi);
		if (!(lo == 0 && hi == 12 * ((int64) 1 << (2 * SC_MAX_ORDER)) - 1))
			res = with_cell_bounds(res, cell, geop, leop, (Expr *) int8_const(lo),
								   (Expr *) int8_const(hi));
	}
	/* and, on a table with a shard key, the key's cells */
	{
		Index		varno;
		AttrNumber	attnum;
		int			order;

		if (shard_key_for(root, lsecond(scan->args), lthird(scan->args), &varno, &attnum, &order))
		{
			skycell_region_from_datum(rg->constvalue, &reg);
			res = with_shard_key(root, res, lsecond(scan->args), lthird(scan->args), &reg);
			sc_region_free(&reg);
		}
	}
	return res;
}

/*
 * Whether a clause is the range slots the rewrite gives a per-row region
 * (adql.c): an AND / OR of comparisons of the cell with
 * skycell_region_bound(rg, ...), every one of them on this region.  *cell,
 * if asked, gets the cell expression they compare.
 */
static bool
region_slot_clause(Node *n, Node *rg, Node **cell)
{
	if (n == NULL)
		return false;
	if (IsA(n, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) n;
		ListCell   *lc;

		if (b->boolop == NOT_EXPR)
			return false;
		foreach(lc, b->args)
			if (!region_slot_clause(lfirst(lc), rg, cell))
				return false;
		return true;
	}
	if (IsA(n, OpExpr) && list_length(((OpExpr *) n)->args) == 2)
	{
		FuncExpr   *r = (FuncExpr *) lsecond(((OpExpr *) n)->args);
		char	   *name;

		if (!IsA(r, FuncExpr) || list_length(r->args) != 5 ||
			(name = get_func_name(r->funcid)) == NULL ||
			strcmp(name, "skycell_region_bound") != 0 || !equal(linitial(r->args), rg))
			return false;
		if (cell != NULL)
			*cell = linitial(((OpExpr *) n)->args);
		return true;
	}
	return false;
}

/* whether a scan clause is one of those comparisons for this region scan */
static bool
region_implies_clause(FuncExpr *driving, Node *clause)
{
	OpExpr	   *op;
	Const	   *rg = (Const *) lfourth(driving->args);
	Node	   *r;
	int64		lo,
				hi,
				v;

	/* a per-row region: its range slots, which its covering implies (0.54) */
	if (!IsA(rg, Const))
		return region_slot_clause(clause, (Node *) rg, NULL);
	if (!IsA(clause, OpExpr) || list_length(((OpExpr *) clause)->args) != 2 ||
		!IsA(rg, Const) || rg->constisnull)
		return false;
	op = (OpExpr *) clause;
	set_opfuncid(op);
	if (op->opfuncid != F_INT8GE && op->opfuncid != F_INT8LE)
		return false;
	r = lsecond(op->args);
	if (!IsA(r, Const) || ((Const *) r)->constisnull ||
		!equal(linitial(op->args), linitial(driving->args)))
		return false;
	region_cell_bounds(rg->constvalue, &lo, &hi);
	v = DatumGetInt64(((Const *) r)->constvalue);
	return op->opfuncid == F_INT8GE ? v <= lo : v >= hi;
}

/*
 * Whether a scan clause is one of the comparisons above for this cone, which
 * the cone implies: cell >= / <= its bound function, or that function folded
 * to a constant (a partition's copy of a constant cone's quals is simplified
 * again).  Whoever wrote it, the scan can leave it out.
 */
static bool
cone_implies_clause(FuncExpr *driving, Node *clause)
{
	OpExpr	   *op;
	Node	   *r;
	bool		ge;
	List	   *a = list_copy_tail(driving->args, 3);
	int64		lo,
				hi,
				v;

	if (!IsA(clause, OpExpr) || list_length(((OpExpr *) clause)->args) != 2)
		return false;
	op = (OpExpr *) clause;
	set_opfuncid(op);
	if (op->opfuncid != F_INT8GE && op->opfuncid != F_INT8LE)
		return false;
	ge = op->opfuncid == F_INT8GE;
	if (!equal(linitial(op->args), linitial(driving->args)))
		return false;
	r = lsecond(op->args);
	if (IsA(r, FuncExpr))
	{
		char	   *name = get_func_name(((FuncExpr *) r)->funcid);

		return name != NULL &&
			strcmp(name, ge ? "skycell_cone_cell_lo" : "skycell_cone_cell_hi") == 0 &&
			equal(((FuncExpr *) r)->args, a);
	}
	if (!IsA(r, Const) || ((Const *) r)->constisnull ||
		!IsA(linitial(a), Const) || !IsA(lsecond(a), Const) || !IsA(lthird(a), Const) ||
		((Const *) linitial(a))->constisnull || ((Const *) lsecond(a))->constisnull ||
		((Const *) lthird(a))->constisnull)
		return false;
	cone_cell_bounds(DatumGetFloat8(((Const *) linitial(a))->constvalue),
					 DatumGetFloat8(((Const *) lsecond(a))->constvalue),
					 DatumGetFloat8(((Const *) lthird(a))->constvalue), &lo, &hi);
	v = DatumGetInt64(((Const *) r)->constvalue);
	return ge ? v <= lo : v >= hi;
}

/*
 * Whether varno is a partition (or inheritance child) being planned for its
 * parent: PostgreSQL simplifies a partition's copy of the parent's quals
 * again (apply_child_basequals()), and a call the parent kept for the scan
 * was a top-level term there, though the partition, absent from the join
 * tree, cannot show it.  The partition keeps it too (0.44), and gets the scan.
 */
static bool
is_child_rel(PlannerInfo *root, Index varno)
{
	return root->append_rel_array != NULL && varno < (Index) root->simple_rel_array_size &&
		root->append_rel_array[varno] != NULL;
}

static bool
keep_cone(PlannerInfo *root, FuncExpr *written, FuncExpr *fcall)
{
	List	   *args = fcall->args;
	List	   *vars;
	ListCell   *lc;
	Index		varno = 0;

	if (!skycell_custom_scan || root == NULL || root->parse == NULL ||
		list_length(args) != 6)
		return false;
	vars = sc_pull_local_vars((Node *) list_make3(linitial(args), lsecond(args), lthird(args)),
						   PVC_INCLUDE_AGGREGATES | PVC_INCLUDE_WINDOWFUNCS | PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varlevelsup != 0 || (varno != 0 && v->varno != varno))
			return false;
		varno = v->varno;
	}
	if (varno == 0 || varno > list_length(root->parse->rtable) ||
		rt_fetch(varno, root->parse->rtable)->rtekind != RTE_RELATION)
		return false;
	if (cone_kind(args, varno) == CONE_NONE)
		return false;
	/* the scan reads heap tuples; any other table AM keeps the rewrite */
	{
		RangeTblEntry *rte = rt_fetch(varno, root->parse->rtable);

		if (rte->relkind == RELKIND_RELATION || rte->relkind == RELKIND_MATVIEW)
		{
			Relation	r = table_open(rte->relid, NoLock);	/* locked by the parser */
			bool		heap = table_slot_callbacks(r) == &TTSOpsBufferHeapTuple;

			table_close(r, NoLock);
			if (!heap)
				return false;
		}
		else if (rte->relkind != RELKIND_PARTITIONED_TABLE)
			return false;
	}
	if (!is_child_rel(root, varno) &&
		!jointree_has_term(root, (Node *) root->parse->jointree, written))
		return false;
	cone_funcid = fcall->funcid;
	return true;
}

/*
 * The same decision for a constant composite region, which the simplify step
 * (adql.c) turns into skycell_region_scan(cell, ra, dec, region) when the scan
 * can answer it: cell, ra and dec read one heap relation, the region is a
 * constant, and the call is a top-level AND term.  Otherwise the composite
 * keeps the range rewrite.
 */
bool
region_scan_keep(PlannerInfo *root, FuncExpr *written, FuncExpr *fcall)
{
	List	   *args = fcall->args;
	List	   *vars;
	ListCell   *lc;
	Index		varno = 0;
	RangeTblEntry *rte;

	if (!skycell_custom_scan || root == NULL || root->parse == NULL ||
		list_length(args) != 4 || !IsA(llast(args), Const) ||
		((Const *) llast(args))->constisnull)
		return false;
	vars = sc_pull_local_vars((Node *) list_make3(linitial(args), lsecond(args), lthird(args)),
							  PVC_INCLUDE_AGGREGATES | PVC_INCLUDE_WINDOWFUNCS | PVC_INCLUDE_PLACEHOLDERS);
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
	if (rte->rtekind != RTE_RELATION ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW &&
		 rte->relkind != RELKIND_PARTITIONED_TABLE))
		return false;
	if (rte->relkind != RELKIND_PARTITIONED_TABLE)
	{
		Relation	r = table_open(rte->relid, NoLock);	/* locked by the parser */
		bool		heap = table_slot_callbacks(r) == &TTSOpsBufferHeapTuple;

		table_close(r, NoLock);
		if (!heap)
			return false;
	}
	if (!is_child_rel(root, varno) &&
		!jointree_has_term(root, (Node *) root->parse->jointree, written))
		return false;
	region_scan_funcid = fcall->funcid;
	return true;
}

bool
cone_scan_keep(PlannerInfo *root, FuncExpr *fcall)
{
	return keep_cone(root, fcall, fcall);
}

/*
 * The same for a Q3C-shaped spelling (skycell_join, skycell_radial_query):
 * cone6 is the six-argument skycell_cone it stands for, with the cell
 * expression synthesised; written is the call as the query wrote it, which is
 * what has to be a top-level AND term.
 */
bool
cone_scan_keep_as(PlannerInfo *root, FuncExpr *written, FuncExpr *cone6)
{
	return keep_cone(root, written, cone6);
}

/* a B-tree on the cell expression, comparing int8; the smallest if several */
static IndexOptInfo *
cell_index(RelOptInfo *rel, Node *cell)
{
	IndexOptInfo *best = NULL;
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ix = (IndexOptInfo *) lfirst(lc);

		if (ix->relam != BTREE_AM_OID || ix->ncolumns < 1 || ix->hypothetical ||
			ix->opcintype[0] != INT8OID || (ix->indpred != NIL && !ix->predOK))
			continue;
		if (ix->indexkeys[0] != 0)
		{
			if (!IsA(cell, Var) || ((Var *) cell)->varno != rel->relid ||
				((Var *) cell)->varattno != ix->indexkeys[0])
				continue;
		}
		else if (ix->indexprs == NIL || !equal(linitial(ix->indexprs), cell))
			continue;
		if (!OidIsValid(get_opfamily_member(ix->opfamily[0], INT8OID, INT8OID,
											BTGreaterEqualStrategyNumber)) ||
			!OidIsValid(get_opfamily_member(ix->opfamily[0], INT8OID, INT8OID,
											BTLessEqualStrategyNumber)))
			continue;
		if (best == NULL || ix->pages < best->pages)
			best = ix;
	}
	return best;
}

/*
 * How well the heap follows the cell order: the correlation ANALYZE keeps for
 * the cell column (or, for an expression, for the index's own column), as
 * btcostestimate reads it.  0 -- the pessimistic answer -- when there are no
 * statistics.
 */
static double
cell_correlation(RangeTblEntry *rte, IndexOptInfo *ix, Node *cell)
{
	Oid			statrel = ix->indexoid;
	AttrNumber	attnum = 1;
	HeapTuple	tp;
	double		corr = 0.0;

	if (ix->indexkeys[0] != 0)
	{
		statrel = rte->relid;
		attnum = ((Var *) cell)->varattno;
	}
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(statrel),
						 Int16GetDatum(attnum), BoolGetDatum(false));
	if (HeapTupleIsValid(tp))
	{
		AttStatsSlot sslot;

		if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers == 1)
				corr = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
		ReleaseSysCache(tp);
	}
	return corr;
}

/*
 * What a cross-match cone costs per outer row, estimated before any outer row
 * is known: coverings computed at sample centres and averaged.  The centres
 * are taken evenly through the cell column's histogram, so they fall where
 * the rows are, as the targets of a cross-match usually do (uniform centres
 * when there is no histogram).  The radius is the planner's estimate of the
 * radius argument, or 1 arcsec when it has none.
 */
#define JOIN_SAMPLES 16

typedef struct
{
	double		nranges;		/* ranges per covering */
	double		cand;			/* rows the ranges hold */
	double		matches;		/* rows inside the cone */
	double		steps;			/* cell classifications per covering */
	double		ntotal;			/* rows in the relation */
	Oid			statrel;		/* where its density comes from */
	bool		sampled;		/* measured on sampled probes (join_cone_sample) */
} join_estimate;

/*
 * A column, or a plain cast of one, of a numeric type: safe to evaluate at
 * plan time (no cast here can fail on a value the column holds).
 */
static bool
plain_column(Node *n)
{
	Oid			t;

	if (IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	else if (IsA(n, FuncExpr) && ((FuncExpr *) n)->funcformat == COERCE_IMPLICIT_CAST &&
			 list_length(((FuncExpr *) n)->args) == 1)
		n = linitial(((FuncExpr *) n)->args);
	if (!IsA(n, Var) || ((Var *) n)->varlevelsup != 0 || ((Var *) n)->varattno <= 0)
		return false;
	t = ((Var *) n)->vartype;
	return t == FLOAT8OID || t == FLOAT4OID || t == NUMERICOID ||
		t == INT2OID || t == INT4OID || t == INT8OID;
}

/*
 * A centre coordinate the sample can read: a plain column, or one inside the
 * CASE that guards the centre of a cone added for distance() (guard_centre()
 * in adql.c), whose first branch is the column itself. The sample evaluates
 * the expression as written, so a row the guard turns to NULL is skipped.
 */
static bool
centre_column(Node *n)
{
	if (IsA(n, CaseExpr) && ((CaseExpr *) n)->arg == NULL &&
		list_length(((CaseExpr *) n)->args) >= 1)
		n = (Node *) ((CaseWhen *) linitial(((CaseExpr *) n)->args))->result;
	return plain_column(n);
}

static bool
sampleable_rel(PlannerInfo *root, int varno, RangeTblEntry **rte)
{
	*rte = planner_rt_fetch(varno, root);
	return (*rte)->rtekind == RTE_RELATION && !(*rte)->inh &&
		((*rte)->relkind == RELKIND_RELATION || (*rte)->relkind == RELKIND_MATVIEW);
}

/*
 * What a cross-match's cones hold, measured: the density model cannot know
 * that the targets of a cross-match usually sit on catalogue sources (a
 * probe drawn from another survey has its counterpart in the cone), so it
 * underestimates the matches by orders of magnitude for small radii.  When
 * the centre (and radius, unless constant) are plain columns of one outer
 * table and the cones are small, read PROBE_SAMPLES of its rows spread over
 * its pages, cover each probe as the scan will, and count the inner rows
 * the ranges hold and the cone keeps -- as get_actual_variable_range() reads
 * an index to estimate a range.  Anything else keeps the density estimate.
 *
 * The sample reads at most PROBE_MAX_CAND inner rows.  One the budget cuts
 * short has still measured cones holding more than the model said -- the
 * case of probes sitting on clusters, which the model, spreading each cell's
 * rows evenly over it, misses most -- so it raises the estimate to what it
 * read, and never lowers it.  The budget bounds what planning reads, cold on
 * a heap out of cell order a random page a row: it is not raised to sample
 * larger cones.
 */
#define PROBE_SAMPLES	32
#define PROBE_SLOTS		(2 * PROBE_SAMPLES)	/* rows a block share rounds up to */
#define PROBE_MAX_CAND	4000	/* inner rows read in all */

typedef struct
{
	double		ra0,
				dec0,
				radius;
	bool		null;
} probe_centre;

static bool
join_cone_sample(PlannerInfo *root, List *args, const sc_density *dens, join_estimate *e)
{
	Node	   *rarg = (Node *) list_nth(args, 5);
	bool		rconst = IsA(rarg, Const);
	Relids		ov,
				iv;
	int			ovarno,
				ivarno;
	RangeTblEntry *orte,
			   *irte;
	RelOptInfo *irelinfo;
	IndexOptInfo *ix;
	Relation	orel,
				irel,
				ind;
	BlockNumber nblocks;
	MemoryContext cxt,
				old;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *x_ra0,
			   *x_dec0,
			   *x_r = NULL,
			   *x_ra,
			   *x_dec;
	TupleTableSlot *oslot,
			   *islot;
	TableScanDesc tscan = NULL;
	IndexScanDesc iscan;
	ScanKeyData keys[2];
	Oid			opfamily;
	sc_cover_params p;
	probe_centre probes[PROBE_SLOTS];
	int			nprobes = 0;
	double		sum_n = 0,
				sum_r = 0,
				sum_c = 0,
				sum_m = 0;
	int			nblk,
				per_blk;
	bool		overrun = false;

	/* centre and radius: plain columns of one table; cell, ra, dec: of another */
	if (!centre_column(list_nth(args, 3)) || !centre_column(list_nth(args, 4)) ||
		!(rconst ? !((Const *) rarg)->constisnull : plain_column(rarg)) ||
		!plain_column(lsecond(args)) || !plain_column(lthird(args)))
		return false;
	ov = pull_varnos(root, (Node *) list_make3(list_nth(args, 3), list_nth(args, 4), rarg));
	iv = pull_varnos(root, (Node *) list_make3(linitial(args), lsecond(args), lthird(args)));
	if (!bms_get_singleton_member(ov, &ovarno) || !bms_get_singleton_member(iv, &ivarno) ||
		ovarno == ivarno || !sampleable_rel(root, ovarno, &orte) || !sampleable_rel(root, ivarno, &irte))
		return false;
	irelinfo = find_base_rel(root, ivarno);
	if ((ix = cell_index(irelinfo, linitial(args))) == NULL || !ActiveSnapshotSet())
		return false;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "skycell probe sample", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	orel = table_open(orte->relid, NoLock);	/* locked by the parser */
	irel = table_open(irte->relid, NoLock);
	nblocks = RelationGetNumberOfBlocks(orel);
	if (nblocks == 0 || orel->rd_tableam->scan_set_tidrange == NULL ||
		table_slot_callbacks(irel) != &TTSOpsBufferHeapTuple)
	{
		table_close(irel, NoLock);
		table_close(orel, NoLock);
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
		return false;
	}
	ind = index_open(ix->indexoid, AccessShareLock);
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	x_ra0 = ExecPrepareExpr((Expr *) copyObject(list_nth(args, 3)), estate);
	x_dec0 = ExecPrepareExpr((Expr *) copyObject(list_nth(args, 4)), estate);
	if (!rconst)
		x_r = ExecPrepareExpr((Expr *) copyObject(rarg), estate);
	x_ra = ExecPrepareExpr((Expr *) copyObject(lsecond(args)), estate);
	x_dec = ExecPrepareExpr((Expr *) copyObject(lthird(args)), estate);
	oslot = table_slot_create(orel, NULL);
	islot = table_slot_create(irel, NULL);

	opfamily = ind->rd_opfamily[0];
	ScanKeyInit(&keys[0], 1, BTGreaterEqualStrategyNumber,
				get_opcode(get_opfamily_member(opfamily, INT8OID, INT8OID,
											   BTGreaterEqualStrategyNumber)), Int64GetDatum(0));
	ScanKeyInit(&keys[1], 1, BTLessEqualStrategyNumber,
				get_opcode(get_opfamily_member(opfamily, INT8OID, INT8OID,
											   BTLessEqualStrategyNumber)), Int64GetDatum(0));
#if PG_VERSION_NUM >= 180000
	iscan = index_beginscan(irel, ind, GetActiveSnapshot(), NULL, 2, 0);
#else
	iscan = index_beginscan(irel, ind, GetActiveSnapshot(), 2, 0);
#endif
	skycell_scan_params(&p, dens);

	/* the probes first: cheap, a few outer pages */
	nblk = (int) Min((BlockNumber) PROBE_SAMPLES, nblocks);
	per_blk = (PROBE_SAMPLES + nblk - 1) / nblk;
	for (int k = 0; k < nblk; k++)
	{
		BlockNumber b = (BlockNumber) (((uint64) k * nblocks) / nblk);
		ItemPointerData lo,
					hi;
		int			got = 0;

		ItemPointerSet(&lo, b, FirstOffsetNumber);
		ItemPointerSet(&hi, b, MaxOffsetNumber);
		if (tscan == NULL)
			tscan = table_beginscan_tidrange(orel, GetActiveSnapshot(), &lo, &hi);
		else
			table_rescan_tidrange(tscan, &lo, &hi);
		while (got < per_blk &&
			   table_scan_getnextslot_tidrange(tscan, ForwardScanDirection, oslot))
		{
			bool		n1,
						n2,
						n3 = false;
			probe_centre *pc = &probes[nprobes++];

			got++;
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = oslot;
			pc->ra0 = DatumGetFloat8(ExecEvalExprSwitchContext(x_ra0, econtext, &n1));
			pc->dec0 = DatumGetFloat8(ExecEvalExprSwitchContext(x_dec0, econtext, &n2));
			pc->radius = rconst ? DatumGetFloat8(((Const *) rarg)->constvalue)
				: DatumGetFloat8(ExecEvalExprSwitchContext(x_r, econtext, &n3));
			pc->null = n1 || n2 || n3;
		}
	}

	/*
	 * Then their cones, in bit-reversed order, so that a sample the budget
	 * cuts short is still spread over the outer table, not its first pages.
	 */
	for (int k = 0; k < PROBE_SLOTS && !overrun; k++)
	{
		int			j = 0;
		const probe_centre *pc;
		double		thr,
					cosdec0;
		sc_region	reg;
		sc_cover	cov;

		for (int bit = 1, rk = k; bit < PROBE_SLOTS; bit <<= 1, rk >>= 1)
			j = (j << 1) | (rk & 1);
		if (j >= nprobes)
			continue;
		pc = &probes[j];
		sum_n += 1;				/* a NULL probe is a probe that matches nothing */
		if (pc->null || sc_region_cone(&reg, pc->ra0, pc->dec0, pc->radius, true) != NULL)
			continue;
		sc_cover_compute(&reg, dens, &p, &cov);
		thr = pc->radius < 0 ? -1.0
			: (pc->radius >= 180.0 ? 2.0 : pow(sin(pc->radius * M_PI / 360.0), 2));
		cosdec0 = cos(pc->dec0 * M_PI / 180.0);
		sum_r += cov.n;
		for (int i = 0; i < cov.n && !overrun; i++)
		{
			keys[0].sk_argument = Int64GetDatum(cov.r[i].lo);
			keys[1].sk_argument = Int64GetDatum(cov.r[i].hi);
			index_rescan(iscan, keys, 2, NULL, 0);
			while (index_getnext_slot(iscan, ForwardScanDirection, islot))
			{
				bool		m1,
							m2;
				double		ra,
							dec,
							sdd,
							sda;

				econtext->ecxt_scantuple = islot;
				ra = DatumGetFloat8(ExecEvalExprSwitchContext(x_ra, econtext, &m1));
				dec = DatumGetFloat8(ExecEvalExprSwitchContext(x_dec, econtext, &m2));
				if (++sum_c > PROBE_MAX_CAND)
				{
					overrun = true;
					break;
				}
				if (m1 || m2)
					continue;
				/* skycell_in_cone's haversine */
				sdd = sin((dec - pc->dec0) * M_PI / 360.0);
				sda = sin((ra - pc->ra0) * M_PI / 360.0);
				if (sdd * sdd + cos(dec * M_PI / 180.0) * cosdec0 * sda * sda <= thr)
					sum_m += 1;
			}
		}
		sc_cover_free(&cov);
	}

	index_endscan(iscan);
	if (tscan)
		table_endscan(tscan);
	ExecDropSingleTupleTableSlot(oslot);
	ExecDropSingleTupleTableSlot(islot);
	FreeExecutorState(estate);
	index_close(ind, NoLock);
	table_close(irel, NoLock);
	table_close(orel, NoLock);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);

	if (sum_n == 0)
		return false;
	if (overrun)
	{
		/*
		 * The cones held more than the model expected: what was read --
		 * whole probes and the one the budget stopped -- is a lower bound
		 * on the probes it reached, so it may only raise the estimate.
		 */
		e->cand = fmax(e->cand, sum_c / sum_n);
		e->matches = fmax(e->matches, sum_m / sum_n);
		return true;
	}
	e->nranges = sum_r / sum_n;
	e->cand = sum_c / sum_n;
	e->matches = sum_m / sum_n;
	e->sampled = true;
	return true;
}

/*
 * join_cone_estimate() is asked for the same clause by the selectivity
 * request, the parameterized path's row count and its cost: keep the last
 * few answers for the query being planned (planner_hook bumps the
 * generation, so nothing carries over to the next query).
 */
#define EST_CACHE 8
static uint64 plan_generation = 0;
static struct
{
	uint64		gen;
	List	   *args;
	join_estimate e;
}			est_cache[EST_CACHE];
static int	est_next = 0;
static planner_hook_type prev_planner_hook = NULL;

static get_relation_info_hook_type prev_get_relation_info_hook = NULL;
static PlannerInfo *distances_root = NULL;
static uint64 distances_gen = 0;

/*
 * A query level's first relation is looked up after its quals are simplified
 * and before they are distributed: the moment to make ADQL's integer
 * predicates and distance comparisons indexable
 * (skycell_index_adql_predicates() in adql.c).
 */
static void
cone_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_get_relation_info_hook)
		prev_get_relation_info_hook(root, relid, inhparent, rel);
	if (root != distances_root || plan_generation != distances_gen)
	{
		distances_root = root;
		distances_gen = plan_generation;
		skycell_index_adql_predicates(root);
	}
}

static PlannedStmt *
cone_planner(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams)
{
	plan_generation++;
	skycell_fold_query_regions(parse);
	if (prev_planner_hook)
		return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
	return standard_planner(parse, query_string, cursorOptions, boundParams);
}

static bool join_cone_estimate_uncached(PlannerInfo *root, List *args, join_estimate *e);

static bool
join_cone_estimate(PlannerInfo *root, List *args, join_estimate *e)
{
	bool		ok;

	for (int i = 0; i < EST_CACHE; i++)
		if (est_cache[i].gen == plan_generation && plan_generation != 0 && est_cache[i].args == args)
		{
			*e = est_cache[i].e;
			return true;
		}
	ok = join_cone_estimate_uncached(root, args, e);
	if (ok && plan_generation != 0)
	{
		est_cache[est_next].gen = plan_generation;
		est_cache[est_next].args = args;
		est_cache[est_next].e = *e;
		est_next = (est_next + 1) % EST_CACHE;
	}
	return ok;
}

static bool
join_cone_estimate_uncached(PlannerInfo *root, List *args, join_estimate *e)
{
	Node	   *cell = linitial(args);
	Node	   *rarg = estimate_expression_value(root, (Node *) list_nth(args, 5));
	double		radius = 1.0 / 3600.0;
	sc_density	dens;
	sc_cover_params p;
	bool		uses_cell_ops;
	int			n = 0;

	memset(e, 0, sizeof(*e));
	if (IsA(rarg, Const) && !((Const *) rarg)->constisnull)
		radius = DatumGetFloat8(((Const *) rarg)->constvalue);
	density_for_var(root, cell, &dens, &e->statrel, &uses_cell_ops);
	if (!(dens.ntotal > 0))
		return false;
	e->ntotal = dens.ntotal;
	skycell_scan_params(&p, &dens);
	for (int k = 0; k < JOIN_SAMPLES; k++)
	{
		double		ra,
					dec;
		sc_region	reg;
		sc_cover	cov;

		if (dens.nbounds >= 2)
		{
			sc_vec3		v = sc_pix2vec(29, dens.bounds[(int) ((k + 0.5) * dens.nbounds / JOIN_SAMPLES)]);

			ra = atan2(v.y, v.x) * 180.0 / M_PI;
			dec = asin(fmax(-1.0, fmin(1.0, v.z))) * 180.0 / M_PI;
		}
		else
		{
			/* a Fibonacci sphere */
			ra = fmod(k * 137.50776405003785, 360.0);
			dec = asin(1.0 - 2.0 * (k + 0.5) / JOIN_SAMPLES) * 180.0 / M_PI;
		}
		if (ra < 0)
			ra += 360.0;
		if (sc_region_cone(&reg, ra, dec, radius, true) != NULL)
			continue;
		sc_cover_compute(&reg, &dens, &p, &cov);
		e->nranges += cov.n;
		e->cand += cov.exp_rows;
		e->matches += (cov.area > 0) ? cov.exp_rows * fmin(1.0, reg.area / cov.area) : 0;
		e->steps += cov.steps;
		sc_cover_free(&cov);
		n++;
	}
	if (n == 0)
		return false;
	e->nranges /= n;
	e->cand /= n;
	e->matches /= n;
	e->steps /= n;
	/* small cones: measure what sampled probes' cones actually hold */
	if (e->cand * PROBE_SAMPLES <= PROBE_MAX_CAND)
		(void) join_cone_sample(root, args, &dens, e);
	return true;
}

/*
 * What a per-row region costs per outer row (0.54): the regions of up to
 * REGION_SAMPLES outer rows, read spread over the outer table's pages,
 * covered against the relation's density as the scan will cover them, and
 * averaged.  The region must be an expression of one plain table's columns
 * (a skyregion column, or REGION() of an STC-S text one); anything else keeps
 * the range slots, whose rows the planner cannot estimate.
 */
#define REGION_SAMPLES 8

/*
 * Sampling and covering the regions costs a few milliseconds of planning on a
 * dense catalogue, so the estimate is kept for the backend: keyed by the
 * outer table, the region expression, the inner relation's density source,
 * and both tables' row counts, so that an ANALYZE or a load that changes
 * them takes a new sample.
 */
#define REGION_EST_CACHE 16

typedef struct
{
	Oid			orel;
	Oid			statrel;
	double		otuples,
				ntotal;
	char	   *key;
	join_estimate e;
} region_est_entry;

static region_est_entry region_est_cache[REGION_EST_CACHE];
static int	region_est_n = 0,
			region_est_next = 0;
static MemoryContext region_est_cxt = NULL;

static bool region_join_estimate_uncached(PlannerInfo *root, List *args, join_estimate *e);

static bool
region_join_estimate(PlannerInfo *root, List *args, join_estimate *e)
{
	Node	   *rg = lfourth(args);
	Relids		ov = pull_varnos(root, rg);
	int			ovarno;
	RangeTblEntry *orte;
	RelOptInfo *orelinfo;
	sc_density	dens;
	Oid			statrel;
	bool		uses_cell_ops;
	char	   *key;
	region_est_entry *c;

	if (!bms_get_singleton_member(ov, &ovarno) || !sampleable_rel(root, ovarno, &orte))
		return false;
	orelinfo = find_base_rel(root, ovarno);
	density_for_var(root, linitial(args), &dens, &statrel, &uses_cell_ops);
	key = nodeToString(rg);
	for (int i = 0; i < region_est_n; i++)
	{
		c = &region_est_cache[i];
		if (c->orel == orte->relid && c->statrel == statrel && c->ntotal == dens.ntotal &&
			c->otuples == orelinfo->tuples && strcmp(c->key, key) == 0)
		{
			*e = c->e;
			return true;
		}
	}
	if (!region_join_estimate_uncached(root, args, e))
		return false;
	if (region_est_cxt == NULL)
		region_est_cxt = AllocSetContextCreate(TopMemoryContext, "skycell region estimates",
											   ALLOCSET_SMALL_SIZES);
	c = &region_est_cache[region_est_next];
	if (region_est_next < region_est_n)
		pfree(c->key);
	c->orel = orte->relid;
	c->statrel = statrel;
	c->ntotal = dens.ntotal;
	c->otuples = orelinfo->tuples;
	c->key = MemoryContextStrdup(region_est_cxt, key);
	c->e = *e;
	region_est_next = (region_est_next + 1) % REGION_EST_CACHE;
	region_est_n = Max(region_est_n, region_est_next == 0 ? REGION_EST_CACHE : region_est_next);
	return true;
}

static bool
region_join_estimate_uncached(PlannerInfo *root, List *args, join_estimate *e)
{
	Node	   *cell = linitial(args),
			   *rg = lfourth(args);
	Relids		ov;
	int			ovarno;
	RangeTblEntry *orte;
	sc_density	dens;
	sc_cover_params p;
	bool		uses_cell_ops;
	Relation	orel;
	BlockNumber nblocks;
	MemoryContext cxt,
				old;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *x_rg;
	TupleTableSlot *oslot;
	TableScanDesc tscan = NULL;
	int			n = 0,
				nblk,
				per_blk;

	memset(e, 0, sizeof(*e));
	ov = pull_varnos(root, rg);
	if (!bms_get_singleton_member(ov, &ovarno) || !sampleable_rel(root, ovarno, &orte) ||
		contain_volatile_functions(rg) || contain_subplans(rg) || !ActiveSnapshotSet())
		return false;
	density_for_var(root, cell, &dens, &e->statrel, &uses_cell_ops);
	if (!(dens.ntotal > 0))
		return false;
	e->ntotal = dens.ntotal;
	skycell_scan_params(&p, &dens);

	cxt = AllocSetContextCreate(CurrentMemoryContext, "skycell region sample", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	orel = table_open(orte->relid, NoLock);	/* locked by the parser */
	nblocks = RelationGetNumberOfBlocks(orel);
	if (nblocks == 0 || orel->rd_tableam->scan_set_tidrange == NULL)
	{
		table_close(orel, NoLock);
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
		return false;
	}
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	x_rg = ExecPrepareExpr((Expr *) copyObject(rg), estate);
	oslot = table_slot_create(orel, NULL);
	nblk = (int) Min((BlockNumber) REGION_SAMPLES, nblocks);
	per_blk = (REGION_SAMPLES + nblk - 1) / nblk;
	for (int k = 0; k < nblk && n < REGION_SAMPLES; k++)
	{
		BlockNumber b = (BlockNumber) (((uint64) k * nblocks) / nblk);
		ItemPointerData lo,
					hi;
		int			got = 0;

		ItemPointerSet(&lo, b, FirstOffsetNumber);
		ItemPointerSet(&hi, b, MaxOffsetNumber);
		if (tscan == NULL)
			tscan = table_beginscan_tidrange(orel, GetActiveSnapshot(), &lo, &hi);
		else
			table_rescan_tidrange(tscan, &lo, &hi);
		while (got < per_blk && n < REGION_SAMPLES &&
			   table_scan_getnextslot_tidrange(tscan, ForwardScanDirection, oslot))
		{
			bool		isnull;
			Datum		d;
			sc_region	reg;
			sc_cover	cov;

			got++;
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = oslot;
			d = ExecEvalExprSwitchContext(x_rg, econtext, &isnull);
			n++;				/* a NULL region is a row that matches nothing */
			if (isnull)
				continue;
			skycell_region_from_datum(d, &reg);
			sc_cover_compute(&reg, &dens, &p, &cov);
			e->nranges += cov.n;
			e->cand += cov.exp_rows;
			e->matches += cov.area > 0 ? sc_region_expected_rows(&reg, &dens, &cov) : 0;
			e->steps += cov.steps;
			sc_cover_free(&cov);
			sc_region_free(&reg);
		}
	}
	if (tscan != NULL)
		table_endscan(tscan);
	ExecDropSingleTupleTableSlot(oslot);
	FreeExecutorState(estate);
	table_close(orel, NoLock);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	if (n == 0)
		return false;
	e->nranges /= n;
	e->cand /= n;
	e->matches /= n;
	e->steps /= n;
	e->sampled = true;
	return true;
}

/*
 * Selectivity of a cross-match or run-time cone, for skycell_support's
 * SupportRequestSelectivity: the rows one outer row's cone is expected to
 * hold over the rows in the relation, so that the join's estimated size is
 * the outer rows times the matches per row.  -1 when the cone is not one the
 * custom scan takes (skycell_support then declines).
 */
double
cone_join_selectivity(PlannerInfo *root, List *args)
{
	List	   *vars;
	Index		varno = 0;
	ListCell   *lc;
	join_estimate e;

	if (!skycell_custom_scan || list_length(args) != 6)
		return -1;
	vars = sc_pull_local_vars((Node *) list_make3(linitial(args), lsecond(args), lthird(args)),
						   PVC_INCLUDE_AGGREGATES | PVC_INCLUDE_WINDOWFUNCS | PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varlevelsup != 0 || (varno != 0 && v->varno != varno))
			return -1;
		varno = v->varno;
	}
	if (varno == 0)
		return -1;
	switch (cone_kind(args, varno))
	{
		case CONE_JOIN:
		case CONE_RUNTIME:
			break;
		default:
			return -1;
	}
	if (!join_cone_estimate(root, args, &e))
		return -1;
	return e.matches / e.ntotal;
}

/*
 * How many times a scan parameterized by these outer relations runs: the
 * smallest row estimate among them, as get_loop_count() (static in
 * indxpath.c) has it, without its semijoin adjustment.
 */
static double
cone_loop_count(PlannerInfo *root, Relids outer)
{
	double		result = 0.0;
	int			relid = -1;

	while ((relid = bms_next_member(outer, relid)) >= 0)
	{
		RelOptInfo *orel;

		if (relid >= root->simple_rel_array_size ||
			(orel = root->simple_rel_array[relid]) == NULL ||
			IS_DUMMY_REL(orel) || orel->rows <= 0.0)
			continue;
		if (result == 0.0 || orel->rows < result)
			result = orel->rows;
	}
	return result > 1.0 ? result : 1.0;
}

/*
 * Cost of walking the covering, in the planner's units, for both ways the
 * executor can do it, mirroring what PostgreSQL charges for the same work:
 * a descent per range (btcostestimate), the leaf pages and index tuples the
 * ranges hold, the heap, then the recheck on every candidate row and the
 * projection on the ones that pass.  Only the heap differs:
 *
 *   ordered  one index scan per range, heap fetched in cell order;
 *   bitmap   the ranges' TIDs collected first, each heap page then read once
 *            in physical order (cost_bitmap_heap_scan's page count).
 *
 * *bitmap reports which is cheaper; that is the mode the scan runs in.
 *
 * loops is how many times the scan runs: the outer rows of a cross-match
 * (cone_loop_count()), 1 otherwise.  As cost_index(), compute_bitmap_pages()
 * and genericcostestimate() have it, the pages the scans read together are
 * then estimated across all of them (index_pages_fetched(), which knows
 * effective_cache_size) and charged per scan: a page an earlier probe read is
 * not a random read again.  Without this every probe of a cross-match paid
 * for its pages cold, about ten times what the scan costs, and a parallel
 * sequential scan that runs ten times slower was planned instead.
 *
 * recheck is the per-row cost of the cross-match's join clause, which the
 * scan evaluates on every candidate, as cost_index() charges a path's join
 * clauses; 0 for a cone with constant or run-time bounds.
 */
static void
cone_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ix, double nr, double exp_rows,
		  double rows, double corr, double loops, Cost recheck,
		  Cost *startup, Cost *total, bool *bitmap)
{
	double		cand = fmax(exp_rows, rows);
	double		ituples = fmax(ix->tuples, 1.0);
	double		ipages = fmax((double) ix->pages, 1.0);
	double		T = fmax((double) rel->pages, 1.0);
	double		rpp = (rel->tuples > 0 && rel->pages > 0) ? rel->tuples / rel->pages : 100.0;
	double		descent = (ceil(log2(ituples)) + (Max(ix->tree_height, 0) + 1) * 50.0)
		* cpu_operator_cost;
	double		index_pages = nr + cand * ipages / ituples;
	Cost		common,
				index_io,
				ordered,
				bm;

	/* a descent per range, then the leaf pages the ranges hold */
	if (loops > 1.0)
		index_io = index_pages_fetched(index_pages * loops, ix->pages, ipages, root)
			* random_page_cost / loops;
	else
		index_io = index_pages * random_page_cost;
	common = nr * descent
		+ index_io
		+ cand * (cpu_index_tuple_cost + 2.0 * cpu_operator_cost)
		+ cand * (cpu_tuple_cost + rel->baserestrictcost.per_tuple + recheck)
		+ rows * rel->reltarget->cost.per_tuple;

	/*
	 * Both modes read the same few pages, in order, when the heap follows the
	 * cell order (io_clustered: one random page to enter each range, then
	 * sequential); they part when it does not -- an ordered scan then pays a
	 * page per row, a bitmap scan each distinct page once.  Each is
	 * interpolated from its uncorrelated cost towards io_clustered by
	 * correlation squared, as cost_index does; the bitmap also pays for being
	 * built, so on a clustered heap the ordered scan is the cheaper one.
	 */
	{
		double		min_pages = fmin(T, nr + cand / rpp);
		Cost		io_clustered,
					ordered_uncorr,
					bitmap_uncorr;
		double		pages;
		double		per_page;
		double		c2 = corr * corr;

		if (loops > 1.0)
		{
			/* the loops' pages together, charged per loop, at random cost */
			io_clustered = index_pages_fetched(min_pages * loops, rel->pages, ipages, root)
				* random_page_cost / loops;
			ordered_uncorr = index_pages_fetched(cand * loops, rel->pages, ipages, root)
				* random_page_cost / loops;
			pages = index_pages_fetched(cand * loops, rel->pages, ipages, root) / loops;
		}
		else
		{
			io_clustered = nr * random_page_cost + fmax(0.0, min_pages - nr) * seq_page_cost;
			ordered_uncorr = index_pages_fetched(cand, rel->pages, ipages, root) * random_page_cost;
			pages = fmin(T, (2.0 * T * cand) / (2.0 * T + cand));
		}
		per_page = (pages >= 2.0)
			? random_page_cost - (random_page_cost - seq_page_cost) * sqrt(pages / T)
			: random_page_cost;
		bitmap_uncorr = pages * per_page;

		ordered = ordered_uncorr + c2 * (io_clustered - ordered_uncorr);
		bm = bitmap_uncorr + c2 * (io_clustered - bitmap_uncorr)
			+ cand * 0.1 * cpu_operator_cost;
	}

	*bitmap = bm < ordered;
	*startup = rel->baserestrictcost.startup + rel->reltarget->cost.startup;
	*total = *startup + common + Min(ordered, bm);
}

/*
 * A path whose covering is computed at execution: per outer row for a
 * cross-match cone (ppi, the parameterization by the outer relations), per
 * rescan for a run-time one (ppi NULL).  Costed from coverings sampled at
 * plan time (join_cone_estimate()).
 */
static void
add_runtime_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte, FuncExpr *f,
				 ParamPathInfo *ppi)
{
	Node	   *cell = linitial(f->args);
	IndexOptInfo *ix;
	join_estimate e;
	CustomPath *cp;
	bool		bitmap;
	QualCost	recheck = {0, 0};
	double		rows = ppi ? ppi->ppi_rows : rel->rows;
	AttrNumber	attnum = IsA(cell, Var) ? ((Var *) cell)->varattno : 1;

	if ((ix = cell_index(rel, cell)) == NULL ||
		!(list_length(f->args) == 4 ? region_join_estimate(root, f->args, &e)
		  : join_cone_estimate(root, f->args, &e)))
		return;
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = ppi;
	cp->path.parallel_aware = false;

	/*
	 * Not parallel aware -- one scan does not split its cone among workers
	 * -- but parallel safe: under a Parallel Seq Scan of the probes, each
	 * worker covers and scans the cones of its own outer rows, as it would
	 * an inner index scan.  The executor only reads: the index, the heap,
	 * and the density from the catalogue and the density map.
	 */
	cp->path.parallel_safe = rel->consider_parallel &&
		is_parallel_safe(root, (Node *) f) &&
		(ppi == NULL || is_parallel_safe(root, (Node *) ppi->ppi_clauses));
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.pathkeys = NIL;
	/* the cone itself is rechecked on every candidate, as a join clause */
	if (ppi)
		cost_qual_eval(&recheck, ppi->ppi_clauses, root);
	cone_cost(root, rel, ix, e.nranges, e.cand, rows, cell_correlation(rte, ix, cell),
			  ppi ? cone_loop_count(root, ppi->ppi_req_outer) : 1.0, recheck.per_tuple,
			  &cp->path.startup_cost, &cp->path.total_cost, &bitmap);
	cp->path.startup_cost += recheck.startup;
	cp->path.total_cost += recheck.startup;
	/* the covering itself, each time: ten operator calls a classified cell */
	cp->path.total_cost += e.steps * 10.0 * cpu_operator_cost;
	cp->flags = 0;
	cp->custom_paths = NIL;
	/* no bounds: cone_begin() then computes the covering each time */
	cp->custom_private = list_make4(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
											  ObjectIdGetDatum(ix->indexoid), false, true),
									makeNullConst(INT8ARRAYOID, -1, InvalidOid),
									makeBoolConst(bitmap, false), f);
	cp->custom_private = lappend(cp->custom_private,
								 list_make4(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
													  ObjectIdGetDatum(rte->relid), false, true),
											makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
													  ObjectIdGetDatum(e.statrel), false, true),
											makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
													  Int16GetDatum(attnum), false, true),
											makeBoolConst(ppi != NULL, false)));
	cp->methods = &cone_path_methods;
	add_path(rel, &cp->path);
}

/*
 * A region read from another relation's row -- the footprint of an
 * observation, the sources inside it -- reaches this relation as the
 * rewrite's range slots and exact test (adql.c), whose rows the planner
 * cannot estimate: each slot is taken for a ninth of the relation, and a scan
 * of every row looked cheaper.  Offer the scan parameterized by that
 * relation, each outer row's region covered as the scan runs, as a
 * cross-match's cone is (0.54).
 */
static void
add_region_join_paths(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	ListCell   *lc;

	foreach(lc, rel->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		FuncExpr   *x = (FuncExpr *) ri->clause;
		Node	   *pos,
				   *rg,
				   *ra,
				   *dec,
				   *cell = NULL;
		char	   *name;
		Relids		outer;
		ListCell   *lc2;
		Oid			t[4];
		Oid			fn;

		if (ri->pseudoconstant || !IsA(x, FuncExpr) || list_length(x->args) != 3 ||
			!join_clause_is_movable_to(ri, rel) ||
			(name = get_func_name(x->funcid)) == NULL || strcmp(name, "skycell_in_region") != 0)
			continue;
		pos = linitial(x->args);
		rg = lsecond(x->args);
		outer = pull_varnos(root, rg);
		if (bms_is_empty(outer) || bms_overlap(outer, rel->relids) ||
			!bms_is_subset(pull_varnos(root, pos), rel->relids) ||
			contain_volatile_functions(rg) || contain_subplans(rg))
			continue;
		foreach(lc2, rel->joininfo)
		{
			Node	   *c = (Node *) lfirst_node(RestrictInfo, lc2)->clause;

			if (region_slot_clause(c, rg, &cell))
				break;
			cell = NULL;
		}
		if (cell == NULL || !skycell_pos_coords(x->funcid, pos, &ra, &dec))
			continue;
		t[0] = INT8OID;
		t[1] = FLOAT8OID;
		t[2] = FLOAT8OID;
		t[3] = exprType(rg);
		fn = LookupFuncName(list_make2(makeString(get_namespace_name(get_func_namespace(x->funcid))),
									   makeString(pstrdup("skycell_region_scan"))),
							4, t, true);
		if (!OidIsValid(fn))
			continue;
		region_scan_funcid = fn;
		outer = bms_union(bms_difference(ri->clause_relids, rel->relids), rel->lateral_relids);
		add_runtime_path(root, rel, rte,
						 makeFuncExpr(fn, BOOLOID, list_make4(copyObject(cell), ra, dec, copyObject(rg)),
									  InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL),
						 get_baserel_parampathinfo(root, rel, outer));
	}
}

static void
cone_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!skycell_custom_scan || rel->indexlist == NIL)
		return;
	if (rel->reloptkind != RELOPT_BASEREL && rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;
	if (rte->rtekind != RTE_RELATION || rte->inh ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW))
		return;

	/*
	 * The scan slot's type is fixed when the scan state is created, before
	 * the relation is open, and the quals are compiled for it: heap only.
	 */
	{
		Relation	r = table_open(rte->relid, NoLock);	/* locked by the parser */
		bool		heap = table_slot_callbacks(r) == &TTSOpsBufferHeapTuple;

		table_close(r, NoLock);
		if (!heap)
			return;
	}

	add_nearest_path(root, rel, rte);
	if (rel->joininfo != NIL)
		add_region_join_paths(root, rel, rte);
	if (!OidIsValid(cone_funcid) && !OidIsValid(region_scan_funcid))
		return;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		FuncExpr   *f = (FuncExpr *) ri->clause;
		IndexOptInfo *ix;
		sc_region	reg;
		sc_cover	cov;
		sc_density	dens;
		CustomPath *cp,
				   *pp;
		Datum	   *b;
		Const	   *bounds;
		bool		bitmap;

		if (ri->pseudoconstant || !IsA(f, FuncExpr))
			continue;
		if (OidIsValid(region_scan_funcid) && f->funcid == region_scan_funcid &&
			list_length(f->args) == 4)
		{
			/* a constant composite: its covering's ranges, as a cone's */
			if ((ix = cell_index(rel, linitial(f->args))) == NULL ||
				!skycell_const_region_cover(root, f->args, &reg, &cov, &dens))
				continue;
			sc_region_free(&reg);
		}
		else
		{
			if (f->funcid != cone_funcid || list_length(f->args) != 6)
				continue;
			if (cone_kind(f->args, rel->relid) == CONE_RUNTIME)
			{
				add_runtime_path(root, rel, rte, f, NULL);
				continue;
			}
			if ((ix = cell_index(rel, linitial(f->args))) == NULL)
				continue;
			if (!skycell_const_cone_cover(root, f->args, &reg, &cov, &dens))
				continue;
		}

		/* copy the ranges out of the covering memo straight away */
		b = palloc(sizeof(Datum) * Max(2 * cov.n, 1));
		for (int i = 0; i < cov.n; i++)
		{
			b[2 * i] = Int64GetDatum(cov.r[i].lo);
			b[2 * i + 1] = Int64GetDatum(cov.r[i].hi);
		}
		bounds = makeConst(INT8ARRAYOID, -1, InvalidOid, -1,
						   PointerGetDatum(construct_array_builtin(b, 2 * cov.n, INT8OID)),
						   false, false);

		cp = makeNode(CustomPath);
		cp->path.pathtype = T_CustomScan;
		cp->path.parent = rel;
		cp->path.pathtarget = rel->reltarget;
		cp->path.param_info = NULL;
		cp->path.parallel_aware = false;
		/* a constant cone is a restriction clause: consider_parallel judged it */
		cp->path.parallel_safe = rel->consider_parallel;
		cp->path.parallel_workers = 0;
		cp->path.rows = rel->rows;
		cp->path.pathkeys = NIL;
		cone_cost(root, rel, ix, cov.n, cov.exp_rows, rel->rows,
				  cell_correlation(rte, ix, linitial(f->args)), 1.0, 0,
				  &cp->path.startup_cost, &cp->path.total_cost, &bitmap);
		cp->flags = 0;
		cp->custom_paths = NIL;
		/* the clause this path answers rides along for cone_plan(), which drops it */
		cp->custom_private = list_make4(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
												  ObjectIdGetDatum(ix->indexoid), false, true),
										bounds, makeBoolConst(bitmap, false), f);
		cp->methods = &cone_path_methods;
		/*
		 * A wide cone or composite reads many pages: offer it in parallel too,
		 * its chunks shared out among the processes (cone_claim()), with as
		 * many workers as PostgreSQL gives a scan of the pages it expects to
		 * read, and its cost and rows divided as PostgreSQL divides its own.
		 * The copy is made before add_path(), which frees a path it rejects.
		 */
		pp = NULL;
		if (rel->consider_parallel && max_parallel_workers_per_gather > 0 &&
			rel->tuples > 0 && rel->pages > 0)
		{
			double		heap_pages = fmin((double) rel->pages,
										  cov.exp_rows * (double) rel->pages / rel->tuples);
			int			workers = compute_parallel_worker(rel, heap_pages, -1,
														  max_parallel_workers_per_gather);

			if (workers > 0)
			{
				double		divisor = workers;

				if (parallel_leader_participation && 1.0 - 0.3 * workers > 0)
					divisor += 1.0 - 0.3 * workers;
				pp = makeNode(CustomPath);
				*pp = *cp;
				pp->path.parallel_aware = true;
				pp->path.parallel_safe = true;
				pp->path.parallel_workers = workers;
				pp->path.rows = clamp_row_est(rel->rows / divisor);
				pp->path.total_cost = cp->path.startup_cost +
					(cp->path.total_cost - cp->path.startup_cost) / divisor;
			}
		}
		add_path(rel, &cp->path);
		if (pp != NULL)
			add_partial_path(rel, &pp->path);
	}

	/*
	 * A cross-match cone is a join clause: offer the scan parameterized by the
	 * relations its centre and radius read, for the inner side of a nested
	 * loop, where each outer row gets its own covering.
	 */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		FuncExpr   *f = (FuncExpr *) ri->clause;
		Relids		inner,
					outer;

		if (ri->pseudoconstant || !IsA(f, FuncExpr) || f->funcid != cone_funcid ||
			list_length(f->args) != 6 || !join_clause_is_movable_to(ri, rel))
			continue;
		inner = pull_varnos(root, (Node *) list_make3(linitial(f->args), lsecond(f->args),
													 lthird(f->args)));
		outer = pull_varnos(root, (Node *) list_make3(list_nth(f->args, 3), list_nth(f->args, 4),
													 list_nth(f->args, 5)));
		if (bms_is_empty(inner) || !bms_is_subset(inner, rel->relids) ||
			bms_overlap(outer, rel->relids))
			continue;
		outer = bms_union(bms_difference(ri->clause_relids, rel->relids), rel->lateral_relids);
		if (bms_is_empty(outer))
			continue;
		add_runtime_path(root, rel, rte, f, get_baserel_parampathinfo(root, rel, outer));
	}

}

/* ------------------------------------------------------------------ */
/* nearest neighbours: ORDER BY distance(...) [LIMIT k]               */
/* ------------------------------------------------------------------ */

/*
 * A B-tree on the cell has no notion of distance, so a query ordered by the
 * distance to a point gets no index path from PostgreSQL: it sorts the whole
 * relation.  SkycellNearest returns the rows in distance order from the cell
 * index itself, ring by ring.  It covers the cone of radius r1 around the
 * centre, reads its rows, keeps those with distance <= r1, sorts them and
 * returns them; then the cone of radius r2 > r1, keeping r1 < distance <= r2;
 * and so on.  Each ring's rows all come after the previous ring's, so the
 * output is in order, and a LIMIT above stops the scan at the ring that
 * completes it.  The first radius is the one the density expects to hold the
 * rows the query asks for; each later one grows the area by what the rings so
 * far found.  A ring that would reach most of the relation is the last one,
 * read sequentially: it returns every row not yet returned, those with a NULL
 * or NaN distance included, in the order ORDER BY gives them.
 *
 * The distance kept is the ORDER BY expression itself, evaluated per row, so
 * the order is the one a Sort would produce.  The cones are covered a hair
 * wider than the ring, as for a distance comparison (adql.c), so no row on a
 * ring's edge can be missed, and less the cells wholly inside the previous
 * ring's cone, whose rows were all returned: a ring reads its own band and
 * the cells the previous edge crossed.  The query's other conditions are
 * tested as a ring is read, so the rows they reject are never sorted.
 *
 * Rows come out of the ring's buffer, not the heap, so a query that reads a
 * system column of the relation (ctid, a row lock) is left to PostgreSQL.
 */

/* does the query read a system column of rel? */
static bool
reads_system_column(RelOptInfo *rel)
{
	Bitmapset  *attrs = NULL;
	ListCell   *lc;
	int			x = -1;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	foreach(lc, rel->baserestrictinfo)
		pull_varattnos((Node *) lfirst_node(RestrictInfo, lc)->clause, rel->relid, &attrs);
	while ((x = bms_next_member(attrs, x)) >= 0)
		if (x + FirstLowInvalidHeapAttributeNumber < 0)
			return true;
	return false;
}

/* ascending, NULLs last, by float8's own ordering: what the scan produces */
static bool
nearest_pathkey(PathKey *pk)
{
#if PG_VERSION_NUM >= 180000
	if (pk->pk_cmptype != COMPARE_LT)
		return false;
#else
	if (pk->pk_strategy != BTLessStrategyNumber)
		return false;
#endif
	return !pk->pk_nulls_first &&
		get_opfamily_member(pk->pk_opfamily, FLOAT8OID, FLOAT8OID,
							BTLessStrategyNumber) == Float8LessOperator;
}

/* the fewest rows a first ring is sized for */
#define NEAREST_MIN_ROWS 16.0
/* a ring is sized for this many times the rows the query is expected to read */
#define NEAREST_MARGIN 1.25
/* the rows a LIMIT is taken to ask for when its count is a parameter */
#define NEAREST_UNKNOWN_LIMIT 100.0

/*
 * The cost of one ring of c candidate rows: the cone scan's own cost for
 * them, the distance on each, and their sort.
 */
static void
nearest_ring_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ix, double corr,
				  Cost per_row, double c, Cost *cost, bool *bitmap)
{
	Cost		startup,
				total;

	/* a ring is a few ranges, whatever its size: covering steps and descents */
	cone_cost(root, rel, ix, 4.0, c, c, corr, 1.0, per_row, &startup, &total, bitmap);
	*cost = total + 30.0 * 10.0 * cpu_operator_cost
		+ c * (cpu_tuple_cost + 2.0 * cpu_operator_cost * log2(fmax(c, 2.0)));
}

/*
 * The radius the relation's conditions confine the answer to, when they say
 * the distance ordered by is below one: the comparison itself (as
 * skycell_distance_below(), adql.c's spelling of it once a cone carries the
 * estimate, or as written), or a cone round the same centre.  The radius
 * expressions, each read once by the scan, which then never reads past the
 * least of them: without that, the 10 nearest within 1 arcmin of a patch that
 * holds fewer would have the rings widen to the whole relation.
 */
static double cap_area(double r);

static bool
radius_expr_ok(PlannerInfo *root, RelOptInfo *rel, Node *r)
{
	return exprType(r) == FLOAT8OID && !contain_volatile_functions(r) &&
		!contain_subplans(r) && !bms_is_member(rel->relid, pull_varnos(root, r));
}

/*
 * The radii the conditions keep the answer beyond: dist > x, dist >= x,
 * x < dist, x <= dist (0.45).  No row nearer than the greatest of them can
 * pass, so the rings start there instead of at the centre.
 */
static List *
nearest_lower_bounds(PlannerInfo *root, RelOptInfo *rel, Node *dist)
{
	List	   *bounds = NIL;
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
	{
		Node	   *q = (Node *) lfirst_node(RestrictInfo, lc)->clause;
		Node	   *r = NULL;

		if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
		{
			OpExpr	   *op = (OpExpr *) q;
			Oid			fn = OidIsValid(op->opfuncid) ? op->opfuncid : get_opcode(op->opno);

			if ((fn == F_FLOAT8GT || fn == F_FLOAT8GE) && equal(linitial(op->args), dist))
				r = lsecond(op->args);
			else if ((fn == F_FLOAT8LT || fn == F_FLOAT8LE) && equal(lsecond(op->args), dist))
				r = linitial(op->args);
		}
		if (r != NULL && radius_expr_ok(root, rel, r))
			bounds = lappend(bounds, copyObject(r));
	}
	return bounds;
}

static List *
nearest_bounds(PlannerInfo *root, RelOptInfo *rel, Node *dist, Node *ra0, Node *dec0)
{
	List	   *bounds = NIL;
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
	{
		Node	   *q = (Node *) lfirst_node(RestrictInfo, lc)->clause;
		Node	   *r = NULL;

		if (IsA(q, FuncExpr))
		{
			FuncExpr   *f = (FuncExpr *) q;
			char	   *name = get_func_name(f->funcid);
			int			n = list_length(f->args);

			if (name == NULL)
				continue;
			if (strcmp(name, "skycell_distance_below") == 0 && n == 3 &&
				equal(linitial(f->args), dist))
				r = lsecond(f->args);
			else if (strcmp(name, "skycell_cone") == 0 && n == 6 &&
					 equal(list_nth(f->args, 3), ra0) && equal(list_nth(f->args, 4), dec0))
				r = list_nth(f->args, 5);
			else if (strcmp(name, "skycell_in_cone") == 0 && n >= 5 &&
					 equal(lthird(f->args), ra0) && equal(lfourth(f->args), dec0))
				r = list_nth(f->args, 4);
		}
		else if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
		{
			OpExpr	   *op = (OpExpr *) q;
			Oid			fn = OidIsValid(op->opfuncid) ? op->opfuncid : get_opcode(op->opno);

			if ((fn == F_FLOAT8LT || fn == F_FLOAT8LE) && equal(linitial(op->args), dist))
				r = lsecond(op->args);
			else if ((fn == F_FLOAT8GT || fn == F_FLOAT8GE) && equal(lsecond(op->args), dist))
				r = linitial(op->args);
		}
		if (r != NULL && radius_expr_ok(root, rel, r))
			bounds = lappend(bounds, copyObject(r));
	}
	return bounds;
}

static void
add_nearest_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	PathKey    *pk;
	ListCell   *lc;

	if (!skycell_nearest_scan || root->query_pathkeys == NIL)
		return;
	pk = linitial_node(PathKey, root->query_pathkeys);
	if (!nearest_pathkey(pk) || reads_system_column(rel))
		return;

	/*
	 * The ORDER BY expression as this relation sees it.  For a partition
	 * (RELOPT_OTHER_MEMBER_REL) that is the parent's expression translated to
	 * the partition's columns, a child member of the class; offering the scan
	 * there lets PostgreSQL merge the partitions' ordered rows under the
	 * LIMIT (Merge Append).  PostgreSQL 18 keeps child members apart from
	 * ec_members, reachable through an EquivalenceMemberIterator.
	 */
	{
		List	   *members = NIL;
#if PG_VERSION_NUM >= 180000
		EquivalenceMemberIterator it;
		EquivalenceMember *m;

		setup_eclass_member_iterator(&it, pk->pk_eclass, rel->relids);
		while ((m = eclass_member_iterator_next(&it)) != NULL)
			members = lappend(members, m);
#else
		members = list_copy(pk->pk_eclass->ec_members);
#endif
		lc = NULL;
		foreach(lc, members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
			Node	   *expr = (Node *) em->em_expr;
			Node	   *cell,
					   *ra0,
					   *dec0;
			IndexOptInfo *ix;
			sc_density	dens;
			Oid			statrel = InvalidOid;
			bool		uses_cell_ops;
			CustomPath *cp;
			QualCost	dist_cost;
			double		n = fmax(rel->tuples, 1.0),
						sel = fmin(1.0, fmax(rel->rows / n, 1e-9)),
						want,
						k0,
						corr;
			Cost		first,
						all;
			bool		bitmap,
						bitmap_all;
			List	   *bounds,
					   *lower;
			ListCell   *lb;
			double		bound = -1;

			if (em->em_is_const || !bms_equal(em->em_relids, rel->relids))
				continue;
			while (IsA(expr, RelabelType))
				expr = (Node *) ((RelabelType *) expr)->arg;
			/* the cell expression, or a stored column holding it (README, setup 2) */
			if (!skycell_distance_order(expr, rel->relid, &cell, &ra0, &dec0) ||
				!skycell_cell_density(root, &cell, &dens, &statrel, &uses_cell_ops) ||
				(ix = cell_index(rel, cell)) == NULL)
				continue;

			/*
			 * The first ring holds the rows the query reads -- the LIMIT, over the
			 * fraction of rows the other conditions keep -- with a margin, so that it
			 * usually completes the query alone; a LIMIT whose count is a
			 * parameter is taken as NEAREST_UNKNOWN_LIMIT rows, and the rings
			 * grow from there if it asks for more.  Without a LIMIT it is the
			 * whole relation, read through the index and sorted: never cheaper
			 * than the sequential scan and sort PostgreSQL plans itself.
			 */
			if (root->limit_tuples > 0)
				want = root->limit_tuples;
			else if (root->parse->limitCount != NULL &&
					 !(IsA(root->parse->limitCount, Const) &&
					   ((Const *) root->parse->limitCount)->constisnull))
				want = NEAREST_UNKNOWN_LIMIT;	/* LIMIT $1: the rings will grow */
			else
				want = rel->rows;
			k0 = fmin(n, fmax(NEAREST_MIN_ROWS, NEAREST_MARGIN * want / sel));

			/*
			 * A radius the conditions bound the answer to caps the first ring --
			 * by the rows a cone of it holds at the relation's mean density, when
			 * it is a constant -- and there is no reading past it.
			 */
			bounds = nearest_bounds(root, rel, expr, ra0, dec0);
			lower = nearest_lower_bounds(root, rel, expr);
			foreach(lb, bounds)
			{
				Const	   *b = (Const *) lfirst(lb);

				if (IsA(b, Const) && !b->constisnull &&
					(bound < 0 || DatumGetFloat8(b->constvalue) < bound))
					bound = fmax(0.0, DatumGetFloat8(b->constvalue));
			}
			if (bound >= 0)
				k0 = fmin(k0, fmax(NEAREST_MIN_ROWS, n * cap_area(bound) / (4.0 * M_PI)));
			corr = cell_correlation(rte, ix, cell);
			cost_qual_eval_node(&dist_cost, expr, root);
			nearest_ring_cost(root, rel, ix, corr, dist_cost.per_tuple, k0, &first, &bitmap);
			nearest_ring_cost(root, rel, ix, corr, dist_cost.per_tuple, n, &all, &bitmap_all);

			cp = makeNode(CustomPath);
			cp->path.pathtype = T_CustomScan;
			cp->path.parent = rel;
			cp->path.pathtarget = rel->reltarget;
			cp->path.param_info = NULL;
			cp->path.parallel_aware = false;
			cp->path.parallel_safe = rel->consider_parallel && is_parallel_safe(root, expr);
			cp->path.parallel_workers = 0;
			cp->path.rows = rel->rows;
			cp->path.pathkeys = list_make1(pk);
			/*
			 * Nothing comes out before the first ring is sorted.  Reading on
			 * past it costs about the whole relation's ring again, twice over
			 * for the cells each ring's edge shares with the next.
			 */
			cp->path.startup_cost = first;
			cp->path.total_cost = (k0 >= n || bound >= 0) ? first : first + 2.0 * all;
			/* ExecScan projects, so a computed target needs no Result above */
			cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
			cp->custom_paths = NIL;
			cp->custom_private = list_make5(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
													  ObjectIdGetDatum(ix->indexoid), false, true),
											makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
													  ObjectIdGetDatum(statrel), false, true),
											makeBoolConst(bitmap, false),
											list_make3(float8_const(k0), float8_const(want),
													   makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
																 Int16GetDatum(IsA(cell, Var) ? ((Var *) cell)->varattno : 1),
																 false, true)),
											list_concat(list_make3(expr, ra0, dec0), bounds));
			cp->custom_private = lappend(cp->custom_private, lower);
			cp->methods = &nearest_path_methods;
			add_path(rel, &cp->path);
			return;
		}
	}
}

/*
 * The plan: every condition on the relation stays a qual, applied to the rows
 * as they come out in order.  custom_private is laid out as cone_begin()
 * reads a run-time cone's -- (index, NULL bounds, mode, no index filter,
 * relation, statistics relation, attribute, not joined) -- then the first
 * ring's rows and the rows the query was planned to read; custom_exprs holds
 * the centre, the ORDER BY expression and the radii the answer is bounded by.
 */
/* does an entry of the target list hold the expression? */
static bool
tlist_has_expr_walker(Node *node, Node *expr)
{
	if (node == NULL)
		return false;
	if (equal(node, expr))
		return true;
	return expression_tree_walker(node, tlist_has_expr_walker, expr);
}

static bool
tlist_has_expr(List *tlist, Node *expr)
{
	return tlist_has_expr_walker((Node *) tlist, expr);
}

typedef struct
{
	Node	   *expr;
	Param	   *param;
} expr_param_context;

static Node *
expr_to_param_mutator(Node *node, expr_param_context *cx)
{
	if (node == NULL)
		return NULL;
	if (equal(node, cx->expr))
		return (Node *) copyObject(cx->param);
	return expression_tree_mutator(node, expr_to_param_mutator, cx);
}

static Plan *
nearest_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *p = best_path->custom_private;
	List	   *e = (List *) list_nth(p, 4);
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = extract_actual_clauses(clauses, false);
	cs->scan.scanrelid = rel->relid;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_private = list_make5(linitial(p),
									makeNullConst(INT8ARRAYOID, -1, InvalidOid),
									lthird(p),
									makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
											  Int16GetDatum(0), false, true),
									makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
											  Int16GetDatum(0), false, true));
	cs->custom_private = lappend(cs->custom_private,
								 makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
										   ObjectIdGetDatum(rte->relid), false, true));
	cs->custom_private = lappend(cs->custom_private, lsecond(p));
	cs->custom_private = lappend(cs->custom_private, lthird((List *) lfourth(p)));
	cs->custom_private = lappend(cs->custom_private, makeBoolConst(false, false));
	cs->custom_private = lappend(cs->custom_private, linitial((List *) lfourth(p)));
	cs->custom_private = lappend(cs->custom_private, lsecond((List *) lfourth(p)));
	cs->custom_exprs = list_concat(list_make3(copyObject(lsecond(e)), copyObject(lthird(e)),
											  copyObject(linitial(e))),
								   copyObject(list_copy_tail(e, 3)));
	/* the lower bounds after the upper ones, and how many upper ones there are */
	cs->custom_private = lappend(cs->custom_private,
								 makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
										   Int32GetDatum(list_length(e) - 3), false, true));
	cs->custom_exprs = list_concat(cs->custom_exprs,
								   copyObject((List *) list_nth(p, 5)));

	/*
	 * A parameter for the distance, should the target list compute it: the
	 * scan has computed it already, for every row it returns.  The plan's
	 * target list keeps the expression, which the nodes above it match; the
	 * scan's projection reads the parameter in its place (0.52).  The target
	 * list is not final here -- a projection can be pushed into the scan
	 * later -- so the parameter is always assigned, and used if it is.
	 */
	cs->custom_private = lappend(cs->custom_private,
								 makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
										   Int32GetDatum(assign_special_exec_param(root)),
										   false, true));
	cs->custom_scan_tlist = NIL;
	cs->methods = &nearest_scan_methods;
	return &cs->scan.plan;
}

static Oid
in_cone_oid(void)
{
	if (in_cone_for != cone_funcid || !OidIsValid(in_cone_funcid))
	{
		Oid			types[6] = {FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID};

		in_cone_funcid = lookup_sibling_func(cone_funcid, "skycell_in_cone", 6, types);
		in_cone_for = cone_funcid;
	}
	return in_cone_funcid;
}

/*
 * The cone this scan is driven by is rechecked with skycell_in_cone(ra, dec,
 * ...): the ranges never return a row whose cell is NULL, which is all
 * skycell_cone's strictness on that argument would add, and the cell
 * expression need not be computed per row.  Any other cone qual on the same
 * relation is kept as written.
 */

/*
 * The index attribute holding column arg of relation rel (a plain float8
 * column, as skycell_in_cone reads it), or 0.  Any column past the cell key
 * will do -- an INCLUDE column or a further key -- as long as the index can
 * return it.
 */
static AttrNumber
index_column_of(RelOptInfo *rel, Oid indexoid, Node *arg)
{
	Var		   *v = (Var *) arg;
	ListCell   *lc;

	if (!IsA(arg, Var) || v->varno != rel->relid || v->varlevelsup != 0 ||
		v->varattno <= 0 || v->vartype != FLOAT8OID)
		return 0;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ix = (IndexOptInfo *) lfirst(lc);

		if (ix->indexoid != indexoid)
			continue;
		for (int k = 1; k < ix->ncolumns; k++)
			if (ix->indexkeys[k] == v->varattno && ix->canreturn[k])
				return (AttrNumber) (k + 1);
	}
	return 0;
}

static Plan *
cone_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		  List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	FuncExpr   *driving = (FuncExpr *) lfourth(best_path->custom_private);
	Oid			indexoid = DatumGetObjectId(linitial_node(Const, best_path->custom_private)->constvalue);
	AttrNumber	ira = 0,
				idec = 0;
	List	   *quals = NIL;
	ListCell   *lc;

	/* a composite's own call is its exact test, and has no cone to filter on */
	if (skycell_index_filter && driving->funcid != region_scan_funcid)
	{
		ira = index_column_of(rel, indexoid, lsecond(driving->args));
		idec = index_column_of(rel, indexoid, lthird(driving->args));
		if (ira == 0 || idec == 0)
			ira = idec = 0;
	}

	foreach(lc, clauses)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);

		if (ri->pseudoconstant)
			continue;
		/* a partition bound next to the cone or region, which it implies */
		if (driving->funcid != region_scan_funcid
			? cone_implies_clause(driving, (Node *) ri->clause)
			: region_implies_clause(driving, (Node *) ri->clause))
			continue;
		if ((Node *) ri->clause == (Node *) driving && driving->funcid == region_scan_funcid)
		{
			/* the exact test, without computing each row's cell, which it never reads */
			FuncExpr   *f = copyObject(driving);

			linitial(f->args) = makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
										  Int64GetDatum(0), false, true);
			quals = lappend(quals, f);
		}
		else if ((Node *) ri->clause == (Node *) driving)
		{
			List	   *a = list_copy_tail(copyObject(driving->args), 1);

			a = lappend(a, makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
									 Float8GetDatum(-1.0), false, true));
			quals = lappend(quals, makeFuncExpr(in_cone_oid(), BOOLOID, a, InvalidOid,
												InvalidOid, COERCE_EXPLICIT_CALL));
		}
		else
			quals = lappend(quals, ri->clause);
	}

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = quals;
	cs->scan.scanrelid = rel->relid;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_exprs = NIL;
	cs->custom_private = list_make5(linitial(best_path->custom_private),
									lsecond(best_path->custom_private),
									lthird(best_path->custom_private),
									makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
											  Int16GetDatum(ira), false, true),
									makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
											  Int16GetDatum(idec), false, true));
	/* a constant cone's centre and radius, for the index filter */
	if (ira != 0)
		cs->custom_exprs = list_copy_tail(copyObject(driving->args), 3);
	if (list_length(best_path->custom_private) > 4)
	{
		/*
		 * A cross-match cone: centre and radius are evaluated per outer row,
		 * createplan having turned the outer relation's columns into nestloop
		 * parameters, and the density they are covered against is loaded at
		 * execution from (relid, statistics relation, attnum).
		 */
		cs->custom_exprs = list_copy_tail(copyObject(driving->args), 3);
		cs->custom_private = list_concat(cs->custom_private,
										 (List *) list_nth(best_path->custom_private, 4));
	}
	cs->custom_scan_tlist = NIL;
	cs->methods = &cone_scan_methods;
	return &cs->scan.plan;
}

/* ------------------------------------------------------------------ */
/* execution                                                          */
/* ------------------------------------------------------------------ */

static Node *
cone_create_state(CustomScan *cscan)
{
	ConeScanState *st = (ConeScanState *) newNode(sizeof(ConeScanState), T_CustomScanState);

	st->css.methods = &cone_exec_methods;
	st->css.slotOps = &TTSOpsBufferHeapTuple;	/* checked heap at plan time */
	return (Node *) st;
}

static void cone_release_bitmap(ConeScanState *st, bool end);
static void cone_key_range(ConeScanState *st);

/* about this many chunks to share out among a parallel scan's processes */
#define CONE_PARALLEL_CHUNKS 64

/*
 * The covering's ranges cut into about CONE_PARALLEL_CHUNKS chunks, each range
 * into pieces in proportion to its width in cells, so that a process that
 * claims a chunk has a share of the work and the last ones to finish do not
 * wait on one long range.  The same in every process.
 */
static void
cone_make_chunks(ConeScanState *st)
{
	double		total = 0;
	int			n = 0,
				cap = CONE_PARALLEL_CHUNKS + st->nranges;
	int64	   *c = MemoryContextAlloc(st->css.ss.ps.state->es_query_cxt,
									   sizeof(int64) * 2 * cap);

	for (int i = 0; i < st->nranges; i++)
		total += (double) (st->bounds[2 * i + 1] - st->bounds[2 * i]) + 1.0;
	for (int i = 0; i < st->nranges; i++)
	{
		int64		lo = st->bounds[2 * i],
					hi = st->bounds[2 * i + 1];
		uint64		w = (uint64) (hi - lo) + 1;
		int			k = (int) Max(1.0, floor(CONE_PARALLEL_CHUNKS * (double) w / Max(total, 1.0)));

		if ((uint64) k > w)
			k = (int) w;
		for (int j = 0; j < k; j++)
		{
			int64		a = lo + (int64) (w / k * j),
						b = (j == k - 1) ? hi : lo + (int64) (w / k * (j + 1)) - 1;

			if (n == cap)
			{
				cap *= 2;
				c = repalloc(c, sizeof(int64) * 2 * cap);
			}
			c[2 * n] = a;
			c[2 * n + 1] = b;
			n++;
		}
	}
	st->bounds = c;
	st->nranges = n;
}

/* the next chunk this process scans: from the shared counter, or in turn */
static int
cone_claim(ConeScanState *st)
{
	uint32		i = (st->next != NULL) ? pg_atomic_fetch_add_u32(st->next, 1) : st->local_next++;

	return (i >= (uint32) st->nranges) ? st->nranges : (int) i;
}

static void
cone_begin(CustomScanState *node, EState *estate, int eflags)
{
	ConeScanState *st = (ConeScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Const	   *oidc = linitial_node(Const, cscan->custom_private);
	Const	   *bc = lsecond_node(Const, cscan->custom_private);
	Const	   *modec = lthird_node(Const, cscan->custom_private);
	Relation	heap = node->ss.ss_currentRelation;
	LOCKMODE	lockmode = exec_rt_fetch(cscan->scan.scanrelid, estate)->rellockmode;
	Oid			opfamily;

	st->indexoid = DatumGetObjectId(oidc->constvalue);
	if (bc->constisnull)
	{
		/* a cross-match cone: covered per outer row, by cone_cover_row() */
		st->runtime = true;
		st->need_cover = true;
		st->nranges = 0;
		st->bounds = NULL;
		st->relid = DatumGetObjectId(list_nth_node(Const, cscan->custom_private, 5)->constvalue);
		st->statrel = DatumGetObjectId(list_nth_node(Const, cscan->custom_private, 6)->constvalue);
		st->attnum = DatumGetInt16(list_nth_node(Const, cscan->custom_private, 7)->constvalue);
		st->joined = DatumGetBool(list_nth_node(Const, cscan->custom_private, 8)->constvalue);
	}
	else
	{
		ArrayType  *arr = DatumGetArrayTypeP(bc->constvalue);

		st->nranges = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) / 2;
		st->bounds = (const int64 *) ARR_DATA_PTR(arr);
		st->plan_nranges = st->nranges;
		st->parallel = cscan->scan.plan.parallel_aware;
	}
	st->args = ExecInitExprList(cscan->custom_exprs, &node->ss.ps);
	st->ira = DatumGetInt16(list_nth_node(Const, cscan->custom_private, 3)->constvalue);
	st->idec = DatumGetInt16(list_nth_node(Const, cscan->custom_private, 4)->constvalue);
	st->bitmap = DatumGetBool(modec->constvalue);
	st->cur = -1;
	st->index = index_open(st->indexoid, lockmode);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	opfamily = st->index->rd_opfamily[0];
	ScanKeyInit(&st->keys[0], 1, BTGreaterEqualStrategyNumber,
				get_opcode(get_opfamily_member(opfamily, INT8OID, INT8OID,
											   BTGreaterEqualStrategyNumber)),
				Int64GetDatum(0));
	ScanKeyInit(&st->keys[1], 1, BTLessEqualStrategyNumber,
				get_opcode(get_opfamily_member(opfamily, INT8OID, INT8OID,
											   BTLessEqualStrategyNumber)),
				Int64GetDatum(0));
	if (table_slot_callbacks(heap) != &TTSOpsBufferHeapTuple)
		elog(ERROR, "skycell: custom cone scan on a non-heap relation");
	/*
	 * PostgreSQL 18 gave index_beginscan()/_bitmap() an instrumentation
	 * argument (EXPLAIN's index search counts) and has the table AM walk a
	 * bitmap itself, so its heap scan is begun with the first bitmap, as
	 * BitmapHeapScan does.
	 */
	/*
	 * With the index filter the entries are read one by one, with their
	 * included columns, in either mode: the bitmap mode then collects only
	 * the TIDs that pass, instead of every TID of every range.
	 */
#if PG_VERSION_NUM >= 180000
	if (st->bitmap && st->ira == 0)
		st->iscan = index_beginscan_bitmap(st->index, estate->es_snapshot, NULL, 2);
	else
		st->iscan = index_beginscan(heap, st->index, estate->es_snapshot, NULL, 2, 0);
#else
	if (st->bitmap && st->ira == 0)
		st->iscan = index_beginscan_bitmap(st->index, estate->es_snapshot, 2);
	else
		st->iscan = index_beginscan(heap, st->index, estate->es_snapshot, 2, 0);
	/* PostgreSQL 17 alone asks whether the scan returns tuples: it does */
	if (st->bitmap)
#if PG_VERSION_NUM >= 170000
		st->hscan = table_beginscan_bm(heap, estate->es_snapshot, 0, NULL, true);
#else
		st->hscan = table_beginscan_bm(heap, estate->es_snapshot, 0, NULL);
#endif
#endif
	if (st->ira != 0)
		st->iscan->xs_want_itup = true;

	/*
	 * A constant covering on a partition is clipped to the keys the
	 * partition holds (0.44), so that a cone over another part of the sky
	 * descends none of its index; a plain table, all of whose keys a
	 * covering of its own rows meets, is not.  A parallel scan is not: its
	 * processes must cut identical chunks, and the keys are read whatever
	 * their visibility, so a row inserted meanwhile could move one process's
	 * interval and not another's.
	 */
	if (!st->runtime && !st->parallel && heap->rd_rel->relispartition && st->nranges > 0)
	{
		int64	   *c = MemoryContextAlloc(estate->es_query_cxt, sizeof(int64) * 2 * st->nranges);
		int			n = 0;

		cone_key_range(st);
		for (int i = 0; i < st->nranges; i++)
		{
			int64		lo = st->bounds[2 * i],
						hi = st->bounds[2 * i + 1];

			if (hi < st->kmin || lo > st->kmax)
				continue;
			c[2 * n] = Max(lo, st->kmin);
			c[2 * n + 1] = Min(hi, st->kmax);
			n++;
		}
		st->bounds = c;
		st->nranges = n;
	}
	if (st->parallel)
		cone_make_chunks(st);
}

/* the cone the index filter tests, from its centre and radius in degrees */
static void
cone_set_centre(ConeScanState *st, double ra0, double dec0, double radius)
{
	double		thr = radius < 0 ? -1.0
		: (radius >= 180.0 ? 2.0 : pow(sin(radius * M_PI / 360.0), 2));

	st->cra = ra0;
	st->cdec = dec0;
	st->cosdec0 = cos(dec0 * M_PI / 180.0);

	/*
	 * A hair wider than skycell_in_cone's threshold, so that rounding can
	 * only let a borderline entry through, never hold one back: the plan's
	 * qual still applies the exact test to every row the filter passes.
	 */
	st->thr = thr < 0 ? thr : thr * (1.0 + 1e-9) + 1e-300;
	st->have_centre = true;
}

/*
 * The index filter's test of the current index entry: skycell_in_cone's
 * haversine on the included ra and dec.  A NULL coordinate fails, as the
 * strict exact test would.
 */
static bool
cone_entry_passes(ConeScanState *st)
{
	IndexScanDesc s = st->iscan;
	bool		n1,
				n2;
	double		ra,
				dec,
				sdd,
				sda;

	ra = DatumGetFloat8(index_getattr(s->xs_itup, st->ira, s->xs_itupdesc, &n1));
	dec = DatumGetFloat8(index_getattr(s->xs_itup, st->idec, s->xs_itupdesc, &n2));
	if (!n1 && !n2)
	{
		sdd = sin((dec - st->cdec) * M_PI / 360.0);
		sda = sin((ra - st->cra) * M_PI / 360.0);
		if (sdd * sdd + cos(dec * M_PI / 180.0) * st->cosdec0 * sda * sda <= st->thr)
			return true;
	}
	/*
	 * Counted in the node's instrumentation, not the scan state: a parallel
	 * query adds its workers' counts into the leader's (InstrAggNode), so
	 * EXPLAIN ANALYZE reports them all however the probes were shared out.
	 */
	InstrCountFiltered2(&st->css.ss.ps, 1);
	return false;
}

/* the next TID of the current range whose entry passes the index filter */
static ItemPointer
cone_next_tid(ConeScanState *st)
{
	ItemPointer tid;

	while ((tid = index_getnext_tid(st->iscan, ForwardScanDirection)) != NULL)
		if (cone_entry_passes(st))
			return tid;
	return NULL;
}

static void
cone_load_density(ConeScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext old;

	/*
	 * The density points into the backend's cache, which an invalidation
	 * processed since the last row -- by the outer side, say -- may have
	 * freed.  Evaluating the arguments can do that too, so callers check
	 * after them; nothing between this and the covering takes a lock.
	 */
	if (st->have_dens && st->dens_generation == skycell_density_generation())
		return;
	old = MemoryContextSwitchTo(estate->es_query_cxt);
	if (st->have_dens && st->dens.bounds)
		pfree((void *) st->dens.bounds);	/* a copy in es_query_cxt */
	st->dens_generation = skycell_density_generation();
	skycell_load_density(st->relid, st->statrel, st->attnum, &st->dens);
	MemoryContextSwitchTo(old);
	st->have_dens = true;
}

/*
 * The index's lowest and highest non-NULL key, into kmin and kmax (kmin >
 * kmax when it has none).  index_getnext_tid() returns entries whatever
 * their visibility, so the interval holds every key the scan's snapshot can
 * see, and the snapshot is the statement's, so no rescan finds a key outside
 * it.
 */
static void
cone_key_range(ConeScanState *st)
{
	Relation	heap = st->css.ss.ss_currentRelation;
	Snapshot	snap = st->css.ss.ps.state->es_snapshot;
	IndexScanDesc s;
	ScanKeyData notnull;
	bool		isnull;

	st->have_keyrange = true;
	st->kmin = 1;
	st->kmax = 0;
	ScanKeyEntryInitialize(&notnull, SK_ISNULL | SK_SEARCHNOTNULL, 1,
						   InvalidStrategy, InvalidOid, InvalidOid, InvalidOid, (Datum) 0);
#if PG_VERSION_NUM >= 180000
	s = index_beginscan(heap, st->index, snap, NULL, 1, 0);
#else
	s = index_beginscan(heap, st->index, snap, 1, 0);
#endif
	s->xs_want_itup = true;
	index_rescan(s, &notnull, 1, NULL, 0);
	if (index_getnext_tid(s, ForwardScanDirection) != NULL)
	{
		Datum		lo = index_getattr(s->xs_itup, 1, s->xs_itupdesc, &isnull);

		index_rescan(s, &notnull, 1, NULL, 0);
		if (!isnull && index_getnext_tid(s, BackwardScanDirection) != NULL)
		{
			Datum		hi = index_getattr(s->xs_itup, 1, s->xs_itupdesc, &isnull);

			if (!isnull)
			{
				st->kmin = DatumGetInt64(lo);
				st->kmax = DatumGetInt64(hi);
			}
			else
			{
				st->kmin = PG_INT64_MIN;	/* never: the key is NOT NULL */
				st->kmax = PG_INT64_MAX;
			}
		}
		else
		{
			st->kmin = PG_INT64_MIN;
			st->kmax = PG_INT64_MAX;
		}
	}
	index_endscan(s);

	/*
	 * Keys over all but 1% of the sky (an unpartitioned all-sky catalogue)
	 * leave nothing to test a cone against.
	 */
	st->keys_partial = st->kmin > st->kmax ||
		(double) st->kmax - (double) st->kmin < 0.99 * 12.0 * pow(4.0, SC_MAX_ORDER);
}

/*
 * Cover the cone (ra0, dec0, radius) into the scan state's ranges, and return
 * the rows the density expects inside the cone itself.  The covering is built
 * in the per-tuple context, which ExecScan resets before every fetch, and the
 * ranges are copied out of it.
 */
static double
cone_cover_at(ConeScanState *st, double ra0, double dec0, double radius)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext old;
	sc_region	reg;
	sc_cover	cov;
	sc_cover_params p;
	double		rows;

	cone_load_density(st);
	if (!st->have_keyrange)
		cone_key_range(st);
	old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	check_err(sc_region_cone(&reg, ra0, dec0, radius, true));
	skycell_scan_params(&p, &st->dens);
	sc_cover_compute(&reg, &st->dens, &p, &cov);
	MemoryContextSwitchTo(old);
	if (cov.n > st->rt_cap)
	{
		st->rt_cap = Max(cov.n, 2 * st->rt_cap);
		st->rt_bounds = (st->rt_bounds == NULL)
			? MemoryContextAlloc(estate->es_query_cxt, sizeof(int64) * 2 * st->rt_cap)
			: repalloc(st->rt_bounds, sizeof(int64) * 2 * st->rt_cap);
	}
	/* clipped to the keys the index holds (cone_key_range()) */
	st->nranges = 0;
	for (int r = 0; r < cov.n; r++)
	{
		int64		lo = cov.r[r].lo,
					hi = cov.r[r].hi;

		if (hi < st->kmin || lo > st->kmax)
			continue;
		st->rt_bounds[2 * st->nranges] = Max(lo, st->kmin);
		st->rt_bounds[2 * st->nranges + 1] = Min(hi, st->kmax);
		st->nranges++;
	}
	st->bounds = st->rt_bounds;
	st->ncoverings += 1;
	st->nranges_total += st->nranges;
	rows = (cov.area > 0) ? cov.exp_rows * fmin(1.0, reg.area / cov.area) : 0.0;
	sc_cover_free(&cov);
	return rows;
}

/* the covering of a region datum, as cone_cover_at() covers a cone (0.54) */
static void
cone_cover_region(ConeScanState *st, Datum d)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext old;
	sc_region	reg;
	sc_cover	cov;
	sc_cover_params p;

	cone_load_density(st);
	if (!st->have_keyrange)
		cone_key_range(st);
	old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	skycell_region_from_datum(d, &reg);
	skycell_scan_params(&p, &st->dens);
	sc_cover_compute(&reg, &st->dens, &p, &cov);
	MemoryContextSwitchTo(old);
	if (cov.n > st->rt_cap)
	{
		st->rt_cap = Max(cov.n, 2 * st->rt_cap);
		st->rt_bounds = (st->rt_bounds == NULL)
			? MemoryContextAlloc(estate->es_query_cxt, sizeof(int64) * 2 * st->rt_cap)
			: repalloc(st->rt_bounds, sizeof(int64) * 2 * st->rt_cap);
	}
	st->nranges = 0;
	for (int r = 0; r < cov.n; r++)
	{
		int64		lo = cov.r[r].lo,
					hi = cov.r[r].hi;

		if (hi < st->kmin || lo > st->kmax)
			continue;
		st->rt_bounds[2 * st->nranges] = Max(lo, st->kmin);
		st->rt_bounds[2 * st->nranges + 1] = Min(hi, st->kmax);
		st->nranges++;
	}
	st->bounds = st->rt_bounds;
	st->ncoverings += 1;
	st->nranges_total += st->nranges;
	sc_cover_free(&cov);
	sc_region_free(&reg);
}

/*
 * A cross-match cone's covering for the current outer row: centre and radius
 * evaluated (the outer row's columns are nestloop parameters by now) and
 * covered against the relation's density with the custom scan's parameters.
 * NULL arguments match nothing, as the strict skycell_cone would; an invalid
 * centre is an error, as it is in the rewrite's run-time bounds and in
 * skycell_cone_ranges().
 */
static void
cone_cover_row(ConeScanState *st)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	double		v[3];
	int			i = 0;
	ListCell   *lc;

	st->need_cover = false;
	st->nranges = 0;
	if (list_length(st->args) == 1)
	{
		/* a per-row region (0.54): a NULL one matches nothing */
		bool		isnull;
		Datum		d = ExecEvalExpr((ExprState *) linitial(st->args), econtext, &isnull);

		if (!isnull)
			cone_cover_region(st, d);
		return;
	}
	foreach(lc, st->args)
	{
		bool		isnull;
		Datum		d = ExecEvalExpr((ExprState *) lfirst(lc), econtext, &isnull);

		if (isnull)
			return;
		v[i++] = DatumGetFloat8(d);
	}
	if (!st->have_keyrange)
		cone_key_range(st);

	/*
	 * A cone that can hold none of the index's keys -- another partition's
	 * part of the sky -- reads nothing, and is not covered: the seed test is
	 * a fraction of a covering's cost.  An invalid centre still raises its
	 * error, here as in the covering.
	 */
	if (st->keys_partial)
	{
		sc_region	reg;

		check_err(sc_region_cone(&reg, v[0], v[1], v[2], true));
		if (st->kmin > st->kmax || !sc_cone_may_meet_keys(&reg, st->kmin, st->kmax))
		{
			st->ncoverings += 1;
			return;
		}
	}
	if (st->ira != 0)
		cone_set_centre(st, v[0], v[1], v[2]);
	(void) cone_cover_at(st, v[0], v[1], v[2]);
}

/* point the scan keys at range i and restart the index scan there */
static void
cone_range(ConeScanState *st, int i)
{
	st->keys[0].sk_argument = Int64GetDatum(st->bounds[2 * i]);
	st->keys[1].sk_argument = Int64GetDatum(st->bounds[2 * i + 1]);
	index_rescan(st->iscan, st->keys, 2, NULL, 0);
}

/*
 * The next visible row of the current range whose entry passes the index
 * filter: index_getnext_slot() with the filter between the entry and the
 * heap, following a HOT chain as it does.
 */
static bool
cone_next_filtered(ConeScanState *st, TupleTableSlot *slot)
{
	for (;;)
	{
		if (!st->iscan->xs_heap_continue && cone_next_tid(st) == NULL)
			return false;
		if (index_fetch_heap(st->iscan, slot))
			return true;
	}
}

/* ordered: the next row of the current range, moving on when it runs out */
static TupleTableSlot *
cone_next_ordered(ConeScanState *st, TupleTableSlot *slot)
{
	for (;;)
	{
		if (st->cur >= st->nranges)
			return ExecClearTuple(slot);
		if (st->cur >= 0 &&
			(st->ira != 0 ? cone_next_filtered(st, slot)
			 : index_getnext_slot(st->iscan, ForwardScanDirection, slot)))
			return slot;
		st->cur = st->parallel ? cone_claim(st) : st->cur + 1;
		if (st->cur >= st->nranges)
			return ExecClearTuple(slot);
		cone_range(st, st->cur);
	}
}

/*
 * bitmap: on the first call collect every range's TIDs, then return the rows
 * page by page in physical order.  A lossy page returns all its rows; the
 * plan's quals hold the exact test, and the ranges cover the cone, so that
 * test alone decides -- no range condition needs rechecking.
 */
static void
cone_build_bitmap_of(ConeScanState *st, int from, int to)
{
	st->tbm = tbm_create(work_mem * (Size) 1024, NULL);
	for (int i = from; i < to; i++)
	{
		cone_range(st, i);
		if (st->ira == 0)
			index_getbitmap(st->iscan, st->tbm);
		else
		{
			ItemPointer tid;

			while ((tid = cone_next_tid(st)) != NULL)
				tbm_add_tuples(st->tbm, tid, 1, false);
		}
	}
}

/*
 * The ranges' TIDs into a bitmap: all of them, or in a parallel scan the next
 * chunk this process claims (false when none is left).
 */
static bool
cone_build_bitmap(ConeScanState *st)
{
	if (st->parallel)
	{
		int			c = cone_claim(st);

		if (c >= st->nranges)
			return false;
		cone_build_bitmap_of(st, c, c + 1);
	}
	else
		cone_build_bitmap_of(st, 0, st->nranges);
	return true;
}

#if PG_VERSION_NUM >= 180000
/* PostgreSQL 18: the table AM walks the bitmap itself, through a read stream */
static TupleTableSlot *
cone_next_bitmap(ConeScanState *st, TupleTableSlot *slot)
{
	bool		recheck;
	uint64		lossy = 0,
				exact = 0;

	for (;;)
	{
		if (st->tbm == NULL)
		{
			TBMIterator it;

			if (!cone_build_bitmap(st))
				return ExecClearTuple(slot);
			it = tbm_begin_iterate(st->tbm, NULL, InvalidDsaPointer);
			if (st->hscan == NULL)
				st->hscan = table_beginscan_bm(st->css.ss.ss_currentRelation,
											   st->css.ss.ps.state->es_snapshot, 0, NULL);
			st->hscan->st.rs_tbmiterator = it;
		}
		if (table_scan_bitmap_next_tuple(st->hscan, slot, &recheck, &lossy, &exact))
			return slot;
		if (!st->parallel)
			return ExecClearTuple(slot);
		cone_release_bitmap(st, false);	/* this chunk done: claim the next */
	}
}
#else
static TupleTableSlot *
cone_next_bitmap(ConeScanState *st, TupleTableSlot *slot)
{
	for (;;)
	{
		if (st->tbm == NULL)
		{
			if (!cone_build_bitmap(st))
				return ExecClearTuple(slot);
			st->it = tbm_begin_iterate(st->tbm);
			st->tbmres = NULL;
		}
		if (st->tbmres == NULL)
		{
			if ((st->tbmres = tbm_iterate(st->it)) == NULL)
			{
				if (!st->parallel)
					return ExecClearTuple(slot);
				cone_release_bitmap(st, false);	/* this chunk done: claim the next */
				continue;
			}
			if (!table_scan_bitmap_next_block(st->hscan, st->tbmres))
			{
				st->tbmres = NULL;
				continue;
			}
		}
		if (table_scan_bitmap_next_tuple(st->hscan, st->tbmres, slot))
			return slot;
		st->tbmres = NULL;
	}
}
#endif

static TupleTableSlot *
cone_next(ScanState *ss)
{
	ConeScanState *st = (ConeScanState *) ss;

	if (st->need_cover)
		cone_cover_row(st);
	else if (st->ira != 0 && !st->have_centre)
	{
		/* a constant cone: its centre and radius are evaluated once */
		ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
		double		v[3];
		int			i = 0;
		ListCell   *lc;

		foreach(lc, st->args)
		{
			bool		isnull;

			v[i++] = DatumGetFloat8(ExecEvalExpr((ExprState *) lfirst(lc), econtext, &isnull));
			if (isnull)
				v[2] = -1.0;	/* matches nothing, as the strict test does */
		}
		cone_set_centre(st, v[0], v[1], v[2]);
	}
	return st->bitmap ? cone_next_bitmap(st, ss->ss_ScanTupleSlot)
		: cone_next_ordered(st, ss->ss_ScanTupleSlot);
}

/* the plan's quals hold the exact test; ExecScan applies them to EPQ rows too */
static bool
cone_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
cone_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, cone_next, cone_recheck);
}

/*
 * Releasing the bitmap mode, in the order each version's own BitmapHeapScan
 * uses.  On PostgreSQL 18 the iterator lives in the heap scan, and the heap
 * scan's read stream may still reference the bitmap's pages, so the iterator
 * is ended and the scan reset or ended before the bitmap is freed.
 */
static void
cone_release_bitmap(ConeScanState *st, bool end)
{
#if PG_VERSION_NUM >= 180000
	if (st->hscan)
	{
		if (!tbm_exhausted(&st->hscan->st.rs_tbmiterator))
			tbm_end_iterate(&st->hscan->st.rs_tbmiterator);
		if (end)
			table_endscan(st->hscan);
		else
			table_rescan(st->hscan, NULL);
	}
	if (st->tbm)
		tbm_free(st->tbm);
#else
	if (st->it)
		tbm_end_iterate(st->it);
	if (st->tbm)
		tbm_free(st->tbm);
	st->it = NULL;
	st->tbmres = NULL;
	if (st->hscan)
	{
		if (end)
			table_endscan(st->hscan);
		else
			table_rescan(st->hscan, NULL);
	}
#endif
	st->tbm = NULL;
	if (end)
		st->hscan = NULL;
}

static void
cone_end(CustomScanState *node)
{
	ConeScanState *st = (ConeScanState *) node;

	cone_release_bitmap(st, true);
	if (st->iscan)
		index_endscan(st->iscan);
	if (st->index)
		index_close(st->index, NoLock);
}

static void
cone_rescan(CustomScanState *node)
{
	ConeScanState *st = (ConeScanState *) node;

	st->cur = -1;
	st->need_cover = st->runtime;	/* new outer row, new covering */
	st->local_next = 0;
	cone_release_bitmap(st, false);
	ExecScanReScan(&node->ss);
}

/* a parallel scan's shared state: the next chunk to claim */
static Size
cone_estimate_dsm(CustomScanState *node, ParallelContext *pcxt)
{
	return MAXALIGN(sizeof(pg_atomic_uint32));
}

static void
cone_initialize_dsm(CustomScanState *node, ParallelContext *pcxt, void *coordinate)
{
	ConeScanState *st = (ConeScanState *) node;

	pg_atomic_init_u32((pg_atomic_uint32 *) coordinate, 0);
	st->next = (pg_atomic_uint32 *) coordinate;
}

static void
cone_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt, void *coordinate)
{
	pg_atomic_write_u32((pg_atomic_uint32 *) coordinate, 0);
}

static void
cone_initialize_worker(CustomScanState *node, shm_toc *toc, void *coordinate)
{
	((ConeScanState *) node)->next = (pg_atomic_uint32 *) coordinate;
}

static void
cone_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ConeScanState *st = (ConeScanState *) node;

	ExplainPropertyText("Index", RelationGetRelationName(st->index), es);
	if (!st->runtime)
		ExplainPropertyInteger("Ranges", NULL, st->plan_nranges, es);	/* the plan's covering */
	else if (es->analyze && st->ncoverings > 0)
	{
		/*
		 * An average, from the coverings this process computed: in a parallel
		 * query the workers' are not sent back (only a parallel-aware node
		 * gets shared memory), so it is the leader's share of the probes.
		 */
		ExplainPropertyFloat(node->ss.ps.worker_instrument ? "Ranges per Covering (leader)"
							 : "Ranges per Covering", NULL,
							 st->nranges_total / st->ncoverings, 1, es);
	}
	else
		ExplainPropertyText("Ranges", st->joined ? "per outer row" : "per rescan", es);
	ExplainPropertyText("Mode", st->bitmap ? "bitmap" : "ordered", es);
	if (st->ira != 0)
	{
		TupleDesc	itd = RelationGetDescr(st->index);
		char	   *cols = psprintf("%s, %s",
									NameStr(TupleDescAttr(itd, st->ira - 1)->attname),
									NameStr(TupleDescAttr(itd, st->idec - 1)->attname));

		ExplainPropertyText("Index Filter", cols, es);
		if (es->analyze && node->ss.ps.instrument)
			ExplainPropertyFloat("Rows Removed by Index Filter", NULL,
								 node->ss.ps.instrument->nfiltered2, 0, es);
	}
}

/* ------------------------------------------------------------------ */
/* nearest neighbours: execution                                      */
/* ------------------------------------------------------------------ */

typedef struct NearestRow
{
	double		d;
	bool		isnull;
	HeapTuple	tup;
} NearestRow;

typedef struct NearestScanState
{
	ConeScanState cone;			/* must be first: the ranges and their scans */
	ExprState  *dist;			/* the ORDER BY expression */
	ExprState  *prequal;		/* the conditions safe to test in the ring */
	TupleTableSlot *fetch;		/* a row as the heap returns it */
	double		k0;				/* rows the first ring should hold */
	double		want;			/* rows the query was planned to read */
	double		n_inside,		/* rows read nearer than r, and of them */
				n_passed;		/* those the conditions kept */
	bool		started,
				final,			/* the next ring is the rest of the relation */
				done;			/* no ring after the current one */
	double		ra0,
				dec0;
	double		rprev,			/* the current ring is (rprev, r]; rprev < 0 */
				r;				/* for the first, which starts at the centre */
	double		bound;			/* no ring reads past this radius; < 0: none */
	bool		last;			/* the current ring reaches the bound */
	int			nupper;			/* bounds among the args: the upper ones first */

	/*
	 * The current ring's rows: copies of their heap tuples with their
	 * distance, sorted in memory, while they fit in work_mem; past that, a
	 * tuplesort of the relation's columns and the distance, which can spill.
	 */
	MemoryContext ringcxt;
	NearestRow *rows;
	int			nrows,
				cap,
				next,
				sorted;			/* rows[0 .. sorted) are in order (0.51) */
	double		nout;			/* rows returned so far */
	double		bf_cells,		/* best-first: cells read, split, batches */
				bf_splits,
				bf_batches;
	ExprState  *rowdec;			/* the row's declination, when the distance is
								 * skycell_dist() of a column pair (0.51) */
	ParamExecData *distparam;	/* the distance the projection reads, set as a
								 * row is returned; NULL: it computes it (0.52) */
	Size		mem;
	Tuplesortstate *sort;
	TupleDesc	sortdesc;		/* the relation's columns, then the distance */
	TupleTableSlot *put,
			   *got;

	double		nrings,			/* for EXPLAIN ANALYZE */
				nread,
				nprobes;		/* cells probed for emptiness */
	double		band;			/* after a lower bound: the area of the ring
								 * beyond it, sr; 0 before one */
	IndexScanDesc probe;		/* for those probes; created on first use */
} NearestScanState;

static Node *
nearest_create_state(CustomScan *cscan)
{
	NearestScanState *ns = (NearestScanState *) newNode(sizeof(NearestScanState),
														T_CustomScanState);

	ns->cone.css.methods = &nearest_exec_methods;
	/* rows come out of the ring's buffer as heap tuples, not from a buffer page */
	ns->cone.css.slotOps = &TTSOpsHeapTuple;
	return (Node *) ns;
}

static void
nearest_begin(CustomScanState *node, EState *estate, int eflags)
{
	NearestScanState *ns = (NearestScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Relation	heap = node->ss.ss_currentRelation;
	TupleDesc	hd = RelationGetDescr(heap);

	cone_begin(node, estate, eflags);

	/*
	 * Off a partition, no key range: the cells an all-sky table holds clip
	 * nothing, and reading its two ends costs a query of a few rows a third
	 * of its time (0.44 did, at every query).  A partition reads them, at the
	 * first ring, so that rings over another part of the sky read nothing.
	 */
	if (!heap->rd_rel->relispartition)
	{
		ns->cone.have_keyrange = true;
		ns->cone.kmin = PG_INT64_MIN;
		ns->cone.kmax = PG_INT64_MAX;
		ns->cone.keys_partial = false;
	}
	ns->k0 = DatumGetFloat8(list_nth_node(Const, cscan->custom_private, 9)->constvalue);
	ns->want = DatumGetFloat8(list_nth_node(Const, cscan->custom_private, 10)->constvalue);
	ns->nupper = list_length(cscan->custom_private) > 11
		? DatumGetInt32(list_nth_node(Const, cscan->custom_private, 11)->constvalue)
		: list_length(cscan->custom_exprs) - 3;
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * Without a parent, so that it reads whichever slot it is handed -- the
	 * heap's, not the scan's own -- the way PostgreSQL compiles an index's
	 * expressions.  It holds no sub-plan (adql.c checks).
	 */
	ns->dist = ExecInitExpr((Expr *) lthird(cscan->custom_exprs), NULL);

	/*
	 * The projection reads the distance from the parameter, not computing it
	 * again; not under EvalPlanQual, whose rows the rings did not read.  Its
	 * second compilation costs a few distances: only for queries of more
	 * rows than that.
	 */
	if (list_length(cscan->custom_private) > 12 && node->ss.ps.ps_ProjInfo != NULL &&
		estate->es_epq_active == NULL && ns->want >= 32)
	{
		int			paramid = DatumGetInt32(list_nth_node(Const, cscan->custom_private,
														   12)->constvalue);

		if (paramid >= 0 &&
			tlist_has_expr((List *) cscan->scan.plan.targetlist,
						   (Node *) lthird(cscan->custom_exprs)))
		{
			Param	   *prm = makeNode(Param);
			expr_param_context cx;
			List	   *tl;

			prm->paramkind = PARAM_EXEC;
			prm->paramid = paramid;
			prm->paramtype = FLOAT8OID;
			prm->paramtypmod = -1;
			prm->paramcollid = InvalidOid;
			prm->location = -1;
			cx.expr = (Node *) lthird(cscan->custom_exprs);
			cx.param = prm;
			if (exprType(cx.expr) == FLOAT8OID)
			{
				tl = (List *) expr_to_param_mutator((Node *) cscan->scan.plan.targetlist, &cx);
				node->ss.ps.ps_ProjInfo =
					ExecBuildProjectionInfo(tl, node->ss.ps.ps_ExprContext,
											node->ss.ps.ps_ResultTupleSlot, &node->ss.ps,
											node->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
				ns->distparam = &estate->es_param_exec_vals[paramid];
				ns->distparam->execPlan = NULL;
			}
		}
	}

	/*
	 * When the distance is skycell_dist(ra, dec, ra0, dec0) -- either pair
	 * the row's, the other free of its columns -- the row's declination
	 * alone rejects a row a ring's covering read from beyond the ring: the
	 * distance is never less than the difference in declination.  A
	 * subtraction instead of the trigonometry, for the rows of the cells the
	 * ring's edge crosses (0.51).
	 */
	{
		FuncExpr   *f = (FuncExpr *) lthird(cscan->custom_exprs);

		if (IsA(f, FuncExpr) && list_length(f->args) == 4)
		{
			char	   *name = get_func_name(f->funcid);

			if (name != NULL && strcmp(name, "skycell_dist") == 0)
			{
				Node	   *a = (Node *) list_make2(linitial(f->args), lsecond(f->args)),
						   *b = (Node *) list_make2(lthird(f->args), lfourth(f->args));
				bool		av = contain_var_clause(a),
							bv = contain_var_clause(b);

				if (av != bv)
					ns->rowdec = ExecInitExpr((Expr *) (av ? lsecond(f->args) : lfourth(f->args)),
											  NULL);
			}
		}
	}

	/*
	 * The relation's conditions, tested as a row is read: a row they reject
	 * is never copied or sorted, which matters when they reject the nearest
	 * rows -- each ring would otherwise sort rows only to have the scan's
	 * filter drop them.  Not a volatile one, which would then run twice on
	 * the rows the scan returns, and not one with a sub-plan, which needs the
	 * plan state; ExecScan still tests them all on the rows returned.
	 */
	{
		List	   *safe = NIL;
		ListCell   *lc;

		foreach(lc, cscan->scan.plan.qual)
		{
			Node	   *q = (Node *) lfirst(lc);

			if (!contain_volatile_functions(q) && !contain_subplans(q))
				safe = lappend(safe, q);
		}
		ns->prequal = safe ? ExecInitQual(safe, NULL) : NULL;
	}
	ns->fetch = table_slot_create(heap, &estate->es_tupleTable);
	ns->ringcxt = AllocSetContextCreate(estate->es_query_cxt, "skycell nearest ring",
										ALLOCSET_DEFAULT_SIZES);
	ns->sortdesc = CreateTemplateTupleDesc(hd->natts + 1);
	for (int i = 1; i <= hd->natts; i++)
		TupleDescCopyEntry(ns->sortdesc, i, hd, i);
	TupleDescInitEntry(ns->sortdesc, hd->natts + 1, "distance", FLOAT8OID, -1, 0);
	ns->put = ExecInitExtraTupleSlot(estate, ns->sortdesc, &TTSOpsVirtual);
	ns->got = ExecInitExtraTupleSlot(estate, ns->sortdesc, &TTSOpsMinimalTuple);
}

/* solid angle of a cone of radius r degrees */
static double
cap_area(double r)
{
	return 2.0 * M_PI * (1.0 - cos(fmin(r, 180.0) * M_PI / 180.0));
}

/* radius in degrees of a cone of solid angle a, 180 for the sky and beyond */
static double
cap_radius(double a)
{
	double		c = 1.0 - a / (2.0 * M_PI);

	return (c <= -1.0) ? 180.0 : acos(c) * 180.0 / M_PI;
}

/* one row into the tuplesort, as the relation's columns and the distance */
static void
nearest_put_sorted(NearestScanState *ns, HeapTuple tup, double d, bool isnull)
{
	int			natts = ns->sortdesc->natts - 1;

	ExecClearTuple(ns->put);
	heap_deform_tuple(tup, RelationGetDescr(ns->cone.css.ss.ss_currentRelation),
					  ns->put->tts_values, ns->put->tts_isnull);
	ns->put->tts_values[natts] = Float8GetDatum(d);
	ns->put->tts_isnull[natts] = isnull;
	ExecStoreVirtualTuple(ns->put);
	tuplesort_puttupleslot(ns->sort, ns->put);
}

/* the ring outgrew work_mem: what it holds so far goes into a tuplesort */
static void
nearest_spill(NearestScanState *ns)
{
	AttrNumber	key = ns->sortdesc->natts;
	Oid			lt = Float8LessOperator;
	Oid			coll = InvalidOid;
	bool		nulls_first = false;

	ns->sort = tuplesort_begin_heap(ns->sortdesc, 1, &key, &lt, &coll, &nulls_first,
									work_mem, NULL, TUPLESORT_NONE);
	for (int i = 0; i < ns->nrows; i++)
		nearest_put_sorted(ns, ns->rows[i].tup, ns->rows[i].d, ns->rows[i].isnull);
	ns->nrows = 0;
	MemoryContextReset(ns->ringcxt);
	ns->rows = NULL;
	ns->cap = 0;
}

/* keep a candidate row if it belongs to the current ring */
static void
nearest_consider(NearestScanState *ns, TupleTableSlot *row)
{
	ExprContext *econtext = ns->cone.css.ss.ps.ps_ExprContext;
	bool		isnull;
	double		d;
	Datum		v;
	HeapTuple	tup;
	MemoryContext old;

	ResetExprContext(econtext);
	econtext->ecxt_scantuple = row;
	if (ns->rowdec != NULL && !ns->final)
	{
		Datum		dv = ExecEvalExprSwitchContext(ns->rowdec, econtext, &isnull);
		double		dec = isnull ? 0.0 : DatumGetFloat8(dv);

		/* a valid declination farther from the centre's than the ring reaches */
		if (!isnull && dec >= -90.0 && dec <= 90.0 &&
			fabs(dec - ns->dec0) > ns->r * (1.0 + 1e-9) + 1e-12)
		{
			ns->nread += 1;
			return;
		}
	}
	v = ExecEvalExprSwitchContext(ns->dist, econtext, &isnull);
	d = isnull ? 0.0 : DatumGetFloat8(v);
	ns->nread += 1;
	if (!ns->final)
	{
		/* a NULL or NaN distance sorts last: the final ring's */
		if (isnull || isnan(d) || d > ns->r)
			return;
		if (ns->rprev >= 0 && !(d > ns->rprev))
			return;				/* an inner ring's, returned already */
		ns->n_inside += 1;
	}
	else if (!isnull && !isnan(d) && ns->rprev >= 0 && !(d > ns->rprev))
		return;
	if (ns->prequal && !ExecQual(ns->prequal, econtext))
	{
		InstrCountFiltered1(&ns->cone.css.ss.ps, 1);
		return;
	}
	ns->n_passed += 1;

	if (ns->sort == NULL)
	{
		old = MemoryContextSwitchTo(ns->ringcxt);
		tup = ExecCopySlotHeapTuple(row);
		ns->mem += HEAPTUPLESIZE + tup->t_len + sizeof(NearestRow);
		if (ns->nrows == ns->cap)
		{
			ns->cap = ns->cap ? 2 * ns->cap : 256;
			ns->rows = ns->rows ? repalloc(ns->rows, sizeof(NearestRow) * ns->cap)
				: palloc(sizeof(NearestRow) * ns->cap);
		}
		ns->rows[ns->nrows].d = d;
		ns->rows[ns->nrows].isnull = isnull;
		ns->rows[ns->nrows].tup = tup;
		ns->nrows++;
		MemoryContextSwitchTo(old);
		if (ns->mem > (Size) work_mem * 1024)
			nearest_spill(ns);
		return;
	}
	tup = ExecFetchSlotHeapTuple(row, false, NULL);
	nearest_put_sorted(ns, tup, d, isnull);
}

/*
 * By distance, as float8's ordering has it (NaN after every number, and equal
 * to NaN), NULL last.  A sort specialised for the rows, with the comparison
 * inline: through qsort()'s function pointer, sorting was a third of a ring.
 */
static inline int
nearest_row_cmp(const NearestRow *x, const NearestRow *y)
{
	if (x->isnull || y->isnull)
		return (int) x->isnull - (int) y->isnull;
	if (isnan(x->d) || isnan(y->d))
		return (int) isnan(x->d) - (int) isnan(y->d);
	return (x->d > y->d) - (x->d < y->d);
}

#define ST_SORT nearest_sort_rows_any
#define ST_ELEMENT_TYPE NearestRow
#define ST_COMPARE(a, b) nearest_row_cmp(a, b)
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

/* the same for rows whose distances are all numbers: a comparison of two (0.52) */
#define ST_SORT nearest_sort_rows_finite
#define ST_ELEMENT_TYPE NearestRow
#define ST_COMPARE(a, b) (((a)->d > (b)->d) - ((a)->d < (b)->d))
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

static void
nearest_sort_rows(NearestRow *rows, int n)
{
	for (int i = 0; i < n; i++)
	{
		if (rows[i].isnull || isnan(rows[i].d))
		{
			nearest_sort_rows_any(rows, n);
			return;
		}
	}
	nearest_sort_rows_finite(rows, n);
}

/*
 * Rows rearranged so that rows[0 .. m) are the m nearest, in no order, and
 * every row after them is no nearer than any of them (quickselect, median of
 * three).  A ring holds more rows than the query was planned to read, about
 * twice where the rows cluster, and sorting them all was a sixth of a search:
 * only the rows needed are sorted, the rest if the query asks for them (0.51).
 */
static void
nearest_select_rows(NearestRow *rows, int n, int m)
{
	int			lo = 0,
				hi = n - 1;

	while (hi > lo)
	{
		int			mid = lo + (hi - lo) / 2,
					i,
					j;
		NearestRow	t,
					pivot;

		/* the median of rows[lo], rows[mid], rows[hi] as the pivot */
		if (nearest_row_cmp(&rows[mid], &rows[lo]) < 0)
		{
			t = rows[mid]; rows[mid] = rows[lo]; rows[lo] = t;
		}
		if (nearest_row_cmp(&rows[hi], &rows[lo]) < 0)
		{
			t = rows[hi]; rows[hi] = rows[lo]; rows[lo] = t;
		}
		if (nearest_row_cmp(&rows[hi], &rows[mid]) < 0)
		{
			t = rows[hi]; rows[hi] = rows[mid]; rows[mid] = t;
		}
		pivot = rows[mid];
		i = lo;
		j = hi;
		while (i <= j)
		{
			while (nearest_row_cmp(&rows[i], &pivot) < 0)
				i++;
			while (nearest_row_cmp(&rows[j], &pivot) > 0)
				j--;
			if (i <= j)
			{
				t = rows[i]; rows[i] = rows[j]; rows[j] = t;
				i++;
				j--;
			}
		}
		/* rows[lo .. j] <= pivot <= rows[i .. hi] */
		if (m <= j)
			hi = j;
		else if (m >= i)
			lo = i;
		else
			return;
	}
}

/*
 * The radius whose cone the density expects to hold k rows around the centre:
 * the density of the finest HEALPix cell round the centre that still holds a
 * few times k rows -- a crowded field gets a small first ring, empty sky a
 * wide one.  A lookup per order, where computing coverings to size the ring
 * cost more than reading it.
 */
static double
nearest_first_radius(NearestScanState *ns, double k, double n)
{
	const sc_density *d = &ns->cone.dens;
	int64_t		pix = sc_ang2pix(SC_MAX_ORDER, ns->ra0, ns->dec0);
	double		rho = n / (4.0 * M_PI);

	for (int m = 0; m <= SC_MAX_ORDER; m++)
	{
		int64_t		p = pix >> (2 * (SC_MAX_ORDER - m));
		double		rows = sc_density_rows(d, sc_pix_lo(m, p), sc_pix_hi(m, p));

		if (rows < 4.0 * k)
			break;
		rho = rows / (4.0 * M_PI / (12.0 * pow(4.0, m)));
	}
	return cap_radius(k / fmax(rho, 1e-300));
}

/*
 * The cells wholly inside the cone (ra0, dec0, r), as sorted, disjoint ranges
 * of order-29 cells (lo, hi pairs, palloc'd into *out): from the twelve base
 * cells down, refining only the cells the cone's edge crosses, to the order
 * whose cells are a sixteenth of r across.  The cap test's IN is never wrong,
 * so every row of these cells is nearer the centre than r.  Stops refining
 * after NEAREST_INTERIOR_STEPS classifications: fewer cells, still all inside.
 */
#define NEAREST_INTERIOR_STEPS 4096

static int
cone_interior(double ra0, double dec0, double r, int64 **out)
{
	sc_region	reg;
	struct
	{
		int			order;
		int64_t		pix;
	}			stack[12 + 3 * (SC_MAX_ORDER + 1)];
	int			top = 0,
				n = 0,
				cap = 64,
				steps = 0,
				maxo;
	int64	   *b = palloc(sizeof(int64) * 2 * cap);

	if (sc_region_cone(&reg, ra0, dec0, r, true) != NULL)
	{
		*out = b;
		return 0;
	}
	maxo = (int) floor(log2(1.0233267079464885 / fmax(r * M_PI / 180.0 / 16.0, 1e-12)));
	maxo = Max(0, Min(maxo, SC_MAX_ORDER));
	/* children pushed last to first, so that they come off in cell order */
	for (int i = 11; i >= 0; i--)
	{
		stack[top].order = 0;
		stack[top++].pix = i;
	}
	while (top > 0)
	{
		int			order = stack[--top].order;
		int64_t		pix = stack[top].pix;
		double		fo;
		sc_class	cls = sc_region_classify_cap(&reg, order, pix, &fo);

		steps++;
		if (cls == SC_IN)
		{
			int64		lo = sc_pix_lo(order, pix),
						hi = sc_pix_hi(order, pix);

			if (n > 0 && b[2 * n - 1] + 1 == lo)
				b[2 * n - 1] = hi;
			else
			{
				if (n == cap)
				{
					cap *= 2;
					b = repalloc(b, sizeof(int64) * 2 * cap);
				}
				b[2 * n] = lo;
				b[2 * n + 1] = hi;
				n++;
			}
		}
		else if (cls == SC_PARTIAL && order < maxo && steps < NEAREST_INTERIOR_STEPS)
		{
			for (int c = 3; c >= 0; c--)
			{
				stack[top].order = order + 1;
				stack[top++].pix = 4 * pix + c;
			}
		}
	}
	*out = b;
	return n;
}

/* ranges a less ranges x, both sorted, disjoint, inclusive (lo, hi) pairs */
static int
ranges_minus(const int64 *a, int na, const int64 *x, int nx, int64 *out)
{
	int			n = 0,
				j = 0;

	for (int i = 0; i < na; i++)
	{
		int64		lo = a[2 * i],
					hi = a[2 * i + 1];

		while (j < nx && x[2 * j + 1] < lo)
			j++;
		for (int k = j; k < nx && x[2 * k] <= hi && lo <= hi; k++)
		{
			if (x[2 * k] > lo)
			{
				out[2 * n] = lo;
				out[2 * n + 1] = x[2 * k] - 1;
				n++;
			}
			if (x[2 * k + 1] >= hi)
				lo = hi + 1;	/* the rest of the range is excluded */
			else
				lo = Max(lo, x[2 * k + 1] + 1);
		}
		if (lo <= hi)
		{
			out[2 * n] = lo;
			out[2 * n + 1] = hi;
			n++;
		}
	}
	return n;
}

/* drop the current ring's rows */
static void
nearest_clear_ring(NearestScanState *ns)
{
	ExecClearTuple(ns->cone.css.ss.ss_ScanTupleSlot);
	if (ns->sort)
		tuplesort_end(ns->sort);
	ns->sort = NULL;
	if (ns->ringcxt)
		MemoryContextReset(ns->ringcxt);
	ns->rows = NULL;
	ns->nrows = ns->cap = ns->next = ns->sorted = 0;
	ns->mem = 0;
}

/*
 * After a ring that held nothing, how far the rows really start.  A best-first
 * walk over HEALPix cells, nearest first, each probed for an index entry: an
 * empty cell is dropped, an occupied one split, until an occupied cell is
 * small next to its distance.  Every cell nearer than it was found empty, so
 * its least distance from the centre is a lower bound on every row's -- a
 * proof, from the index alone, with no statistics.  A partition holding
 * another part of the sky, or empty sky round the centre, is then crossed in
 * one step where rings grown blindly read past the rows by up to their own
 * growth.  The walk is capped; the least distance left in its queue is a
 * lower bound all the same.  Returns it in degrees, with *at the cell where
 * rows were found (the centre itself when the cap was reached).
 */
#define NEAREST_PROBE_BUDGET 4000

typedef struct
{
	double		d;				/* least distance from the centre, radians */
	int64		pix;
	int			order;
} probe_cell;

static void
probe_push(probe_cell **h, int *n, int *cap, probe_cell c)
{
	int			i;

	if (*n == *cap)
	{
		*cap *= 2;
		*h = repalloc(*h, sizeof(probe_cell) * *cap);
	}
	i = (*n)++;
	while (i > 0 && (*h)[(i - 1) / 2].d > c.d)
	{
		(*h)[i] = (*h)[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	(*h)[i] = c;
}

static probe_cell
probe_pop(probe_cell *h, int *n)
{
	probe_cell	top = h[0],
				last = h[--(*n)];
	int			i = 0;

	for (;;)
	{
		int			c = 2 * i + 1;

		if (c >= *n)
			break;
		if (c + 1 < *n && h[c + 1].d < h[c].d)
			c++;
		if (h[c].d >= last.d)
			break;
		h[i] = h[c];
		i = c;
	}
	if (*n > 0)
		h[i] = last;
	return top;
}

/* does the index hold any entry in cell (order, pix)? */
static bool
nearest_cell_occupied(NearestScanState *ns, int order, int64 pix)
{
	ConeScanState *st = &ns->cone;
	ScanKeyData keys[2];

	if (ns->probe == NULL)
	{
		Relation	heap = st->css.ss.ss_currentRelation;
		EState	   *estate = st->css.ss.ps.state;

#if PG_VERSION_NUM >= 180000
		ns->probe = index_beginscan(heap, st->index, estate->es_snapshot, NULL, 2, 0);
#else
		ns->probe = index_beginscan(heap, st->index, estate->es_snapshot, 2, 0);
#endif
	}
	memcpy(keys, st->keys, sizeof(keys));
	keys[0].sk_argument = Int64GetDatum(sc_pix_lo(order, pix));
	keys[1].sk_argument = Int64GetDatum(sc_pix_hi(order, pix));
	index_rescan(ns->probe, keys, 2, NULL, 0);
	ns->nprobes += 1;
	return index_getnext_tid(ns->probe, ForwardScanDirection) != NULL;
}

static double
nearest_lower_bound(NearestScanState *ns, sc_vec3 *at)
{
	sc_vec3		c = sc_radec2vec(ns->ra0, ns->dec0);
	int			n = 0,
				cap = 256,
				pops = 0;
	probe_cell *h = palloc(sizeof(probe_cell) * cap);
	double		lb = M_PI;

	*at = c;
	for (int f = 0; f < 12; f++)
		probe_push(&h, &n, &cap, (probe_cell) {
			fmax(0.0, sc_angle(c, sc_pix2vec(0, f)) - sc_pixrad(0) - 1e-12), f, 0});
	while (n > 0)
	{
		probe_cell	p;

		if (++pops > NEAREST_PROBE_BUDGET)
		{
			lb = h[0].d;		/* nothing nearer than the queue's nearest */
			break;
		}
		p = probe_pop(h, &n);
		if (!nearest_cell_occupied(ns, p.order, p.pix))
			continue;
		/* occupied, and small next to its distance: the rows start about here */
		if (p.order >= SC_MAX_ORDER || 2.0 * sc_pixrad(p.order) <= 0.02 * fmax(p.d, sc_pixrad(SC_MAX_ORDER)))
		{
			lb = p.d;
			*at = sc_pix2vec(p.order, p.pix);
			break;
		}
		for (int k = 0; k < 4; k++)
		{
			int64		cp = 4 * p.pix + k;

			probe_push(&h, &n, &cap, (probe_cell) {
				fmax(0.0, sc_angle(c, sc_pix2vec(p.order + 1, cp)) - sc_pixrad(p.order + 1) - 1e-12),
				cp, p.order + 1});
		}
	}
	pfree(h);
	return lb * (180.0 / M_PI);
}

/* the density round v: the finest cell round it that still holds a few times k rows */
static double
nearest_density_at(NearestScanState *ns, sc_vec3 v, double k, double n)
{
	const sc_density *d = &ns->cone.dens;
	int64_t		pix = sc_vec2pix(SC_MAX_ORDER, v);
	double		rho = n / (4.0 * M_PI);

	for (int m = 0; m <= SC_MAX_ORDER; m++)
	{
		int64_t		p = pix >> (2 * (SC_MAX_ORDER - m));
		double		rows = sc_density_rows(d, sc_pix_lo(m, p), sc_pix_hi(m, p));

		if (rows < 4.0 * k)
			break;
		rho = rows / (4.0 * M_PI / (12.0 * pow(4.0, m)));
	}
	return rho;
}

/* collect and sort the next ring's rows, and choose the one after it */
/*
 * Best-first first ring (skycell.nearest_bestfirst, 0.52).
 *
 * Instead of a cone sized from the density and covered whole, a queue of
 * HEALPix cells ordered by the least distance a point of each can have from
 * the centre.  A cell the statistics expect to hold many rows is split into
 * its four children, without touching the index; the others are gathered
 * into a batch, read through one TID bitmap when the batch is expected to
 * hold the rows still wanted, their exact distances computed.  Once k rows
 * are held, D_k, the k-th distance, bounds the answer: a row farther is never
 * copied, and when the nearest cell left in the queue is farther than D_k
 * every row within D_k has been read.  Those rows are then the first ring --
 * a ring of radius D_k, ties included -- and the rings carry on from there if
 * the query wants more.  The NESTED cell ids make every cell, at any order,
 * one range of the index.
 */
static bool nearest_bestfirst = true;

/* the least distance from c (unit vector) of any point of the cell, degrees; a hair low */
static double
cell_lower_bound(sc_vec3 c, int order, int64 pix)
{
	double		d = sc_angle(c, sc_pix2vec(order, pix)) - sc_pixrad(order);

	return d <= 0 ? 0.0 : fmax(0.0, d * (180.0 / M_PI) * (1.0 - 1e-9) - 1e-12);
}

static int
bestfirst_range_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return (x > y) - (x < y);
}

/* the greatest distance from c of any point of the cell, degrees; a hair high */
static double
cell_upper_bound(sc_vec3 c, int order, int64 pix)
{
	double		d = sc_angle(c, sc_pix2vec(order, pix)) + sc_pixrad(order);

	return d * (180.0 / M_PI) * (1.0 + 1e-9) + 1e-12;
}

/* a batch's rows: their distances, the rows within the bound copied */
static void
bestfirst_read_batch(NearestScanState *ns, int64 *b, int nb, double *dk)
{
	ConeScanState *st = &ns->cone;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	EState	   *estate = st->css.ss.ps.state;

	if (nb > st->rt_cap)
	{
		st->rt_cap = Max(nb, 2 * st->rt_cap);
		st->rt_bounds = (st->rt_bounds == NULL)
			? MemoryContextAlloc(estate->es_query_cxt, sizeof(int64) * 2 * st->rt_cap)
			: repalloc(st->rt_bounds, sizeof(int64) * 2 * st->rt_cap);
	}
	/* in key order, adjacent cells (siblings read whole) merged: one index descent each */
	qsort(b, nb, sizeof(int64) * 2, bestfirst_range_cmp);
	st->nranges = 0;
	for (int i = 0; i < nb; i++)
	{
		int64		lo = Max(b[2 * i], st->kmin),
					hi = Min(b[2 * i + 1], st->kmax);

		if (lo > hi)
			continue;
		if (st->nranges > 0 && st->rt_bounds[2 * st->nranges - 1] + 1 >= lo)
		{
			st->rt_bounds[2 * st->nranges - 1] = Max(hi, st->rt_bounds[2 * st->nranges - 1]);
			continue;
		}
		st->rt_bounds[2 * st->nranges] = lo;
		st->rt_bounds[2 * st->nranges + 1] = hi;
		st->nranges++;
	}
	st->bounds = st->rt_bounds;
	st->cur = -1;
	cone_release_bitmap(st, false);
	ns->bf_batches += 1;
	for (;;)
	{
		TupleTableSlot *row = cone_next_bitmap(st, ns->fetch);
		bool		isnull;
		double		d;
		Datum		v;

		if (TupIsNull(row))
			break;
		ns->nread += 1;
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = row;
		if (ns->rowdec != NULL && isfinite(*dk))
		{
			Datum		dv = ExecEvalExprSwitchContext(ns->rowdec, econtext, &isnull);
			double		dec = isnull ? 0.0 : DatumGetFloat8(dv);

			if (!isnull && dec >= -90.0 && dec <= 90.0 &&
				fabs(dec - ns->dec0) > *dk * (1.0 + 1e-9) + 1e-12)
				continue;
		}
		v = ExecEvalExprSwitchContext(ns->dist, econtext, &isnull);
		d = isnull ? 0.0 : DatumGetFloat8(v);
		/* a NULL or NaN distance sorts last: the final ring's */
		if (isnull || isnan(d) || d > *dk)
			continue;
		if (ns->prequal && !ExecQual(ns->prequal, econtext))
		{
			InstrCountFiltered1(&ns->cone.css.ss.ps, 1);
			continue;
		}
		{
			MemoryContext old = MemoryContextSwitchTo(ns->ringcxt);

			if (ns->nrows == ns->cap)
			{
				ns->cap = ns->cap ? 2 * ns->cap : 256;
				ns->rows = ns->rows ? repalloc(ns->rows, sizeof(NearestRow) * ns->cap)
					: palloc(sizeof(NearestRow) * ns->cap);
			}
			ns->rows[ns->nrows].d = d;
			ns->rows[ns->nrows].isnull = false;
			ns->rows[ns->nrows].tup = ExecCopySlotHeapTuple(row);
			ns->nrows++;
			MemoryContextSwitchTo(old);
		}
	}
	ExecClearTuple(ns->fetch);
}

/* with need rows held: D_k, and the rows beyond it dropped */
static void
bestfirst_bound(NearestScanState *ns, int need, double *dk)
{
	double		m = 0;
	int			k = 0;

	if (ns->nrows < need)
		return;
	if (ns->nrows > need)
		nearest_select_rows(ns->rows, ns->nrows, need);
	for (int i = 0; i < need; i++)
		m = fmax(m, ns->rows[i].d);
	*dk = m;
	/* keep every row within it, ties included */
	for (int i = 0; i < ns->nrows; i++)
	{
		if (ns->rows[i].d <= m)
			ns->rows[k++] = ns->rows[i];
		else
			heap_freetuple(ns->rows[i].tup);
	}
	ns->nrows = k;
}

/*
 * Best-first holds the rows it reads in memory, with no tuplesort to spill
 * to: used when what it may hold -- a few times the rows wanted, at the
 * heap's average width -- fits in work_mem.  Rings, which spill, otherwise.
 */
static bool
bestfirst_fits(NearestScanState *ns)
{
	Relation	rel = ns->cone.css.ss.ss_currentRelation;
	double		need = fmax(1.0, ns->want - ns->nout),
				tuples = rel->rd_rel->reltuples,
				pages = rel->rd_rel->relpages,
				width = (tuples > 0 && pages > 0) ? pages * BLCKSZ / tuples : 256.0;

	return 3.0 * need * (width + HEAPTUPLESIZE + sizeof(NearestRow)) <= work_mem * 1024.0;
}

static void
nearest_bestfirst_ring(NearestScanState *ns)
{
	ConeScanState *st = &ns->cone;
	sc_vec3		c = sc_radec2vec(ns->ra0, ns->dec0);
	int			need = (int) fmin(1e9, fmax(1.0, ns->want - ns->nout));
	double		split_rows = fmax(8.0, need / 8.0),
				edge_rows = 8.0,
				dk = INFINITY,
				batch_rows = 0,
				maxd = -1.0;
	int			hn = 0,
				hcap = 256,
				nb = 0,
				bcap = 64;
	probe_cell *h = palloc(sizeof(probe_cell) * hcap);
	int64	   *b = palloc(sizeof(int64) * 2 * bcap);
	bool		exhausted;

	/* a partition's ranges are clipped to the keys it holds */
	if (!st->have_keyrange)
		cone_key_range(st);
	for (int f = 0; f < 12; f++)
		probe_push(&h, &hn, &hcap, (probe_cell) {cell_lower_bound(c, 0, f), f, 0});
	for (;;)
	{
		probe_cell	cell;
		int64		lo,
					hi;
		double		exp;

		/* nothing left nearer than D_k: read what is gathered, or stop */
		if (hn == 0 || h[0].d > dk)
		{
			if (nb == 0)
				break;
			bestfirst_read_batch(ns, b, nb, &dk);
			bestfirst_bound(ns, need, &dk);
			nb = 0;
			batch_rows = 0;
			continue;
		}
		cell = probe_pop(h, &hn);
		lo = sc_pix_lo(cell.order, cell.pix);
		hi = sc_pix_hi(cell.order, cell.pix);
		if (hi < st->kmin || lo > st->kmax)
			continue;			/* none of the index's keys: another partition's */
		exp = sc_density_rows(&st->dens, lo, hi);
		/*
		 * Split a cell while it is expected to hold many rows; once D_k is
		 * known, a cell wholly within it is read whole, and one crossing it
		 * is split finer, so that little is read beyond it.
		 */
		if (cell.order < SC_MAX_ORDER &&
			(!isfinite(dk) ? exp > split_rows
			 : exp > edge_rows && cell_upper_bound(c, cell.order, cell.pix) > dk))
		{
			ns->bf_splits += 1;
			for (int ch = 0; ch < 4; ch++)
			{
				int64		p = 4 * cell.pix + ch;

				probe_push(&h, &hn, &hcap,
						   (probe_cell) {cell_lower_bound(c, cell.order + 1, p), p, cell.order + 1});
			}
			continue;
		}
		if (nb == bcap)
		{
			bcap *= 2;
			b = repalloc(b, sizeof(int64) * 2 * bcap);
		}
		b[2 * nb] = lo;
		b[2 * nb + 1] = hi;
		nb++;
		ns->bf_cells += 1;
		batch_rows += exp;
		/* before D_k is known, read once the batch should complete the k; after, gather all */
		if (!isfinite(dk) && batch_rows >= fmax(16.0, need - ns->nrows))
		{
			bestfirst_read_batch(ns, b, nb, &dk);
			bestfirst_bound(ns, need, &dk);
			nb = 0;
			batch_rows = 0;
		}
	}
	exhausted = (hn == 0);
	for (int i = 0; i < ns->nrows; i++)
		maxd = fmax(maxd, ns->rows[i].d);
	ns->n_inside = ns->n_passed = ns->nrows;
	if (exhausted && !isfinite(dk))
	{
		/* the whole relation read: what is left has a NULL or NaN distance */
		ns->rprev = maxd;
		ns->final = true;
	}
	else
	{
		ns->rprev = isfinite(dk) ? dk : maxd;
		ns->r = cap_radius(fmin(4.0 * M_PI, 2.0 * cap_area(fmax(ns->rprev, 1e-9))));
		ns->final = ns->r >= 180.0 || ns->nrows * 2.0 >= 0.5 * fmax(st->dens.ntotal, 1.0);
	}
	pfree(h);
	pfree(b);
}

static void
nearest_ring(NearestScanState *ns)
{
	ConeScanState *st = &ns->cone;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	EState	   *estate = st->css.ss.ps.state;
	double		n;

	nearest_clear_ring(ns);
	cone_load_density(st);
	n = fmax(st->dens.ntotal, 1.0);
	if (!ns->started)
	{
		bool		null1,
					null2;
		Datum		a = ExecEvalExpr((ExprState *) linitial(st->args), econtext, &null1);
		Datum		b = ExecEvalExpr((ExprState *) lsecond(st->args), econtext, &null2);
		double		lower;

		ns->started = true;
		ns->rprev = -1.0;
		ns->band = 0;
		ns->n_inside = ns->n_passed = 0;
		ns->last = false;

		/*
		 * The radii the conditions bound the answer to, the least of them a
		 * hair wider, as the rings are: rows the conditions keep are no
		 * farther.  A NULL one keeps no row at all.
		 */
		ns->bound = -1.0;
		lower = -1.0;
		for (int i = 3; i < list_length(st->args); i++)
		{
			bool		isnull;
			Datum		v = ExecEvalExpr((ExprState *) list_nth(st->args, i), econtext, &isnull);
			double		b = isnull ? -1.0 : DatumGetFloat8(v);

			if (isnull || isnan(b))
			{
				ns->done = true;
				return;
			}
			if (i >= 3 + ns->nupper)
			{
				/* a lower bound (0.45): no row nearer passes; a hair narrower */
				if (b > lower)
					lower = b;
				continue;
			}
			b = fmin(180.0, fmax(0.0, b) * (1.0 + 1e-9) + 1e-12);
			if (ns->bound < 0 || b < ns->bound)
				ns->bound = b;
		}
		if (lower >= 180.0 || (ns->bound >= 0 && lower >= 0 && lower >= ns->bound))
		{
			ns->done = true;		/* no distance is both beyond and within */
			return;
		}
		ns->ra0 = null1 ? 0.0 : DatumGetFloat8(a);
		ns->dec0 = null2 ? 0.0 : DatumGetFloat8(b);
		/*
		 * No centre to search around: every distance is NULL, or whatever
		 * the expression makes of it, and the one ring is the relation.
		 */
		if (null1 || null2 || !isfinite(ns->ra0) || !(ns->dec0 >= -90.0 && ns->dec0 <= 90.0))
			ns->final = true;
		else
		{
			ns->r = nearest_first_radius(ns, ns->k0, n);

			/*
			 * Beyond a lower bound the first ring is the annulus from it
			 * that holds as many rows, at the density round the centre.
			 */
			if (lower > 0)
			{
				ns->rprev = lower * (1.0 - 1e-9) - 1e-12;
				if (ns->rprev > 0)
					ns->r = cap_radius(cap_area(ns->rprev) + cap_area(ns->r));
				else
					ns->rprev = -1.0;
			}
			ns->final = ns->r >= 180.0 || ns->k0 >= 0.5 * n;
			if (ns->bound >= 0 && (ns->final || ns->r >= ns->bound))
			{
				ns->r = ns->bound;
				ns->final = false;
				ns->last = true;
			}
		}
	}

	/* the first ring best-first: an unordered heap, no bounds, rows that fit in memory */
	if (nearest_bestfirst && ns->nrings == 0 && !ns->final && ns->rprev < 0 &&
		ns->bound < 0 && st->bitmap && !st->parallel && bestfirst_fits(ns))
	{
		nearest_bestfirst_ring(ns);
		goto finish;
	}

	if (ns->final)
	{
		TableScanDesc seq = table_beginscan(st->css.ss.ss_currentRelation,
											estate->es_snapshot, 0, NULL);

		while (table_scan_getnextslot(seq, ForwardScanDirection, ns->fetch))
			nearest_consider(ns, ns->fetch);
		table_endscan(seq);
		ns->done = true;
	}
	else
	{
		double		grow;

		/* a hair wider than the ring, as for a distance comparison */
		(void) cone_cover_at(st, ns->ra0, ns->dec0, fmin(180.0, ns->r * (1.0 + 1e-9) + 1e-12));

		/*
		 * Less the cells wholly inside the previous ring's cone -- a hair
		 * narrower than it -- whose rows were all returned already: a ring
		 * then reads its own band, and the cells the previous edge crossed.
		 */
		if (ns->rprev > 0 && ns->rprev * (1.0 - 1e-9) - 1e-12 > 0)
		{
			MemoryContext old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
			int64	   *inner;
			int			ni = cone_interior(ns->ra0, ns->dec0,
										   ns->rprev * (1.0 - 1e-9) - 1e-12, &inner);

			if (ni > 0 && st->nranges > 0)
			{
				int64	   *b = palloc(sizeof(int64) * 2 * (st->nranges + ni));
				int			nb = ranges_minus(st->bounds, st->nranges, inner, ni, b);

				MemoryContextSwitchTo(old);
				if (nb > st->rt_cap)
				{
					st->rt_cap = Max(nb, 2 * st->rt_cap);
					st->rt_bounds = repalloc(st->rt_bounds, sizeof(int64) * 2 * st->rt_cap);
				}
				memcpy(st->rt_bounds, b, sizeof(int64) * 2 * nb);
				st->bounds = st->rt_bounds;
				st->nranges = nb;
			}
			MemoryContextSwitchTo(old);
		}
		st->cur = -1;
		cone_release_bitmap(st, false);
		for (;;)
		{
			TupleTableSlot *row = st->bitmap ? cone_next_bitmap(st, ns->fetch)
				: cone_next_ordered(st, ns->fetch);

			if (TupIsNull(row))
				break;
			nearest_consider(ns, row);
		}

		/*
		 * The next cone: grown to where, at the density and the share the
		 * conditions kept so far, the rows kept would be what the query was
		 * planned to read, with a margin -- at least twice the area, and
		 * twice while the conditions have kept nothing: a ring too wide
		 * costs every row it reads past the ones needed, one too narrow only
		 * a covering and a few descents.  Sixteen times while the cones have
		 * held nothing.  Once that reaches most of the relation, the next
		 * ring is the rest of it.
		 */
		if (ns->n_inside <= 0)
		{
			/*
			 * Nothing yet: find where the rows start (nearest_lower_bound), and
			 * make the next ring the band from there that holds the rows the
			 * query was planned to read at the density found there.  The rows
			 * nearer than the bound do not exist, so the ring starts at it.
			 */
			sc_vec3		at = {0, 0, 0};
			double		lb = ns->band > 0 ? ns->r : nearest_lower_bound(ns, &at),
						from = fmax(lb, ns->r),
						rn;

			/*
			 * The band beyond the bound: sized for the rows at the density
			 * found there, and eight times its area after each one that
			 * holds nothing -- a thin annulus far from the centre meets a
			 * partition's rows over a sliver of its area -- but not twice
			 * the cone's area, which there would reach past the rows by tens
			 * of degrees.  The bound is searched for once a scan: past it,
			 * the rings themselves have shown how far there is nothing.
			 */
			if (ns->band <= 0)
			{
				double		k = fmax(NEAREST_MIN_ROWS, NEAREST_MARGIN * ns->want);

				ns->band = k / fmax(nearest_density_at(ns, at, k, n), 1e-300);
			}
			else
				ns->band *= 8.0;
			rn = cap_radius(fmin(4.0 * M_PI, cap_area(from) + ns->band));
			grow = fmax(1.0 + 1e-9, cap_area(rn) / fmax(cap_area(ns->r), 1e-300));
			ns->rprev = fmax(ns->r, lb * (1.0 - 1e-9) - 1e-12);
		}
		else
		{
			if (ns->n_passed <= 0)
				grow = 2.0;
			else
				grow = fmin(64.0, fmax(2.0, NEAREST_MARGIN * ns->want / ns->n_passed));
			ns->rprev = ns->r;
		}
		ns->r = cap_radius(cap_area(ns->r) * grow);
		ns->final = ns->r >= 180.0 || ns->n_inside * grow >= 0.5 * n;
		if (ns->last)
			ns->done = true;	/* that ring reached the bound */
		else if (ns->bound >= 0 && (ns->final || ns->r >= ns->bound))
		{
			ns->r = ns->bound;
			ns->final = false;
			ns->last = true;
		}
	}
finish:
	ExecClearTuple(ns->fetch);
	ResetExprContext(econtext);
	if (ns->sort)
		tuplesort_performsort(ns->sort);
	else
	{
		/* the rows the query still wants, in order; the rest when asked for */
		double		need = fmax(1.0, ns->want - ns->nout);

		if (ns->nrows > 1 && need < 0.5 * ns->nrows)
		{
			nearest_select_rows(ns->rows, ns->nrows, (int) need);
			nearest_sort_rows(ns->rows, (int) need);
			ns->sorted = (int) need;
		}
		else
		{
			if (ns->nrows > 1)
				nearest_sort_rows(ns->rows, ns->nrows);
			ns->sorted = ns->nrows;
		}
	}
	ns->nrings += 1;
}

static TupleTableSlot *
nearest_next(ScanState *ss)
{
	NearestScanState *ns = (NearestScanState *) ss;
	TupleTableSlot *out = ss->ss_ScanTupleSlot;

	for (;;)
	{
		if (ns->sort == NULL && ns->next < ns->nrows)
		{
			if (ns->next >= ns->sorted)
			{
				/* past the rows the query was planned to read: the rest, in order */
				nearest_sort_rows(ns->rows + ns->sorted, ns->nrows - ns->sorted);
				ns->sorted = ns->nrows;
			}
			ns->nout += 1;
			if (ns->distparam != NULL)
			{
				ns->distparam->value = Float8GetDatum(ns->rows[ns->next].d);
				ns->distparam->isnull = ns->rows[ns->next].isnull;
			}
			ExecStoreHeapTuple(ns->rows[ns->next++].tup, out, false);
			return out;
		}
		if (ns->sort != NULL && tuplesort_gettupleslot(ns->sort, true, false, ns->got, NULL))
		{
			slot_getallattrs(ns->got);
			if (ns->distparam != NULL)
			{
				int			natts = ns->sortdesc->natts - 1;

				ns->distparam->value = ns->got->tts_values[natts];
				ns->distparam->isnull = ns->got->tts_isnull[natts];
			}
			ExecStoreHeapTuple(heap_form_tuple(out->tts_tupleDescriptor, ns->got->tts_values,
											   ns->got->tts_isnull), out, true);
			return out;
		}
		if (ns->done)
			return ExecClearTuple(out);
		nearest_ring(ns);
	}
}

static TupleTableSlot *
nearest_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, nearest_next, cone_recheck);
}

static void
nearest_reset(NearestScanState *ns)
{
	nearest_clear_ring(ns);
	ns->started = ns->final = ns->done = false;
	ns->nout = 0;
	ns->cone.cur = -1;
}

static void
nearest_end(CustomScanState *node)
{
	NearestScanState *ns = (NearestScanState *) node;

	if (ns->probe != NULL)
	{
		index_endscan(ns->probe);
		ns->probe = NULL;
	}
	nearest_reset((NearestScanState *) node);
	cone_end(node);
}

static void
nearest_rescan(CustomScanState *node)
{
	NearestScanState *ns = (NearestScanState *) node;

	nearest_reset(ns);
	cone_release_bitmap(&ns->cone, false);
	ExecScanReScan(&node->ss);
}

static void
nearest_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	NearestScanState *ns = (NearestScanState *) node;

	ExplainPropertyText("Index", RelationGetRelationName(ns->cone.index), es);
	ExplainPropertyText("Mode", ns->cone.bitmap ? "bitmap" : "ordered", es);
	if (es->analyze)
	{
		double		loops = (node->ss.ps.instrument && node->ss.ps.instrument->nloops > 0)
			? node->ss.ps.instrument->nloops : 1.0;

		ExplainPropertyFloat("Rings", NULL, ns->nrings / loops, 1, es);
		ExplainPropertyFloat("Rows Read by Rings", NULL, ns->nread / loops, 0, es);
		ExplainPropertyFloat("Rows Inside Rings", NULL, ns->n_inside / loops, 0, es);
		if (ns->bf_batches > 0)
		{
			ExplainPropertyFloat("Best-first Cells Read", NULL, ns->bf_cells / loops, 0, es);
			ExplainPropertyFloat("Best-first Cells Split", NULL, ns->bf_splits / loops, 0, es);
			ExplainPropertyFloat("Best-first Batches", NULL, ns->bf_batches / loops, 0, es);
		}
		if (ns->nprobes > 0)
			ExplainPropertyFloat("Cells Probed", NULL, ns->nprobes / loops, 0, es);
	}
}

/* ------------------------------------------------------------------ */

void
cone_scan_init(void)
{
	DefineCustomBoolVariable("skycell.nearest_bestfirst",
							 "Find a nearest-neighbour search's first rows on a heap out of "
							 "cell order by a best-first walk over HEALPix cells, instead of "
							 "a first ring.",
							 NULL, &nearest_bestfirst, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("skycell.custom_scan",
							 "Answer cones and cross-matches with skycell's own scan node "
							 "instead of rewriting them into B-tree range conditions.",
							 "The planner sees one clause and one path, instead of building "
							 "and estimating a path per range; the scan walks the ranges in "
							 "cell order, or through a TID bitmap when the heap is not "
							 "ordered by cell. Off restores the rewrite.",
							 &skycell_custom_scan, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("skycell.index_filter",
							 "Test cones on the index entries when the cell index also "
							 "holds the position columns.",
							 "With an index such as (skycell_ang2cell(ra, dec)) INCLUDE "
							 "(ra, dec), the custom scan reads ra and dec from each index "
							 "entry and fetches from the table only the rows inside the "
							 "cone. Off fetches every row of the covering cells.",
							 &skycell_index_filter, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("skycell.nearest_scan",
							 "Return rows ordered by their distance to a point from the cell "
							 "index, for ORDER BY distance(...) LIMIT k.",
							 "The scan reads widening rings around the point through the "
							 "cell index and sorts each ring, so a LIMIT stops it after the "
							 "ring that completes it. Off leaves such a query to a sort of "
							 "the whole relation. Needs skycell.custom_scan.",
							 &skycell_nearest_scan, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	RegisterCustomScanMethods(&cone_scan_methods);
	RegisterCustomScanMethods(&nearest_scan_methods);

	/*
	 * The library is usually loaded by the first skycell call a query makes,
	 * which is while that query is being planned, before cone_planner() has
	 * run for it: count that planning as a generation of its own.
	 */
	plan_generation = 1;
	prev_planner_hook = planner_hook;
	planner_hook = cone_planner;
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = cone_set_rel_pathlist;
	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook = cone_get_relation_info;
}
