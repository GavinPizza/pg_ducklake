CREATE TABLE table_missing_attrs(id int);
INSERT INTO table_missing_attrs VALUES (1);

SELECT * FROM table_missing_attrs;
SELECT id from table_missing_attrs;

ALTER TABLE table_missing_attrs ADD COLUMN a int DEFAULT 10;
ALTER TABLE table_missing_attrs ADD COLUMN b int DEFAULT 20;
ALTER TABLE table_missing_attrs ADD COLUMN c int DEFAULT NULL;
ALTER TABLE table_missing_attrs ADD COLUMN d int DEFAULT 30;
ALTER TABLE table_missing_attrs ADD COLUMN e int DEFAULT 0;

SELECT * FROM table_missing_attrs;
SELECT a, c, d, e FROM table_missing_attrs;

INSERT INTO table_missing_attrs(id, a, b) VALUES (2, 100, 200);
ALTER TABLE table_missing_attrs ADD COLUMN f TEXT DEFAULT 'abcdefghijklmnopqrstuvwxyz';

SELECT * FROM table_missing_attrs;
SELECT a, c, d, f FROM table_missing_attrs;

DROP TABLE table_missing_attrs;

CREATE TABLE table_dropped_attrs(a int, b text, c int, d text);
INSERT INTO table_dropped_attrs VALUES (1, 'x', 10, 'p'), (2, 'y', 20, 'q'), (3, 'z', 30, 'r');

SELECT * FROM table_dropped_attrs ORDER BY a;

SELECT attname, attnum, attisdropped
FROM pg_attribute
WHERE attrelid = 'table_dropped_attrs'::regclass AND attnum > 0;

ALTER TABLE table_dropped_attrs DROP COLUMN b;
ALTER TABLE table_dropped_attrs DROP COLUMN d;

SELECT attname, attnum, attisdropped
FROM pg_attribute
WHERE attrelid = 'table_dropped_attrs'::regclass AND attnum > 0;

\set pwd `pwd`
\set parquet_file_path '\'' :pwd '/tmp_check/table_dropped_attrs.parquet'  '\''

COPY table_dropped_attrs TO :parquet_file_path (FORMAT PARQUET);
SELECT * FROM read_parquet(:parquet_file_path) r ORDER BY r['a'];
COPY table_dropped_attrs (c, a) TO :parquet_file_path (FORMAT PARQUET);
SELECT * FROM read_parquet(:parquet_file_path) r ORDER BY r['a'];
SELECT * FROM table_dropped_attrs ORDER BY a;
COPY table_dropped_attrs TO STDOUT;
SELECT c, a FROM table_dropped_attrs ORDER BY a;
SELECT a, c FROM table_dropped_attrs WHERE c > 10 ORDER BY a;
SELECT a FROM table_dropped_attrs WHERE c > 10 ORDER BY a;
SELECT a, c FROM table_dropped_attrs ORDER BY c DESC;
SELECT COUNT(*) FROM table_dropped_attrs;

ALTER TABLE table_dropped_attrs ADD COLUMN e int DEFAULT 99;

SELECT * FROM table_dropped_attrs ORDER BY a;
SELECT e, c, a FROM table_dropped_attrs ORDER BY a;

-- Drop the first column so live attnos (3, 5) no longer line up with
-- DuckDB column indexes (0, 1).
ALTER TABLE table_dropped_attrs DROP COLUMN a;

SELECT * FROM table_dropped_attrs ORDER BY c;
SELECT e, c FROM table_dropped_attrs ORDER BY c;
COPY table_dropped_attrs TO :parquet_file_path (FORMAT PARQUET);
SELECT * FROM read_parquet(:parquet_file_path) r ORDER BY r['c'];

DROP TABLE table_dropped_attrs;
