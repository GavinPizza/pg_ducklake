# The ORDER BY pushdown optimizer opens indexes from inside a DuckDB
# OptimizerExtension callback, which runs under the ClientContextLock held by
# ClientContext::Prepare. A Postgres error there must unwind as a C++ exception:
# a longjmp skips that lock's destructor and deadlocks the backend's own DuckDB
# connection, so s_after, not s_query, is the real assertion here.
#
# A regression does not show up as a diff. s_after never returns, isolationtester
# cancels it after 360s, and the run takes ~10 minutes and leaves a backend that
# ignores SIGTERM and needs SIGKILL.

setup
{
  CREATE TABLE iso_opt_heap (id int, val text);
  INSERT INTO iso_opt_heap SELECT g, 'v' || g FROM generate_series(1, 50) g;
  CREATE INDEX iso_opt_heap_idx ON iso_opt_heap (id);
  CREATE TABLE iso_opt_lake (x int) USING ducklake;
  INSERT INTO iso_opt_lake VALUES (1), (2), (3);
}

teardown
{
  DROP TABLE iso_opt_lake;
  DROP TABLE iso_opt_heap;
}

session blocker
# REINDEX takes AccessExclusiveLock on the index but only ShareLock on the table,
# so the scanner can still plan and reach index_open before it blocks.
step b_begin   { BEGIN; }
step b_reindex { REINDEX INDEX iso_opt_heap_idx; }
step b_commit  { COMMIT; }

session scanner
# Without lock_timeout the scanner waits instead of erroring, and nothing unwinds.
setup { SET lock_timeout = '500ms'; }
# The DuckLake join routes this through DuckDB; the ORDER BY over the indexed heap
# is what makes the optimizer open iso_opt_heap's indexes.
step s_query { SELECT h.id FROM (SELECT id FROM iso_opt_heap ORDER BY id) h JOIN iso_opt_lake ON h.id = iso_opt_lake.x; }
step s_after { SELECT count(*) FROM iso_opt_lake; }

permutation b_begin b_reindex s_query s_after b_commit
