CREATE EXTENSION pg_vortex;
SELECT vortex_version();

-- read_vortex is registered as the marker UDF that the planner_hook
-- offloads to DuckDB.
SELECT proname, pronargs, prorettype::regtype
FROM pg_proc
WHERE proname = 'read_vortex' AND pronamespace = 'public'::regnamespace;
