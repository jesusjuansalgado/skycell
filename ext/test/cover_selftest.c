/*
 * Brute-force validation of sc_cover_compute():
 *   - no false negatives: every catalogue point inside the region has its
 *     order-29 cell inside one of the ranges (cones and convex polygons,
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

				sc_region_cone(&r, qra, qde, radii[ri]);
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
			if ((err = sc_region_poly(&r, nv, pra, pde)) != NULL) { fp_q++; sc_region_free(&r); continue; }
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
		printf("polygons: %d built (%d rejected as non-convex), ranges %.1f, true %.0f covered %.0f (fp %.3f)\n",
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
			if (sc_region_poly(&r, nv, pra, pde) != NULL)
				continue;				/* degenerate or non-convex: not our case */
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
			sc_region_cone(&reg, cra, cde, r);
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
