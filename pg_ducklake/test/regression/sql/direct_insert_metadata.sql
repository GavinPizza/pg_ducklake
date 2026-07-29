-- Direct-insert (fast path) metadata commit protocol.
-- The fast path hand-writes the DuckLake commit; these tests pin the parts of
-- the protocol other DuckLake machinery relies on.

CALL ducklake.set_option('data_inlining_row_limit', 100);

-- ============================================================
-- 1. next_file_id must advance on a fast-path commit
-- ============================================================
-- DuckLake keys its table-stats cache on (next_file_id, schema_version,
-- table_id) and bumps next_file_id on inlined-only commits to signal a data
-- change. A fast-path commit that carries it forward leaves stale cached
-- stats (next_row_id, record_count, min/max) in every open DuckDB instance.

CREATE TABLE dim_t (id int, val text) USING ducklake;
INSERT INTO dim_t VALUES (0, 'seed');  -- normal path: creates the inlined data table

SELECT next_file_id AS file_id_before
FROM ducklake.ducklake_snapshot ORDER BY snapshot_id DESC LIMIT 1 \gset

SELECT ducklake.reset_direct_insert_stats();
INSERT INTO dim_t VALUES (1, 'fast');
-- fast path handled the insert above
SELECT pattern, reason, count FROM ducklake.direct_insert_stats() WHERE count > 0;

SELECT next_file_id > :file_id_before AS file_id_advanced
FROM ducklake.ducklake_snapshot ORDER BY snapshot_id DESC LIMIT 1;

-- ============================================================
-- 2. row ids must stay unique when fast-path and normal-path
--    inserts interleave
-- ============================================================
-- Load this backend's DuckDB table-stats cache, then advance next_row_id
-- through the fast path (PG metadata only). The normal-path insert below
-- must not seed its row ids from the stale cache entry.
SELECT count(*) FROM dim_t WHERE id >= 0;
INSERT INTO dim_t VALUES (2, 'fast2');
BEGIN;  -- transaction block: fast path disengages, normal DuckLake path
INSERT INTO dim_t VALUES (3, 'slow');
COMMIT;

SELECT it.table_name AS dim_inl
FROM ducklake.ducklake_inlined_data_tables it
JOIN ducklake.ducklake_table t USING (table_id)
WHERE t.table_name = 'dim_t' AND t.end_snapshot IS NULL
ORDER BY it.schema_version DESC LIMIT 1 \gset

SELECT row_id, count(*) AS dup_count
FROM ducklake.:dim_inl GROUP BY row_id HAVING count(*) > 1;

SELECT count(*) AS inlined_rows, count(DISTINCT row_id) AS distinct_row_ids
FROM ducklake.:dim_inl;

-- deleting one row must not take an unrelated row with it
DELETE FROM dim_t WHERE id = 3;
SELECT * FROM dim_t ORDER BY id;

-- ============================================================
-- 3. global column stats must not claim ranges that exclude
--    fast-path rows
-- ============================================================
-- The fast path maintains min/max by widening them to cover the rows it
-- writes, so the recorded range never excludes a committed row. Stale claims
-- are trusted by DuckLake readers of this catalog (global stats feed DuckDB
-- optimizer statistics, which fold provably-false filters) and are
-- perpetuated by later stats merges.

CREATE TABLE stats_t (id int) USING ducklake;
INSERT INTO stats_t SELECT i FROM generate_series(1, 200) i;  -- normal path, real stats

-- cache the (accurate, soon stale) stats in this backend
SELECT count(*) FROM stats_t WHERE id = 150;

SELECT ducklake.reset_direct_insert_stats();
INSERT INTO stats_t VALUES (1000);  -- outside the recorded min/max
INSERT INTO stats_t VALUES (NULL);  -- would violate contains_null = false
SELECT pattern, reason, count FROM ducklake.direct_insert_stats() WHERE count > 0;

-- A stale contains_null = false is not a lost optimization: the optimizer
-- folds IS NULL to false and the row disappears from the result.
SELECT s.min_value, s.max_value, s.contains_null
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'stats_t';

SELECT count(*) FROM stats_t WHERE id = 1000;
SELECT count(*) FROM stats_t WHERE id IS NULL;
SELECT count(*) FROM stats_t;

-- ============================================================
-- 4. contains_null is maintained for every column, not just
--    the ones whose type has a maintainable min/max
-- ============================================================
-- Seeded from parquet so contains_null starts known-false. An inline-only
-- table would leave it unknown instead, which section 6 covers.
CALL ducklake.set_option('data_inlining_row_limit', 0);
CREATE TABLE nullstats (id int, v int, f double precision, w int, bs bytea) USING ducklake;
INSERT INTO nullstats SELECT i, i, i::float8, i, '\x01'::bytea FROM generate_series(1, 200) i;
CALL ducklake.set_option('data_inlining_row_limit', 100);
INSERT INTO nullstats VALUES (901, 7, 1.5, 7, '\x02');  -- normal path: creates the inlined data table

-- f (DOUBLE) and bs (BLOB) are excluded from min/max by StatsEligible, so they
-- pin that null-ness is not gated by that exclusion. BLOB additionally is the
-- only fast-path-reachable type whose persisted row can carry extra_stats.
SELECT ducklake.reset_direct_insert_stats();
INSERT INTO nullstats VALUES (902, NULL, NULL, 8, NULL), (903, NULL, NULL, 9, NULL);
SELECT pattern, reason, count FROM ducklake.direct_insert_stats() WHERE count > 0;

-- v's bounds must stay at [1,200]: an all-NULL batch moves no bound.
SELECT s.column_id, s.contains_null, s.min_value, s.max_value
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'nullstats' ORDER BY s.column_id;

SELECT count(*) FROM nullstats WHERE v IS NULL;
SELECT count(*) FROM nullstats WHERE f IS NULL;

-- A batch with no NULLs must not reset it: the flip is one-way.
INSERT INTO nullstats VALUES (904, 8, 2.5, 10, '\x03');
SELECT s.column_id, s.contains_null
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'nullstats' ORDER BY s.column_id;
SELECT count(*) FROM nullstats WHERE v IS NULL;

-- w's first NULL, so column 4 must flip here and nowhere earlier -- otherwise
-- this asserts nothing about the COPY writer.
COPY nullstats (id, v, f, w, bs) FROM STDIN WITH (FORMAT csv, NULL '');
905,,3.5,,\x04
\.
SELECT s.column_id, s.contains_null
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'nullstats' ORDER BY s.column_id;
SELECT count(*) FROM nullstats WHERE v IS NULL;
SELECT count(*) FROM nullstats WHERE w IS NULL;
SELECT count(*) FROM nullstats;

-- ============================================================
-- 5. the UNNEST writer maintains it too
-- ============================================================
-- Its own null branch, so a fix applied to the VALUES and COPY writers can miss
-- it. Needs a dedicated table: an UNNEST naming a column subset does not reach
-- the fast path at all.
CALL ducklake.set_option('data_inlining_row_limit', 0);
CREATE TABLE nullstats_un (id int, v int) USING ducklake;
INSERT INTO nullstats_un SELECT i, i FROM generate_series(1, 200) i;
CALL ducklake.set_option('data_inlining_row_limit', 100);
INSERT INTO nullstats_un VALUES (901, 7);

PREPARE nullstats_un_ins (int[], int[]) AS
  INSERT INTO nullstats_un (id, v) SELECT UNNEST($1), UNNEST($2);
SELECT ducklake.reset_direct_insert_stats();
EXECUTE nullstats_un_ins(ARRAY[902, 903], ARRAY[NULL, NULL]::int[]);
SELECT pattern, reason, count FROM ducklake.direct_insert_stats() WHERE count > 0;

SELECT s.column_id, s.contains_null, s.min_value, s.max_value
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'nullstats_un' ORDER BY s.column_id;
SELECT count(*) FROM nullstats_un WHERE v IS NULL;
SELECT count(*) FROM nullstats_un;
DEALLOCATE nullstats_un_ins;

-- ============================================================
-- 6. a table that never flushed keeps contains_null unknown
-- ============================================================
-- The commonest fast-path shape, and the one where correctness rests on the
-- value staying unknown rather than being maintained: MergeStats' has_null_count
-- degrade is sticky, so no widen can ever write it. Seeding false here instead
-- would fold IS NULL to false and lose rows.
CALL ducklake.set_option('data_inlining_row_limit', 1000);
CREATE TABLE nullstats_inl (id int, v int) USING ducklake;
INSERT INTO nullstats_inl VALUES (1, 1);  -- normal path: creates the inlined data table
SELECT ducklake.reset_direct_insert_stats();
INSERT INTO nullstats_inl VALUES (2, NULL), (3, 3);
SELECT pattern, reason, count FROM ducklake.direct_insert_stats() WHERE count > 0;

SELECT s.column_id, s.contains_null IS NULL AS unknown
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'nullstats_inl' ORDER BY s.column_id;
SELECT count(*) FROM nullstats_inl WHERE v IS NULL;

SELECT * FROM ducklake.flush_inlined_data('nullstats_inl'::regclass);
SELECT count(*) FROM nullstats_inl WHERE v IS NULL;

-- Cleanup
DROP TABLE dim_t;
DROP TABLE stats_t;
DROP TABLE nullstats;
DROP TABLE nullstats_un;
DROP TABLE nullstats_inl;
CALL ducklake.set_option('data_inlining_row_limit', 0);
