\echo Use "ALTER EXTENSION skycell UPDATE TO '0.36'" to load this file. \quit

-- Nothing in the catalog changes. 0.36 is the library's: a cross-match can
-- run in parallel workers (the custom scan is parallel safe), and the
-- skyregion GiST opclasses test a point query without converting it to a
-- unit vector again at every index entry. Answers are unchanged. The version
-- moves so that a database can tell, from pg_extension.extversion, which of
-- these it has.
