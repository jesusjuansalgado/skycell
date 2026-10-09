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
extern bool density_for_expr(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
							  bool *uses_cell_ops);
extern bool region_area_histogram(Oid selfid, PlannerInfo *root, Node *region_expr,
								   double **areas, int *n);
extern void density_for_var(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
							 bool *uses_cell_ops);
extern bool gin_moc_index_for_region(PlannerInfo *root, Node *rg, Node **moc_expr,
									  int *max_order);
extern Expr *array_overlap_expr(Node *left, Expr *right);

extern bool skycell_const_cone_cover(PlannerInfo *root, List *args, sc_region *reg,
									 sc_cover *cov, sc_density *dens);

/* cone_scan.c: the custom scan for constant cones (skycell.custom_scan) */
extern void cone_scan_init(void);
extern bool cone_scan_keep(PlannerInfo *root, FuncExpr *fcall);

/* adql.c: a skypos/skyregion datum, as cover.c understands it */
extern sc_vec3 skycell_pos_from_datum(Datum d);
extern void skycell_region_from_datum(Datum d, sc_region *r);
extern void skycell_region_from_datum_lite(Datum d, sc_region *r);

#endif
