/*
 * sc_alloc.h -- allocation hooks so the geometry code builds both inside a
 * PostgreSQL backend (palloc, freed with the memory context) and standalone.
 */
#ifndef SKYCELL_ALLOC_H
#define SKYCELL_ALLOC_H

#ifdef SKYCELL_PG
#include "postgres.h"
#define SC_MALLOC(sz)		palloc(sz)
#define SC_REALLOC(p, sz)	repalloc((p), (sz))
#define SC_FREE(p)			pfree(p)
#else
#include <stdlib.h>
#define SC_MALLOC(sz)		malloc(sz)
#define SC_REALLOC(p, sz)	realloc((p), (sz))
#define SC_FREE(p)			free(p)
#endif

#endif
