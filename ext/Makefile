MODULE_big = skycell
OBJS = src/skycell.o src/adql.o src/astro.o src/cover.o src/healpix.o
EXTENSION = skycell
DATA = sql/skycell--0.1.sql sql/skycell--0.2.sql sql/skycell--0.1--0.2.sql
PG_CPPFLAGS = -DSKYCELL_PG
REGRESS = skycell adql
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

# standalone geometry tests (no PostgreSQL needed)
selftest:
	$(CC) -O2 -o test/healpix_selftest test/healpix_selftest.c src/healpix.c -lm && ./test/healpix_selftest
	$(CC) -O2 -o test/cover_selftest test/cover_selftest.c src/cover.c src/healpix.c -lm && ./test/cover_selftest
