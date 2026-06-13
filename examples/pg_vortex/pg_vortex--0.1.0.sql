\echo Use "CREATE EXTENSION pg_vortex" to load this file. \quit

CREATE FUNCTION vortex_version()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_vortex_version'
LANGUAGE C STRICT;

-- Marker UDF: the planner_hook intercepts calls and offloads to DuckDB's
-- read_vortex table function; the C stub errors if reached directly.
CREATE FUNCTION read_vortex(path text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'duckdb_only_function'
LANGUAGE C STRICT;
