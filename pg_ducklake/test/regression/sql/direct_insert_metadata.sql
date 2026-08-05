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

-- bs (BLOB) has no maintainable min/max, so it pins that null-ness is not gated
-- by that exclusion. Its bounds stay stale, which is safe because ToStats()
-- returns nullptr for BLOB and nothing on the read side consults them.
-- extra_stats is likewise safe to never touch: it is only ever constructed for
-- GEOMETRY and VARIANT, and SupportsInlining rejects both, so no reachable type
-- can carry it.
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

-- ============================================================
-- 7. FLOAT/DOUBLE: bounds are maintained, NaN is tracked
--    separately instead of being folded into them
-- ============================================================
-- The vulnerable state is contains_nan = false, which is precisely what a
-- parquet write persists, and the condition under which ToStats() trusts a
-- floating-point min/max. A stale bound is then a live zone map that prunes
-- away the rows this writer just committed.
--
-- recycle_ddb() stands in for a new session throughout this section. The
-- inlined-data-table registration is cached per backend, so a scan issued by
-- the session that called ensure_inlined_data_table() does not see the inlined
-- rows at all and would assert nothing about pruning.
CALL ducklake.set_option('data_inlining_row_limit', 0);
CREATE TABLE fstats (v double precision, w real) USING ducklake;
INSERT INTO fstats SELECT i, i FROM generate_series(1, 200) i;
CALL ducklake.set_option('data_inlining_row_limit', 100);
SELECT count(*) FROM ducklake.ensure_inlined_data_table('fstats'::regclass);

SELECT s.column_id, s.contains_nan, s.min_value, s.max_value
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'fstats' ORDER BY s.column_id;

SELECT ducklake.reset_direct_insert_stats();
INSERT INTO fstats VALUES (1000, 1000);  -- outside the recorded bounds
SELECT pattern, reason, count FROM ducklake.direct_insert_stats() WHERE count > 0;

-- Widened, and stored in DuckDB's canonical float encoding ("1000.0", not the
-- "1000" PG's float8out emits) so the read path's Value(text)->cast round-trips.
SELECT s.column_id, s.contains_nan, s.min_value, s.max_value
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'fstats' ORDER BY s.column_id;

CALL ducklake.recycle_ddb();
SELECT count(*) AS v_eq_1000 FROM fstats WHERE v = 1000;
SELECT count(*) AS w_eq_1000 FROM fstats WHERE w = 1000;
SELECT count(*) AS v_gt_500 FROM fstats WHERE v > 500;
SELECT count(*) AS total FROM fstats;

-- Infinity is an ordinary bound: PG prints "Infinity", DuckDB's canonical form
-- is "inf", and the stored bytes must be the latter or the read-side cast fails.
INSERT INTO fstats VALUES ('Infinity', '-Infinity');
SELECT s.column_id, s.min_value, s.max_value
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'fstats' ORDER BY s.column_id;
CALL ducklake.recycle_ddb();
SELECT count(*) AS v_inf FROM fstats WHERE v = 'Infinity'::float8;
SELECT count(*) AS w_neg_inf FROM fstats WHERE w = '-Infinity'::real;

-- NaN never enters min/max -- PG's float8 btree proc sorts it above every other
-- value, so folding it in would make it the max and describe a bound nothing can
-- compare against. It sets contains_nan instead, which is what makes ToStats()
-- decline to build a statistic at all.
INSERT INTO fstats VALUES ('NaN', 5);
SELECT s.column_id, s.contains_nan, s.min_value, s.max_value
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'fstats' ORDER BY s.column_id;

-- A later batch without a NaN must not clear it: the flip is one-way.
INSERT INTO fstats VALUES (7, 7);
SELECT s.column_id, s.contains_nan
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'fstats' ORDER BY s.column_id;

CALL ducklake.recycle_ddb();
SELECT count(*) AS total FROM fstats;
DROP TABLE fstats;

-- ============================================================
-- 8. the property, not the type list: no column of any
--    fast-path-reachable type may keep a stale live bound
-- ============================================================
-- One column per PG type the direct-insert writer accepts, every stats row
-- seeded from parquet, then one direct insert outside every seeded bound. A
-- type added to DuckLake's ToStats() later, or one this writer never
-- considered, fails here instead of silently losing rows. An assertion over a
-- fixed list of eligible types would not.
--
-- Not reachable, so deliberately absent: TIMESTAMPTZ, LIST/STRUCT/MAP, VARIANT
-- and GEOMETRY all make DuckDBTypeToInlinedOid decline the whole insert.
CALL ducklake.set_option('data_inlining_row_limit', 0);
CREATE TABLE allty (
  c_bool boolean, c_i2 smallint, c_i4 integer, c_i8 bigint,
  c_f4 real, c_f8 double precision, c_num numeric(10,2),
  c_txt text, c_vc varchar(20), c_bpchar char(5), c_uuid uuid,
  c_date date, c_time time, c_ts timestamp,
  c_bytea bytea, c_json json, c_interval interval
) USING ducklake;
INSERT INTO allty SELECT
  (i % 2 = 0), i::smallint, i, i::bigint, i::real, i::float8, i::numeric(10,2),
  'x'||lpad(i::text, 4, '0'), 'y'||lpad(i::text, 4, '0'), 'z'||lpad(i::text, 3, '0'),
  ('00000000-0000-0000-0000-'||lpad(i::text, 12, '0'))::uuid,
  ('2020-01-01'::date + i * INTERVAL '1 day')::date,
  ('00:00:00'::time + i * INTERVAL '1 second'),
  ('2020-01-01'::timestamp + i * INTERVAL '1 second'),
  (lpad(i::text, 4, '0'))::bytea, ('{"a":'||i||'}')::json, (i * INTERVAL '1 second')
FROM generate_series(1, 200) i;
CALL ducklake.set_option('data_inlining_row_limit', 100);
SELECT count(*) FROM ducklake.ensure_inlined_data_table('allty'::regclass);

SELECT ducklake.reset_direct_insert_stats();
INSERT INTO allty VALUES (true, 9999, 9999, 9999, 9999, 9999, 9999.99,
  'zzzz', 'zzzz', 'zzzz', 'ffffffff-0000-0000-0000-000000000000'::uuid,
  '2099-12-31'::date, '23:59:59'::time, '2099-12-31 23:59:59'::timestamp,
  ('9999')::bytea, '{"zz":1}'::json, '9999 seconds'::interval);
SELECT pattern, reason, count FROM ducklake.direct_insert_stats() WHERE count > 0;

CALL ducklake.recycle_ddb();
-- Every one of these must return 1. c_bool is absent because both its values
-- are already in the seeded range, so it has no outside. c_bpchar compares
-- against the blank-padded form char(5) actually stores.
SELECT count(*) AS total FROM allty;
SELECT count(*) AS c_i2 FROM allty WHERE c_i2 = 9999;
SELECT count(*) AS c_i4 FROM allty WHERE c_i4 = 9999;
SELECT count(*) AS c_i8 FROM allty WHERE c_i8 = 9999;
SELECT count(*) AS c_f4 FROM allty WHERE c_f4 = 9999;
SELECT count(*) AS c_f8 FROM allty WHERE c_f8 = 9999;
SELECT count(*) AS c_num FROM allty WHERE c_num = 9999.99;
SELECT count(*) AS c_txt FROM allty WHERE c_txt = 'zzzz';
SELECT count(*) AS c_vc FROM allty WHERE c_vc = 'zzzz';
SELECT count(*) AS c_bpchar FROM allty WHERE c_bpchar = 'zzzz ';
SELECT count(*) AS c_uuid FROM allty WHERE c_uuid = 'ffffffff-0000-0000-0000-000000000000';
SELECT count(*) AS c_date FROM allty WHERE c_date = '2099-12-31';
SELECT count(*) AS c_time FROM allty WHERE c_time = '23:59:59';
SELECT count(*) AS c_ts FROM allty WHERE c_ts = '2099-12-31 23:59:59';
SELECT count(*) AS c_bytea FROM allty WHERE c_bytea = ('9999')::bytea;
SELECT count(*) AS c_json FROM allty WHERE c_json::text = '{"zz":1}';
SELECT count(*) AS c_interval FROM allty WHERE c_interval = '9999 seconds'::interval;
DROP TABLE allty;

-- ============================================================
-- 9. default deny: a column this writer did not maintain has
--    its statistic degraded, not left stale
-- ============================================================
-- Nested children are the case that cannot be fixed by extending the
-- accumulator: DuckLake persists a stats row per child field id, the read path
-- resolves those through field_references and prunes on them, and the
-- accumulator's own metadata query cannot see them. So they are degraded to
-- unknown -- pruning off, answers correct -- by the fallback that keys on "not
-- maintained" rather than on a type list.
--
-- COPY FROM STDIN is the only writer that reaches a table with a LIST column
-- (the direct-insert path declines it outright), so the child stats row is
-- asserted from the catalog rather than through a scan.
CALL ducklake.set_option('data_inlining_row_limit', 0);
CREATE TABLE childstats (id int, arr integer[]) USING ducklake;
INSERT INTO childstats SELECT i, ARRAY[i, i + 1] FROM generate_series(1, 200) i;
CALL ducklake.set_option('data_inlining_row_limit', 100);
SELECT count(*) FROM ducklake.ensure_inlined_data_table('childstats'::regclass);

-- The element column carries a live [1,201] bound nothing here maintains.
SELECT c.column_id, c.column_name, c.parent_column, s.min_value, s.max_value, s.contains_null
FROM ducklake.ducklake_column c
JOIN ducklake.ducklake_table_column_stats s USING (table_id, column_id)
JOIN ducklake.ducklake_table t ON t.table_id = c.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'childstats' AND c.end_snapshot IS NULL ORDER BY c.column_id;

COPY childstats (id, arr) FROM STDIN WITH (FORMAT csv);
9999,"{9999,10000}"
\.

-- id widens; element degrades to unknown; the LIST row itself is inert
-- (ToStats() returns nullptr for it) so it is left alone.
SELECT c.column_id, c.column_name, c.parent_column, s.min_value, s.max_value, s.contains_null
FROM ducklake.ducklake_column c
JOIN ducklake.ducklake_table_column_stats s USING (table_id, column_id)
JOIN ducklake.ducklake_table t ON t.table_id = c.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'childstats' AND c.end_snapshot IS NULL ORDER BY c.column_id;

-- Degrading is one-way and sticky, so it costs nothing after the first time:
-- a batch inside every maintained bound must not rewrite the row at all.
SELECT s.ctid::text AS elem_ctid
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'childstats' AND s.column_id = 3 \gset

COPY childstats (id, arr) FROM STDIN WITH (FORMAT csv);
50,"{50,51}"
51,"{51,52}"
\.
SELECT s.ctid::text = :'elem_ctid' AS untouched
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'childstats' AND s.column_id = 3;
DROP TABLE childstats;

-- ------------------------------------------------------------------
-- A bound the widen cannot even compare against is unmaintained too.
--
-- After an out-of-transaction ALTER COLUMN TYPE, a persisted DATE bound outside
-- TIMESTAMP range makes MergeStats' non-Try cast throw -- and makes the read
-- path throw the same way, at plan time, for any query touching the column.
-- Leaving it is the stale-and-live case; degrading it is what the fallback is
-- for, and it is the only thing here that can restore those queries.
-- ------------------------------------------------------------------
CALL ducklake.set_option('data_inlining_row_limit', 1000);
CREATE TABLE dstale (d DATE) USING ducklake;
SET ducklake.enable_direct_insert = false;
INSERT INTO dstale VALUES ('2024-01-01'), ('5874897-12-31');
SELECT count(*) > 0 AS flushed FROM ducklake.flush_inlined_data('dstale'::regclass);
SET ducklake.enable_direct_insert = true;
ALTER TABLE dstale ALTER COLUMN d TYPE TIMESTAMP;

SELECT s.min_value, s.max_value
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'dstale';

INSERT INTO dstale VALUES ('2026-06-01 00:00:00');
SELECT s.min_value, s.max_value
FROM ducklake.ducklake_table_column_stats s
JOIN ducklake.ducklake_table t ON t.table_id = s.table_id AND t.end_snapshot IS NULL
WHERE t.table_name = 'dstale';

CALL ducklake.recycle_ddb();
SELECT count(*) AS total FROM dstale;
SELECT count(*) AS before_2100 FROM dstale WHERE d < '2100-01-01';
DROP TABLE dstale;

-- Cleanup
DROP TABLE dim_t;
DROP TABLE stats_t;
DROP TABLE nullstats;
DROP TABLE nullstats_un;
DROP TABLE nullstats_inl;
CALL ducklake.set_option('data_inlining_row_limit', 0);
