-- Apache Arrow file/stream reader (delegated to DuckDB's nanoarrow extension).
--
-- The SQL function is created unconditionally so user scripts can call
-- read_arrow() regardless of how pg_duckdb was built. The underlying
-- C function (pgduckdb_read_arrow) emits a friendly ereport when pg_duckdb
-- was compiled without WITH_NANOARROW; when built with it, the planner
-- hook routes the query to DuckDB where the bundled nanoarrow extension
-- handles the read.

CREATE FUNCTION @extschema@.read_arrow(path text)
RETURNS SETOF duckdb.row
SET search_path = pg_catalog, pg_temp
AS 'MODULE_PATHNAME', 'pgduckdb_read_arrow'
LANGUAGE C;

CREATE FUNCTION @extschema@.read_arrow(path text[])
RETURNS SETOF duckdb.row
SET search_path = pg_catalog, pg_temp
AS 'MODULE_PATHNAME', 'pgduckdb_read_arrow'
LANGUAGE C;

-- read_vortex function
CREATE FUNCTION @extschema@.read_vortex(path text)
RETURNS SETOF duckdb.row
SET search_path = pg_catalog, pg_temp
AS 'MODULE_PATHNAME', 'duckdb_only_function'
LANGUAGE C;

-- read_text function for single path
CREATE FUNCTION @extschema@.read_text(path text)
RETURNS SETOF duckdb.row
SET search_path = pg_catalog, pg_temp
AS 'MODULE_PATHNAME', 'duckdb_only_function'
LANGUAGE C;

-- read_text function for array of paths
CREATE FUNCTION @extschema@.read_text(path text[])
RETURNS SETOF duckdb.row
SET search_path = pg_catalog, pg_temp
AS 'MODULE_PATHNAME', 'duckdb_only_function'
LANGUAGE C;

-- read_blob function for single path
CREATE FUNCTION @extschema@.read_blob(path text)
RETURNS SETOF duckdb.row
SET search_path = pg_catalog, pg_temp
AS 'MODULE_PATHNAME', 'duckdb_only_function'
LANGUAGE C;

-- read_blob function for array of paths
CREATE FUNCTION @extschema@.read_blob(path text[])
RETURNS SETOF duckdb.row
SET search_path = pg_catalog, pg_temp
AS 'MODULE_PATHNAME', 'duckdb_only_function'
LANGUAGE C;
