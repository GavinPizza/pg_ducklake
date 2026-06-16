#pragma once

#define PGDUCKLAKE_PG_EXTENSION "pg_ducklake"

// Catalog in DuckDB. Where DuckLake metadata + tables live.
#define PGDUCKLAKE_DUCKDB_CATALOG        "pgducklake"
#define PGDUCKLAKE_DUCKDB_CATALOG_QUOTED "'pgducklake'"

// Companion catalog backed by libpgddb's PostgresStorageExtension. Lets
// DuckDB queries reach PG heap tables / foreign tables / views.
#define PGDUCKLAKE_PG_STORAGE_CATALOG "pgduckdb"

// Metadata schema in PostgreSQL.
#define PGDUCKLAKE_PG_SCHEMA        "ducklake"
#define PGDUCKLAKE_PG_SCHEMA_QUOTED "'ducklake'"

// Must match `CREATE ACCESS METHOD ducklake` in the SQL and the table_am_get_name_hook prefix.
#define PGDUCKLAKE_TABLE_AM "ducklake"

#define PGDUCKLAKE_SORTED_AM "ducklake_sorted"
