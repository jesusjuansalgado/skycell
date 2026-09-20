/*
 * cover.h -- cost-based HEALPix coverings of sky regions.
 *
 * A covering is a sorted list of disjoint order-29 pixel ranges [lo, hi]
 * whose union contains the region.  Unlike a fixed-depth covering, the
 * refinement is driven by a cost model:
 *
 *     cost = n_ranges * range_cost + expected_false_positive_rows
 *
 * where expected rows come from a density model -- normally the histogram
 * PostgreSQL's ANALYZE keeps for the cell column.  Because HEALPix is equal
 * area and nested, a histogram of order-29 cell ids *is* a sky density map:
 * rows between two bounds divided by (hi - lo) * pixel_area.
 *
 * Pure C: no PostgreSQL dependencies (allocation via sc_alloc.h).
 */
#ifndef SKYCELL_COVER_H
#define SKYCELL_COVER_H

#include "healpix.h"

typedef enum
{
	SC_OUT = 0,
	SC_PARTIAL = 1,
	SC_IN = 2
} sc_class;

typedef enum
{
	SC_REGION_CONE = 1,
	SC_REGION_POLY = 2
} sc_region_kind;

typedef struct sc_region
{
	sc_region_kind kind;
	double		area;			/* steradians */

	/* cone */
	sc_vec3		center;
	double		radius;			/* radians */
	int64_t		center_pix;		/* the centre's order-29 cell (containment test) */
	double		out_c2[SC_MAX_ORDER + 1];	/* chord^2 beyond which a cell is OUT */
	double		in_c2[SC_MAX_ORDER + 1];	/* chord^2 below which a cell is IN */

	/* convex polygon: vertices CCW, unit edge normals, interior n.p >= 0 */
	int			nv;
	sc_vec3    *v;
	sc_vec3    *n;
	double		sin_rho[SC_MAX_ORDER + 1];
} sc_region;

typedef struct sc_density
{
	double		ntotal;			/* rows in the table */
	int			nbounds;		/* histogram bounds (0 or 1 = uniform) */
	const int64_t *bounds;		/* sorted order-29 cell ids */

	/*
	 * Pages in the table.  The covering does not use it; it is carried for the
	 * caller's cost model (auto_range_cost() in skycell.c).  Last in the struct
	 * so that positional initialisers elsewhere keep their meaning.
	 */
	double		relpages;
} sc_density;

typedef struct sc_cover_params
{
	int			max_order;		/* deepest order to refine to */
	int			max_ranges;		/* hard cap on output ranges */
	double		range_cost;		/* cost of one extra index range, in rows */
	double		split_cost;		/* cost of examining 4 children, in rows */
	int			max_steps;		/* cap on cell classifications */

	/*
	 * Robustness guard against density underestimates: a partially covered
	 * cell may not be larger than max_area_ratio * area(region), and gaps are
	 * only merged under the same bound.  0 disables (trust the density model).
	 */
	double		max_area_ratio;

	int			use_seed;		/* fast start for small cones (default on) */
	int			direct;			/* closed-form order for cones (default on) */

	/*
	 * Diagnostics, not for production use.  force_order >= 0 covers at that
	 * order whatever the cost model says, which is how test/bench measure the
	 * cost of every order and check where the model's choice lands on that
	 * curve.  -1 (the default) lets the model choose.
	 */
	int			force_order;
} sc_cover_params;

typedef struct sc_range
{
	int64_t		lo,
				hi;
} sc_range;

typedef struct sc_cover
{
	int			n;
	sc_range   *r;
	double		exp_rows;		/* expected rows inside the covering */
	double		area;			/* steradians covered */
	int			steps;			/* cell classifications performed */
	int			deepest;		/* deepest order reached */
	double		rho;			/* density the order was chosen from, rows/sr */
	int			order;			/* order the cost model asked for (-1: n/a) */
} sc_cover;

/* region constructors; return NULL on success or a static error message */
const char *sc_region_cone(sc_region *r, double ra_deg, double dec_deg, double radius_deg);
const char *sc_region_poly(sc_region *r, int nv, const double *ra_deg, const double *dec_deg);
void		sc_region_free(sc_region *r);

/* exact point-in-region test */
int			sc_region_contains(const sc_region *r, sc_vec3 p);

/* region predicates: a inside b, and a meets b (ADQL CONTAINS / INTERSECTS) */
int			sc_region_contains_region(const sc_region *a, const sc_region *b);
int			sc_region_overlaps(const sc_region *a, const sc_region *b);

/* distance from a point to a region's boundary; 0 inside (radians) */
double		sc_region_distance(const sc_region *r, sc_vec3 p);

/* a representative interior point (ADQL CENTROID) */
sc_vec3		sc_region_centroid(const sc_region *r);

/* use exact cell corners for cones (1, default) or the bounding cap (0) */
extern int	sc_exact_cells;

/* classify the pixel (order, pix) against the region (conservative) */
sc_class	sc_region_classify(const sc_region *r, int order, int64_t pix, double *f_out);

/* the same, always by the bounding cap: cheaper, looser */
sc_class	sc_region_classify_cap(const sc_region *r, int order, int64_t pix, double *f_out);

/* expected number of rows with cell in [lo, hi] */
double		sc_density_rows(const sc_density *d, int64_t lo, int64_t hi);

void		sc_cover_params_default(sc_cover_params *p);
void		sc_cover_compute(const sc_region *r, const sc_density *d,
							 const sc_cover_params *p, sc_cover *out);
void		sc_cover_free(sc_cover *c);

/* steradians per order-29 pixel */
#define SC_PIX29_AREA (4.0 * 3.14159265358979323846 / (double) SC_NPIX29)

#endif
