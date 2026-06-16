-- Table AM 'ducklake' is created once the extension is installed

-- Filtered to our extensions so the row set is stable whether or not other
-- DuckDB extensions (e.g. pg_duckdb in the coexistence CI run) are installed.
SELECT extname, extowner, extnamespace, extrelocatable, extversion, extconfig, extcondition
FROM pg_extension WHERE extname IN ('plpgsql', 'pg_ducklake') ORDER BY extname = 'pg_ducklake';

SELECT amname FROM pg_am WHERE amname = 'ducklake';

CREATE TABLE t (a int) USING ducklake;

SELECT relname
FROM pg_class
WHERE relam = (SELECT oid FROM pg_am WHERE amname = 'ducklake');

DROP TABLE t;

-- CASCADE: DuckLake-internal tables (e.g. inlined data tables) live in the
-- ducklake schema and are reclaimed lazily by DuckLake GC, so they may
-- still exist here. They hold no data once the ducklake tables are dropped.
DROP EXTENSION pg_ducklake CASCADE;

SELECT oid FROM pg_namespace WHERE nspname = 'ducklake';

CREATE EXTENSION pg_ducklake;

-- Verify catalog works after DROP+CREATE in same backend
CREATE TABLE t2 (a int, b text) USING ducklake;
INSERT INTO t2 VALUES (1, 'hello'), (2, 'world');
SELECT * FROM t2;
DROP TABLE t2;
