MODULE_big = skycell
OBJS = src/skycell.o src/cone_scan.o src/adql.o src/astro.o src/cover.o src/healpix.o src/gist_region.o src/spgist_region.o src/gist_region_box.o src/gist_region_box4.o src/gist_point_cap.o src/gist_point_cap4.o
EXTENSION = skycell
# Every version script and every upgrade script, so that adding a version means
# adding a file and nothing else.
DATA = $(wildcard sql/skycell--*.sql)
PG_CPPFLAGS = -DSKYCELL_PG
REGRESS = skycell adql cone_scan polygon parallel
REGRESS_OPTS = --inputdir=test --outputdir=test
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS does not track header dependencies: without these, changing a struct in
# cover.h rebuilt cover.o but not skycell.o, and the two disagreed about the
# size of an sc_region on the stack (a crash that only a PostgreSQL build,
# with its stack protector, reported).
src/skycell.o src/adql.o src/astro.o: src/cover.h src/healpix.h src/sc_alloc.h src/skycell_internal.h
src/cover.o: src/cover.h src/healpix.h src/sc_alloc.h
src/healpix.o: src/healpix.h
src/gist_region.o: src/cover.h src/healpix.h src/skycell_internal.h
src/spgist_region.o: src/cover.h src/healpix.h src/skycell_internal.h
src/gist_region_box.o: src/cover.h src/healpix.h src/skycell_internal.h
src/gist_region_box4.o: src/cover.h src/healpix.h src/skycell_internal.h
src/gist_point_cap.o: src/cover.h src/healpix.h src/skycell_internal.h
src/gist_point_cap4.o: src/cover.h src/healpix.h src/skycell_internal.h

# standalone geometry tests (no PostgreSQL needed)
selftest:
	$(CC) -O2 -o test/healpix_selftest test/healpix_selftest.c src/healpix.c -lm && ./test/healpix_selftest
	$(CC) -O2 -o test/cover_selftest test/cover_selftest.c src/cover.c src/healpix.c -lm && ./test/cover_selftest

# Coverings identical to a reference revision's (test/cover_equiv.c): for a
# change that should make them cheaper and nothing else.  Both builds use
# PGXS's code-generation flags, so floating point rounds as it does in the
# extension.
EQUIV_REF ?= HEAD
EQUIV_N ?= 2000000
EQUIV_CFLAGS = -O2 -fPIC -shared -fno-strict-aliasing -fwrapv -fexcess-precision=standard
.PHONY: cover-equiv
cover-equiv:
	rm -rf test/equiv && mkdir -p test/equiv/ref
	for f in cover.c cover.h healpix.c healpix.h sc_alloc.h; do \
	  git show $(EQUIV_REF):ext/src/$$f > test/equiv/ref/$$f || exit 1; done
	$(CC) $(EQUIV_CFLAGS) -Itest/equiv/ref test/equiv/ref/cover.c test/equiv/ref/healpix.c -lm -o test/equiv/libref.so
	$(CC) $(EQUIV_CFLAGS) -Isrc src/cover.c src/healpix.c -lm -o test/equiv/libnew.so
	$(CC) -O2 -Isrc test/cover_equiv.c -ldl -lm -o test/equiv/cover_equiv
	./test/equiv/cover_equiv test/equiv/libref.so test/equiv/libnew.so $(EQUIV_N)

# Bring databases that already have skycell up to the version just installed.
#
# `make install` deliberately does not do this.  It copies files into the
# PostgreSQL share directory, it may run with no server up at all, and issuing
# DDL against live databases as a side effect of a file install is a surprise
# nobody wants -- which is why no PostgreSQL extension does it.  Upgrading is a
# separate, explicit step; this target is the convenience, not a hidden action.
#
#   make upgrade                 -- every database the connection can see
#   make upgrade PGDATABASE=foo  -- just that one
#
# Connection settings come from the usual PG* environment variables.
PSQL ?= psql
.PHONY: upgrade
upgrade:
	@v=`sed -n "s/^default_version *= *'\(.*\)'/\1/p" skycell.control`; \
	if [ -z "$$v" ]; then echo "cannot read default_version from skycell.control" >&2; exit 1; fi; \
	echo "installed version: $$v"; \
	if [ -n "$$PGDATABASE" ]; then dbs="$$PGDATABASE"; else \
	  dbs=`$(PSQL) -X -Atq -d postgres -c "SELECT datname FROM pg_database WHERE datallowconn AND NOT datistemplate ORDER BY 1"`; \
	fi; \
	n=0; \
	for db in $$dbs; do \
	  cur=`$(PSQL) -X -Atq -d "$$db" -c "SELECT extversion FROM pg_extension WHERE extname='skycell'" 2>/dev/null`; \
	  if [ -z "$$cur" ]; then continue; fi; \
	  n=`expr $$n + 1`; \
	  if [ "$$cur" = "$$v" ]; then echo "  $$db: already $$v"; else \
	    echo "  $$db: $$cur -> $$v"; \
	    $(PSQL) -X -q -d "$$db" -c "ALTER EXTENSION skycell UPDATE TO '$$v'" || exit 1; \
	  fi; \
	done; \
	if [ $$n -eq 0 ]; then echo "  no database has skycell installed"; fi
