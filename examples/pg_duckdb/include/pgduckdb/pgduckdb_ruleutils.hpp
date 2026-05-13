#pragma once

extern "C" {
#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
}

namespace pgduckdb {

/* Installs pg_duckdb's ruleutils hook impls into libpgddb. Called from _PG_init. */
void InitRuleutilsHooks();

} // namespace pgduckdb

extern "C" {

/*
 * DDL deparsers. These compose CREATE TABLE / ALTER TABLE / RENAME / CREATE
 * VIEW statements for DuckDB. They have pg_duckdb-specific policy
 * (MotherDuck checks, duckdb table-AM, duckdb.row arguments) interleaved
 * with the generic deparse mechanics, so they live consumer-side rather
 * than in libpgddb.
 */
char *pgduckdb_get_tabledef(Oid relation_id);
char *pgduckdb_get_alter_tabledef(Oid relation_oid, AlterTableStmt *alter_stmt);
char *pgduckdb_get_rename_relationdef(Oid relation_oid, RenameStmt *rename_stmt);
char *pgduckdb_get_viewdef(const ViewStmt *stmt, const char *postgres_schema_name, const char *view_name,
                           const char *duckdb_query_string);

/*
 * The pg_duckdb db-and-schema policy. Also registered as
 * pgddb_db_and_schema_hook so libpgddb-side pgddb_db_and_schema_string can
 * dispatch through it.
 */
List *pgduckdb_db_and_schema(const char *postgres_schema_name, const char *duckdb_table_am_name);

} // extern "C"
