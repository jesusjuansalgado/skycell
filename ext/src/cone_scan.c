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
#include "optimizer/cost.h"
#include "optimizer/planner.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "cover.h"
#include "skycell_internal.h"

static bool skycell_custom_scan = true;

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
	sc_density	dens;
	int64	   *rt_bounds;		/* bounds' storage, rt_cap ranges */
	int			rt_cap;
	double		ncoverings,		/* for EXPLAIN ANALYZE */
				nranges_total;
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

		if (equal(x, lfirst(b)))
			continue;
		if (IsA(x, Var) || IsA(x, Const))
			return false;
		if (!equal(eval_const_expressions(root, copyObject(x)), lfirst(b)))
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
	vars = pull_var_clause((Node *) list_make3(linitial(args), lsecond(args), lthird(args)),
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
	if (!jointree_has_term(root, (Node *) root->parse->jointree, written))
		return false;
	cone_funcid = fcall->funcid;
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
 */
#define PROBE_SAMPLES	32
#define PROBE_MAX_CAND	4000	/* inner rows read in all, before giving up */

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
	double		sum_n = 0,
				sum_r = 0,
				sum_c = 0,
				sum_m = 0,
				read = 0;
	int			nblk,
				per_blk;
	bool		ok = true;

	/* centre and radius: plain columns of one table; cell, ra, dec: of another */
	if (!plain_column(list_nth(args, 3)) || !plain_column(list_nth(args, 4)) ||
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

	nblk = (int) Min((BlockNumber) PROBE_SAMPLES, nblocks);
	per_blk = (PROBE_SAMPLES + nblk - 1) / nblk;
	for (int k = 0; k < nblk && ok; k++)
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
		while (got < per_blk && ok &&
			   table_scan_getnextslot_tidrange(tscan, ForwardScanDirection, oslot))
		{
			bool		n1,
						n2,
						n3 = false;
			double		ra0,
						dec0,
						radius;
			double		thr,
						cosdec0;
			sc_region	reg;
			sc_cover	cov;

			got++;
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = oslot;
			ra0 = DatumGetFloat8(ExecEvalExprSwitchContext(x_ra0, econtext, &n1));
			dec0 = DatumGetFloat8(ExecEvalExprSwitchContext(x_dec0, econtext, &n2));
			radius = rconst ? DatumGetFloat8(((Const *) rarg)->constvalue)
				: DatumGetFloat8(ExecEvalExprSwitchContext(x_r, econtext, &n3));
			sum_n += 1;			/* a NULL probe is a probe that matches nothing */
			if (n1 || n2 || n3 || sc_region_cone(&reg, ra0, dec0, radius, true) != NULL)
				continue;
			sc_cover_compute(&reg, dens, &p, &cov);
			thr = radius < 0 ? -1.0 : (radius >= 180.0 ? 2.0 : pow(sin(radius * M_PI / 360.0), 2));
			cosdec0 = cos(dec0 * M_PI / 180.0);
			sum_r += cov.n;
			for (int i = 0; i < cov.n && ok; i++)
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
					sum_c += 1;
					if (++read > PROBE_MAX_CAND)
					{
						ok = false;
						break;
					}
					if (m1 || m2)
						continue;
					/* skycell_in_cone's haversine */
					sdd = sin((dec - dec0) * M_PI / 360.0);
					sda = sin((ra - ra0) * M_PI / 360.0);
					if (sdd * sdd + cos(dec * M_PI / 180.0) * cosdec0 * sda * sda <= thr)
						sum_m += 1;
				}
			}
			sc_cover_free(&cov);
		}
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

	if (!ok || sum_n == 0)
		return false;
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

static PlannedStmt *
cone_planner(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams)
{
	plan_generation++;
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
	vars = pull_var_clause((Node *) list_make3(linitial(args), lsecond(args), lthird(args)),
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
cone_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ix, double nr, double exp_rows,
		  double rows, double corr, Cost *startup, Cost *total, bool *bitmap)
{
	double		cand = fmax(exp_rows, rows);
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
	double		rows = ppi ? ppi->ppi_rows : rel->rows;
	AttrNumber	attnum = IsA(cell, Var) ? ((Var *) cell)->varattno : 1;

	if ((ix = cell_index(rel, cell)) == NULL || !join_cone_estimate(root, f->args, &e))
		return;
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = ppi;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.pathkeys = NIL;
	cone_cost(root, rel, ix, e.nranges, e.cand, rows, cell_correlation(rte, ix, cell),
			  &cp->path.startup_cost, &cp->path.total_cost, &bitmap);
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
		if (cone_kind(f->args, rel->relid) == CONE_RUNTIME)
		{
			add_runtime_path(root, rel, rte, f, NULL);
			continue;
		}
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
		cone_cost(root, rel, ix, cov.n, cov.exp_rows, rel->rows,
				  cell_correlation(rte, ix, linitial(f->args)),
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
		st->relid = DatumGetObjectId(list_nth_node(Const, cscan->custom_private, 3)->constvalue);
		st->statrel = DatumGetObjectId(list_nth_node(Const, cscan->custom_private, 4)->constvalue);
		st->attnum = DatumGetInt16(list_nth_node(Const, cscan->custom_private, 5)->constvalue);
		st->joined = DatumGetBool(list_nth_node(Const, cscan->custom_private, 6)->constvalue);
		st->args = ExecInitExprList(cscan->custom_exprs, &node->ss.ps);
	}
	else
	{
		ArrayType  *arr = DatumGetArrayTypeP(bc->constvalue);

		st->nranges = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) / 2;
		st->bounds = (const int64 *) ARR_DATA_PTR(arr);
	}
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
#if PG_VERSION_NUM >= 180000
	if (st->bitmap)
		st->iscan = index_beginscan_bitmap(st->index, estate->es_snapshot, NULL, 2);
	else
		st->iscan = index_beginscan(heap, st->index, estate->es_snapshot, NULL, 2, 0);
#else
	if (st->bitmap)
	{
		st->iscan = index_beginscan_bitmap(st->index, estate->es_snapshot, 2);
		st->hscan = table_beginscan_bm(heap, estate->es_snapshot, 0, NULL);
	}
	else
		st->iscan = index_beginscan(heap, st->index, estate->es_snapshot, 2, 0);
#endif
}

/*
 * A cross-match cone's covering for the current outer row: centre and radius
 * evaluated (the outer row's columns are nestloop parameters by now), covered
 * against the relation's density with the custom scan's parameters, and the
 * ranges copied into the scan state.  The covering is built in the per-tuple
 * context, which ExecScan resets before every fetch.  NULL arguments match
 * nothing, as the strict skycell_cone would; an invalid centre is an error, as
 * it is in the rewrite's run-time bounds and in skycell_cone_ranges().
 */
static void
cone_cover_row(ConeScanState *st)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	EState	   *estate = st->css.ss.ps.state;
	double		v[3];
	int			i = 0;
	ListCell   *lc;
	MemoryContext old;
	sc_region	reg;
	sc_cover	cov;
	sc_cover_params p;

	st->need_cover = false;
	st->nranges = 0;
	foreach(lc, st->args)
	{
		bool		isnull;
		Datum		d = ExecEvalExpr((ExprState *) lfirst(lc), econtext, &isnull);

		if (isnull)
			return;
		v[i++] = DatumGetFloat8(d);
	}
	if (!st->have_dens)
	{
		old = MemoryContextSwitchTo(estate->es_query_cxt);
		skycell_load_density(st->relid, st->statrel, st->attnum, &st->dens);
		MemoryContextSwitchTo(old);
		st->have_dens = true;
	}
	old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	check_err(sc_region_cone(&reg, v[0], v[1], v[2], true));
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
	for (int r = 0; r < cov.n; r++)
	{
		st->rt_bounds[2 * r] = cov.r[r].lo;
		st->rt_bounds[2 * r + 1] = cov.r[r].hi;
	}
	st->bounds = st->rt_bounds;
	st->nranges = cov.n;
	st->ncoverings += 1;
	st->nranges_total += cov.n;
	sc_cover_free(&cov);
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
static void
cone_build_bitmap(ConeScanState *st)
{
	st->tbm = tbm_create(work_mem * (Size) 1024, NULL);
	for (int i = 0; i < st->nranges; i++)
	{
		cone_range(st, i);
		index_getbitmap(st->iscan, st->tbm);
	}
}

#if PG_VERSION_NUM >= 180000
/* PostgreSQL 18: the table AM walks the bitmap itself, through a read stream */
static TupleTableSlot *
cone_next_bitmap(ConeScanState *st, TupleTableSlot *slot)
{
	bool		recheck;
	uint64		lossy = 0,
				exact = 0;

	if (st->tbm == NULL)
	{
		TBMIterator it;

		cone_build_bitmap(st);
		it = tbm_begin_iterate(st->tbm, NULL, InvalidDsaPointer);
		if (st->hscan == NULL)
			st->hscan = table_beginscan_bm(st->css.ss.ss_currentRelation,
										   st->css.ss.ps.state->es_snapshot, 0, NULL);
		st->hscan->st.rs_tbmiterator = it;
	}
	if (table_scan_bitmap_next_tuple(st->hscan, slot, &recheck, &lossy, &exact))
		return slot;
	return ExecClearTuple(slot);
}
#else
static TupleTableSlot *
cone_next_bitmap(ConeScanState *st, TupleTableSlot *slot)
{
	if (st->tbm == NULL)
	{
		cone_build_bitmap(st);
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
#endif

static TupleTableSlot *
cone_next(ScanState *ss)
{
	ConeScanState *st = (ConeScanState *) ss;

	if (st->need_cover)
		cone_cover_row(st);
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
	cone_release_bitmap(st, false);
	ExecScanReScan(&node->ss);
}

static void
cone_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ConeScanState *st = (ConeScanState *) node;

	ExplainPropertyText("Index", RelationGetRelationName(st->index), es);
	if (!st->runtime)
		ExplainPropertyInteger("Ranges", NULL, st->nranges, es);
	else if (es->analyze && st->ncoverings > 0)
		ExplainPropertyFloat("Ranges per Covering", NULL,
							 st->nranges_total / st->ncoverings, 1, es);
	else
		ExplainPropertyText("Ranges", st->joined ? "per outer row" : "per rescan", es);
	ExplainPropertyText("Mode", st->bitmap ? "bitmap" : "ordered", es);
}

/* ------------------------------------------------------------------ */

void
cone_scan_init(void)
{
	DefineCustomBoolVariable("skycell.custom_scan",
							 "Answer constant cones with skycell's own scan node instead of "
							 "rewriting them into B-tree range conditions.",
							 "The planner sees one clause and one path, instead of building "
							 "and estimating a path per range; the scan walks the ranges in "
							 "cell order, or through a TID bitmap when the heap is not "
							 "ordered by cell.",
							 &skycell_custom_scan, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	RegisterCustomScanMethods(&cone_scan_methods);
	prev_planner_hook = planner_hook;
	planner_hook = cone_planner;
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = cone_set_rel_pathlist;
}
