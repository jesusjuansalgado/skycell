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
 * With skycell.custom_scan on, a constant cone on a single base relation is
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
 * Limits of this first version: only the six-argument skycell_cone with
 * constant parameters (joins, generic plans and the Q3C-shaped spellings keep
 * the rewrite); a cone inside an OR is no longer indexable while the GUC is
 * on; and the bitmap mode does not prefetch (effective_io_concurrency).
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_statistic.h"
#include "commands/explain.h"
#include "miscadmin.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/tidbitmap.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "cover.h"
#include "skycell_internal.h"

static bool skycell_custom_scan = false;

/* the six-argument skycell_cone, as last seen by cone_scan_keep() */
static Oid	cone_funcid = InvalidOid;

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
	TIDBitmap  *tbm;			/* bitmap: NULL until the first fetch */
	TBMIterator *it;
	TBMIterateResult *tbmres;	/* bitmap: page being returned, or NULL */
} ConeScanState;

/* ------------------------------------------------------------------ */
/* planning                                                           */
/* ------------------------------------------------------------------ */

/*
 * Called from skycell_support's SupportRequestSimplify: true to leave this
 * skycell_cone call unrewritten for the custom path.  Only a cone whose
 * parameters are constants and whose cell/ra/dec come from one base relation;
 * whether an index can serve it is only known once the planner has the
 * relation's index list, and a relation without one gets a sequential scan
 * either way.
 */
bool
cone_scan_keep(PlannerInfo *root, FuncExpr *fcall)
{
	List	   *args = fcall->args;
	List	   *vars;
	ListCell   *lc;
	Index		varno = 0;

	if (!skycell_custom_scan || root == NULL || root->parse == NULL ||
		list_length(args) != 6)
		return false;
	for (int i = 3; i <= 5; i++)
	{
		Node	   *a = list_nth(args, i);

		if (!IsA(a, Const) || ((Const *) a)->constisnull)
			return false;
	}
	vars = pull_var_clause((Node *) list_make3(linitial(args), lsecond(args), lthird(args)), 0);
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
	cone_funcid = fcall->funcid;
	return true;
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
 */
static void
cone_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ix, const sc_cover *cov,
		  double corr, Cost *startup, Cost *total, bool *bitmap)
{
	double		nr = cov->n;
	double		cand = fmax(cov->exp_rows, rel->rows);
	double		ituples = fmax(ix->tuples, 1.0);
	double		ipages = fmax((double) ix->pages, 1.0);
	double		T = fmax((double) rel->pages, 1.0);
	double		rpp = (rel->tuples > 0 && rel->pages > 0) ? rel->tuples / rel->pages : 100.0;
	double		descent = (ceil(log2(ituples)) + (Max(ix->tree_height, 0) + 1) * 50.0)
		* cpu_operator_cost;
	Cost		common,
				ordered,
				bm;

	common = nr * descent
		+ (nr + cand * ipages / ituples) * random_page_cost
		+ cand * (cpu_index_tuple_cost + 2.0 * cpu_operator_cost)
		+ cand * (cpu_tuple_cost + rel->baserestrictcost.per_tuple)
		+ rel->rows * rel->reltarget->cost.per_tuple;

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
		Cost		io_clustered = nr * random_page_cost + fmax(0.0, min_pages - nr) * seq_page_cost;
		Cost		ordered_uncorr = index_pages_fetched(cand, rel->pages, ipages, root) * random_page_cost;
		double		pages = fmin(T, (2.0 * T * cand) / (2.0 * T + cand));
		double		per_page = (pages >= 2.0)
			? random_page_cost - (random_page_cost - seq_page_cost) * sqrt(pages / T)
			: random_page_cost;
		Cost		bitmap_uncorr = pages * per_page;
		double		c2 = corr * corr;

		ordered = ordered_uncorr + c2 * (io_clustered - ordered_uncorr);
		bm = bitmap_uncorr + c2 * (io_clustered - bitmap_uncorr)
			+ cand * 0.1 * cpu_operator_cost;
	}

	*bitmap = bm < ordered;
	*startup = rel->baserestrictcost.startup + rel->reltarget->cost.startup;
	*total = *startup + common + Min(ordered, bm);
}

static void
cone_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!skycell_custom_scan || !OidIsValid(cone_funcid) || rel->indexlist == NIL)
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

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		FuncExpr   *f = (FuncExpr *) ri->clause;
		IndexOptInfo *ix;
		sc_region	reg;
		sc_cover	cov;
		sc_density	dens;
		CustomPath *cp;
		Datum	   *b;
		Const	   *bounds;
		bool		bitmap;

		if (ri->pseudoconstant || !IsA(f, FuncExpr) || f->funcid != cone_funcid ||
			list_length(f->args) != 6)
			continue;
		if ((ix = cell_index(rel, linitial(f->args))) == NULL)
			continue;
		if (!skycell_const_cone_cover(root, f->args, &reg, &cov, &dens))
			continue;

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
		cp->path.parallel_safe = false;
		cp->path.parallel_workers = 0;
		cp->path.rows = rel->rows;
		cp->path.pathkeys = NIL;
		cone_cost(root, rel, ix, &cov, cell_correlation(rte, ix, linitial(f->args)),
				  &cp->path.startup_cost, &cp->path.total_cost, &bitmap);
		cp->flags = 0;
		cp->custom_paths = NIL;
		/* the clause this path answers rides along for cone_plan(), which drops it */
		cp->custom_private = list_make4(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
												  ObjectIdGetDatum(ix->indexoid), false, true),
										bounds, makeBoolConst(bitmap, false), f);
		cp->methods = &cone_path_methods;
		add_path(rel, &cp->path);
	}
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
static Plan *
cone_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		  List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	FuncExpr   *driving = (FuncExpr *) lfourth(best_path->custom_private);
	List	   *quals = NIL;
	ListCell   *lc;

	foreach(lc, clauses)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);

		if (ri->pseudoconstant)
			continue;
		if ((Node *) ri->clause == (Node *) driving)
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
	cs->custom_private = list_make3(linitial(best_path->custom_private),
									lsecond(best_path->custom_private),
									lthird(best_path->custom_private));
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

static void
cone_begin(CustomScanState *node, EState *estate, int eflags)
{
	ConeScanState *st = (ConeScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Const	   *oidc = linitial_node(Const, cscan->custom_private);
	Const	   *bc = lsecond_node(Const, cscan->custom_private);
	Const	   *modec = lthird_node(Const, cscan->custom_private);
	ArrayType  *arr = DatumGetArrayTypeP(bc->constvalue);
	Relation	heap = node->ss.ss_currentRelation;
	LOCKMODE	lockmode = exec_rt_fetch(cscan->scan.scanrelid, estate)->rellockmode;
	Oid			opfamily;

	st->indexoid = DatumGetObjectId(oidc->constvalue);
	st->nranges = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) / 2;
	st->bounds = (const int64 *) ARR_DATA_PTR(arr);
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
	if (st->bitmap)
	{
		st->iscan = index_beginscan_bitmap(st->index, estate->es_snapshot, 2);
		st->hscan = table_beginscan_bm(heap, estate->es_snapshot, 0, NULL);
	}
	else
		st->iscan = index_beginscan(heap, st->index, estate->es_snapshot, 2, 0);
}

/* point the scan keys at range i and restart the index scan there */
static void
cone_range(ConeScanState *st, int i)
{
	st->keys[0].sk_argument = Int64GetDatum(st->bounds[2 * i]);
	st->keys[1].sk_argument = Int64GetDatum(st->bounds[2 * i + 1]);
	index_rescan(st->iscan, st->keys, 2, NULL, 0);
}

/* ordered: the next row of the current range, moving on when it runs out */
static TupleTableSlot *
cone_next_ordered(ConeScanState *st, TupleTableSlot *slot)
{
	for (;;)
	{
		if (st->cur >= st->nranges)
			return ExecClearTuple(slot);
		if (st->cur >= 0 && index_getnext_slot(st->iscan, ForwardScanDirection, slot))
			return slot;
		if (++st->cur >= st->nranges)
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
static TupleTableSlot *
cone_next_bitmap(ConeScanState *st, TupleTableSlot *slot)
{
	if (st->tbm == NULL)
	{
		st->tbm = tbm_create(work_mem * (Size) 1024, NULL);
		for (int i = 0; i < st->nranges; i++)
		{
			cone_range(st, i);
			index_getbitmap(st->iscan, st->tbm);
		}
		st->it = tbm_begin_iterate(st->tbm);
		st->tbmres = NULL;
	}
	for (;;)
	{
		if (st->tbmres == NULL)
		{
			if ((st->tbmres = tbm_iterate(st->it)) == NULL)
				return ExecClearTuple(slot);
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

static TupleTableSlot *
cone_next(ScanState *ss)
{
	ConeScanState *st = (ConeScanState *) ss;

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

static void
cone_free_bitmap(ConeScanState *st)
{
	if (st->it)
		tbm_end_iterate(st->it);
	if (st->tbm)
		tbm_free(st->tbm);
	st->it = NULL;
	st->tbm = NULL;
	st->tbmres = NULL;
}

static void
cone_end(CustomScanState *node)
{
	ConeScanState *st = (ConeScanState *) node;

	cone_free_bitmap(st);
	if (st->hscan)
		table_endscan(st->hscan);
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
	cone_free_bitmap(st);
	if (st->hscan)
		table_rescan(st->hscan, NULL);
	ExecScanReScan(&node->ss);
}

static void
cone_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ConeScanState *st = (ConeScanState *) node;

	ExplainPropertyText("Index", RelationGetRelationName(st->index), es);
	ExplainPropertyInteger("Ranges", NULL, st->nranges, es);
	ExplainPropertyText("Mode", st->bitmap ? "bitmap" : "ordered", es);
}

/* ------------------------------------------------------------------ */

void
cone_scan_init(void)
{
	DefineCustomBoolVariable("skycell.custom_scan",
							 "Answer constant cones with skycell's own scan node instead of "
							 "rewriting them into B-tree range conditions (experimental).",
							 "The planner sees one clause and one path, instead of building "
							 "and estimating a path per range; the scan walks the ranges in "
							 "cell order, or through a TID bitmap when the heap is not "
							 "ordered by cell.",
							 &skycell_custom_scan, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	RegisterCustomScanMethods(&cone_scan_methods);
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = cone_set_rel_pathlist;
}
