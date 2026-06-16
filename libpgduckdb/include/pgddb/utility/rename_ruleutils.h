#pragma once

#include "postgres.h"
#include "fmgr.h"

/* Rename vendor/pg_ruleutils.h functions to pgddb_* to avoid conflicts with PostgreSQL's. */
#define pg_get_indexdef_string           pgddb_pg_get_indexdef_string
#define pg_get_indexdef_columns          pgddb_pg_get_indexdef_columns
#define pg_get_indexdef_columns_extended pgddb_pg_get_indexdef_columns_extended
#define pg_get_querydef                  pgddb_pg_get_querydef_internal
#define pg_get_partkeydef_columns        pgddb_pg_get_partkeydef_columns
#define pg_get_partconstrdef_string      pgddb_pg_get_partconstrdef_string
#define pg_get_constraintdef_command     pgddb_pg_get_constraintdef_command
#define deparse_expression               pgddb_deparse_expression
#define deparse_context_for              pgddb_deparse_context_for
#define deparse_context_for_plan_tree    pgddb_deparse_context_for_plan_tree
#define set_deparse_context_plan         pgddb_set_deparse_context_plan
#define select_rtable_names_for_explain  pgddb_select_rtable_names_for_explain
#define generate_collation_name          pgddb_generate_collation_name
#define generate_opclass_name            pgddb_generate_opclass_name
#define get_range_partbound_string       pgddb_get_range_partbound_string
#define pg_get_statisticsobjdef_string   pgddb_pg_get_statisticsobjdef_string
#define get_list_partvalue_string        pgddb_get_list_partvalue_string

/* Route generate_qualified_relation_name / generate_relation_name to pgddb_relation_name. */
#define generate_qualified_relation_name          pgddb_relation_name
#define generate_relation_name(relid, namespaces) pgddb_relation_name(relid)

#define declare_pgddb_ruleutils_function(original_name) extern Datum pgddb_##original_name(PG_FUNCTION_ARGS);

#define pg_get_viewdef           pgddb_pg_get_viewdef
#define pg_get_viewdef_ext       pgddb_pg_get_viewdef_ext
#define pg_get_viewdef_wrap      pgddb_pg_get_viewdef_wrap
#define pg_get_viewdef_name      pgddb_pg_get_viewdef_name
#define pg_get_viewdef_name_ext  pgddb_pg_get_viewdef_name_ext
#define pg_get_triggerdef        pgddb_pg_get_triggerdef
#define pg_get_triggerdef_ext    pgddb_pg_get_triggerdef_ext
#define pg_get_indexdef_name     pgddb_pg_get_indexdef_name
#define pg_get_indexdef_name_ext pgddb_pg_get_indexdef_name_ext
declare_pgddb_ruleutils_function(pg_get_viewdef);
declare_pgddb_ruleutils_function(pg_get_viewdef_ext);
declare_pgddb_ruleutils_function(pg_get_viewdef_wrap);
declare_pgddb_ruleutils_function(pg_get_viewdef_name);
declare_pgddb_ruleutils_function(pg_get_viewdef_name_ext);
declare_pgddb_ruleutils_function(pg_get_triggerdef);
declare_pgddb_ruleutils_function(pg_get_triggerdef_ext);
declare_pgddb_ruleutils_function(pg_get_indexdef);
declare_pgddb_ruleutils_function(pg_get_indexdef_ext);
