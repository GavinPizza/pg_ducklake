CREATE EXTENSION pg_vortex;
SELECT vortex_version();

-- read_vortex is registered as the marker UDF that the planner_hook
-- offloads to DuckDB.
SELECT proname, pronargs, prorettype::regtype
FROM pg_proc
WHERE proname = 'read_vortex' AND pronamespace = 'public'::regnamespace;

-- Path is relative to the temp-instance's PGDATA (tmp_check/data) -- the
-- backend's CWD at query time. data/sample.vortex sits at the same level
-- as Makefile, so ../../data/ resolves there.
\set fixture '\'../../data/sample.vortex\''

-- Full read: planner-hook offload + CustomScan + type conversion.
SELECT * FROM read_vortex(:fixture) AS r(x bigint, y bigint) ORDER BY x;

-- EXPLAIN to show the plan is a VortexScan custom-scan node.
EXPLAIN (COSTS OFF) SELECT * FROM read_vortex(:fixture) AS r(x bigint, y bigint);

-- Projection: only one column survives the deparse.
SELECT x FROM read_vortex(:fixture) AS r(x bigint, y bigint) ORDER BY x;

-- Filter: WHERE clause is deparsed and pushed to DuckDB.
SELECT * FROM read_vortex(:fixture) AS r(x bigint, y bigint) WHERE x > 2 ORDER BY x;

-- Aggregate: runs on the DuckDB side.
SELECT count(*) AS rows, sum(x) AS sum_x, sum(y) AS sum_y
FROM read_vortex(:fixture) AS r(x bigint, y bigint);

-- ORDER BY + LIMIT.
SELECT * FROM read_vortex(:fixture) AS r(x bigint, y bigint) ORDER BY y DESC LIMIT 3;

-- Missing file: DuckDB error propagates back through pg_vortex/CreatePlan.
SELECT * FROM read_vortex('/nonexistent.vortex') AS r(x bigint, y bigint);
