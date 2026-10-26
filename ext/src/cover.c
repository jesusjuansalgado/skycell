/*
 * cover.c -- cost-based HEALPix coverings of sky regions (see cover.h).
 */
#include "sc_alloc.h"
#include <math.h>
#include <string.h>
#include "cover.h"

/*
 * Numerical margin added to every OUT decision, in radians.
 *
 * Classifying a cell OUT removes it from the covering, so it is the only
 * classification that can lose rows; IN and PARTIAL cannot (the exact
 * predicate re-tests every row the index returns).  The geometry here is a
 * handful of dot products, cross products and an asin per cell, each of which
 * carries a relative error of a few ulp, so the accumulated error on an angle
 * is bounded well below 1e-13 rad for any unit vectors.  2e-13 rad (40 pas)
 * is that bound with room to spare, and is far below the 0.4 mas resolution
 * of an order-29 cell, so it costs nothing in false positives.
 */
#define SC_ANG_EPS 2e-13

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef Max
#define Max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef Min
#define Min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifdef SC_COVER_TRACE
#include <stdio.h>
int			sc_cover_trace = 0;
#define TRACE(...) do { if (sc_cover_trace) fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define TRACE(...) ((void) 0)
#endif

static inline double
clamp01(double x)
{
	return x < 0 ? 0 : (x > 1 ? 1 : x);
}

static inline sc_vec3
vnormalize(sc_vec3 a)
{
	double		n = sqrt(sc_dot(a, a));

	a.x /= n;
	a.y /= n;
	a.z /= n;
	return a;
}

/* ------------------------------------------------------------------ */
/* regions                                                            */
/* ------------------------------------------------------------------ */

const char *
sc_region_cone(sc_region *r, double ra_deg, double dec_deg, double radius_deg,
			   bool need_covering)
{
	memset(r, 0, sizeof(*r));
	if (!isfinite(ra_deg) || !isfinite(dec_deg) || !isfinite(radius_deg))
		return "cone parameters must be finite";
	if (dec_deg < -90.0 || dec_deg > 90.0)
		return "declination must be within [-90, 90]";

	r->kind = SC_REGION_CONE;
	r->center = sc_radec2vec(ra_deg, dec_deg);
	r->center_pix = sc_ang2pix(SC_MAX_ORDER, ra_deg, dec_deg);
	r->radius = radius_deg * (M_PI / 180.0);
	if (r->radius > M_PI)
		r->radius = M_PI;
	r->area = (r->radius < 0) ? 0.0 : 4.0 * M_PI * pow(sin(r->radius / 2.0), 2);

	/*
	 * out_c2[]/in_c2[] are filled lazily, order by order, by
	 * ensure_cone_bounds() below -- need_covering is kept only as the call
	 * site's declaration of intent (see cover.h); memset() above already
	 * zeroed filled[] and both tables, which is the lazy state.
	 */
	(void) need_covering;
	return NULL;
}

/*
 * Fill out_c2[order]/in_c2[order] for a cone, if not already done.
 *
 * Called from sc_region_classify_cap(), the only reader, immediately before
 * it uses either value -- one order at a time, so a covering that only ever
 * visits a handful of the 30 orders (the common case: a seed order up to
 * whatever order the cost model chose) pays for a handful, not for every
 * order whether visited or not.  sc_region itself is always backend-local
 * and never shared across calls, so mutating it through a cast-away-const
 * pointer here is safe; see cover.h on why the parameter stays const.
 */
static inline __attribute__((always_inline)) void
ensure_cone_bounds(const sc_region *r_in, int order)
{
	sc_region  *r = (sc_region *) r_in;
	double		rho,
				a,
				b;

	if (r->filled & ((uint32_t) 1 << order))
		return;

	rho = sc_pixrad(order);
	a = r->radius + rho;
	b = r->radius - rho;
	if (r->radius < 0)
		r->out_c2[order] = -1.0;	/* empty region: everything OUT */
	else if (a >= M_PI)
		r->out_c2[order] = 5.0;		/* chord^2 <= 4: never OUT */
	else
		r->out_c2[order] = 4.0 * pow(sin(fmin(M_PI, a + SC_ANG_EPS) / 2.0), 2);
	r->in_c2[order] = (b < 0) ? -1.0 : 4.0 * pow(sin(b / 2.0), 2);
	r->filled |= (uint32_t) 1 << order;
}

static const char *
poly_setup(sc_region *r)
{
	for (int i = 0; i < r->nv; i++)
	{
		/*
		 * a x (b - a), not a x b: the same vector, but for a short edge the
		 * difference is exact and a x b is not -- its rounding, ~1e-16 in
		 * each component, is relative to the edge length once normalised,
		 * and tilts the edge plane by ~1e-16 / length rad.
		 */
		sc_vec3		a = r->v[i],
					b = r->v[(i + 1) % r->nv];
		sc_vec3		c = sc_cross(a, (sc_vec3) {b.x - a.x, b.y - a.y, b.z - a.z});

		if (sqrt(sc_dot(c, c)) < 1e-15)
			return "polygon has a zero-length edge (repeated vertex?)";
		r->n[i] = vnormalize(c);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* non-convex polygons                                                */
/* ------------------------------------------------------------------ */

/*
 * A polygon whose vertices all lie in the open hemisphere around a point g
 * maps, under the gnomonic projection tangent at g, to a plane polygon whose
 * edges are straight: the projection sends great circles to lines.  So a
 * non-convex polygon is cut into convex pieces in that plane -- ear clipping
 * into triangles, then Hertel-Mehlhorn merging, which drops every diagonal
 * whose removal leaves both sides convex (at most 4x the minimum number of
 * pieces) -- and each piece is an ordinary convex polygon on the sphere,
 * with the polygon's own vertices.  Containment is "in some piece"; the
 * boundary ring is kept whole for everything else (distances, edge
 * crossings, cell classification), so the diagonals never show.
 */
typedef struct
{
	double		x,
				y;
} pt2;

static inline pt2
gnomonic(const sc_vec3 *f, sc_vec3 v)
{
	double		w = sc_dot(v, f[0]);

	return (pt2) {sc_dot(v, f[1]) / w, sc_dot(v, f[2]) / w};
}

static inline sc_vec3
gnomonic_inv(const sc_vec3 *f, pt2 p)
{
	return vnormalize((sc_vec3) {f[0].x + p.x * f[1].x + p.y * f[2].x,
								 f[0].y + p.x * f[1].y + p.y * f[2].y,
								 f[0].z + p.x * f[1].z + p.y * f[2].z});
}

/* the tangent plane at unit g: f = {g, e1, e2}, with (e1, e2, g) right-handed */
static void
plane_frame(sc_vec3 g, sc_vec3 *f)
{
	f[0] = g;
	f[1] = vnormalize(sc_cross(g, fabs(g.z) < 0.9 ? (sc_vec3) {0, 0, 1} : (sc_vec3) {1, 0, 0}));
	f[2] = sc_cross(g, f[1]);
}

/* twice the signed area of (a, b, c): > 0 counter-clockwise */
static inline double
orient2(pt2 a, pt2 b, pt2 c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/*
 * Positions in the plane are good to about this, in tangent-plane units
 * (radians near g): a vertex given in degrees rounds to ~5e-16 rad, and
 * the projection adds a few ulp.  Turns and gaps below it are noise.
 */
#define SC_PLANE_EPS 1e-14

/* is the turn a -> b -> c straight (to rounding), forward or back? */
static inline int
straight2(pt2 a, pt2 b, pt2 c)
{
	/* b within SC_PLANE_EPS of the line through a and c, or of a or c */
	double		lac = Max(hypot(c.x - a.x, c.y - a.y),
						  Max(hypot(b.x - a.x, b.y - a.y), hypot(c.x - b.x, c.y - b.y)));

	return fabs(orient2(a, b, c)) <= SC_PLANE_EPS * lac;
}

/* do the closed segments a-b and c-d meet? */
static int
segments_meet(pt2 a, pt2 b, pt2 c, pt2 d)
{
	double		o1 = orient2(a, b, c),
				o2 = orient2(a, b, d),
				o3 = orient2(c, d, a),
				o4 = orient2(c, d, b),
				lab = hypot(b.x - a.x, b.y - a.y);

	/*
	 * Collinear to rounding -- two edges along one great circle, like the
	 * tops of a U's arms -- and the signs below are noise: do their extents
	 * along a-b overlap?
	 */
	if (fabs(o1) <= SC_PLANE_EPS * lab && fabs(o2) <= SC_PLANE_EPS * lab)
	{
		double		ux = b.x - a.x,
					uy = b.y - a.y,
					tc = (c.x - a.x) * ux + (c.y - a.y) * uy,
					td = (d.x - a.x) * ux + (d.y - a.y) * uy,
					tb = ux * ux + uy * uy;

		return Max(tc, td) >= 0 && Min(tc, td) <= tb;
	}
	return ((o1 <= 0 && o2 >= 0) || (o1 >= 0 && o2 <= 0)) &&
		((o3 <= 0 && o4 >= 0) || (o3 >= 0 && o4 <= 0));
}

/* spherical area of the triangle (a, b, c), Van Oosterom & Strackee */
static double
tri_area(sc_vec3 a, sc_vec3 b, sc_vec3 c)
{
	double		num = fabs(sc_dot(a, sc_cross(b, c)));
	double		den = 1.0 + sc_dot(a, b) + sc_dot(b, c) + sc_dot(c, a);

	return 2.0 * atan2(num, den);
}

static const char *poly_setup(sc_region *r);

/* reverse the ring in place (v[] and its projection), and redo the normals */
static const char *
poly_reverse(sc_region *r, pt2 *pt)
{
	for (int i = 0; i < r->nv / 2; i++)
	{
		sc_vec3		t = r->v[i];
		pt2			q = pt[i];

		r->v[i] = r->v[r->nv - 1 - i];
		r->v[r->nv - 1 - i] = t;
		pt[i] = pt[r->nv - 1 - i];
		pt[r->nv - 1 - i] = q;
	}
	return poly_setup(r);
}

/* one diagonal of the triangulation: triangle ta has edge a->b, tb has b->a */
typedef struct
{
	int			a,
				b,
				ta,
				tb;
} diag_t;

static int
uf_find(int *parent, int i)
{
	while (parent[i] != i)
		i = parent[i] = parent[parent[i]];
	return i;
}

/* position of vertex index x in the part list p[0..n) */
static int
part_pos(const int *p, int n, int x)
{
	for (int i = 0; i < n; i++)
		if (p[i] == x)
			return i;
	return -1;
}

/*
 * Set up r (ring in r->v[]/r->n[], vertex mean g) as a non-convex polygon.
 * Returns NULL or an error message.
 */
static const char *
poly_nonconvex(sc_region *r, sc_vec3 g)
{
	int			nv = r->nv;
	sc_vec3	   *f = r->frame;
	pt2		   *pt;
	int		   *prev,
			   *next,
			   *owner,
			   *tri,
			   *parent,
			  **part,
			   *plen;
	diag_t	   *diag;
	int			ntri = 0,
				ndiag = 0,
				left = nv,
				i,
				stall = 0;
	double		area2 = 0;
	const char *err = NULL;

	plane_frame(g, f);

	pt = SC_MALLOC(sizeof(pt2) * nv);
	prev = SC_MALLOC(sizeof(int) * nv);
	next = SC_MALLOC(sizeof(int) * nv);
	owner = SC_MALLOC(sizeof(int) * nv);
	tri = SC_MALLOC(sizeof(int) * 3 * nv);
	parent = SC_MALLOC(sizeof(int) * nv);
	plen = SC_MALLOC(sizeof(int) * nv);
	part = SC_MALLOC(sizeof(int *) * nv);
	diag = SC_MALLOC(sizeof(diag_t) * 3 * nv);
	memset(part, 0, sizeof(int *) * nv);

	/* every vertex strictly inside g's hemisphere, else the plane cannot hold it */
	for (i = 0; i < nv; i++)
	{
		if (sc_dot(r->v[i], g) <= 1e-9)
		{
			err = "polygon must be smaller than a hemisphere";
			goto done;
		}
		pt[i] = gnomonic(f, r->v[i]);
	}

	/* counter-clockwise in the plane is interior-on-the-left on the sphere */
	for (i = 0; i < nv; i++)
		area2 += pt[i].x * pt[(i + 1) % nv].y - pt[(i + 1) % nv].x * pt[i].y;
	if (area2 == 0)
	{
		err = "polygon has zero area";
		goto done;
	}
	if (area2 < 0 && (err = poly_reverse(r, pt)) != NULL)
		goto done;

	/*
	 * Simple: no two edges meet except adjacent ones at their shared vertex,
	 * and no edge doubles back along the one before it.
	 */
	for (i = 0; i < nv; i++)
	{
		pt2			a = pt[i],
					b = pt[(i + 1) % nv],
					c = pt[(i + 2) % nv];

		if (straight2(a, b, c) &&
			(b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y) < 0)
		{
			err = "polygon edges must not cross (the polygon must be simple)";
			goto done;
		}
		for (int j = i + 2; j < nv; j++)
		{
			if (i == 0 && j == nv - 1)
				continue;		/* adjacent across the wrap */
			if (segments_meet(a, b, pt[j], pt[(j + 1) % nv]))
			{
				err = "polygon edges must not cross (the polygon must be simple)";
				goto done;
			}
		}
	}

	/*
	 * Ear clipping.  owner[i] is the triangle on the far side of the
	 * remaining ring's edge i -> next[i], or -1 for an edge of the polygon
	 * itself; each ear clipped across an owned edge records that diagonal.
	 */
	for (i = 0; i < nv; i++)
	{
		prev[i] = (i + nv - 1) % nv;
		next[i] = (i + 1) % nv;
		owner[i] = -1;
	}
	i = 0;
	while (left >= 3)
	{
		int			p = prev[i],
					q = next[i],
					ear;

		if (straight2(pt[p], pt[i], pt[q]))
		{
			/* a vertex on a straight run: drop it, no triangle */
			next[p] = q;
			prev[q] = p;
			owner[p] = -1;
			left--;
			stall = 0;
			i = p;
			continue;
		}
		ear = orient2(pt[p], pt[i], pt[q]) > 0;
		for (int k = next[q]; ear && k != p; k = next[k])
			if (orient2(pt[p], pt[i], pt[k]) >= 0 && orient2(pt[i], pt[q], pt[k]) >= 0 &&
				orient2(pt[q], pt[p], pt[k]) >= 0)
				ear = 0;		/* another vertex inside or on the triangle */
		if (!ear)
		{
			i = q;
			if (++stall > left)
			{
				/*
				 * No ear left.  For a simple polygon that only happens when
				 * what remains is a sliver thinner than SC_PLANE_EPS -- the
				 * rounding of the cuts so far -- and then the triangles are
				 * already the polygon.
				 */
				double		a2 = 0,
							per = 0;
				int			k = i;

				do
				{
					a2 += pt[k].x * pt[next[k]].y - pt[next[k]].x * pt[k].y;
					per += hypot(pt[next[k]].x - pt[k].x, pt[next[k]].y - pt[k].y);
					k = next[k];
				} while (k != i);
				if (fabs(a2) <= SC_PLANE_EPS * per)
					break;
				err = "polygon is degenerate (no triangulation)";
				goto done;
			}
			continue;
		}

		tri[3 * ntri] = p;
		tri[3 * ntri + 1] = i;
		tri[3 * ntri + 2] = q;
		if (owner[p] >= 0)
			diag[ndiag++] = (diag_t) {p, i, ntri, owner[p]};
		if (owner[i] >= 0)
			diag[ndiag++] = (diag_t) {i, q, ntri, owner[i]};
		if (left == 3)
		{
			if (owner[q] >= 0)
				diag[ndiag++] = (diag_t) {q, p, ntri, owner[q]};
			ntri++;
			break;
		}
		next[p] = q;
		prev[q] = p;
		owner[p] = ntri;		/* the ring's new edge p -> q is the triangle's q -> p */
		ntri++;
		left--;
		stall = 0;
		i = q;
	}
	if (ntri == 0)
	{
		err = "polygon has zero area";
		goto done;
	}

	for (int t = 0; t < ntri; t++)
		r->area += tri_area(r->v[tri[3 * t]], r->v[tri[3 * t + 1]], r->v[tri[3 * t + 2]]);

	/* Hertel-Mehlhorn: drop each diagonal that leaves both corners convex */
	for (int t = 0; t < ntri; t++)
	{
		parent[t] = t;
		plen[t] = 3;
		part[t] = SC_MALLOC(sizeof(int) * 3);
		memcpy(part[t], &tri[3 * t], sizeof(int) * 3);
	}
	for (int d = 0; d < ndiag; d++)
	{
		int			A = uf_find(parent, diag[d].ta),
					B = uf_find(parent, diag[d].tb),
					na = plen[A],
					nb = plen[B],
					pa = part_pos(part[A], na, diag[d].a),	/* A: ... a, b ... */
					pb = part_pos(part[B], nb, diag[d].b),	/* B: ... b, a ... */
				   *m;
		int			a_prev = part[A][(pa + na - 1) % na],
					b_next = part[A][(pa + 2) % na],
					a_next = part[B][(pb + 2) % nb],
					b_prev = part[B][(pb + nb - 1) % nb];
		int			k = 0;

		if (orient2(pt[a_prev], pt[diag[d].a], pt[a_next]) < 0 ||
			orient2(pt[b_prev], pt[diag[d].b], pt[b_next]) < 0)
			continue;			/* the merged piece would be reflex at a or b */

		/* A from b round to a, then B after a round to before b */
		m = SC_MALLOC(sizeof(int) * (na + nb - 2));
		for (int j = 0; j < na; j++)
			m[k++] = part[A][(pa + 1 + j) % na];
		for (int j = 0; j < nb - 2; j++)
			m[k++] = part[B][(pb + 2 + j) % nb];
		SC_FREE(part[A]);
		SC_FREE(part[B]);
		part[B] = NULL;
		part[A] = m;
		plen[A] = k;
		parent[B] = A;
	}

	/* the pieces, as convex polygons on the sphere */
	r->parts = SC_MALLOC(sizeof(sc_region) * ntri);
	for (int t = 0; t < ntri; t++)
	{
		sc_region  *q;

		if (parent[t] != t)
			continue;
		q = &r->parts[r->nparts++];
		memset(q, 0, sizeof(*q));
		q->kind = SC_REGION_POLY;
		q->nv = plen[t];
		q->v = SC_MALLOC(sizeof(sc_vec3) * q->nv);
		q->n = SC_MALLOC(sizeof(sc_vec3) * q->nv);
		for (int j = 0; j < q->nv; j++)
			q->v[j] = r->v[part[t][j]];
		if ((err = poly_setup(q)) != NULL)
			goto done;
	}

done:
	for (int t = 0; t < ntri; t++)
		if (part[t])
			SC_FREE(part[t]);
	SC_FREE(pt);
	SC_FREE(prev);
	SC_FREE(next);
	SC_FREE(owner);
	SC_FREE(tri);
	SC_FREE(parent);
	SC_FREE(plen);
	SC_FREE(part);
	SC_FREE(diag);
	return err;
}

const char *
sc_region_poly(sc_region *r, int nv, const double *ra_deg, const double *dec_deg,
			   bool need_covering)
{
	sc_vec3		g = {0, 0, 0};
	double		orient = 0;
	const char *err;
	int			convex = 1;

	memset(r, 0, sizeof(*r));
	if (nv < 3)
		return "polygon needs at least 3 vertices";

	r->kind = SC_REGION_POLY;
	r->nv = nv;
	r->v = SC_MALLOC(sizeof(sc_vec3) * nv);
	r->n = SC_MALLOC(sizeof(sc_vec3) * nv);
	for (int i = 0; i < nv; i++)
	{
		if (!isfinite(ra_deg[i]) || !isfinite(dec_deg[i]) ||
			dec_deg[i] < -90.0 || dec_deg[i] > 90.0)
			return "polygon vertex out of range";
		r->v[i] = sc_radec2vec(ra_deg[i], dec_deg[i]);
		g.x += r->v[i].x;
		g.y += r->v[i].y;
		g.z += r->v[i].z;
	}
	if (sqrt(sc_dot(g, g)) < 1e-9)
		return "polygon must be smaller than a hemisphere";
	g = vnormalize(g);

	if ((err = poly_setup(r)) != NULL)
		return err;
	for (int i = 0; i < nv; i++)
		orient += sc_dot(r->n[i], g);
	if (orient < 0)
	{
		/* clockwise: reverse so the interior is on the left of each edge */
		for (int i = 0; i < nv / 2; i++)
		{
			sc_vec3		t = r->v[i];

			r->v[i] = r->v[nv - 1 - i];
			r->v[nv - 1 - i] = t;
		}
		if ((err = poly_setup(r)) != NULL)
			return err;
	}

	/* convex: every vertex on the inner side of every edge */
	for (int i = 0; i < nv && convex; i++)
	{
		if (sc_dot(r->n[i], g) <= 0)
			convex = 0;
		for (int j = 0; j < nv && convex; j++)
		{
			if (j == i || j == (i + 1) % nv)
				continue;
			if (sc_dot(r->n[i], r->v[j]) < -1e-12)
				convex = 0;
		}
	}

	if (!convex)
	{
		if ((err = poly_nonconvex(r, g)) != NULL)
			return err;
	}
	else
	{
		/* area: fan of triangles (g, v_i, v_i+1) */
		for (int i = 0; i < nv; i++)
			r->area += tri_area(g, r->v[i], r->v[(i + 1) % nv]);
	}

	if (!need_covering)
		return NULL;				/* sin_rho[]: only sc_region_classify_cap() reads this */

	for (int k = 0; k <= SC_MAX_ORDER; k++)
		r->sin_rho[k] = sin(sc_pixrad(k) + SC_ANG_EPS);
	return NULL;
}

void
sc_region_free(sc_region *r)
{
	if (r->v)
		SC_FREE(r->v);
	if (r->n)
		SC_FREE(r->n);
	r->v = r->n = NULL;
	if (r->parts)
	{
		for (int i = 0; i < r->nparts; i++)
			sc_region_free(&r->parts[i]);
		SC_FREE(r->parts);
	}
	r->parts = NULL;
	r->nparts = 0;
}

int
sc_region_contains(const sc_region *r, sc_vec3 p)
{
	if (r->kind == SC_REGION_CONE)
		return r->radius >= 0 &&
			sc_chord2(p, r->center) <= 4.0 * pow(sin(r->radius / 2.0), 2);

	/*
	 * -1e-12, not 0: p and r->n[] are typically computed independently (e.g.
	 * the two sides of a region-region overlap test on identical or
	 * boundary-touching polygons), so a point exactly on the boundary can
	 * come out a hair negative by construction rounding alone.  The cone
	 * branch above is already boundary-inclusive (<=); without this slack
	 * the polygon branch is not, and a polygon fails to contain its own
	 * vertices -- see poly_setup's identical tolerance on the same kind of
	 * dot product, a few lines up in this file.
	 */
	if (r->nparts > 0)
	{
		/* non-convex: in some convex piece (shared diagonals overlap in the slack) */
		for (int k = 0; k < r->nparts; k++)
			if (sc_region_contains(&r->parts[k], p))
				return 1;
		return 0;
	}
	for (int i = 0; i < r->nv; i++)
		if (sc_dot(r->n[i], p) < -1e-12)
			return 0;
	return 1;
}

/* angular distance from p to the minor great-circle arc a-b (unit normal n) */
static double arc_dist(sc_vec3 p, sc_vec3 a, sc_vec3 b, sc_vec3 n);

/* exact cell geometry for cone classification (see classify_cone_exact) */
int			sc_exact_cells = 1;



static double
arc_dist(sc_vec3 p, sc_vec3 a, sc_vec3 b, sc_vec3 n)
{
	double		s = sc_dot(n, p);
	sc_vec3		q = {p.x - s * n.x, p.y - s * n.y, p.z - s * n.z};

	if (sc_dot(sc_cross(a, q), n) >= 0 && sc_dot(sc_cross(q, b), n) >= 0)
		return asin(fmin(1.0, fabs(s)));
	return fmin(sc_angle(p, a), sc_angle(p, b));
}

/*
 * Classify a cell against a cone using the cell's own geometry.
 *
 * Only OUT can lose rows -- IN and PARTIAL both keep the cell, and the exact
 * predicate re-tests every row the index returns -- so OUT is the only
 * decision that needs a bound rather than an estimate.  It uses
 *
 *     R_cell = min(sc_pixrad(order), max_i angle(centre, corner_i)),
 *
 * an upper bound on the distance from the cell's centre to any of its points:
 * sc_pixrad is healpix_base's analytic worst case over the sphere, and the
 * corner term is the cell's own extent, which test/healpix_selftest.c checks
 * over 1.6M boundary points at every order (the extremum of distance from the
 * centre over a cell is attained at a corner).  A cell is OUT when its whole
 * cap lies beyond the cone.
 *
 * IN may use the corners and edge midpoints directly, tightly: calling a cell
 * IN that is only partly inside costs false positives, never rows.
 *
 * An earlier version also decided OUT from the distance to the corner chords,
 * inflating them by 4x the edge's departure from its chord at the midpoint.
 * That is not a bound: where an edge crosses its chord plane near the midpoint
 * the estimate collapses, and healpix_selftest measures the true departure at
 * up to 159x the midpoint value (order 18, deterministic -- fixed seed).  The
 * absolute error was ~1e-8 rad, below the 0.4 mas leaf cell, but small is not
 * zero, so the test is gone.  healpix_selftest prints this ratio itself each
 * run; if it changes, this comment (and the paper's Sect. 2.4) are stale.
 */
static sc_class
classify_cone_exact(const sc_region *r, int order, int64_t pix, double *f_out)
{
	sc_vec3		c[4],
				mid[4],
				centre;
	double		dmax = 0,
				bulge = 0,
				rcell = 0,
				dcentre;

	if (r->radius < 0)
	{
		*f_out = 1.0;
		return SC_OUT;
	}

	sc_pix_corners(order, pix, c);
	centre = sc_pix2vec(order, pix);
	for (int e = 0; e < 4; e++)
	{
		mid[e] = sc_pix_edge_point(order, pix, e, 0.5);
		rcell = fmax(rcell, sc_angle(centre, c[e]));
		/* how far this edge's midpoint leaves the corner chord: IN only */
		bulge = fmax(bulge, sc_angle(centre, mid[e]) - rcell);
	}
	rcell = fmin(rcell, sc_pixrad(order));
	bulge = fmax(bulge, 0.0);

	for (int i = 0; i < 4; i++)
		dmax = fmax(dmax, fmax(sc_angle(r->center, c[i]), sc_angle(r->center, mid[i])));

	/* IN: tight, and safe even if it is optimistic (the cell is kept whole) */
	if (dmax + bulge <= r->radius)
	{
		*f_out = 0.0;
		return SC_IN;
	}

	/* OUT: the cell's proven cap must clear the cone entirely */
	dcentre = sc_angle(r->center, centre);
	if (dcentre - rcell > r->radius + SC_ANG_EPS)
	{
		*f_out = 1.0;
		return SC_OUT;
	}
	*f_out = clamp01(0.5 + (dcentre - rcell - r->radius) / (2.0 * fmax(dmax, 1e-300)));
	return SC_PARTIAL;
}

/*
 * Classify pixel (order, pix).  The pixel is approximated by the cap
 * (centre, sc_pixrad(order)) that contains it, so OUT and IN are always
 * correct; PARTIAL may be returned for pixels that are really OUT or IN.
 * *f_out receives a rough estimate of the fraction of the pixel outside the
 * region (used only to prioritise refinement).
 */
/* the cap test alone: cheap, conservative, used while descending */
sc_class
sc_region_classify_cap(const sc_region *r, int order, int64_t pix, double *f_out)
{
	sc_vec3		c;
	double		rho;

	c = sc_pix2vec(order, pix);
	rho = sc_pixrad(order);

	if (r->kind == SC_REGION_CONE)
	{
		double		c2 = sc_chord2(c, r->center);
		double		d;

		ensure_cone_bounds(r, order);
		if (c2 > r->out_c2[order])
		{
			*f_out = 1.0;
			return SC_OUT;
		}
		if (c2 <= r->in_c2[order])
		{
			*f_out = 0.0;
			return SC_IN;
		}
		d = 2.0 * asin(fmin(1.0, sqrt(c2) / 2.0));
		*f_out = clamp01(0.5 + (d - r->radius) / (2.0 * rho));
		return SC_PARTIAL;
	}
	else if (r->nparts > 0)
	{
		/*
		 * Non-convex: a cap that does not reach the boundary ring is wholly
		 * inside or wholly outside, as its centre is.  Only edges whose great
		 * circle passes within the cap (|n.c| <= sin(rho + eps)) can reach it;
		 * the pieces' diagonals are not boundary and play no part.
		 */
		double		s = r->sin_rho[order];
		double		dist = M_PI;
		int			inside;

		for (int i = 0; i < r->nv; i++)
			if (fabs(sc_dot(r->n[i], c)) <= s)
				dist = fmin(dist, arc_dist(c, r->v[i], r->v[(i + 1) % r->nv], r->n[i]));
		inside = sc_region_contains(r, c);
		if (dist > rho + SC_ANG_EPS)
		{
			*f_out = inside ? 0.0 : 1.0;
			return inside ? SC_IN : SC_OUT;
		}
		*f_out = clamp01(0.5 - (inside ? dist : -dist) / (2.0 * rho));
		return SC_PARTIAL;
	}
	else
	{
		double		s = r->sin_rho[order];
		double		mind = 2.0;
		int			all_in = 1;
		double		sd;

		for (int i = 0; i < r->nv; i++)
		{
			double		d = sc_dot(r->n[i], c);

			if (d < -s)
			{
				*f_out = 1.0;
				return SC_OUT;
			}
			if (d < s)
				all_in = 0;
			if (d < mind)
				mind = d;
		}
		if (all_in)
		{
			*f_out = 0.0;
			return SC_IN;
		}
		if (mind >= 0)
			sd = asin(fmin(1.0, mind));
		else
		{
			double		dist = M_PI;

			for (int i = 0; i < r->nv; i++)
				dist = fmin(dist, arc_dist(c, r->v[i], r->v[(i + 1) % r->nv], r->n[i]));
			if (dist > rho + SC_ANG_EPS)
			{
				*f_out = 1.0;
				return SC_OUT;
			}
			sd = -dist;
		}
		*f_out = clamp01(0.5 - sd / (2.0 * rho));
		return SC_PARTIAL;
	}
}

sc_class
sc_region_classify(const sc_region *r, int order, int64_t pix, double *f_out)
{
	if (r->kind == SC_REGION_CONE && sc_exact_cells)
		return classify_cone_exact(r, order, pix, f_out);
	return sc_region_classify_cap(r, order, pix, f_out);
}

/*
 * sc_region_classify(...) == SC_OUT, for callers that keep IN and PARTIAL
 * cells alike.  classify_cone_exact() answers OUT only when its OUT condition
 * dcentre - rcell > radius + SC_ANG_EPS holds and its IN test does not fire
 * first.  That condition needs the centre and corners alone, so evaluate it
 * first, with the same vectors, sc_angle() calls and fmax/fmin as that
 * function: when it fails the answer is "not OUT" without the edge midpoints
 * (most of the trig), and only a cell about to be dropped pays for the full
 * test.  The answer is identical, not close.
 */
static int
cell_is_out(const sc_region *r, int order, int64_t pix)
{
	double		fo;

	if (r->kind == SC_REGION_CONE && sc_exact_cells && r->radius >= 0)
	{
		sc_vec3		centre = sc_pix2vec(order, pix),
					c[4];
		double		dcentre = sc_angle(r->center, centre),
					rcell = 0;

		if (dcentre <= r->radius + SC_ANG_EPS)
			return 0;			/* rcell >= 0: the OUT condition cannot hold */
		sc_pix_corners(order, pix, c);
		for (int e = 0; e < 4; e++)
			rcell = fmax(rcell, sc_angle(centre, c[e]));
		rcell = fmin(rcell, sc_pixrad(order));
		if (!(dcentre - rcell > r->radius + SC_ANG_EPS))
			return 0;
	}
	return sc_region_classify(r, order, pix, &fo) == SC_OUT;
}

/* distance from p to the region's boundary, 0 if inside (radians) */
double
sc_region_distance(const sc_region *r, sc_vec3 p)
{
	double		d;

	if (r->kind == SC_REGION_CONE)
	{
		d = sc_angle(p, r->center) - r->radius;
		return d > 0 ? d : 0.0;
	}
	if (sc_region_contains(r, p))
		return 0.0;
	d = M_PI;
	for (int i = 0; i < r->nv; i++)
		d = fmin(d, arc_dist(p, r->v[i], r->v[(i + 1) % r->nv], r->n[i]));
	return d;
}

/*
 * A polygon's centre of mass: the direction of the integral of the position
 * vector over it.  Two ways to the same integral, each where it is well
 * conditioned:
 *
 * - By Stokes, half the sum over the edges of arc length times unit edge
 *   normal.  Exact, but the terms are as long as the edges and cancel down
 *   to the area, so the error relative to the polygon's size grows as
 *   1e-16 / size^2: 1e-6 at a hundredth of a degree, worse than useless
 *   below a milliarcsecond.  Used above 0.01 rad, where it is good to
 *   ~1e-13 rad.
 *
 * - In the gnomonic plane at the vertex mean g, the integrand is
 *   (X e1 + Y e2 + g) / (1 + X^2 + Y^2)^2 dX dY, smooth and nearly flat on
 *   a small polygon.  A degree-5 rule (Dunavant, 7 points) on the signed fan
 *   of triangles from g -- valid for a non-convex polygon too -- is exact to
 *   ~size^6 relatively, and works in coordinates of the polygon's own size,
 *   so its rounding is ~1e-16 rad absolute.  Used up to 0.01 rad.
 */
static sc_vec3
poly_centroid(const sc_region *r)
{
	static const double qw[7] = {0.225, 0.132394152788506, 0.132394152788506, 0.132394152788506,
								 0.125939180544827, 0.125939180544827, 0.125939180544827};
	static const double qa = 0.059715871789770, qb = 0.470142064105115,
				qc = 0.797426985353087, qd = 0.101286507323456;
	const double ql[7][3] = {{1.0 / 3, 1.0 / 3, 1.0 / 3},
							 {qa, qb, qb}, {qb, qa, qb}, {qb, qb, qa},
							 {qc, qd, qd}, {qd, qc, qd}, {qd, qd, qc}};
	sc_vec3		g = {0, 0, 0},
				f[3],
				sum = {0, 0, 0};
	double		size = 0,
				i0 = 0,
				ix = 0,
				iy = 0;
	int			nv = r->nv;

	for (int i = 0; i < nv; i++)
	{
		g.x += r->v[i].x;
		g.y += r->v[i].y;
		g.z += r->v[i].z;
	}
	g = vnormalize(g);
	for (int i = 0; i < nv; i++)
		size = fmax(size, sc_angle(g, r->v[i]));

	if (size > 0.01)
	{
		for (int i = 0; i < nv; i++)
		{
			double		len = sc_angle(r->v[i], r->v[(i + 1) % nv]);

			sum.x += len * r->n[i].x;
			sum.y += len * r->n[i].y;
			sum.z += len * r->n[i].z;
		}
		return vnormalize(sum);
	}

	plane_frame(g, f);
	for (int i = 0; i < nv; i++)
	{
		pt2			p = gnomonic(f, r->v[i]),
					q = gnomonic(f, r->v[(i + 1) % nv]);
		double		a = 0.5 * (p.x * q.y - q.x * p.y);

		for (int k = 0; k < 7; k++)
		{
			double		x = ql[k][1] * p.x + ql[k][2] * q.x,
						y = ql[k][1] * p.y + ql[k][2] * q.y,
						w = 1.0 + x * x + y * y,
						d = a * qw[k] / (w * w);

			i0 += d;
			ix += d * x;
			iy += d * y;
		}
	}
	return gnomonic_inv(f, (pt2) {ix / i0, iy / i0});
}

sc_vec3
sc_region_centroid(const sc_region *r)
{
	if (r->kind == SC_REGION_CONE)
		return r->center;
	return poly_centroid(r);
}

/* the farthest point of a region from p, as an angle (radians) */
static double
region_farthest(const sc_region *r, sc_vec3 p)
{
	double		d = 0;

	sc_vec3		q = {-p.x, -p.y, -p.z};

	if (r->kind == SC_REGION_CONE)
		return sc_angle(p, r->center) + r->radius;

	/*
	 * A polygon: the farthest point is on the boundary unless the polygon
	 * holds p's antipode.  Along one edge the distance from p peaks at a
	 * vertex, or inside the edge where it passes the point of its great
	 * circle farthest from p (p's antipode projected onto the circle) -- the
	 * second only for edges more than 90 degrees from p, so for most
	 * polygons this is the vertex maximum, computed as such.
	 */
	if (sc_region_contains(r, q))
		return M_PI;
	for (int i = 0; i < r->nv; i++)
	{
		sc_vec3		a = r->v[i],
					b = r->v[(i + 1) % r->nv],
					n = r->n[i];
		double		s = sc_dot(n, q);
		sc_vec3		f = {q.x - s * n.x, q.y - s * n.y, q.z - s * n.z};

		d = fmax(d, sc_angle(p, a));
		if (sc_dot(sc_cross(a, f), n) > 0 && sc_dot(sc_cross(f, b), n) > 0)
			d = fmax(d, M_PI - asin(fmin(1.0, fabs(s))));
	}
	return d;
}

void
sc_region_bounding_cap(const sc_region *r, sc_vec3 *center, double *radius)
{
	*center = sc_region_centroid(r);
	*radius = (r->kind == SC_REGION_CONE) ? r->radius : region_farthest(r, *center);
}

/* do the minor arcs a1-a2 and b1-b2 cross? */
static int
arcs_cross(sc_vec3 a1, sc_vec3 a2, sc_vec3 na, sc_vec3 b1, sc_vec3 b2, sc_vec3 nb)
{
	double		d1 = sc_dot(na, b1),
				d2 = sc_dot(na, b2),
				e1 = sc_dot(nb, a1),
				e2 = sc_dot(nb, a2);
	sc_vec3		x;
	double		nn;

	if (d1 * d2 > 0 || e1 * e2 > 0)
		return 0;				/* both endpoints on one side of the other's plane */
	x = sc_cross(na, nb);
	nn = sqrt(sc_dot(x, x));
	if (nn < 1e-15)
		return 0;				/* coincident great circles: treated as no crossing */
	x.x /= nn;
	x.y /= nn;
	x.z /= nn;
	/* one of +-x is the crossing; it must lie inside both arcs */
	for (int s = 0; s < 2; s++)
	{
		sc_vec3		p = s ? (sc_vec3) {-x.x, -x.y, -x.z} : x;

		if (sc_dot(sc_cross(a1, p), na) >= 0 && sc_dot(sc_cross(p, a2), na) >= 0 &&
			sc_dot(sc_cross(b1, p), nb) >= 0 && sc_dot(sc_cross(p, b2), nb) >= 0)
			return 1;
	}
	return 0;
}

/*
 * a inside b, b a non-convex polygon.  A cone is inside when its centre is
 * and no boundary edge of b comes nearer the centre than the radius.  A
 * polygon is inside when its vertices are and none of its edges leaves b:
 * all of a then lies in b's hemisphere, so each edge of a is a straight
 * segment in b's gnomonic plane.  Cut it wherever it meets b's boundary
 * (crossing, touching or running along it); between two cuts the edge is
 * wholly in or wholly out, as the midpoint shows.  Extra cuts are harmless,
 * so near-misses count as meetings.
 */
static int
contained_in_nonconvex(const sc_region *a, const sc_region *b)
{
	const sc_vec3 *f = b->frame;
	double	   *t;
	int			ok = 1;

	if (a->kind == SC_REGION_CONE)
	{
		if (!sc_region_contains(b, a->center))
			return 0;
		for (int j = 0; j < b->nv; j++)
			if (arc_dist(a->center, b->v[j], b->v[(j + 1) % b->nv], b->n[j]) < a->radius)
				return 0;
		return 1;
	}
	for (int i = 0; i < a->nv; i++)
		if (!sc_region_contains(b, a->v[i]) || sc_dot(a->v[i], f[0]) <= 0)
			return 0;

	t = SC_MALLOC(sizeof(double) * (2 * b->nv + 2));
	for (int i = 0; i < a->nv && ok; i++)
	{
		pt2			P = gnomonic(f, a->v[i]),
					Q = gnomonic(f, a->v[(i + 1) % a->nv]),
					u = {Q.x - P.x, Q.y - P.y};
		double		uu = u.x * u.x + u.y * u.y;
		int			nt = 0;
		const double slack = 1e-9;

		t[nt++] = 0.0;
		t[nt++] = 1.0;
		for (int j = 0; j < b->nv; j++)
		{
			pt2			C = gnomonic(f, b->v[j]),
						D = gnomonic(f, b->v[(j + 1) % b->nv]),
						w = {D.x - C.x, D.y - C.y},
						cp = {C.x - P.x, C.y - P.y};
			double		den = u.x * w.y - u.y * w.x,
						scale = sqrt(uu * (w.x * w.x + w.y * w.y));

			if (fabs(den) > 1e-12 * scale)
			{
				double		tt = (cp.x * w.y - cp.y * w.x) / den,
							ss = (cp.x * u.y - cp.y * u.x) / den;

				if (tt > -slack && tt < 1 + slack && ss > -slack && ss < 1 + slack)
					t[nt++] = fmin(1.0, fmax(0.0, tt));
			}
			else
			{
				/* parallel, perhaps collinear: cut at b's endpoints along a's edge */
				double		tc = (cp.x * u.x + cp.y * u.y) / uu,
							td = ((D.x - P.x) * u.x + (D.y - P.y) * u.y) / uu;

				if (tc > 0 && tc < 1)
					t[nt++] = tc;
				if (td > 0 && td < 1)
					t[nt++] = td;
			}
		}

		/* insertion sort: nt is small */
		for (int k = 1; k < nt; k++)
		{
			double		x = t[k];
			int			m = k - 1;

			for (; m >= 0 && t[m] > x; m--)
				t[m + 1] = t[m];
			t[m + 1] = x;
		}
		for (int k = 0; k + 1 < nt && ok; k++)
		{
			double		mid = 0.5 * (t[k] + t[k + 1]);

			if (t[k + 1] - t[k] <= 1e-15)
				continue;
			if (!sc_region_contains(b, gnomonic_inv(f, (pt2) {P.x + mid * u.x, P.y + mid * u.y})))
				ok = 0;
		}
	}
	SC_FREE(t);
	return ok;
}

/* ADQL CONTAINS: is every point of a also in b? */
int
sc_region_contains_region(const sc_region *a, const sc_region *b)
{
	if (a->kind == SC_REGION_CONE && a->radius < 0)
		return 1;				/* the empty region is inside anything */
	if (b->kind == SC_REGION_CONE)
		return region_farthest(a, b->center) <= b->radius;
	if (b->nparts > 0)
		return contained_in_nonconvex(a, b);

	/* b is a convex polygon (a may be non-convex: its edges join its vertices) */
	if (a->kind == SC_REGION_CONE)
	{
		if (!sc_region_contains(b, a->center))
			return 0;
		for (int i = 0; i < b->nv; i++)
			if (asin(fmin(1.0, sc_dot(b->n[i], a->center))) < a->radius)
				return 0;
		return 1;
	}
	for (int i = 0; i < a->nv; i++)
		if (!sc_region_contains(b, a->v[i]))
			return 0;
	return 1;					/* b convex: a's vertices inside is enough */
}

/* ADQL INTERSECTS: do a and b share any point? */
int
sc_region_overlaps(const sc_region *a, const sc_region *b)
{
	if ((a->kind == SC_REGION_CONE && a->radius < 0) ||
		(b->kind == SC_REGION_CONE && b->radius < 0))
		return 0;
	if (a->kind == SC_REGION_CONE && b->kind == SC_REGION_CONE)
		return sc_angle(a->center, b->center) <= a->radius + b->radius;
	if (a->kind == SC_REGION_CONE)
		return sc_region_distance(b, a->center) <= a->radius;
	if (b->kind == SC_REGION_CONE)
		return sc_region_distance(a, b->center) <= b->radius;

	/* two simple polygons: a vertex inside the other, or crossing edges */
	for (int i = 0; i < a->nv; i++)
		if (sc_region_contains(b, a->v[i]))
			return 1;
	for (int i = 0; i < b->nv; i++)
		if (sc_region_contains(a, b->v[i]))
			return 1;
	for (int i = 0; i < a->nv; i++)
		for (int j = 0; j < b->nv; j++)
			if (arcs_cross(a->v[i], a->v[(i + 1) % a->nv], a->n[i],
						   b->v[j], b->v[(j + 1) % b->nv], b->n[j]))
				return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* density model                                                      */
/* ------------------------------------------------------------------ */

/* fraction of rows with cell < x, piecewise linear over histogram buckets */
static double
hist_cdf(const sc_density *d, int64_t x)
{
	const int64_t *b = d->bounds;
	int			nb = d->nbounds - 1;
	int			lo = 0,
				hi = nb - 1;
	double		w,
				frac;

	if (x <= b[0])
		return 0.0;
	if (x > b[nb])
		return 1.0;
	while (lo < hi)
	{
		int			mid = (lo + hi + 1) / 2;

		if (b[mid] < x)
			lo = mid;
		else
			hi = mid - 1;
	}
	w = (double) (b[lo + 1] - b[lo]);
	frac = (w > 0) ? (double) (x - b[lo]) / w : 1.0;
	return (lo + fmin(frac, 1.0)) / nb;
}

double
sc_density_rows(const sc_density *d, int64_t lo, int64_t hi)
{
	if (hi < lo)
		return 0.0;

	/*
	 * The count map, when there is one.  Binary search for the first cell that
	 * can overlap, then sum: whole cells contribute their count, the two ends
	 * contribute the fraction of their cell that the interval covers.  Inside
	 * one map cell this is still an assumption of uniformity, but the map is
	 * built so that a cell holds few enough rows for that to be harmless.
	 */
	if (d != NULL && d->nmap > 0)
	{
		int			a = 0,
					b = d->nmap - 1,
					i;
		double		rows = 0;

		while (a < b)					/* first cell with map_hi >= lo */
		{
			int			m = (a + b) / 2;

			if (d->map_hi[m] < lo)
				a = m + 1;
			else
				b = m;
		}
		for (i = a; i < d->nmap && d->map_lo[i] <= hi; i++)
		{
			int64_t		s = (d->map_lo[i] > lo) ? d->map_lo[i] : lo;
			int64_t		e = (d->map_hi[i] < hi) ? d->map_hi[i] : hi;
			double		span = (double) (d->map_hi[i] - d->map_lo[i]) + 1.0;

			if (e < s)
				continue;
			rows += d->map_n[i] * (((double) (e - s) + 1.0) / span);
		}
		return rows;
	}

	if (d == NULL || d->nbounds < 2)
	{
		double		n = (d && d->ntotal > 0) ? d->ntotal : 1e6;

		return n * ((double) (hi - lo) + 1.0) / (double) SC_NPIX29;
	}
	return d->ntotal * (hist_cdf(d, hi + 1) - hist_cdf(d, lo));
}

/* ------------------------------------------------------------------ */
/* covering                                                           */
/* ------------------------------------------------------------------ */

/*
 * Fast seed for small cones.  Descending from the 12 base pixels costs
 * ~10 classifications per order, which dominates per-probe cost in
 * cross-matches.  Instead, bound the disc by an interval in (tt, z), where
 * tt = phi * 2/pi, and push that interval through the same piecewise formulas
 * loc2pix() uses (equatorial zone: jp/jm linear in (tt, z); polar caps:
 * jp/jm = tmp * (tp, 1-tp)).  Every point of the disc lies in the interval,
 * so every pixel it can touch is enumerated -- no neighbour tables and no
 * sampling.  Candidates are then classified like any other cell.
 */
#define SEED_MAX 192

typedef struct
{
	int			n;
	int64_t		pix[SEED_MAX];
} seed_t;

static int
seed_add(seed_t *s, int64_t pix)
{
	for (int i = 0; i < s->n; i++)
		if (s->pix[i] == pix)
			return 1;
	if (s->n >= SEED_MAX)
		return 0;
	s->pix[s->n++] = pix;
	return 1;
}

/* equatorial formula, 0 <= ta <= tb < 4, -2/3 <= za <= zb <= 2/3 */
static int
seed_equatorial(int order, double ta, double tb, double za, double zb, seed_t *s)
{
	int64_t		n = (int64_t) 1 << order;
	const double e = 1e-6;			/* pixels */
	int64_t		jp_lo = (int64_t) floor(n * (0.5 + ta - 0.75 * zb) - e),
				jp_hi = (int64_t) floor(n * (0.5 + tb - 0.75 * za) + e),
				jm_lo = (int64_t) floor(n * (0.5 + ta + 0.75 * za) - e),
				jm_hi = (int64_t) floor(n * (0.5 + tb + 0.75 * zb) + e);

	if (jp_lo < 0)
		jp_lo = 0;
	if (jm_lo < 0)
		jm_lo = 0;
	if ((jp_hi - jp_lo + 1) * (jm_hi - jm_lo + 1) > SEED_MAX)
		return 0;
	for (int64_t jp = jp_lo; jp <= jp_hi; jp++)
		for (int64_t jm = jm_lo; jm <= jm_hi; jm++)
		{
			int64_t		ifp = jp >> order,
						ifm = jm >> order;
			int			face;

			if (ifp > 4 || ifm > 4)
				continue;
			if (ifp == ifm)
				face = (int) (ifp | 4);
			else if (ifp < ifm)
				face = (int) ifp;
			else
				face = (int) (ifm + 8);
			if (!seed_add(s, sc_xyf2nest(order, jm & (n - 1), n - (jp & (n - 1)) - 1, face)))
				return 0;
		}
	return 1;
}

/* polar formula for quadrant q, q <= ta <= tb <= q+1, |dec| in [dnear, dfar] (rad) */
static int
seed_polar(int order, int north, int q, double ta, double tb,
		   double dnear, double dfar, seed_t *s)
{
	int64_t		n = (int64_t) 1 << order;
	const double e = 1e-6;
	/* tmp = n * sqrt(3 (1 - |z|)) written with cos() for precision at the pole */
	double		tmp_lo = n * cos(dfar) / sqrt((1.0 + sin(dfar)) / 3.0) * (1 - 1e-12);
	double		tmp_hi = n * cos(dnear) / sqrt((1.0 + sin(dnear)) / 3.0) * (1 + 1e-12);
	double		tp_lo = fmax(0.0, ta - q),
				tp_hi = fmin(1.0, tb - q);
	int64_t		jp_lo = (int64_t) floor(tp_lo * tmp_lo - e),
				jp_hi = (int64_t) floor(tp_hi * tmp_hi + e),
				jm_lo = (int64_t) floor((1.0 - tp_hi) * tmp_lo - e),
				jm_hi = (int64_t) floor((1.0 - tp_lo) * tmp_hi + e);

	if (jp_lo < 0)
		jp_lo = 0;
	if (jm_lo < 0)
		jm_lo = 0;
	if (jp_hi > n - 1)
		jp_hi = n - 1;
	if (jm_hi > n - 1)
		jm_hi = n - 1;
	if (jp_lo > jp_hi)
		jp_lo = jp_hi;
	if (jm_lo > jm_hi)
		jm_lo = jm_hi;
	if ((jp_hi - jp_lo + 1) * (jm_hi - jm_lo + 1) > SEED_MAX)
		return 0;
	for (int64_t jp = jp_lo; jp <= jp_hi; jp++)
		for (int64_t jm = jm_lo; jm <= jm_hi; jm++)
		{
			int64_t		pix = north ? sc_xyf2nest(order, n - jm - 1, n - jp - 1, q)
				: sc_xyf2nest(order, jp, jm, q + 8);

			if (!seed_add(s, pix))
				return 0;
		}
	return 1;
}

/* enumerate candidate pixels at `order` for a cone; 0 if more than SEED_MAX */
static int
seed_cone_at(const sc_region *r, int order, seed_t *s)
{
	const double z23 = 2.0 / 3.0;
	const double d23 = asin(2.0 / 3.0);
	double		dec_c = asin(fmax(-1.0, fmin(1.0, r->center.z)));
	double		rad = r->radius * (1 + 1e-12) + 1e-15;
	double		dlo = fmax(-M_PI / 2, dec_c - rad),
				dhi = fmin(M_PI / 2, dec_c + rad);
	double		tints[2][2];
	int			nint = 0;

	s->n = 0;

	/* tt interval(s), normalised into [0, 4) */
	if (fabs(dec_c) + rad >= M_PI / 2 - 1e-12)
	{
		tints[0][0] = 0.0;
		tints[0][1] = 4.0;
		nint = 1;
	}
	else
	{
		double		tc = atan2(r->center.y, r->center.x) * (2.0 / M_PI);
		double		dt = asin(fmin(1.0, sin(rad) / cos(dec_c))) * (2.0 / M_PI) * (1 + 1e-12) + 1e-15;
		double		a,
					b;

		if (tc < 0)
			tc += 4.0;
		a = tc - dt;
		b = tc + dt;
		if (b - a >= 4.0)
		{
			tints[0][0] = 0.0;
			tints[0][1] = 4.0;
			nint = 1;
		}
		else if (a < 0)
		{
			tints[0][0] = 0.0;
			tints[0][1] = b;
			tints[1][0] = a + 4.0;
			tints[1][1] = 4.0;
			nint = 2;
		}
		else if (b >= 4.0)
		{
			tints[0][0] = a;
			tints[0][1] = 4.0;
			tints[1][0] = 0.0;
			tints[1][1] = b - 4.0;
			nint = 2;
		}
		else
		{
			tints[0][0] = a;
			tints[0][1] = b;
			nint = 1;
		}
	}

	for (int t = 0; t < nint; t++)
	{
		double		ta = tints[t][0],
					tb = fmin(tints[t][1], 4.0 - 1e-15);

		/* equatorial zone */
		{
			double		za = fmax(sin(dlo), -z23),
						zb = fmin(sin(dhi), z23);

			if (za <= zb && !seed_equatorial(order, ta, tb, za, zb, s))
				return 0;
		}
		/* polar caps */
		for (int north = 0; north <= 1; north++)
		{
			double		dnear = north ? fmax(dlo, d23) : fmax(-dhi, d23);
			double		dfar = north ? dhi : -dlo;

			if (dfar < d23)
				continue;
			dnear = fmax(d23 - 1e-12, dnear - 1e-12);
			dfar = fmin(M_PI / 2, dfar + 1e-12);
			for (int q = (int) floor(ta); q <= 3 && q <= (int) floor(tb); q++)
				if (!seed_polar(order, north, q, fmax(ta, (double) q), fmin(tb, q + 1.0),
								dnear, dfar, s))
					return 0;
		}
	}
	return 1;
}

static int
seed_cone(const sc_region *r, seed_t *s)
{
	int			order;

	if (r->kind != SC_REGION_CONE || r->radius < 0 || r->radius > 10.0 * M_PI / 180)
		return -1;
	order = (r->radius > 0) ? (int) floor(log2(1.0 / (2.0 * r->radius))) : SC_MAX_ORDER;
	if (order > SC_MAX_ORDER)
		order = SC_MAX_ORDER;
	for (; order >= 0; order--)
		if (seed_cone_at(r, order, s))
			return order;
	return -1;
}

typedef struct
{
	int64_t		pix;
	int			order;
	double		pot;			/* expected false-positive rows (priority) */
} cand_t;

typedef struct
{
	cand_t	   *a;
	int			n,
				cap;
} heap_t;

static void
heap_push(heap_t *h, cand_t c)
{
	int			i;

	if (h->n == h->cap)
	{
		h->cap = h->cap ? 2 * h->cap : 64;
		h->a = h->a ? SC_REALLOC(h->a, sizeof(cand_t) * h->cap)
			: SC_MALLOC(sizeof(cand_t) * h->cap);
	}
	i = h->n++;
	while (i > 0 && h->a[(i - 1) / 2].pot < c.pot)
	{
		h->a[i] = h->a[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	h->a[i] = c;
}

static cand_t
heap_pop(heap_t *h)
{
	cand_t		top = h->a[0],
				last = h->a[--h->n];
	int			i = 0;

	for (;;)
	{
		int			l = 2 * i + 1,
					m;

		if (l >= h->n)
			break;
		m = (l + 1 < h->n && h->a[l + 1].pot > h->a[l].pot) ? l + 1 : l;
		if (h->a[m].pot <= last.pot)
			break;
		h->a[i] = h->a[m];
		i = m;
	}
	if (h->n > 0)
		h->a[i] = last;
	return top;
}

typedef struct
{
	sc_range   *a;
	int			n,
				cap;
} rlist_t;

static void
rlist_add(rlist_t *l, int64_t lo, int64_t hi)
{
	if (l->n == l->cap)
	{
		l->cap = l->cap ? 2 * l->cap : 64;
		l->a = l->a ? SC_REALLOC(l->a, sizeof(sc_range) * l->cap)
			: SC_MALLOC(sizeof(sc_range) * l->cap);
	}
	l->a[l->n].lo = lo;
	l->a[l->n].hi = hi;
	l->n++;
}

static int
range_cmp(const void *a, const void *b)
{
	int64_t		x = ((const sc_range *) a)->lo,
				y = ((const sc_range *) b)->lo;

	return (x > y) - (x < y);
}

static int
double_idx_cmp_ctx_values(const void *a, const void *b, const double *vals)
{
	double		x = vals[*(const int *) a],
				y = vals[*(const int *) b];

	return (x > y) - (x < y);
}

/* qsort has no context argument in C99; keep the gap costs in a static */
static const double *gap_sort_vals;
static int
gap_idx_cmp(const void *a, const void *b)
{
	return double_idx_cmp_ctx_values(a, b, gap_sort_vals);
}

void
sc_cover_params_default(sc_cover_params *p)
{
	p->max_order = SC_MAX_ORDER;
	p->max_ranges = 64;
	p->range_cost = 30.0;
	p->split_cost = 1.0;
	p->probe_split_cost = 1.0;
	p->max_steps = 4000;
	p->max_area_ratio = 64.0;
	p->use_seed = 1;
	p->direct = 1;
	p->force_order = -1;
}

static inline double
order_area(int order)
{
	return 4.0 * M_PI / (double) ((int64_t) 12 << (2 * order));
}

static double
potential(const sc_region *r, double e, double f_out, int order)
{
	double		a_cell = order_area(order);
	double		f_small = 1.0 - fmin(1.0, r->area / a_cell);

	return e * fmax(f_out, f_small);
}

/*
 * The cell order to cover a cone with.
 *
 * Cost(s) = ranges(s) * range_cost + rho * (area covered - area of the cone),
 * with cells of size s covering a disc of radius r:
 *
 *     area covered ~ pi (r + beta s)^2       (cells stick out by ~beta s)
 *     ranges       ~ gamma r / s, at least one
 *
 * For s << r that minimises at s = sqrt(gamma range_cost / (2 pi beta rho)) --
 * independent of the radius: the sky's local density and the price of a range
 * decide how finely it is worth cutting.  For s >~ r the covering is a handful
 * of cells and the balance is different, so rather than solve the branches the
 * cost is evaluated at all 30 orders (a few flops each) and the cheapest taken,
 * which also lets max_ranges and the area guard simply drop candidates.
 *
 * rho comes from the density map around the cone's own centre, so a crowded
 * field is cut finer than empty sky with no refinement loop at all.
 */
#define SC_ORDER0_SIZE 1.0233267079464885		/* sqrt(4 pi / 12), radians */
#define SC_COVER_BETA 0.7
#define SC_COVER_GAMMA 2.0

/* orders past the closed-form choice to score on the covering they produce */
int			sc_order_probe = 0;

/* rows a covering must hold, per cell examined, before probing one order finer */
#define SC_PROBE_MIN 8.0

static int
choose_order(const sc_region *r, const sc_density *d, const sc_cover_params *p,
			 double *rho_out)
{
	int			m,
				best = p->max_order;
	int64_t		pix;
	double		rows,
				rho,
				s,
				s_max = INFINITY,
				best_cost = INFINITY;

	/* local density: rows per steradian in a cell a few times the cone */
	s = fmax(4.0 * fmax(r->radius, 1e-12), 1e-6);
	m = (int) floor(log2(SC_ORDER0_SIZE / s));
	m = Max(0, Min(m, SC_MAX_ORDER));
	pix = r->center_pix >> (2 * (SC_MAX_ORDER - m));
	rows = sc_density_rows(d, sc_pix_lo(m, pix), sc_pix_hi(m, pix));
	rho = rows / order_area(m);
	if (!(rho > 0))
		rho = (d && d->ntotal > 0 ? d->ntotal : 1e6) / (4.0 * M_PI);
	if (rho_out)
		*rho_out = rho;

	if (p->force_order >= 0)
		return Min(p->force_order, p->max_order);

	if (p->max_area_ratio > 0)
		s_max = sqrt(p->max_area_ratio * fmax(r->area, order_area(SC_MAX_ORDER)));

	for (int k = 0; k <= p->max_order; k++)
	{
		double		sk = SC_ORDER0_SIZE / (double) ((int64_t) 1 << k);
		double		reach = r->radius + SC_COVER_BETA * sk;
		double		covered = M_PI * reach * reach;
		double		ncells = fmax(1.0, covered / (sk * sk));
		double		nranges = fmax(1.0, fmin(ncells, SC_COVER_GAMMA * r->radius / sk + 1.0));
		double		cost;

		if (sk > s_max)
			continue;
		if (nranges > (double) p->max_ranges)
			continue;
		cost = nranges * p->range_cost + rho * fmax(0.0, covered - r->area);
		if (cost < best_cost)
		{
			best_cost = cost;
			best = k;
		}
	}
	return Max(0, Min(best, p->max_order));
}

/*
 * A covering at the order choose_order() asks for.
 *
 * The cells are enumerated exactly, by pushing the cone's (tt, z) interval
 * through the HEALPix formulas.  That box is loose at high declination, where
 * a small cone spans a wide range of longitude, so the enumeration may only
 * fit at a coarser order than wanted; from there the cells are split down to
 * the target, which costs one cheap cap test per child and keeps the work
 * proportional to the cone's boundary.  Only the last level is classified
 * with the exact cell geometry, which is where dropping a cell actually
 * removes rows.
 */
static int
cover_cone_direct(const sc_region *r, const sc_density *d,
				  const sc_cover_params *p, rlist_t *kept, int *steps,
				  double *rho_out)
{
	seed_t		sd;
	int			target = choose_order(r, d, p, rho_out);
	int			budget = 4 * Max(p->max_ranges, 4) + 16;
	int			k,
				ncur = 0,
				cap = 256;
	int64_t    *cur,
			   *next;

	for (k = target; k >= 0; k--)
		if (seed_cone_at(r, k, &sd))
			break;
	if (k < 0)
		return -1;				/* too many cells: fall back to the descent */

	cap = Max(budget, sd.n) * 4 + 16;
	cur = SC_MALLOC(sizeof(int64_t) * cap);
	next = SC_MALLOC(sizeof(int64_t) * cap);
	for (int i = 0; i < sd.n; i++)
	{
		double		fo;

		(*steps)++;
		if (sc_region_classify_cap(r, k, sd.pix[i], &fo) != SC_OUT)
			cur[ncur++] = sd.pix[i];
	}

	while (k < target && ncur > 0)
	{
		int			nnext = 0;
		int			ok = 1;

		for (int i = 0; i < ncur && ok; i++)
			for (int c = 0; c < 4; c++)
			{
				int64_t		child = 4 * cur[i] + c;
				double		fo;

				(*steps)++;
				if (sc_region_classify_cap(r, k + 1, child, &fo) == SC_OUT)
					continue;
				if (nnext >= cap || nnext >= budget)
				{
					ok = 0;		/* this level is as fine as the budget allows */
					break;
				}
				next[nnext++] = child;
			}
		if (!ok)
			break;
		memcpy(cur, next, sizeof(int64_t) * nnext);
		ncur = nnext;
		k++;
	}

	for (int i = 0; i < ncur; i++)
	{
		(*steps)++;
		if (cell_is_out(r, k, cur[i]))
			continue;
		rlist_add(kept, sc_pix_lo(k, cur[i]), sc_pix_hi(k, cur[i]));
	}
	SC_FREE(cur);
	SC_FREE(next);
	return k;
}


/*
 * Merge gaps that are cheaper to scan than to skip, then the cheapest
 * remaining gaps until the range count fits max_ranges.  Applied to every
 * candidate covering before it is scored, not only to the winner: a fine
 * covering has many separate runs before its gaps are merged, and scoring it
 * on that count charges for ranges the final plan would never contain --
 * which is what kept the probe from ever choosing one.
 */
static void
merge_gaps(rlist_t *l, const sc_density *d, const sc_cover_params *p, double area_cap)
{
	if (l->n > 1)
	{
		int			ng = l->n - 1;
		double	   *gc = SC_MALLOC(sizeof(double) * ng);
		char	   *merge = SC_MALLOC(ng);
		int		   *idx = SC_MALLOC(sizeof(int) * ng);
		int			remaining = l->n,
					nidx = 0,
					m = 0;

		for (int i = 0; i < ng; i++)
		{
			int64_t		glo = l->a[i].hi + 1,
						ghi = l->a[i + 1].lo - 1;

			gc[i] = sc_density_rows(d, glo, ghi);
			merge[i] = gc[i] <= p->range_cost &&
				((double) (ghi - glo) + 1.0) * SC_PIX29_AREA <= area_cap;
			if (merge[i])
				remaining--;
			else
				idx[nidx++] = i;
		}
		if (remaining > p->max_ranges)
		{
			gap_sort_vals = gc;
			qsort(idx, nidx, sizeof(int), gap_idx_cmp);
			for (int j = 0; j < nidx && remaining > p->max_ranges; j++)
			{
				merge[idx[j]] = 1;
				remaining--;
			}
		}
		for (int i = 0; i < ng; i++)
		{
			if (merge[i])
				l->a[m].hi = l->a[i + 1].hi;
			else
				l->a[++m] = l->a[i + 1];
		}
		l->n = m + 1;
		SC_FREE(gc);
		SC_FREE(merge);
		SC_FREE(idx);
	}
}

/*
 * Sort, merge adjacent, and score a candidate covering.
 *
 * score = ranges * range_cost + expected rows inside the covering
 *       + cells examined * step_cost
 *
 * all three in rows.  The region's own rows are the same for every candidate,
 * so they cancel and are not subtracted.  step_cost is passed in rather than
 * read from p->split_cost because the only caller is the order-probe loop,
 * which charges its own probe_split_cost for cells examined -- a different
 * cost than the descent's split/keep decision charges for the same unit.
 */
static double
rlist_score(rlist_t *l, const sc_density *d, const sc_cover_params *p, int steps,
			double step_cost, double region_area, double *rows_out)
{
	double		rows = 0;
	int			m = 0;

	if (l->n <= 0)
		return INFINITY;
	qsort(l->a, l->n, sizeof(sc_range), range_cmp);
	for (int i = 1; i < l->n; i++)
	{
		if (l->a[i].lo <= l->a[m].hi + 1)
		{
			if (l->a[i].hi > l->a[m].hi)
				l->a[m].hi = l->a[i].hi;
		}
		else
			l->a[++m] = l->a[i];
	}
	l->n = m + 1;
	merge_gaps(l, d, p, (p->max_area_ratio > 0)
			   ? p->max_area_ratio * fmax(region_area, order_area(SC_MAX_ORDER)) : INFINITY);
	for (int i = 0; i < l->n; i++)
		rows += sc_density_rows(d, l->a[i].lo, l->a[i].hi);
	if (rows_out)
		*rows_out = rows;
	return l->n * p->range_cost + rows + steps * step_cost;
}

void
sc_cover_compute(const sc_region *r, const sc_density *d,
				 const sc_cover_params *p, sc_cover *out)
{
	heap_t		h = {0};
	rlist_t		kept = {0};
	int			nr = 0;			/* approximate number of ranges */
	int			steps = 0;
	int			deepest = 0;
	int			order = -1;
	double		rho = 0;
	double		area_cap = (p->max_area_ratio > 0)
		? p->max_area_ratio * fmax(r->area, order_area(SC_MAX_ORDER)) : INFINITY;

	memset(out, 0, sizeof(*out));

	if (p->direct && r->kind == SC_REGION_CONE && r->radius >= 0 &&
		r->radius <= 10.0 * M_PI / 180)
	{
		/*
		 * The closed form of choose_order() estimates the number of index
		 * ranges as gamma * r / s -- ranges multiplying as the cells shrink.
		 * They do not: once adjacent cells are merged into ranges the count is
		 * nearly independent of the cell size (measured: 2.5 ranges for a 6'
		 * cone whether the cells are 0.4 degrees or 0.05, where the formula
		 * predicts twenty).  Charging for ranges that never appear stops the
		 * covering several orders too coarse and carries the false positives
		 * that go with it.
		 *
		 * So the closed form is used as a starting point and the orders from
		 * there are scored on the covering they actually produce, stopping at
		 * the first one that is worse.  Each probe costs an enumeration, which
		 * is why the depth is small and why rlist_score() charges for the
		 * cells examined as well as the ranges and the rows.
		 *
		 * Forcing the order directly one to three steps finer than the closed
		 * form is worth 30-48% at 6'-30' on the catalogue and 32% on an
		 * ObsCore-shaped relation *in execution time alone* -- real and
		 * measured, with the range count unchanged.  That is not the whole
		 * bill, though: each extra probe is itself an enumeration, paid at
		 * plan time, and it does not amortise unless the same (ra, dec,
		 * radius) recurs often enough to hit sc_cover_compute()'s caller's
		 * memoised cache.  Measured in total (plan + execution) time against
		 * 1,456 distinct query centres -- i.e. the cache providing no
		 * reuse, the realistic case for a service answering different
		 * users' cones -- probe_orders = 3 is a net loss at several radii
		 * (6', 30', 1 degree) relative to not probing at all, and even
		 * probe_orders = 1 does not uniformly win once probe_split_cost
		 * varies.  See GIST_REGION_DESIGN.md's probe-loop decoupling round.
		 * probe_split_cost exists so that whoever tunes this for a workload
		 * where it does amortise (a repeated query shape, or an offline
		 * covering build) calibrates the probe loop's own enumeration cost
		 * without also detuning the descent's unrelated split/keep decision
		 * or its SC_PROBE_MIN guard below, which used to share split_cost
		 * with it -- that coupling bug is fixed here regardless of whether
		 * probing itself is worth enabling for any given workload.
		 */
		sc_cover_params pk = *p;
		rlist_t		best = {0};
		double		best_cost = INFINITY;
		double		cand_rows = 0;
		int			best_k = -1,
					k0;

		k0 = (p->force_order >= 0) ? p->force_order : choose_order(r, d, p, &rho);

		for (int k = k0; k <= Min(k0 + sc_order_probe, p->max_order); k++)
		{
			rlist_t		cand = {0};
			int			st = 0;
			double		cost;

			pk.force_order = k;
			if (cover_cone_direct(r, d, &pk, &cand, &st, NULL) < 0)
			{
				if (cand.a)
					SC_FREE(cand.a);
				break;
			}
			steps += st;
			cost = rlist_score(&cand, d, p, st, p->probe_split_cost, r->area, &cand_rows);
			if (cost < best_cost)
			{
				if (best.a)
					SC_FREE(best.a);
				best = cand;
				best_cost = cost;
				best_k = k;
			}
			else
			{
				if (cand.a)
					SC_FREE(cand.a);
				break;			/* refining has stopped paying */
			}
			if (p->force_order >= 0)
				break;			/* the caller asked for exactly this order */

			/*
			 * Each further probe costs another enumeration -- about four times
			 * the cells of the one just done -- so it is only worth making
			 * when the rows in this covering dwarf that.  A one-arcsecond cone
			 * covers many times its own area but still holds a handful of
			 * rows: there is nothing there to win back, and the probe would be
			 * pure plan-time loss.
			 */
			if (cand_rows < SC_PROBE_MIN * (st + 1) * fmax(p->probe_split_cost, 1e-3))
				break;
		}

		if (best_k >= 0)
		{
			if (kept.a)
				SC_FREE(kept.a);
			kept = best;
			deepest = best_k;
			order = best_k;
			goto finish;
		}
		kept.n = 0;				/* fell back: start the descent clean */
	}

	{
		seed_t		sd;
		int			so = (p->use_seed) ? seed_cone(r, &sd) : -1;

		if (so >= 0)
		{
			for (int i = 0; i < sd.n; i++)
			{
				double		fo;
				sc_class	cls = sc_region_classify(r, so, sd.pix[i], &fo);

				steps++;
				if (cls == SC_OUT)
					continue;
				nr++;
				if (so > deepest)
					deepest = so;
				if (cls == SC_IN)
					rlist_add(&kept, sc_pix_lo(so, sd.pix[i]), sc_pix_hi(so, sd.pix[i]));
				else
				{
					cand_t		c = {sd.pix[i], so, 0};

					c.pot = potential(r, sc_density_rows(d, sc_pix_lo(so, sd.pix[i]), sc_pix_hi(so, sd.pix[i])), fo, so);
					heap_push(&h, c);
				}
			}
			goto refine;
		}
	}

	for (int f = 0; f < 12; f++)
	{
		double		fo;
		sc_class	cls = sc_region_classify(r, 0, f, &fo);

		steps++;
		if (cls == SC_OUT)
			continue;
		nr++;
		if (cls == SC_IN)
			rlist_add(&kept, sc_pix_lo(0, f), sc_pix_hi(0, f));
		else
		{
			cand_t		c = {f, 0, 0};

			c.pot = potential(r, sc_density_rows(d, sc_pix_lo(0, f), sc_pix_hi(0, f)), fo, 0);
			heap_push(&h, c);
		}
	}

refine:
	while (h.n > 0)
	{
		cand_t		P = heap_pop(&h);
		sc_class	cls[4];
		double		fo[4],
					e[4];
		int			kept_mask = 0,
					nkept = 0,
					runs = 0,
					delta,
					accept = 0;
		double		removed = 0,
					gain;
		int			co = P.order + 1;

		if (P.order >= p->max_order || steps + 4 > p->max_steps)
		{
			rlist_add(&kept, sc_pix_lo(P.order, P.pix), sc_pix_hi(P.order, P.pix));
			if (P.order > deepest)
				deepest = P.order;
			continue;
		}

		for (int c = 0; c < 4; c++)
		{
			int64_t		cp = 4 * P.pix + c;

			cls[c] = sc_region_classify(r, co, cp, &fo[c]);
			steps++;
			e[c] = (cls[c] == SC_IN) ? 0.0
				: sc_density_rows(d, sc_pix_lo(co, cp), sc_pix_hi(co, cp));
			if (cls[c] == SC_OUT)
				removed += e[c];
			else
			{
				kept_mask |= 1 << c;
				nkept++;
				if (c == 0 || !(kept_mask & (1 << (c - 1))))
					runs++;
			}
		}

		if (nkept == 0)
		{
			nr--;				/* conservative parent, no child intersects */
			continue;
		}

		delta = runs - 1;
		gain = removed - p->range_cost * delta;
		if (delta > 0 && nr + delta > p->max_ranges)
			accept = 0;
		else if (gain > p->split_cost)
			accept = 1;
		else if (delta <= 0 && P.pot > p->range_cost)
			accept = 1;			/* no extra ranges now, worth looking deeper */
		if (!accept && order_area(P.order) > area_cap)
			accept = 1;			/* geometric guard */

		TRACE("o%-2d pix %-12lld pot %10.3f cls %d%d%d%d removed %9.3f delta %d gain %9.3f -> %s\n",
			  P.order, (long long) P.pix, P.pot, cls[0], cls[1], cls[2], cls[3],
			  removed, delta, gain, accept ? "split" : "keep");
		if (!accept)
		{
			rlist_add(&kept, sc_pix_lo(P.order, P.pix), sc_pix_hi(P.order, P.pix));
			if (P.order > deepest)
				deepest = P.order;
			continue;
		}

		nr += delta;
		for (int c = 0; c < 4; c++)
		{
			int64_t		cp = 4 * P.pix + c;

			if (cls[c] == SC_IN)
			{
				rlist_add(&kept, sc_pix_lo(co, cp), sc_pix_hi(co, cp));
				if (co > deepest)
					deepest = co;
			}
			else if (cls[c] == SC_PARTIAL)
			{
				cand_t		cc = {cp, co, 0};

				cc.pot = potential(r, e[c], fo[c], co);
				heap_push(&h, cc);
			}
		}
	}
	if (h.a)
		SC_FREE(h.a);

finish:
	out->steps = steps;
	out->deepest = deepest;
	out->rho = rho;
	out->order = order;
	if (kept.n == 0)
	{
		if (kept.a)
			SC_FREE(kept.a);
		return;
	}

	/* sort, merge adjacent */
	qsort(kept.a, kept.n, sizeof(sc_range), range_cmp);
	{
		int			m = 0;

		for (int i = 1; i < kept.n; i++)
		{
			if (kept.a[i].lo <= kept.a[m].hi + 1)
			{
				if (kept.a[i].hi > kept.a[m].hi)
					kept.a[m].hi = kept.a[i].hi;
			}
			else
				kept.a[++m] = kept.a[i];
		}
		kept.n = m + 1;
	}

	merge_gaps(&kept, d, p, area_cap);

	out->n = kept.n;
	out->r = kept.a;
	for (int i = 0; i < kept.n; i++)
	{
		out->exp_rows += sc_density_rows(d, kept.a[i].lo, kept.a[i].hi);
		out->area += ((double) (kept.a[i].hi - kept.a[i].lo) + 1.0) * SC_PIX29_AREA;
	}
}

void
sc_cover_free(sc_cover *c)
{
	if (c->r)
		SC_FREE(c->r);
	c->r = NULL;
	c->n = 0;
}
