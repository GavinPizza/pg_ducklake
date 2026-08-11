-- Scheduled last: until the drop is forwarded this test strands DuckLake
-- tables, which fails unrelated tests from any earlier slot.
BEGIN;
CREATE TABLE sx_t (a int) USING ducklake;
INSERT INTO sx_t VALUES (7), (8);
DROP TABLE sx_t;
COMMIT;

SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name = 'sx_t' AND end_snapshot IS NULL;

CREATE TABLE sx_t (a int) USING ducklake;
INSERT INTO sx_t VALUES (99);
SELECT * FROM sx_t ORDER BY a;
DROP TABLE sx_t;

-- CTAS creates through the direct-insert writer, not the plain CREATE path.
BEGIN;
CREATE TABLE sx_ctas USING ducklake AS SELECT g AS a FROM generate_series(1, 3) g;
DROP TABLE sx_ctas;
COMMIT;
SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name = 'sx_ctas' AND end_snapshot IS NULL;

-- The second CREATE only succeeds if the DROP actually reached DuckDB.
BEGIN;
CREATE TABLE sx_again (a int) USING ducklake;
INSERT INTO sx_again VALUES (1);
DROP TABLE sx_again;
CREATE TABLE sx_again (b text) USING ducklake;
INSERT INTO sx_again VALUES ('x');
COMMIT;
SELECT * FROM sx_again;
SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name = 'sx_again' AND end_snapshot IS NULL;
DROP TABLE sx_again;

CREATE TABLE sx_later (a int) USING ducklake;
DROP TABLE sx_later;
SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name = 'sx_later' AND end_snapshot IS NULL;

CREATE TABLE sx_heap (a int);
DROP TABLE sx_heap;
SELECT count(*) AS dl_rows FROM ducklake.ducklake_table WHERE table_name = 'sx_heap';

-- recycle_ddb reproduces a fresh backend: DuckDB uninitialized, so only the
-- metadata lookup can carry this drop.
CREATE TABLE sx_fresh (a int) USING ducklake;
CALL ducklake.recycle_ddb();
DROP TABLE sx_fresh;
SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name = 'sx_fresh' AND end_snapshot IS NULL;

-- DuckDB matches names case-insensitively, the metadata lookup does not.
BEGIN;
CREATE TABLE sx_case (a int) USING ducklake;
INSERT INTO sx_case VALUES (1), (2);
CREATE TABLE "SX_CASE" (a int);
DROP TABLE "SX_CASE";
COMMIT;
SELECT * FROM sx_case ORDER BY a;
DROP TABLE sx_case;

-- Same hole one level up: the schema name is resolved case-insensitively too.
CREATE TABLE sx_sch (a int) USING ducklake;
INSERT INTO sx_sch VALUES (1), (2);
CREATE SCHEMA "PUBLIC";
CREATE TABLE "PUBLIC".sx_sch (a int);
BEGIN;
INSERT INTO sx_sch VALUES (3);
DROP TABLE "PUBLIC".sx_sch;
COMMIT;
SELECT * FROM sx_sch ORDER BY a;
DROP TABLE sx_sch;
DROP SCHEMA "PUBLIC";

BEGIN;
CREATE TABLE sx_rb (a int) USING ducklake;
DROP TABLE sx_rb;
ROLLBACK;
SELECT count(*) AS rows_left FROM ducklake.ducklake_table WHERE table_name = 'sx_rb';

BEGIN;
CREATE TABLE sx_m1 (a int) USING ducklake;
CREATE TABLE sx_m2 (a int) USING ducklake;
DROP TABLE sx_m1, sx_m2;
COMMIT;
SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name IN ('sx_m1', 'sx_m2') AND end_snapshot IS NULL;

BEGIN;
CREATE SCHEMA sx_s;
CREATE TABLE sx_s.sx_c (a int) USING ducklake;
DROP SCHEMA sx_s CASCADE;
COMMIT;
SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name = 'sx_c' AND end_snapshot IS NULL;

-- Rows under the inlining limit never reach parquet, so the file-backed drop
-- needs a table large enough to flush.
BEGIN;
CREATE TABLE sx_files (a int) USING ducklake;
INSERT INTO sx_files SELECT g FROM generate_series(1, 50) g;
DROP TABLE sx_files;
COMMIT;
SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name = 'sx_files' AND end_snapshot IS NULL;
SELECT count(*) AS live_files FROM ducklake.ducklake_data_file f
JOIN ducklake.ducklake_table t ON f.table_id = t.table_id
WHERE t.table_name = 'sx_files' AND f.end_snapshot IS NULL;

-- The fallback resolves whatever name the DROP used, so a same-transaction
-- rename has to have reached DuckDB first.
BEGIN;
CREATE TABLE sx_ren (a int) USING ducklake;
INSERT INTO sx_ren VALUES (1);
ALTER TABLE sx_ren RENAME TO sx_ren2;
DROP TABLE sx_ren2;
COMMIT;
SELECT count(*) AS live_rows FROM ducklake.ducklake_table
WHERE table_name IN ('sx_ren', 'sx_ren2') AND end_snapshot IS NULL;

CREATE SCHEMA sx_other;
CREATE TABLE sx_other.sx_dup (a int) USING ducklake;
INSERT INTO sx_other.sx_dup VALUES (1), (2);
BEGIN;
CREATE TABLE sx_dup (a int) USING ducklake;
INSERT INTO sx_dup VALUES (9);
DROP TABLE sx_dup;
COMMIT;
SELECT * FROM sx_other.sx_dup ORDER BY a;
DROP TABLE sx_other.sx_dup;
DROP SCHEMA sx_other;

-- An unqualified DROP resolves to pg_temp ahead of public.
CREATE TABLE sx_tmp (a int) USING ducklake;
INSERT INTO sx_tmp VALUES (1), (2);
BEGIN;
INSERT INTO sx_tmp VALUES (3);
CREATE TEMP TABLE sx_tmp (a int);
DROP TABLE sx_tmp;
COMMIT;
SELECT * FROM public.sx_tmp ORDER BY a;
DROP TABLE public.sx_tmp;
