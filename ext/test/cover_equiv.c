/*
 * Covering equivalence, for changes to cover.c or healpix.c meant to make a
 * covering cheaper without changing it.  Two builds of the two files -- a
 * reference revision's and the working tree's -- are loaded side by side and
 * must return the same covering, range for range, with the same step count,
 * expected rows, area, density and order, for the same cone, density model
 * and parameters.  Cones: centres on a clustered catalogue or uniform, some
 * near the poles, radii 0.0001-10 degrees; range_cost 1-1000; uniform and
 * 1001-bound histogram densities.  Also times both.
 *
 *   make cover-equiv [EQUIV_REF=<git revision>] [EQUIV_N=<cones>]
 *   cover_equiv REF.so NEW.so [ncones]
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cover.h"

typedef const char *(*cone_fn) (sc_region *, double, double, double, bool);
typedef void (*cover_fn) (const sc_region *, const sc_density *, const sc_cover_params *, sc_cover *);
typedef void (*free_cover_fn) (sc_cover *);
typedef void (*free_region_fn) (sc_region *);
typedef void (*defaults_fn) (sc_cover_params *);
typedef int64_t (*ang2pix_fn) (int, double, double);

typedef struct
{
	cone_fn		cone;
	cover_fn	cover;
	free_cover_fn free_cover;
	free_region_fn free_region;
	defaults_fn defaults;
	ang2pix_fn	ang2pix;
} lib_t;

static void *
sym(void *h, const char *name)
{
	void	   *p = dlsym(h, name);

	if (!p)
	{
		fprintf(stderr, "missing %s\n", name);
		exit(2);
	}
	return p;
}

static lib_t
load(const char *path)
{
	void	   *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	lib_t		l;

	if (!h)
	{
		fprintf(stderr, "%s\n", dlerror());
		exit(2);
	}
	l.cone = (cone_fn) sym(h, "sc_region_cone");
	l.cover = (cover_fn) sym(h, "sc_cover_compute");
	l.free_cover = (free_cover_fn) sym(h, "sc_cover_free");
	l.free_region = (free_region_fn) sym(h, "sc_region_free");
	l.defaults = (defaults_fn) sym(h, "sc_cover_params_default");
	l.ang2pix = (ang2pix_fn) sym(h, "sc_ang2pix");
	return l;
}

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static double
urand(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (rs >> 11) * (1.0 / 9007199254740992.0);
}
static double
gauss(void)
{
	return sqrt(-2 * log(urand() + 1e-300)) * cos(2 * M_PI * urand());
}

static int
cmp64(const void *a, const void *b)
{
	int64_t		x = *(const int64_t *) a,
				y = *(const int64_t *) b;

	return (x > y) - (x < y);
}

#define NPTS 1000000
#define NB 1001
static double pra[NPTS], pde[NPTS];
static int64_t cells[NPTS], bounds[NB];

/* clustered catalogue, as cover_selftest.c: 40% uniform, 60% in 30 blobs */
static void
make_points(ang2pix_fn ang2pix)
{
	double		cra[30], cde[30], sig[30];

	for (int k = 0; k < 30; k++)
	{
		cra[k] = 360 * urand();
		cde[k] = asin(2 * urand() - 1) * 180 / M_PI;
		sig[k] = 0.01 + 1.0 * urand();
	}
	cde[0] = 89.9;
	cde[1] = -89.95;
	cra[2] = 0.01;
	for (int i = 0; i < NPTS; i++)
	{
		if (urand() < 0.4)
		{
			pra[i] = 360 * urand();
			pde[i] = asin(2 * urand() - 1) * 180 / M_PI;
		}
		else
		{
			int			k = (int) (30 * urand());
			double		d = cde[k] + sig[k] * gauss();
			double		a = cra[k] + sig[k] * gauss() / fmax(cos(cde[k] * M_PI / 180), 0.01);

			if (d > 90) { d = 180 - d; a += 180; }
			if (d < -90) { d = -180 - d; a += 180; }
			a = fmod(a, 360);
			if (a < 0) a += 360;
			pra[i] = a;
			pde[i] = d;
		}
		cells[i] = ang2pix(29, pra[i], pde[i]);
	}
	qsort(cells, NPTS, sizeof(int64_t), cmp64);
	/* an equi-depth histogram of NB bounds, as ANALYZE keeps at target 1000 */
	for (int b = 0; b < NB; b++)
		bounds[b] = cells[(int64_t) b * (NPTS - 1) / (NB - 1)];
}

static int
same(const sc_cover *a, const sc_cover *b)
{
	if (a->n != b->n || a->steps != b->steps || a->deepest != b->deepest ||
		a->order != b->order || a->exp_rows != b->exp_rows || a->area != b->area ||
		a->rho != b->rho)
		return 0;
	for (int i = 0; i < a->n; i++)
		if (a->r[i].lo != b->r[i].lo || a->r[i].hi != b->r[i].hi)
			return 0;
	return 1;
}

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int
main(int argc, char **argv)
{
	lib_t		ref = load(argv[1]),
				new = load(argv[2]);
	long		n = argc > 3 ? atol(argv[3]) : 200000;
	long		bad = 0,
				total = 0;
	double		t_ref = 0,
				t_new = 0;
	const double rcosts[] = {1, 5, 32, 110, 1000};
	const double ntot[] = {5e5, 1e7, 1.8e9};

	make_points(ref.ang2pix);
	for (long i = 0; i < n; i++)
	{
		sc_region ra, rb;
		sc_cover ca, cb;
		sc_cover_params p;
		sc_density d = {0};
		double		lra, lde, rad;
		double		t0;
		int			hist = i % 2;

		/* centres on the catalogue half the time, uniform otherwise */
		if (i % 4 < 2)
		{
			int			k = (int) (urand() * NPTS);

			lra = pra[k];
			lde = pde[k];
		}
		else
		{
			lra = 360 * urand();
			lde = asin(2 * urand() - 1) * 180 / M_PI;
		}
		if (i % 97 == 0)
			lde = (i % 2 ? 1 : -1) * (89.0 + urand());	/* near a pole */
		rad = pow(10, -4 + 5 * urand());	/* 0.0001 to 10 degrees */
		ref.defaults(&p);
		p.range_cost = rcosts[i % 5];
		d.ntotal = ntot[(i / 5) % 3];
		d.relpages = d.ntotal / 50;
		if (hist)
		{
			d.nbounds = NB;
			d.bounds = bounds;
		}
		if (ref.cone(&ra, lra, lde, rad, true) || new.cone(&rb, lra, lde, rad, true))
			continue;
		t0 = now();
		ref.cover(&ra, &d, &p, &ca);
		t_ref += now() - t0;
		t0 = now();
		new.cover(&rb, &d, &p, &cb);
		t_new += now() - t0;
		total++;
		if (!same(&ca, &cb))
		{
			if (bad < 10)
				fprintf(stderr, "differ: ra %.9f dec %.9f r %.9g rc %g n %g hist %d: n %d/%d steps %d/%d rows %.17g/%.17g\n",
						lra, lde, rad, p.range_cost, d.ntotal, hist, ca.n, cb.n,
						ca.steps, cb.steps, ca.exp_rows, cb.exp_rows);
			bad++;
		}
		ref.free_cover(&ca);
		new.free_cover(&cb);
		ref.free_region(&ra);
		new.free_region(&rb);
	}
	printf("%ld coverings compared, %ld differ; ref %.3f us, new %.3f us per covering (new/ref %.3f)\n",
		   total, bad, 1e6 * t_ref / total, 1e6 * t_new / total, t_new / t_ref);
	return bad != 0;
}
