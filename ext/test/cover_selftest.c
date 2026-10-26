/*
 * Brute-force validation of sc_cover_compute():
 *   - no false negatives: every catalogue point inside the region has its
 *     order-29 cell inside one of the ranges (cones, convex and non-convex polygons,
 *     including poles, RA wrap-around and face boundaries);
 *   - reports ranges, steps, false-positive fraction and timing, for a
 *     uniform density model and a histogram density model.
 * Build: cc -O2 -o cover_selftest cover_selftest.c ../src/cover.c ../src/healpix.c -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/cover.h"

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static double urand(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (rs >> 11) * (1.0 / 9007199254740992.0); }
static double gauss(void) { return sqrt(-2 * log(urand() + 1e-300)) * cos(2 * M_PI * urand()); }

#define NPTS 2000000
static double ra[NPTS], de[NPTS];
static int64_t cell[NPTS], sorted[NPTS];
static int cmp64(const void *a, const void *b) { int64_t x = *(const int64_t *) a, y = *(const int64_t *) b; return (x > y) - (x < y); }

static int
in_ranges(const sc_cover *c, int64_t x)
{
	int lo = 0, hi = c->n - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		if (x < c->r[mid].lo) hi = mid - 1;
		else if (x > c->r[mid].hi) lo = mid + 1;
		else return 1;
	}
	return 0;
}

/* clustered catalogue: 40% uniform, 60% in 30 Gaussian blobs incl. poles and RA=0 */
static void
make_points(void)
{
	double cra[30], cde[30], sig[30];
	for (int k = 0; k < 30; k++)
	{
		cra[k] = 360 * urand(); cde[k] = asin(2 * urand() - 1) * 180 / M_PI; sig[k] = 0.05 + 2 * urand();
	}
	cde[0] = 89.9; cde[1] = -89.95; cra[2] = 0.01; cde[2] = 10; cde[3] = 41.81; cra[3] = 45;
	for (int i = 0; i < NPTS; i++)
	{
		if (urand() < 0.4) { ra[i] = 360 * urand(); de[i] = asin(2 * urand() - 1) * 180 / M_PI; }
		else
		{
			int k = (int) (30 * urand());
			double d = cde[k] + sig[k] * gauss();
			double a = cra[k] + sig[k] * gauss() / fmax(cos(cde[k] * M_PI / 180), 0.01);
			if (d > 90) { d = 180 - d; a += 180; }
			if (d < -90) { d = -180 - d; a += 180; }
			a = fmod(a, 360); if (a < 0) a += 360;
			ra[i] = a; de[i] = d;
		}
		cell[i] = sc_ang2pix(SC_MAX_ORDER, ra[i], de[i]);
		sorted[i] = cell[i];
	}
	qsort(sorted, NPTS, sizeof(int64_t), cmp64);
}


/*
 * Non-convex polygon reference, independent of cover.c: shapes are drawn in
 * the gnomonic plane tangent at their own centre c (not the vertex mean the
 * library projects from), and membership is the crossing-number rule there.
 */
typedef struct { double x, y; } xy;
typedef struct { sc_vec3 c, e1, e2; } frame3;

static frame3
frame_at(double ra_deg, double dec_deg)
{
	frame3 f;
	double nn;
	f.c = sc_radec2vec(ra_deg, dec_deg);
	f.e1 = (fabs(f.c.z) < 0.9) ? (sc_vec3) {-f.c.y, f.c.x, 0} : (sc_vec3) {0, -f.c.z, f.c.y};
	nn = sqrt(sc_dot(f.e1, f.e1));
	f.e1.x /= nn; f.e1.y /= nn; f.e1.z /= nn;
	f.e2 = sc_cross(f.c, f.e1);
	return f;
}

static sc_vec3
from_plane(const frame3 *f, xy p)
{
	sc_vec3 v = {f->c.x + p.x * f->e1.x + p.y * f->e2.x, f->c.y + p.x * f->e1.y + p.y * f->e2.y,
				 f->c.z + p.x * f->e1.z + p.y * f->e2.z};
	double nn = sqrt(sc_dot(v, v));
	v.x /= nn; v.y /= nn; v.z /= nn;
	return v;
}

static xy
to_plane(const frame3 *f, sc_vec3 v)
{
	double w = sc_dot(v, f->c);
	return (xy) {sc_dot(v, f->e1) / w, sc_dot(v, f->e2) / w};
}

/* crossing number: 1 inside, 0 outside; *dist gets the distance to the ring */
static int
ref_inside(const xy *P, int n, xy q, double *dist)
{
	int in = 0;
	double d = INFINITY;
	for (int i = 0, j = n - 1; i < n; j = i++)
	{
		double ux = P[i].x - P[j].x, uy = P[i].y - P[j].y;
		double t = ((q.x - P[j].x) * ux + (q.y - P[j].y) * uy) / (ux * ux + uy * uy);
		t = t < 0 ? 0 : (t > 1 ? 1 : t);
		d = fmin(d, hypot(q.x - P[j].x - t * ux, q.y - P[j].y - t * uy));
		if ((P[i].y > q.y) != (P[j].y > q.y) &&
			q.x < P[j].x + (q.y - P[j].y) * ux / uy)
			in = !in;
	}
	*dist = d;
	return in;
}

/*
 * Centre of mass, by brute force: the integral of (X e1 + Y e2 + c) /
 * (1 + X^2 + Y^2)^2 over the polygon in the test's plane at c, on the fan
 * from the origin, every triangle cut into m x m and each piece sampled at
 * its centroid.  Slow and plain, but nothing like the library's code.
 */
static sc_vec3
ref_centroid(const frame3 *f, const xy *P, int n, int m)
{
	double i0 = 0, ix = 0, iy = 0;
	for (int i = 0; i < n; i++)
	{
		xy a = P[i], b = P[(i + 1) % n];
		double area = 0.5 * (a.x * b.y - b.x * a.y) / ((double) m * m);
		for (int u = 0; u < m; u++)
			for (int v = 0; u + v < m; v++)
				for (int up = 0; up < 2; up++)
				{
					/* barycentric centroid of the sub-triangle (u, v) pointing up or down */
					double s1, s2, x, y, w;
					if (up) { if (u + v + 1 >= m) continue; s1 = (u + 2.0 / 3) / m; s2 = (v + 2.0 / 3) / m; }
					else { s1 = (u + 1.0 / 3) / m; s2 = (v + 1.0 / 3) / m; }
					x = s1 * a.x + s2 * b.x; y = s1 * a.y + s2 * b.y;
					w = 1 + x * x + y * y;
					i0 += area / (w * w); ix += area * x / (w * w); iy += area * y / (w * w);
				}
	}
	return from_plane(f, (xy) {ix / i0, iy / i0});
}

/* shape k in unit plane coordinates: L, U, comb, or a random star */
static int
make_shape(int k, xy *P)
{
	static const xy L[] = {{-1, -1}, {1, -1}, {1, -0.4}, {-0.4, -0.4}, {-0.4, 1}, {-1, 1}};
	static const xy U[] = {{-1, -1}, {1, -1}, {1, 1}, {0.4, 1}, {0.4, -0.4}, {-0.4, -0.4}, {-0.4, 1}, {-1, 1}};
	int n = 0;
	switch (k % 4)
	{
		case 0: for (int i = 0; i < 6; i++) P[n++] = L[i]; break;
		case 1: for (int i = 0; i < 8; i++) P[n++] = U[i]; break;
		case 2:					/* comb: five teeth pointing up */
			P[n++] = (xy) {-1, -1}; P[n++] = (xy) {1, -1};
			for (int t = 4; t >= 0; t--)
			{
				double x0 = -1 + 0.4 * t;
				P[n++] = (xy) {x0 + 0.4, 1}; P[n++] = (xy) {x0 + 0.2, 1};
				if (t > 0) { P[n++] = (xy) {x0 + 0.2, -0.5}; P[n++] = (xy) {x0, -0.5}; }
			}
			P[n++] = (xy) {-1, 1};
			break;
		default:				/* star-shaped about the origin: always simple */
			n = 5 + (int) (20 * urand());
			for (int i = 0; i < n; i++)
			{
				double a = 2 * M_PI * (i + 0.8 * urand()) / n, r = 0.2 + 0.8 * urand();
				P[i] = (xy) {r * cos(a), r * sin(a)};
			}
	}
	return n;
}

int
main(void)
{
	int64_t bounds[1001];
	sc_density dens_hist = {NPTS, 1001, bounds}, dens_uni = {NPTS, 0, NULL};
	double radii[] = {1.0 / 3600, 10.0 / 3600, 1.0 / 60, 0.1, 0.5, 1, 3, 10, 45, 120};
	int failures = 0;

	make_points();
	for (int i = 0; i <= 1000; i++)
		bounds[i] = sorted[(int64_t) i * (NPTS - 1) / 1000];

	printf("%-8s %-5s %7s %7s %8s %6s %9s %9s %9s %8s\n", "radius", "model", "queries", "ranges", "steps", "order", "true", "covered", "fp_frac", "us/cov");
	for (int ri = 0; ri < 10; ri++)
		for (int model = 0; model < 2; model++)
		{
			int nq = ri < 6 ? 60 : 12;
			double tot_ranges = 0, tot_steps = 0, tot_true = 0, tot_cov = 0, tot_us = 0, tot_order = 0;
			sc_cover_params p;
			sc_cover_params_default(&p);

			rs = 12345 + ri;		/* same query centres for both models */
			for (int q = 0; q < nq; q++)
			{
				sc_region r;
				sc_cover c;
				double qra, qde;
				int ntrue = 0, ncov = 0;
				clock_t t0;

				if (q % 3 == 0) { int k = (int) (urand() * NPTS); qra = ra[k]; qde = de[k]; }
				else if (q % 3 == 1) { qra = 360 * urand(); qde = asin(2 * urand() - 1) * 180 / M_PI; }
				else { qra = (urand() < 0.5) ? 0.0 : 45.0 * (int) (8 * urand()); qde = (urand() < 0.5) ? 90 - 1e-4 * urand() : 41.8103148958; }

				sc_region_cone(&r, qra, qde, radii[ri], true);
				t0 = clock();
				for (int rep = 0; rep < 5; rep++)
				{
					if (rep) sc_cover_free(&c);
					sc_cover_compute(&r, model ? &dens_hist : &dens_uni, &p, &c);
				}
				tot_us += (double) (clock() - t0) / CLOCKS_PER_SEC * 1e6 / 5;

				for (int i = 0; i < NPTS; i++)
				{
					int inr = in_ranges(&c, cell[i]);
					if (inr) ncov++;
					if (sc_region_contains(&r, sc_radec2vec(ra[i], de[i])))
					{
						ntrue++;
						if (!inr) { failures++; if (failures < 10) printf("FALSE NEGATIVE r=%g at (%g,%g) point (%g,%g)\n", radii[ri], qra, qde, ra[i], de[i]); }
					}
				}
				if (getenv("SC_VERBOSE") && ncov - ntrue > 100)
					printf("   outlier r=%g model=%d q=%d at (%.6f,%.6f): true %d covered %d ranges %d steps %d deepest %d exp_rows %.1f\n",
						   radii[ri], model, q, qra, qde, ntrue, ncov, c.n, c.steps, c.deepest, c.exp_rows);
				tot_ranges += c.n; tot_steps += c.steps; tot_true += ntrue; tot_cov += ncov; tot_order += c.deepest;
				sc_cover_free(&c);
			}
			printf("%-8.4g %-5s %7d %7.1f %8.0f %6.1f %9.0f %9.0f %9.3f %8.1f\n", radii[ri], model ? "hist" : "unif", nq,
				   tot_ranges / nq, tot_steps / nq, tot_order / nq, tot_true / nq, tot_cov / nq,
				   tot_cov > 0 ? 1 - tot_true / tot_cov : 0, tot_us / nq);
		}

	/* convex polygons: random quadrilaterals / pentagons around random centres */
	{
		int nq = 150, fp_q = 0;
		double tot_true = 0, tot_cov = 0, tot_ranges = 0;
		sc_cover_params p;
		sc_cover_params_default(&p);
		for (int q = 0; q < nq; q++)
		{
			double cra = 360 * urand(), cde = asin(2 * urand() - 1) * 180 / M_PI, size = pow(10, -3 + 4 * urand());
			double pra[6], pde[6];
			int nv = 3 + q % 4;
			sc_region r;
			sc_cover c;
			const char *err;
			int ntrue = 0, ncov = 0;
			if (q % 10 == 0) cde = 89.99;
			for (int v = 0; v < nv; v++)
			{
				double ang = 2 * M_PI * v / nv + 0.2 * urand();
				pde[v] = cde + size * sin(ang);
				pra[v] = cra + size * cos(ang) / fmax(cos(cde * M_PI / 180), 1e-3);
				if (pde[v] > 90) pde[v] = 90;
			}
			if ((err = sc_region_poly(&r, nv, pra, pde, true)) != NULL) { fp_q++; sc_region_free(&r); continue; }
			sc_cover_compute(&r, &dens_hist, &p, &c);
			for (int i = 0; i < NPTS; i++)
			{
				int inr = in_ranges(&c, cell[i]);
				if (inr) ncov++;
				if (sc_region_contains(&r, sc_radec2vec(ra[i], de[i])))
				{
					ntrue++;
					if (!inr) { failures++; if (failures < 10) printf("POLY FALSE NEGATIVE size=%g\n", size); }
				}
			}
			tot_true += ntrue; tot_cov += ncov; tot_ranges += c.n;
			sc_cover_free(&c); sc_region_free(&r);
		}
		printf("polygons: %d built (%d rejected), ranges %.1f, true %.0f covered %.0f (fp %.3f)\n",
			   nq - fp_q, fp_q, tot_ranges / (nq - fp_q), tot_true, tot_cov, tot_cov > 0 ? 1 - tot_true / tot_cov : 0);
	}

	/*
	 * Polygon sampling, the same idea as the disc sampling below but for the
	 * convex-polygon path (which is also what box() builds): polygons placed
	 * at the awkward places, with points drawn inside them -- most of them a
	 * hair inside an edge or a vertex, where a covering that is even slightly
	 * too small loses rows.  Every such point's cell must be in the covering.
	 */
	{
		int			npoly = 8000,
					bad = 0,
					built = 0;
		long		pts = 0;
		sc_cover_params p;

		sc_cover_params_default(&p);

		for (int q = 0; q < npoly; q++)
		{
			double		cra,
						cde,
						size = pow(10, -4 + 4.3 * urand());
			double		pra[8],
						pde[8];
			int			nv = 3 + q % 6;
			sc_region	r;
			sc_cover	c;
			sc_vec3		cen = {0, 0, 0};

			/* the places that break naive geometry */
			switch (q % 6)
			{
				case 0: cra = 360 * urand(); cde = 90 - size * urand(); break;      /* pole */
				case 1: cra = 360 * urand(); cde = -90 + size * urand(); break;
				case 2: cra = 360 * urand(); cde = 41.8103148958; break;            /* zone boundary */
				case 3: cra = size * (urand() - 0.5); cde = 90 * (2 * urand() - 1) * 0.98; break;  /* RA wrap */
				case 4: cra = 90.0 * (q % 4); cde = asin(2 * urand() - 1) * 180 / M_PI; break;     /* face edge */
				default: cra = 360 * urand(); cde = asin(2 * urand() - 1) * 180 / M_PI; break;
			}
			for (int v = 0; v < nv; v++)
			{
				double		ang = 2 * M_PI * v / nv + 0.3 * urand();

				pde[v] = cde + size * sin(ang);
				pra[v] = cra + size * cos(ang) / fmax(cos(cde * M_PI / 180), 1e-4);
				if (pde[v] > 90) pde[v] = 90;
				if (pde[v] < -90) pde[v] = -90;
			}
			if (sc_region_poly(&r, nv, pra, pde, true) != NULL)
				continue;				/* degenerate: not our case */
			built++;
			sc_cover_compute(&r, &dens_hist, &p, &c);

			/* the centroid of a convex spherical polygon is inside it */
			for (int v = 0; v < r.nv; v++)
			{
				cen.x += r.v[v].x; cen.y += r.v[v].y; cen.z += r.v[v].z;
			}
			{
				double	nn = sqrt(cen.x * cen.x + cen.y * cen.y + cen.z * cen.z);

				if (nn < 1e-12)
				{
					sc_cover_free(&c); sc_region_free(&r); continue;
				}
				cen.x /= nn; cen.y /= nn; cen.z /= nn;
			}

			for (int k = 0; k < 120; k++)
			{
				int			e = (int) (urand() * r.nv) % r.nv;
				double		t = urand(),
							u,
							nn;
				sc_vec3		a = r.v[e],
							b = r.v[(e + 1) % r.nv],
							edge,
							pt;

				/* a point on the edge arc, then pulled towards the interior */
				edge.x = a.x * (1 - t) + b.x * t;
				edge.y = a.y * (1 - t) + b.y * t;
				edge.z = a.z * (1 - t) + b.z * t;
				nn = sqrt(edge.x * edge.x + edge.y * edge.y + edge.z * edge.z);
				if (nn < 1e-12)
					continue;
				edge.x /= nn; edge.y /= nn; edge.z /= nn;

				/* half the points within a part per billion of the boundary */
				u = (k % 2) ? 1.0 - pow(10, -9 * urand()) : urand();
				pt.x = cen.x + u * (edge.x - cen.x);
				pt.y = cen.y + u * (edge.y - cen.y);
				pt.z = cen.z + u * (edge.z - cen.z);
				nn = sqrt(pt.x * pt.x + pt.y * pt.y + pt.z * pt.z);
				if (nn < 1e-12)
					continue;
				pt.x /= nn; pt.y /= nn; pt.z /= nn;

				if (!sc_region_contains(&r, pt))
					continue;			/* fell outside on rounding: not a miss */
				pts++;
				if (!in_ranges(&c, sc_vec2pix(SC_MAX_ORDER, pt)))
				{
					bad++;
					if (bad < 10)
						printf("POLY FALSE NEGATIVE: case %d size=%g nv=%d\n", q % 6, size, nv);
				}
			}
			sc_cover_free(&c);
			sc_region_free(&r);
		}
		printf("polygon sampling: %d polygons at poles/zone/wrap/face edges, %ld points inside, %d misses\n",
			   built, pts, bad);
		failures += bad;
	}


	/*
	 * Non-convex polygons: L, U, comb and random star shapes, from a few
	 * milliarcseconds to tens of degrees, at the awkward places.  Against the
	 * independent reference above: point membership (uniform and within a
	 * hair of the edges), area, every inside point covered, and CONTAINS /
	 * INTERSECTS for small triangles dropped around and across the notches.
	 * Points nearer the boundary than the library's 1e-12 slack, and region
	 * pairs the sampled reference cannot decide, are skipped, not scored.
	 */
	{
		int npoly = 1200, bad_in = 0, bad_cov = 0, bad_area = 0, bad_cr = 0, bad_ov = 0, bad_cen = 0, built = 0;
		double worst_cen = 0;
		long pts = 0, ins = 0, pairs = 0;
		double tot_parts = 0, tot_nv = 0, tot_true = 0, tot_cov = 0;
		sc_cover_params p;

		sc_cover_params_default(&p);
		rs = 4242;
		for (int q = 0; q < npoly; q++)
		{
			double cra, cde, size = pow(10, -6 + 6 * urand());	/* tangent-plane units ~ radians */
			xy P[40];
			double pra[40], pde[40], ref_area = 0;
			int n, ntrue = 0, ncov = 0;
			sc_region r;
			sc_cover c;
			frame3 f;
			const char *err;

			switch (q % 5)
			{
				case 0: cra = 360 * urand(); cde = 90 - 30 * size * urand(); break;	/* pole */
				case 1: cra = 360 * urand(); cde = 41.8103148958; break;			/* zone boundary */
				case 2: cra = 0; cde = 80 * (2 * urand() - 1); break;				/* RA wrap */
				case 3: cra = 90.0 * (q % 4); cde = asin(2 * urand() - 1) * 180 / M_PI; break;	/* face edge */
				default: cra = 360 * urand(); cde = asin(2 * urand() - 1) * 180 / M_PI; break;
			}
			if (cde > 90) cde = 90;
			f = frame_at(cra, cde);
			n = make_shape(q, P);
			{
				double rot = 2 * M_PI * urand();
				for (int i = 0; i < n; i++)
				{
					xy t = {size * (P[i].x * cos(rot) - P[i].y * sin(rot)), size * (P[i].x * sin(rot) + P[i].y * cos(rot))};
					sc_vec3 v = from_plane(&f, t);
					P[i] = t;
					pde[i] = atan2(v.z, hypot(v.x, v.y)) * 180 / M_PI;
					pra[i] = atan2(v.y, v.x) * 180 / M_PI;
				}
			}
			/* vertices as stored (degrees) and back, so both sides see the same ring */
			for (int i = 0; i < n; i++)
				P[i] = to_plane(&f, sc_radec2vec(pra[i], pde[i]));
			if (q % 2)
				for (int i = 0; i < n / 2; i++)		/* clockwise half the time */
				{
					double t;
					xy u = P[i]; P[i] = P[n - 1 - i]; P[n - 1 - i] = u;
					t = pra[i]; pra[i] = pra[n - 1 - i]; pra[n - 1 - i] = t;
					t = pde[i]; pde[i] = pde[n - 1 - i]; pde[n - 1 - i] = t;
				}
			if ((err = sc_region_poly(&r, n, pra, pde, true)) != NULL)
			{
				bad_in++;
				if (bad_in < 10) printf("NONCONVEX REJECTED: shape %d size=%g: %s\n", q % 4, size, err);
				sc_region_free(&r);
				continue;
			}
			built++;
			tot_parts += r.nparts > 0 ? r.nparts : 1;
			tot_nv += n;

			/* area: signed fan from c (each triangle rounds to ~1e-16 sr either way) */
			for (int i = 0; i < n; i++)
			{
				sc_vec3 a = sc_radec2vec(pra[i], pde[i]), b = sc_radec2vec(pra[(i + 1) % n], pde[(i + 1) % n]);
				ref_area += 2 * atan2(sc_dot(f.c, sc_cross(a, b)), 1 + sc_dot(f.c, a) + sc_dot(a, b) + sc_dot(b, f.c));
			}
			if (fabs(r.area - fabs(ref_area)) > 1e-9 * fabs(ref_area) + 1e-15 * n)
			{
				bad_area++;
				if (bad_area < 10) printf("NONCONVEX AREA: shape %d size=%g: %.17g vs %.17g\n", q % 4, size, r.area, fabs(ref_area));
			}

			/*
			 * centroid: midpoint sampling converges as 1/m^2, so compare
			 * m and 2m and take Richardson's (4 f(2m) - f(m)) / 3 as the
			 * reference: good to ~2e-10 of the size at a radian, ~1e-12 below
			 * a degree, against the library's ~1e-12 everywhere
			 */
			if (q % 4 == 0)
			{
				sc_vec3 a = ref_centroid(&f, P, n, 64), b = ref_centroid(&f, P, n, 128), got = sc_region_centroid(&r), ref;
				double err;
				ref = (sc_vec3) {(4 * b.x - a.x) / 3, (4 * b.y - a.y) / 3, (4 * b.z - a.z) / 3};
				err = sc_angle(got, ref) / size;
				worst_cen = fmax(worst_cen, err);
				if (err > 1e-9 && sc_angle(got, ref) > 1e-15)
				{
					bad_cen++;
					if (bad_cen < 10) printf("NONCONVEX CENTROID: shape %d size=%g off by %g of the size\n", q % 4, size, err);
				}
			}

			sc_cover_compute(&r, &dens_hist, &p, &c);
			for (int k = 0; k < 400; k++)
			{
				xy t;
				double d;
				int ref, got;
				sc_vec3 v;

				if (k % 2)
					t = (xy) {size * 1.1 * (2 * urand() - 1), size * 1.1 * (2 * urand() - 1)};
				else
				{
					/* a hair either side of an edge */
					int e = (int) (urand() * n) % n;
					xy a = P[e], b = P[(e + 1) % n];
					double u = urand(), len = hypot(b.x - a.x, b.y - a.y), off = size * pow(10, -9 * urand()) * (urand() < 0.5 ? 1 : -1);
					t = (xy) {a.x + u * (b.x - a.x) - off * (b.y - a.y) / len, a.y + u * (b.y - a.y) + off * (b.x - a.x) / len};
				}
				v = from_plane(&f, t);
				t = to_plane(&f, v);
				ref = ref_inside(P, n, t, &d);
				if (d < 1e-11)
					continue;
				pts++;
				got = sc_region_contains(&r, v);
				if (got != ref)
				{
					bad_in++;
					if (bad_in < 10) printf("NONCONVEX CONTAINS: shape %d size=%g d=%g ref=%d got=%d\n", q % 4, size, d / size, ref, got);
				}
				if (ref)
				{
					ins++;
					if (!in_ranges(&c, sc_vec2pix(SC_MAX_ORDER, v)))
					{
						bad_cov++;
						if (bad_cov < 10) printf("NONCONVEX FALSE NEGATIVE: shape %d size=%g\n", q % 4, size);
					}
				}
			}
			for (int i = 0; i < NPTS; i += 41)
			{
				int inr = in_ranges(&c, cell[i]);
				if (inr) ncov++;
				if (sc_region_contains(&r, sc_radec2vec(ra[i], de[i])))
				{
					ntrue++;
					if (!inr) { bad_cov++; if (bad_cov < 10) printf("NONCONVEX FALSE NEGATIVE (catalogue): shape %d\n", q % 4); }
				}
			}
			tot_true += ntrue; tot_cov += ncov;

			/* CONTAINS and INTERSECTS: small triangles around the shape */
			for (int k = 0; k < 12; k++)
			{
				xy T[3], m = {size * 1.1 * (2 * urand() - 1), size * 1.1 * (2 * urand() - 1)};
				double tra[3], tde[3], s2 = size * pow(10, -2 + 1.7 * urand()), dmin = INFINITY, step = 0;
				int all_in = 1, clear_out = 0, any_in = 0, gotc, goto_;
				sc_region tr;

				for (int i = 0; i < 3; i++)
				{
					double a = 2 * M_PI * (i + 0.5 * urand()) / 3;
					sc_vec3 v;
					T[i] = (xy) {m.x + s2 * cos(a), m.y + s2 * sin(a)};
					v = from_plane(&f, T[i]);
					tde[i] = atan2(v.z, hypot(v.x, v.y)) * 180 / M_PI;
					tra[i] = atan2(v.y, v.x) * 180 / M_PI;
					T[i] = to_plane(&f, sc_radec2vec(tra[i], tde[i]));
				}
				if (sc_region_poly(&tr, 3, tra, tde, false) != NULL) { sc_region_free(&tr); continue; }
				/* 256 samples per edge: inside by more than a step is inside for sure */
				for (int i = 0; i < 3; i++)
				{
					xy a = T[i], b = T[(i + 1) % 3];
					step = fmax(step, hypot(b.x - a.x, b.y - a.y) / 256);
					for (int j = 0; j < 256; j++)
					{
						double u = j / 256.0, d;
						int in = ref_inside(P, n, (xy) {a.x + u * (b.x - a.x), a.y + u * (b.y - a.y)}, &d);
						dmin = fmin(dmin, d);
						if (!in && d > 1e-9 * size) clear_out = 1;
						if (!in) all_in = 0;
						if (in && d > 1e-9 * size) any_in = 1;
					}
				}
				/* a shape vertex inside the triangle (crossing number on T) */
				for (int i = 0; i < n && !any_in; i++)
				{
					double d;
					if (ref_inside(T, 3, P[i], &d) && d > 1e-9 * size) any_in = 1;
				}
				gotc = sc_region_contains_region(&tr, &r);
				goto_ = sc_region_overlaps(&tr, &r);
				if ((all_in && dmin > step && !gotc) || (clear_out && gotc))
				{
					bad_cr++;
					if (bad_cr < 10) printf("NONCONVEX CONTAINS(tri): shape %d size=%g got=%d all_in=%d dmin/step=%g\n", q % 4, size, gotc, all_in, dmin / step);
				}
				if ((any_in && !goto_) || (!any_in && dmin > step && goto_))
				{
					bad_ov++;
					if (bad_ov < 10) printf("NONCONVEX INTERSECTS(tri): shape %d size=%g got=%d any_in=%d\n", q % 4, size, goto_, any_in);
				}
				pairs++;
				sc_region_free(&tr);
			}
			/* the shape is inside itself, and inside its bounding cap */
			{
				sc_region cap;
				sc_vec3 bc;
				double br;
				sc_region_bounding_cap(&r, &bc, &br);
				sc_region_cone(&cap, atan2(bc.y, bc.x) * 180 / M_PI, atan2(bc.z, hypot(bc.x, bc.y)) * 180 / M_PI,
							   br * 180 / M_PI * (1 + 1e-9) + 1e-12, false);
				if (!sc_region_contains_region(&r, &r) || !sc_region_contains_region(&r, &cap))
				{
					bad_cr++;
					if (bad_cr < 10) printf("NONCONVEX CONTAINS(self/cap): shape %d size=%g\n", q % 4, size);
				}
			}
			sc_cover_free(&c);
			sc_region_free(&r);
		}
		printf("non-convex: %d polygons (%.1f vertices, %.1f convex pieces), %ld points (%ld inside), "
			   "%ld region pairs; fp %.3f; centroid within %.1e of the size; misses: contains %d, covering %d, "
			   "area %d, centroid %d, CONTAINS %d, INTERSECTS %d\n",
			   built, tot_nv / built, tot_parts / built, pts, ins, pairs, tot_cov > 0 ? 1 - tot_true / tot_cov : 0,
			   worst_cen, bad_in, bad_cov, bad_area, bad_cen, bad_cr, bad_ov);
		failures += bad_in + bad_cov + bad_area + bad_cen + bad_cr + bad_ov;
	}

	/*
	 * Disc sampling: many cones at the awkward places (poles, the z=+-2/3
	 * zone boundary, RA wrap, face edges), 200 points sampled inside each
	 * (half of them near the rim); every point's cell must be covered.
	 * Exercises the fast small-cone seed and the base-cell descent.
	 */
	for (int use_seed = 1; use_seed >= 0; use_seed--)
	{
		int ncones = 30000, bad = 0;
		double steps = 0, us = 0, ranges = 0;
		sc_cover_params p;
		sc_density dens_u = {1e9, 0, NULL};
		sc_cover_params_default(&p);
		p.use_seed = use_seed;
		rs = 777;
		for (int q = 0; q < ncones; q++)
		{
			double cra, cde, r = pow(10, -7 + 6.3 * urand()) * 180 / M_PI;	/* ~0.02 mas .. 10 deg */
			sc_region reg;
			sc_cover c;
			clock_t t0;
			switch (q % 6)
			{
				case 0: cra = 360 * urand(); cde = asin(2 * urand() - 1) * 180 / M_PI; break;
				case 1: cra = 360 * urand(); cde = (urand() < 0.5 ? 1 : -1) * (90 - pow(10, -6 + 6 * urand())); break;
				case 2: cra = 360 * urand(); cde = (urand() < 0.5 ? 1 : -1) * (41.8103148958 + (urand() - 0.5) * 2 * r); break;
				case 3: cra = (urand() - 0.5) * 2 * r; cde = asin(2 * urand() - 1) * 180 / M_PI; break;
				case 4: cra = 45.0 * (int) (8 * urand()) + (urand() - 0.5) * r; cde = asin(2 * urand() - 1) * 180 / M_PI; break;
				default: cra = 360 * urand(); cde = (urand() < 0.5 ? 1 : -1) * (90 - r * urand()); break;
			}
			if (cde > 90) cde = 90;
			if (cde < -90) cde = -90;
			sc_region_cone(&reg, cra, cde, r, true);
			t0 = clock();
			sc_cover_compute(&reg, &dens_u, &p, &c);
			us += (double) (clock() - t0) / CLOCKS_PER_SEC * 1e6;
			steps += c.steps;
			ranges += c.n;
			for (int k = 0; k < 200; k++)
			{
				/* random point at angular distance rho <= r from the centre */
				double rho = (k < 100 ? sqrt(urand()) : 1 - 1e-9 * urand()) * r * M_PI / 180 * (1 - 1e-12);
				double az = 2 * M_PI * urand();
				sc_vec3 c0 = reg.center, e1, e2, pt;
				/* orthonormal basis around the centre */
				e1 = (fabs(c0.z) < 0.9) ? (sc_vec3) {-c0.y, c0.x, 0} : (sc_vec3) {0, -c0.z, c0.y};
				{ double nn = sqrt(sc_dot(e1, e1)); e1.x /= nn; e1.y /= nn; e1.z /= nn; }
				e2 = sc_cross(c0, e1);
				pt.x = cos(rho) * c0.x + sin(rho) * (cos(az) * e1.x + sin(az) * e2.x);
				pt.y = cos(rho) * c0.y + sin(rho) * (cos(az) * e1.y + sin(az) * e2.y);
				pt.z = cos(rho) * c0.z + sin(rho) * (cos(az) * e1.z + sin(az) * e2.z);
				if (!in_ranges(&c, sc_vec2pix(SC_MAX_ORDER, pt)))
				{
					bad++;
					if (bad < 10) printf("SEED=%d FALSE NEGATIVE cone (%.10f, %.10f, r=%g deg) case %d\n", use_seed, cra, cde, r, q % 6);
				}
			}
			sc_cover_free(&c);
		}
		printf("disc sampling (seed=%d): %d cones x 200 pts, %d misses; avg %.1f ranges, %.0f steps, %.2f us/cover\n",
			   use_seed, ncones, bad, ranges / ncones, steps / ncones, us / ncones);
		failures += bad;
	}

	printf("%s (%d false negatives)\n", failures ? "FAILED" : "ALL OK", failures);
	return failures ? 1 : 0;
}
