\echo Use "ALTER EXTENSION skycell UPDATE TO '0.31'" to load this file. \quit

-- No SQL objects change in 0.31: the release is the library's cone scan
-- index filter (skycell.index_filter), which an existing database picks up
-- from the new library alone. The update only records the version.
