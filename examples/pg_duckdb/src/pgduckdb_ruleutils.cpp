#include "duckdb.hpp"

#include "pgddb/pg/locale.hpp"
#include "pgddb/pg/relations.hpp"
#include "pgddb/pg/string_utils.hpp"
#include "pgddb/pgddb_duckdb.hpp"
#include "pgddb/pgddb_types.hpp"

extern "C" {
#include "postgres.h"

#include "pgddb/pgddb_ruleutils.h"

#include "access/relation.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/heap.h"
#include "commands/tablecmds.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "pgddb/vendor/pg_ruleutils.h"
#include "pgddb/vendor/pg_list.hpp"
}

#include "pgduckdb/pgduckdb.h"
#include "pgduckdb/pgduckdb_ddl.hpp"
#include "pgduckdb/pgduckdb_metadata_cache.hpp"
#include "pgduckdb/pgduckdb_ruleutils.hpp"
#include "pgduckdb/pgduckdb_table_am.hpp"
#include "pgduckdb/pgduckdb_userdata_cache.hpp"

/* ------------------------------------------------------------------------
 * pg_duckdb composite-type detection helpers (file-local). The hook impls
 * below use these to identify the pgduckdb-specific Oids.
 * ------------------------------------------------------------------------ */

static bool
pgduckdb_is_unresolved_type(Oid type_oid) {
	return type_oid == pgduckdb::DuckdbUnresolvedTypeOid();
}

static bool
pgduckdb_is_duckdb_row(Oid type_oid) {
	return type_oid == pgduckdb::DuckdbRowOid();
}

static bool
pgduckdb_is_fake_type(Oid type_oid) {
	if (pgduckdb_is_unresolved_type(type_oid)) return true;
	if (pgduckdb_is_duckdb_row(type_oid)) return true;
	if (pgduckdb::DuckdbJsonOid() == type_oid) return true;
	return false;
}

static bool
pgduckdb_is_duckdb_subscript_type(Oid type_oid) {
	if (pgduckdb_is_unresolved_type(type_oid)) return true;
	if (pgduckdb_is_duckdb_row(type_oid)) return true;
	if (pgduckdb::DuckdbStructOid() == type_oid) return true;
	if (pgduckdb::DuckdbMapOid() == type_oid) return true;
	return false;
}

/* ------------------------------------------------------------------------
 * Prev-hook slots and hook impls. Each impl checks the pg_duckdb case
 * first and falls through to prev_hook (or sensible default) otherwise.
 * ------------------------------------------------------------------------ */

static pgddb_function_name_hook_t prev_pgddb_function_name_hook = nullptr;
static pgddb_is_fake_type_hook_t prev_pgddb_is_fake_type_hook = nullptr;
static pgddb_var_is_row_hook_t prev_pgddb_var_is_row_hook = nullptr;
static pgddb_subscript_var_hook_t prev_pgddb_subscript_var_hook = nullptr;
static pgddb_func_returns_row_hook_t prev_pgddb_func_returns_row_hook = nullptr;
static pgddb_replace_subquery_with_view_hook_t prev_pgddb_replace_subquery_with_view_hook = nullptr;
static pgddb_show_type_hook_t prev_pgddb_show_type_hook = nullptr;
static pgddb_reconstruct_star_step_hook_t prev_pgddb_reconstruct_star_step_hook = nullptr;
static pgddb_strip_first_subscript_hook_t prev_pgddb_strip_first_subscript_hook = nullptr;
static pgddb_subscript_has_custom_alias_hook_t prev_pgddb_subscript_has_custom_alias_hook = nullptr;
static pgddb_write_row_refname_hook_t prev_pgddb_write_row_refname_hook = nullptr;
static pgddb_db_and_schema_hook_t prev_pgddb_db_and_schema_hook = nullptr;

static char *
pgduckdb_function_name(Oid function_oid, bool *use_variadic_p) {
	if (!pgduckdb::IsDuckdbOnlyFunction(function_oid)) {
		return prev_pgddb_function_name_hook ? prev_pgddb_function_name_hook(function_oid, use_variadic_p) : nullptr;
	}

	/* DuckDB doesn't support variadic functions; just set the flag false. */
	if (use_variadic_p) {
		*use_variadic_p = false;
	}

	auto func_name = get_func_name(function_oid);
	return psprintf("system.main.%s", quote_identifier(func_name));
}

static bool
pgduckdb_is_fake_type_impl(Oid type_oid) {
	if (pgduckdb_is_fake_type(type_oid)) return true;
	return prev_pgddb_is_fake_type_hook ? prev_pgddb_is_fake_type_hook(type_oid) : false;
}

static bool
pgduckdb_var_is_duckdb_row(Var *var) {
	if (var && pgduckdb_is_duckdb_row(var->vartype)) return true;
	return prev_pgddb_var_is_row_hook ? prev_pgddb_var_is_row_hook(var) : false;
}

static bool
pgduckdb_func_returns_duckdb_row(RangeTblFunction *rtfunc) {
	if (rtfunc && IsA(rtfunc->funcexpr, FuncExpr)) {
		FuncExpr *func_expr = castNode(FuncExpr, rtfunc->funcexpr);
		if (pgduckdb_is_duckdb_row(func_expr->funcresulttype)) return true;
	}
	return prev_pgddb_func_returns_row_hook ? prev_pgddb_func_returns_row_hook(rtfunc) : false;
}

static Var *
pgduckdb_duckdb_subscript_var(Expr *expr) {
	if (expr && IsA(expr, SubscriptingRef)) {
		SubscriptingRef *subscript = (SubscriptingRef *)expr;
		if (IsA(subscript->refexpr, Var)) {
			Var *refexpr = (Var *)subscript->refexpr;
			if (pgduckdb_is_duckdb_subscript_type(refexpr->vartype)) {
				return refexpr;
			}
		}
	}
	return prev_pgddb_subscript_var_hook ? prev_pgddb_subscript_var_hook(expr) : nullptr;
}

/*
 * Try to detect the start of a run of Vars in the target list that should be
 * reconstructed as a star (SELECT *). pg_duckdb expands a duckdb.row Var into
 * a sequence of Vars during planning; we put a star back in the deparsed SQL.
 */
static void
pgduckdb_check_for_star_start(StarReconstructionContext *ctx, ListCell *tle_cell) {
	TargetEntry *first_tle = (TargetEntry *)lfirst(tle_cell);

	if (!IsA(first_tle->expr, Var)) return;

	Var *first_var = (Var *)first_tle->expr;

	if (first_var->varattno != 1) return;

	int varno = first_var->varno;
	int varattno = first_var->varattno;

	do {
		TargetEntry *tle = (TargetEntry *)lfirst(tle_cell);

		if (!IsA(tle->expr, Var)) return;

		Var *var = (Var *)tle->expr;

		if (var->varno != varno) return;
		if (var->varattno != varattno) return;

		if (var && pgduckdb_is_duckdb_row(var->vartype)) {
			ctx->varno_star = varno;
			ctx->varattno_star = first_var->varattno;
			ctx->added_current_star = false;
			return;
		}

		varattno++;
	} while ((tle_cell = lnext(ctx->target_list, tle_cell)));
}

static bool
pgduckdb_reconstruct_star_step(StarReconstructionContext *ctx, ListCell *tle_cell) {
	pgduckdb_check_for_star_start(ctx, tle_cell);

	if (!ctx->varno_star) {
		return prev_pgddb_reconstruct_star_step_hook
		           ? prev_pgddb_reconstruct_star_step_hook(ctx, tle_cell)
		           : false;
	}

	TargetEntry *tle = (TargetEntry *)lfirst(tle_cell);

	if (tle->expr && IsA(tle->expr, Var)) {
		Var *var = castNode(Var, tle->expr);

		if (var->varno == ctx->varno_star && var->varattno == ctx->varattno_star) {
			ctx->varattno_star++;

			if (ctx->added_current_star) {
				return true;
			}

			if (!(var && pgduckdb_is_duckdb_row(var->vartype))) {
				return true;
			}

			ctx->added_current_star = true;
			return false;
		}
	}

	ctx->varno_star = 0;
	ctx->varattno_star = 0;
	ctx->added_current_star = false;

	return false;
}

static bool
pgduckdb_replace_subquery_with_view(Query *query, StringInfo buf) {
	FuncExpr *func_expr = pgduckdb::GetDuckdbViewExprFromQuery(query);
	if (!func_expr) {
		return prev_pgddb_replace_subquery_with_view_hook
		           ? prev_pgddb_replace_subquery_with_view_hook(query, buf)
		           : false;
	}

	int i = 0;
	foreach_ptr(Expr, expr, func_expr->args) {
		if (i >= 3) {
			break;
		}

		if (!IsA(expr, Const)) {
			elog(ERROR, "Expected only constant argument to the view function");
		}

		Const *const_val = castNode(Const, expr);
		if (const_val->consttype != TEXTOID) {
			elog(ERROR, "Expected text arguments to the view function, got type %s",
			     format_type_be(const_val->consttype));
		}

		if (const_val->constisnull) {
			elog(ERROR, "Expected non-NULL arguments to the view function");
		}

		if (i > 0) {
			appendStringInfoString(buf, ".");
		}
		appendStringInfoString(buf, quote_identifier(TextDatumGetCString(const_val->constvalue)));

		i++;
	}

	return true;
}

/*
 * Return -1 if the Const has a "fake" pg_duckdb type, so get_const_expr
 * never shows the type cast.
 */
static int
pgduckdb_show_type(Const *constval, int original_showtype) {
	if (pgduckdb_is_fake_type(constval->consttype)) {
		return -1;
	}
	return prev_pgddb_show_type_hook ? prev_pgddb_show_type_hook(constval, original_showtype) : original_showtype;
}

static bool
pgduckdb_subscript_has_custom_alias(Plan *plan, List *rtable, Var *subscript_var, char *colname) {
	int varno;
	int varattno;

	if (subscript_var->varnosyn > 0 && plan == NULL) {
		varno = subscript_var->varnosyn;
		varattno = subscript_var->varattnosyn;
	} else {
		varno = subscript_var->varno;
		varattno = subscript_var->varattno;
	}

	RangeTblEntry *rte = rt_fetch(varno, rtable);
	char *original_column = strVal(list_nth(rte->eref->colnames, varattno - 1));

	if (strcmp(original_column, colname) != 0) return true;
	return prev_pgddb_subscript_has_custom_alias_hook
	           ? prev_pgddb_subscript_has_custom_alias_hook(plan, rtable, subscript_var, colname)
	           : false;
}

/*
 * Rewrite r['mycolumn'] into r.mycolumn for subscript expressions on
 * duckdb.row Vars, so DuckDB produces nicer column names.
 */
static SubscriptingRef *
pgduckdb_strip_first_subscript(SubscriptingRef *sbsref, StringInfo buf) {
	if (!IsA(sbsref->refexpr, Var)) {
		return prev_pgddb_strip_first_subscript_hook
		           ? prev_pgddb_strip_first_subscript_hook(sbsref, buf)
		           : sbsref;
	}

	Var *refvar = (Var *)sbsref->refexpr;
	if (!pgduckdb_is_duckdb_row(refvar->vartype)) {
		return prev_pgddb_strip_first_subscript_hook
		           ? prev_pgddb_strip_first_subscript_hook(sbsref, buf)
		           : sbsref;
	}

	Assert(sbsref->refupperindexpr);
	Oid typoutput;
	bool typIsVarlena;
	Const *constval = castNode(Const, linitial(sbsref->refupperindexpr));
	getTypeOutputInfo(constval->consttype, &typoutput, &typIsVarlena);

	char *extval = OidOutputFunctionCall(typoutput, constval->constvalue);

	appendStringInfo(buf, ".%s", quote_identifier(extval));

	SubscriptingRef *shorter_sbsref = (SubscriptingRef *)copyObjectImpl(sbsref);
	shorter_sbsref->refupperindexpr = list_delete_first(shorter_sbsref->refupperindexpr);
	if (shorter_sbsref->reflowerindexpr) {
		shorter_sbsref->reflowerindexpr = list_delete_first(shorter_sbsref->reflowerindexpr);
	}
	return shorter_sbsref;
}

static char *
pgduckdb_write_row_refname(StringInfo buf, char *refname, bool is_top_level) {
	appendStringInfoString(buf, quote_identifier(refname));

	if (is_top_level) {
		/*
		 * duckdb.row at the top of a target list expands to r.* so DuckDB
		 * unpacks the STRUCT into all columns. NULL means "no attname".
		 */
		appendStringInfoString(buf, ".*");
		return NULL;
	}

	return refname;
}

/* ------------------------------------------------------------------------
 * db_and_schema policy: maps a PG schema name plus the relation's table-AM
 * name into a (duckdb_db, duckdb_schema) pair. pg_duckdb's policy:
 *   - "duckdb" table-AM and "public" schema -> default DB + "main"
 *   - "duckdb" table-AM and "pg_temp" -> "pg_temp" + "main"
 *   - "duckdb" table-AM and a "ddb$<db>[$<schema>]" schema -> MotherDuck routing
 *   - anything else with a non-NULL table-AM name -> table-AM name as DB
 *   - NULL table-AM name -> "pgduckdb" + schema name
 * ------------------------------------------------------------------------ */

extern "C" List *
pgduckdb_db_and_schema(const char *postgres_schema_name, const char *duckdb_table_am_name) {
	/*
	 * Libpgddb's pgddb_relation_name passes the actual AM name (e.g. "heap")
	 * here. The original DuckdbTableAmGetName only signaled the duckdb AM,
	 * so anything else (heap, btree, ...) routes to the default "pgduckdb"
	 * database + verbatim schema, same as a NULL AM name.
	 */
	if (duckdb_table_am_name == nullptr || strcmp("duckdb", duckdb_table_am_name) != 0) {
		return list_make2((void *)"pgduckdb", (void *)postgres_schema_name);
	}

	if (strcmp("pg_temp", postgres_schema_name) == 0) {
		return list_make2((void *)"pg_temp", (void *)"main");
	}

	if (strcmp("public", postgres_schema_name) == 0) {
		auto dbname = pgddb::DuckDBManager::Get().GetDefaultDBName().c_str();
		return list_make2((void *)dbname, (void *)"main");
	}

	if (!pgduckdb::IsMotherDuckEnabled()) {
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
		         errmsg("MotherDuck tables cannot be accessed without MotherDuck credentials"),
		         errhint("Configure MotherDuck credentials for this user using: CREATE USER MAPPING FOR CURRENT_USER "
		                 "SERVER motherduck OPTIONS (token '<your token>');")));
	}

	if (!pgddb::IsDuckdbSchemaName(postgres_schema_name)) {
		auto dbname = pgddb::DuckDBManager::Get().GetDefaultDBName().c_str();
		return list_make2((void *)dbname, (void *)postgres_schema_name);
	}

	StringInfoData db_name;
	StringInfoData schema_name;
	initStringInfo(&db_name);
	initStringInfo(&schema_name);
	const char *saveptr = &postgres_schema_name[4];
	const char *dollar;

	while ((dollar = strchr(saveptr, '$'))) {
		appendBinaryStringInfo(&db_name, saveptr, dollar - saveptr);
		saveptr = dollar + 1;
		if (saveptr[0] == '\0') {
			elog(ERROR, "Schema name is invalid");
		}

		if (saveptr[0] == '$') {
			appendStringInfoChar(&db_name, '$');
		} else {
			break;
		}
	}

	if (!dollar) {
		appendStringInfoString(&db_name, saveptr);
		return list_make2((void *)db_name.data, (char *)"main");
	}

	while ((dollar = strchr(saveptr, '$'))) {
		appendBinaryStringInfo(&schema_name, saveptr, dollar - saveptr);
		saveptr = dollar + 1;

		if (saveptr[0] == '$') {
			appendStringInfoChar(&schema_name, '$');
		} else {
			break;
		}
	}
	appendStringInfoString(&schema_name, saveptr);

	return list_make2(db_name.data, schema_name.data);
}

static List *
pgduckdb_db_and_schema_hook_impl(const char *postgres_schema_name, const char *duckdb_table_am_name) {
	return pgduckdb_db_and_schema(postgres_schema_name, duckdb_table_am_name);
}

/* ------------------------------------------------------------------------
 * DDL deparsers: CREATE TABLE / CREATE VIEW / ALTER TABLE / RENAME.
 * These have pg_duckdb-specific policy checks (MotherDuck owner, duckdb
 * table-AM) and are exposed via pgduckdb_ruleutils.hpp for pgduckdb_ddl.cpp.
 * ------------------------------------------------------------------------ */

/*
 * Take a raw CHECK constraint expression and convert it to a cooked format
 * ready for storage. Vendored from src/backend/catalog/heap.c.
 */
static Node *
cookConstraint(ParseState *pstate, Node *raw_constraint, char *relname) {
	Node *expr;

	expr = transformExpr(pstate, raw_constraint, EXPR_KIND_CHECK_CONSTRAINT);
	expr = coerce_to_boolean(pstate, expr, "CHECK");
	assign_expr_collations(pstate, expr);

	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		                errmsg("only table \"%s\" can be referenced in check constraint", relname)));

	return expr;
}

extern "C" char *
pgduckdb_get_tabledef(Oid relation_oid) {
	Relation relation = relation_open(relation_oid, AccessShareLock);
	const char *relation_name = pgddb_relation_name(relation_oid);
	const char *postgres_schema_name = get_namespace_name_or_temp(relation->rd_rel->relnamespace);
	const char *duckdb_table_am_name = pgduckdb::DuckdbTableAmGetName(relation->rd_tableam);
	const char *db_and_schema = pgddb_db_and_schema_string(postgres_schema_name, duckdb_table_am_name);

	StringInfoData buffer;
	initStringInfo(&buffer);

	if (relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		                errmsg("Using duckdb as a table access method on a partitioned table is not supported")));
	} else if (relation->rd_rel->relkind != RELKIND_RELATION) {
		elog(ERROR, "Only regular tables are supported in DuckDB");
	}

	if (relation->rd_rel->relispartition) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("DuckDB tables cannot be used as a partition")));
	}

	appendStringInfo(&buffer, "CREATE SCHEMA IF NOT EXISTS %s; ", db_and_schema);

	appendStringInfoString(&buffer, "CREATE ");

	if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
		// allowed
	} else if (relation->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT) {
		elog(ERROR, "Only TEMP and non-UNLOGGED tables are supported in DuckDB");
	} else if (relation->rd_rel->relowner != pgduckdb::MotherDuckPostgresUserOid()) {
		elog(ERROR, "MotherDuck tables must be owned by the duckb.postgres_role");
	}

	appendStringInfo(&buffer, "TABLE %s (", relation_name);

	if (list_length(RelationGetFKeyList(relation)) > 0) {
		elog(ERROR, "DuckDB tables do not support foreign keys");
	}

	List *relation_context = pgddb_deparse_context_for(relation_name, relation_oid);

	TupleDesc tuple_descriptor = RelationGetDescr(relation);
	TupleConstr *tuple_constraints = tuple_descriptor->constr;
	AttrDefault *default_value_list = tuple_constraints ? tuple_constraints->defval : NULL;

	bool first_column_printed = false;
	AttrNumber default_value_index = 0;
	for (int i = 0; i < tuple_descriptor->natts; i++) {
		Form_pg_attribute column = TupleDescAttr(tuple_descriptor, i);

		if (column->attisdropped) {
			continue;
		}

		const char *column_name = NameStr(column->attname);

		auto duck_type = pgddb::ConvertPostgresToDuckColumnType(column);
		pgddb::GetPostgresDuckDBType(duck_type, true);

		const char *column_type_name = format_type_with_typemod(column->atttypid, column->atttypmod);

		if (first_column_printed) {
			appendStringInfoString(&buffer, ", ");
		}
		first_column_printed = true;

		appendStringInfo(&buffer, "%s ", quote_identifier(column_name));
		appendStringInfoString(&buffer, column_type_name);

		if (column->attcompression) {
			elog(ERROR, "Column compression is not supported in DuckDB");
		}

		if (column->attidentity) {
			elog(ERROR, "Identity columns are not supported in DuckDB");
		}

		if (column->atthasdef) {
			Assert(tuple_constraints != NULL);
			Assert(default_value_list != NULL);

			AttrDefault *default_value = &(default_value_list[default_value_index]);
			default_value_index++;

			Assert(default_value->adnum == (i + 1));
			Assert(default_value_index <= tuple_constraints->num_defval);

			Node *default_node = (Node *)stringToNode(default_value->adbin);

			char *default_string = pgddb_deparse_expression(default_node, relation_context, false, false);

			if (!column->attgenerated) {
				appendStringInfo(&buffer, " DEFAULT %s", default_string);
			} else if (column->attgenerated == ATTRIBUTE_GENERATED_STORED) {
				elog(ERROR, "DuckDB does not support STORED generated columns");
			} else {
				elog(ERROR, "Unkown generated column type");
			}
		}

		if (column->attnotnull) {
			appendStringInfoString(&buffer, " NOT NULL");
		}

		Oid collation = column->attcollation;
		if (collation != InvalidOid && collation != DEFAULT_COLLATION_OID && !pgddb::pg::IsCLocale(collation)) {
			elog(ERROR, "DuckDB does not support column collations");
		}
	}

	AttrNumber constraint_count = tuple_constraints ? tuple_constraints->num_check : 0;
	ConstrCheck *check_constraint_list = tuple_constraints ? tuple_constraints->check : NULL;

	for (AttrNumber i = 0; i < constraint_count; i++) {
		ConstrCheck *check_constraint = &(check_constraint_list[i]);

		Node *check_node = (Node *)stringToNode(check_constraint->ccbin);

		char *check_string = pgddb_deparse_expression(check_node, relation_context, false, false);

		if (first_column_printed || i > 0) {
			appendStringInfoString(&buffer, ", ");
		}

		appendStringInfo(&buffer, "CONSTRAINT %s CHECK ", quote_identifier(check_constraint->ccname));

		appendStringInfoString(&buffer, "(");
		appendStringInfoString(&buffer, check_string);
		appendStringInfoString(&buffer, ")");
	}

	appendStringInfoString(&buffer, ")");

	if (!pgduckdb::IsDuckdbTableAm(relation->rd_tableam)) {
		elog(ERROR, "Only a table with the DuckDB can be stored in DuckDB, %d %d", relation->rd_rel->relam,
		     pgduckdb::DuckdbTableAmOid());
	}

	if (relation->rd_options) {
		elog(ERROR, "Storage options are not supported in DuckDB");
	}

	relation_close(relation, AccessShareLock);

	return buffer.data;
}

extern "C" char *
pgduckdb_get_viewdef(const ViewStmt *stmt, const char *postgres_schema_name, const char *view_name,
                     const char *duckdb_query_string) {
	StringInfoData buffer;
	initStringInfo(&buffer);

	const char *db_and_schema = pgddb_db_and_schema_string(postgres_schema_name, "duckdb");
	appendStringInfo(&buffer, "CREATE SCHEMA IF NOT EXISTS %s; ", db_and_schema);

	appendStringInfoString(&buffer, "CREATE ");
	if (stmt->replace) {
		appendStringInfoString(&buffer, "OR REPLACE ");
	}
	appendStringInfo(&buffer, "VIEW %s.%s", db_and_schema, quote_identifier(view_name));
	if (stmt->aliases) {
		appendStringInfoChar(&buffer, '(');
		bool first = true;
#if PG_VERSION_NUM >= 150000
		foreach_node(String, alias, stmt->aliases) {
#else
		foreach_ptr(Value, alias, stmt->aliases) {
#endif
			if (!first) {
				appendStringInfoString(&buffer, ", ");
			} else {
				first = false;
			}

			appendStringInfoString(&buffer, quote_identifier(strVal(alias)));
		}
		appendStringInfoChar(&buffer, ')');
	}
	appendStringInfo(&buffer, " AS %s;", duckdb_query_string);
	return buffer.data;
}

extern "C" char *
pgduckdb_get_rename_relationdef(Oid relation_oid, RenameStmt *rename_stmt) {
	if (rename_stmt->renameType != OBJECT_TABLE && rename_stmt->renameType != OBJECT_VIEW &&
	    rename_stmt->renameType != OBJECT_COLUMN) {
		elog(ERROR, "Only renaming tables and columns is supported in DuckDB");
	}

	Relation relation = relation_open(relation_oid, AccessShareLock);
	Assert(pgduckdb::IsDuckdbTable(relation) || pgduckdb::IsMotherDuckView(relation));

	const char *postgres_schema_name = get_namespace_name_or_temp(relation->rd_rel->relnamespace);
	const char *db_and_schema = pgddb_db_and_schema_string(postgres_schema_name, "duckdb");
	const char *old_table_name = psprintf("%s.%s", db_and_schema, quote_identifier(rename_stmt->relation->relname));

	const char *relation_type = "TABLE";
	if (relation->rd_rel->relkind == RELKIND_VIEW) {
		relation_type = "VIEW";
	}

	StringInfoData buffer;
	initStringInfo(&buffer);

	if (rename_stmt->subname) {
		appendStringInfo(&buffer, "ALTER %s %s RENAME COLUMN %s TO %s;", relation_type, old_table_name,
		                 quote_identifier(rename_stmt->subname), quote_identifier(rename_stmt->newname));

	} else {
		appendStringInfo(&buffer, "ALTER %s %s RENAME TO %s;", relation_type, old_table_name,
		                 quote_identifier(rename_stmt->newname));
	}

	relation_close(relation, AccessShareLock);

	return buffer.data;
}

extern "C" char *
pgduckdb_get_alter_tabledef(Oid relation_oid, AlterTableStmt *alter_stmt) {
	Relation relation = relation_open(relation_oid, AccessShareLock);
	const char *relation_name = pgddb_relation_name(relation_oid);

	StringInfoData buffer;
	initStringInfo(&buffer);

	if (get_rel_relkind(relation_oid) != RELKIND_RELATION) {
		elog(ERROR, "Only regular tables are supported in DuckDB");
	}

	if (list_length(RelationGetFKeyList(relation)) > 0) {
		elog(ERROR, "DuckDB tables do not support foreign keys");
	}

	List *relation_context = pgddb_deparse_context_for(relation_name, relation_oid);
	ParseState *pstate = make_parsestate(NULL);
	ParseNamespaceItem *nsitem = addRangeTableEntryForRelation(pstate, relation, AccessShareLock, NULL, false, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	foreach_node(AlterTableCmd, cmd, alter_stmt->cmds) {
		/*
		 * DuckDB doesn't support multiple ALTER TABLE commands in one statement,
		 * so we split them up.
		 */
		appendStringInfo(&buffer, "ALTER TABLE %s ", relation_name);

		switch (cmd->subtype) {
		case AT_AddColumn: {
			ColumnDef *col = castNode(ColumnDef, cmd->def);
			TupleDesc tupdesc = BuildDescForRelation(list_make1(col));
			Form_pg_attribute attribute = TupleDescAttr(tupdesc, 0);
			const char *column_fq_type = format_type_with_typemod(attribute->atttypid, attribute->atttypmod);

			appendStringInfo(&buffer, "ADD COLUMN %s %s", quote_identifier(col->colname), column_fq_type);
			foreach_node(Constraint, constraint, col->constraints) {
				switch (constraint->contype) {
				case CONSTR_NULL: {
					appendStringInfoString(&buffer, " NULL");
					break;
				}
				case CONSTR_NOTNULL: {
					appendStringInfoString(&buffer, " NOT NULL");
					break;
				}
				case CONSTR_DEFAULT: {
					if (constraint->raw_expr) {
						auto expr = cookDefault(pstate, constraint->raw_expr, attribute->atttypid, attribute->atttypmod,
						                        col->colname, attribute->attgenerated);
						char *default_string = pgddb_deparse_expression(expr, relation_context, false, false);
						appendStringInfo(&buffer, " DEFAULT %s", default_string);
					}
					break;
				}
				case CONSTR_CHECK: {
					appendStringInfo(&buffer, "CHECK ");

					auto expr = cookConstraint(pstate, constraint->raw_expr, RelationGetRelationName(relation));

					char *check_string = pgddb_deparse_expression(expr, relation_context, false, false);

					appendStringInfo(&buffer, "(%s); ", check_string);
					break;
				}
				case CONSTR_PRIMARY: {
					appendStringInfoString(&buffer, " PRIMARY KEY");
					break;
				}
				case CONSTR_UNIQUE: {
					appendStringInfoString(&buffer, " UNIQUE");
					break;
				}
				default:
					elog(ERROR, "pg_duckdb does not support this ALTER TABLE yet");
				}
			}

			if (col->collClause || col->collOid != InvalidOid) {
				elog(ERROR, "Column collations are not supported in DuckDB");
			}

			appendStringInfoString(&buffer, "; ");
			break;
		}

		case AT_AlterColumnType: {
			const char *column_name = cmd->name;
			ColumnDef *col = castNode(ColumnDef, cmd->def);
			TupleDesc tupdesc = BuildDescForRelation(list_make1(col));
			Form_pg_attribute attribute = TupleDescAttr(tupdesc, 0);
			const char *column_fq_type = format_type_with_typemod(attribute->atttypid, attribute->atttypmod);

			appendStringInfo(&buffer, "ALTER COLUMN %s TYPE %s; ", quote_identifier(column_name), column_fq_type);
			break;
		}

		case AT_DropColumn: {
			appendStringInfo(&buffer, "DROP COLUMN %s", quote_identifier(cmd->name));

			if (cmd->behavior == DROP_CASCADE) {
				appendStringInfoString(&buffer, " CASCADE");
			} else if (cmd->behavior == DROP_RESTRICT) {
				appendStringInfoString(&buffer, " RESTRICT");
			}

			appendStringInfoString(&buffer, "; ");
			break;
		}

		case AT_ColumnDefault: {
			const char *column_name = cmd->name;
			TupleDesc tupdesc = RelationGetDescr(relation);
			Form_pg_attribute attribute = pgddb::pg::GetAttributeByName(tupdesc, column_name);
			if (!attribute) {
				elog(ERROR, "Column %s not found in table %s", column_name, relation_name);
			}

			appendStringInfo(&buffer, "ALTER COLUMN %s ", quote_identifier(cmd->name));

			if (cmd->def) {
				auto expr = cookDefault(pstate, cmd->def, attribute->atttypid, attribute->atttypmod, column_name,
				                        attribute->attgenerated);
				char *default_string = pgddb_deparse_expression(expr, relation_context, false, false);
				appendStringInfo(&buffer, "SET DEFAULT %s; ", default_string);
			} else {
				appendStringInfoString(&buffer, "DROP DEFAULT; ");
			}
			break;
		}

		case AT_DropNotNull: {
			appendStringInfo(&buffer, "ALTER COLUMN %s DROP NOT NULL; ", quote_identifier(cmd->name));
			break;
		}

		case AT_SetNotNull: {
			appendStringInfo(&buffer, "ALTER COLUMN %s SET NOT NULL; ", quote_identifier(cmd->name));
			break;
		}

		case AT_AddConstraint: {
			Constraint *constraint = castNode(Constraint, cmd->def);

			appendStringInfoString(&buffer, "ADD ");

			switch (constraint->contype) {
			case CONSTR_CHECK: {
				appendStringInfo(&buffer, "CONSTRAINT %s CHECK ",
				                 quote_identifier(constraint->conname ? constraint->conname : ""));

				auto expr = cookConstraint(pstate, constraint->raw_expr, RelationGetRelationName(relation));

				char *check_string = pgddb_deparse_expression(expr, relation_context, false, false);

				appendStringInfo(&buffer, "(%s); ", check_string);
				break;
			}

			case CONSTR_PRIMARY: {
				appendStringInfoString(&buffer, "PRIMARY KEY (");
				ListCell *cell;
				bool first = true;
				foreach (cell, constraint->keys) {
					char *key = strVal(lfirst(cell));
					if (!first) {
						appendStringInfoString(&buffer, ", ");
					}
					appendStringInfoString(&buffer, quote_identifier(key));
					first = false;
				}
				appendStringInfoString(&buffer, "); ");
				break;
			}

			case CONSTR_UNIQUE: {
				appendStringInfoString(&buffer, "UNIQUE (");
				ListCell *ucell;
				bool ufirst = true;
				foreach (ucell, constraint->keys) {
					char *key = strVal(lfirst(ucell));
					if (!ufirst) {
						appendStringInfoString(&buffer, ", ");
					}
					appendStringInfoString(&buffer, quote_identifier(key));
					ufirst = false;
				}
				appendStringInfoString(&buffer, "); ");
				break;
			}

			default: {
				elog(ERROR, "DuckDB does not support this constraint type");
				break;
			}
			}
			break;
		}

		case AT_DropConstraint: {
			appendStringInfo(&buffer, "DROP CONSTRAINT %s", quote_identifier(cmd->name));

			if (cmd->behavior == DROP_CASCADE) {
				appendStringInfoString(&buffer, " CASCADE");
			} else if (cmd->behavior == DROP_RESTRICT) {
				appendStringInfoString(&buffer, " RESTRICT");
			}

			appendStringInfoString(&buffer, "; ");
			break;
		}

		case AT_SetRelOptions:
		case AT_ResetRelOptions: {
			List *options = (List *)cmd->def;
			bool is_set = (cmd->subtype == AT_SetRelOptions);

			if (is_set) {
				appendStringInfoString(&buffer, "SET (");
			} else {
				appendStringInfoString(&buffer, "RESET (");
			}

			ListCell *cell;
			bool first = true;
			foreach (cell, options) {
				DefElem *def = (DefElem *)lfirst(cell);
				if (!first) {
					appendStringInfoString(&buffer, ", ");
				}

				appendStringInfoString(&buffer, quote_identifier(def->defname));

				if (is_set && def->arg) {
					char *val = NULL;
					if (IsA(def->arg, String)) {
						val = strVal(def->arg);
						appendStringInfo(&buffer, " = %s", quote_literal_cstr(val));
					} else if (IsA(def->arg, Integer)) {
						val = psprintf("%d", intVal(def->arg));
						appendStringInfo(&buffer, " = %s", val);
					} else {
						elog(ERROR, "Unsupported option value type");
					}
				}

				first = false;
			}

			appendStringInfoString(&buffer, "); ");
			break;
		}

		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			                errmsg("DuckDB does not support this ALTER TABLE command")));
		}
	}
	relation_close(relation, AccessShareLock);

	return buffer.data;
}

/* ------------------------------------------------------------------------
 * Hook registration.
 * ------------------------------------------------------------------------ */

namespace pgduckdb {

void
InitRuleutilsHooks() {
	prev_pgddb_function_name_hook = pgddb_function_name_hook;
	pgddb_function_name_hook = pgduckdb_function_name;

	prev_pgddb_is_fake_type_hook = pgddb_is_fake_type_hook;
	pgddb_is_fake_type_hook = pgduckdb_is_fake_type_impl;

	prev_pgddb_var_is_row_hook = pgddb_var_is_row_hook;
	pgddb_var_is_row_hook = pgduckdb_var_is_duckdb_row;

	prev_pgddb_subscript_var_hook = pgddb_subscript_var_hook;
	pgddb_subscript_var_hook = pgduckdb_duckdb_subscript_var;

	prev_pgddb_func_returns_row_hook = pgddb_func_returns_row_hook;
	pgddb_func_returns_row_hook = pgduckdb_func_returns_duckdb_row;

	prev_pgddb_replace_subquery_with_view_hook = pgddb_replace_subquery_with_view_hook;
	pgddb_replace_subquery_with_view_hook = pgduckdb_replace_subquery_with_view;

	prev_pgddb_show_type_hook = pgddb_show_type_hook;
	pgddb_show_type_hook = pgduckdb_show_type;

	prev_pgddb_reconstruct_star_step_hook = pgddb_reconstruct_star_step_hook;
	pgddb_reconstruct_star_step_hook = pgduckdb_reconstruct_star_step;

	prev_pgddb_strip_first_subscript_hook = pgddb_strip_first_subscript_hook;
	pgddb_strip_first_subscript_hook = pgduckdb_strip_first_subscript;

	prev_pgddb_subscript_has_custom_alias_hook = pgddb_subscript_has_custom_alias_hook;
	pgddb_subscript_has_custom_alias_hook = pgduckdb_subscript_has_custom_alias;

	prev_pgddb_write_row_refname_hook = pgddb_write_row_refname_hook;
	pgddb_write_row_refname_hook = pgduckdb_write_row_refname;

	prev_pgddb_db_and_schema_hook = pgddb_db_and_schema_hook;
	pgddb_db_and_schema_hook = pgduckdb_db_and_schema_hook_impl;
}

} // namespace pgduckdb
