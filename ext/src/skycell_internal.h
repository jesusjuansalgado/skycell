/*
 * skycell_internal.h -- what the ADQL layer (adql.c) borrows from the
 * planner glue in skycell.c: the density model, the covering memo and the
 * pieces that build B-tree range conditions out of a covering.
 */
#ifndef SKYCELL_INTERNAL_H
#define SKYCELL_INTERNAL_H

#include "nodes/pathnodes.h"
#include "nodes/primnodes.h"
#include "utils/array.h"
#include "cover.h"

extern void check_err(const char *err);
extern void current_params(sc_cover_params *p, int max_ranges, const sc_density *d);
extern ArrayType *moc_for_region(const sc_region *r, int max_cells, int max_order);

extern int	skycell_join_slots;
extern double skycell_rewrite_max_waste;
extern double rewrite_waste_threshold(const sc_density *d);

extern Const *int8_const(int64 v);
extern Const *float8_const(double v);
extern Const *int4_const(int32 v);
extern Expr *int8_cmp(int strategy, Node *left, Expr *right);
extern Expr *range_arm(Node *cell, Expr *lo, Expr *hi);
extern Expr *range_arm_family(Node *cell, Expr *lo, Expr *hi, Oid opfamily);
extern Node *ranges_and_exact(sc_cover *cov, Node *cell, Expr *exact, bool uses_cell_ops);
extern Oid	lookup_sibling_func(Oid funcid, const char *name, int nargs, const Oid *argtypes);
extern Oid	cell_ops_opfamily(void);

extern void cover_cached(const sc_region *reg, const sc_density *d,
						 const sc_cover_params *p, Oid statrel,
						 double ra0, double dec0, double radius, sc_cover *out);
/*
 * pull_var_clause() for planner support code: the same list, except that an
 * outer query's Var (varlevelsup > 0), Aggref or PlaceHolderVar is a
 * parameter at this level and is left out, where pull_var_clause() raises
 * "Upper-level Var found where not expected". Without a PVC_INCLUDE_* flag,
 * aggregates, window functions and placeholders are recursed into.
 */
extern List *sc_pull_local_vars(Node *node, int flags);

extern bool density_for_expr(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
							  bool *uses_cell_ops);
extern bool region_area_histogram(Oid selfid, PlannerInfo *root, Node *region_expr,
								   double **areas, int *n);
extern bool skycell_cell_density(PlannerInfo *root, Node **cell, sc_density *d, Oid *statrel,
								 bool *uses_cell_ops);
extern void density_for_var(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
							 bool *uses_cell_ops);
extern bool gin_moc_index_for_region(PlannerInfo *root, Node *rg, Node **moc_expr,
									  int *max_order);
extern Expr *array_overlap_expr(Node *left, Expr *right);

extern bool skycell_const_cone_cover(PlannerInfo *root, List *args, sc_region *reg,
									 sc_cover *cov, sc_density *dens);

extern void skycell_load_density(Oid relid, Oid statrel, AttrNumber attnum, sc_density *d);
extern uint64 skycell_density_generation(void);
extern void skycell_scan_params(sc_cover_params *p, const sc_density *d);

/* cone_scan.c: the custom scan for constant and cross-match cones (skycell.custom_scan) */
extern void cone_scan_init(void);
extern bool skycell_fold_regions;	/* adql.c: booleans of constant regions as one (0.53) */
extern void skycell_fold_query_regions(Query *parse);
extern void skycell_preconvert_union_query(Query *parse);
extern void skycell_secure_points(Query *parse);
extern void skycell_lateral_barrier_views(Query *parse);
extern bool skycell_planning_unhooked(PlannerInfo *root);
extern bool cone_scan_keep(PlannerInfo *root, FuncExpr *fcall);
extern bool cone_scan_keep_as(PlannerInfo *root, FuncExpr *written, FuncExpr *cone6);
extern Node *cone_with_partition_bounds(PlannerInfo *root, FuncExpr *cone6);

/*
 * A skycell_cone() call that cone_with_partition_bounds() has given its
 * bounds or shard key: inside an AND, PostgreSQL simplifies the members of an
 * AND a simplification returns again, and the call must then be left as it
 * is, or it gains its bounds again, without end.  The mark is the location of
 * one of its arguments (the support function sees a stand-in for the call,
 * whose own location is -1, but the arguments themselves); a location is
 * never compared (equal()) or written out (deparsing).
 */
#define SKYCELL_BOUNDED_LOCATION (-29)
extern void skycell_mark_bounded(FuncExpr *cone6);
extern bool skycell_is_bounded(List *cone_args);
extern Node *region_with_partition_bounds(PlannerInfo *root, FuncExpr *scan);
extern void skycell_index_adql_predicates(PlannerInfo *root);
extern bool skycell_distance_order(Node *expr, Index relid, Node **cell, Node **ra0,
								   Node **dec0);
extern double cone_join_selectivity(PlannerInfo *root, List *args);

/* adql.c: a skypos/skyregion datum, as cover.c understands it */
extern sc_vec3 skycell_pos_from_datum(Datum d);

/*
 * A GiST consistent() function sees the same skypos query on every entry an
 * index scan visits. Converting it to a unit vector is four trigonometric
 * calls, which cost more than the box test they feed, so the point
 * strategies keep the vector in their fn_extra cache, keyed by the
 * coordinates: by value, not by pointer, as a join's outer rows can reuse
 * one address for different points.
 */
typedef struct
{
	bool		valid;
	double		ra,
				dec;
	sc_vec3		v;
}			skycell_pos_cache;

extern sc_vec3 skycell_pos_from_datum_cached(Datum d, skycell_pos_cache *c);
extern void skycell_region_from_datum(Datum d, sc_region *r);
extern bool skycell_pos_coords(Oid funcid, Node *pt, Node **ra, Node **dec);
extern bool skycell_const_region_cover(PlannerInfo *root, List *args, sc_region *reg,
									   sc_cover *cov, sc_density *dens);
extern bool region_scan_keep(PlannerInfo *root, FuncExpr *written, FuncExpr *fcall);
extern bool skycell_union_all_parent(PlannerInfo *root, Index varno);
extern Oid skycell_own_relid(const char *name);
extern FuncExpr *skycell_poly_region_scan(Oid sibling, Node *cell, Node *ra, Node *dec,
										  ArrayType *arr);
extern void skycell_region_from_datum_lite(Datum d, sc_region *r);

/* adql.c: a skyregion's text */
extern Datum skyregion_out(PG_FUNCTION_ARGS);

/* skycell.c: the cells of max_order a region meets, as sorted, merged order-29 pairs */
extern int64 *skycell_region_moc_pairs(const sc_region *reg, int max_order, int *n);

/* skycell.c: a MOC's ASCII or JSON text from sorted, merged order-29 ranges */
extern char *skycell_moc_cstring(const int64 *pairs, int n, int max_order, bool json);

#endif
