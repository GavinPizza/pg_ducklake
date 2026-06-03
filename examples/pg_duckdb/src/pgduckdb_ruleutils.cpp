#include "duckdb.hpp"
#include "pgddb/pg/string_utils.hpp"
#include "pgddb/pgddb_table_am.hpp"
#include "pgddb/pgddb_types.hpp"
#include "pgduckdb/pgduckdb_ddl.hpp"
#include "pgduckdb/pgduckdb_duckdb.hpp"
#include "pgddb/pg/relations.hpp"
#include "pgddb/pg/locale.hpp"

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/heap.h"
#include "catalog/pg_collation.h"
#include "commands/dbcommands.h"
#include "commands/tablecmds.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "lib/stringinfo.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "nodes/print.h"
#include "utils/rls.h"
#include "utils/syscache.h"
#include "storage/lockdefs.h"

#include "pgddb/vendor/pg_ruleutils.h"
#include "pgddb/pgddb_ruleutils.h"
#include "pgddb/vendor/pg_list.hpp"
}

#include "pgduckdb/pgduckdb.h"
#include "pgduckdb/pgduckdb_table_am.hpp"
#include "pgddb/pgddb_duckdb.hpp"
#include "pgduckdb/pgduckdb_metadata_cache.hpp"
#include "pgduckdb/pgduckdb_userdata_cache.hpp"
#include "pgduckdb/pgduckdb_ruleutils.hpp"

extern "C" {

static char *
pgduckdb_function_name(Oid function_oid, bool *use_variadic_p) {
	if (!pgduckdb::IsDuckdbOnlyFunction(function_oid)) {
		return nullptr;
	}

	/*
	 * DuckDB currently doesn't support variadic functions, so we can just
	 * always set this pointer to false.
	 */
	if (use_variadic_p) {
		*use_variadic_p = false;
	}

	auto func_name = get_func_name(function_oid);
	return psprintf("system.main.%s", quote_identifier(func_name));
}

static bool
pgduckdb_is_unresolved_type(Oid type_oid) {
	return type_oid == pgduckdb::DuckdbUnresolvedTypeOid();
}

static bool
pgduckdb_is_duckdb_row(Oid type_oid) {
	return type_oid == pgduckdb::DuckdbRowOid();
}

/*
 * We never want to show some of our unresolved types in the DuckDB query.
 * These types only exist to make the Postgres parser and its type resolution
 * happy. DuckDB can simply figure out the correct type itself without an
 * explicit cast.
 */
static bool
pgduckdb_is_fake_type(Oid type_oid) {
	if (pgduckdb_is_unresolved_type(type_oid)) {
		return true;
	}

	if (pgduckdb_is_duckdb_row(type_oid)) {
		return true;
	}

	if (pgduckdb::DuckdbJsonOid() == type_oid) {
		return true;
	}

	return false;
}

static bool
pgduckdb_is_duckdb_subscript_type(Oid type_oid) {
	if (pgduckdb_is_unresolved_type(type_oid)) {
		return true;
	}

	if (pgduckdb_is_duckdb_row(type_oid)) {
		return true;
	}

	if (pgduckdb::DuckdbStructOid() == type_oid) {
		return true;
	}

	if (pgduckdb::DuckdbMapOid() == type_oid) {
		return true;
	}

	return false;
}

static bool
pgduckdb_var_is_duckdb_row(Var *var) {
	if (!var) {
		return false;
	}
	return pgduckdb_is_duckdb_row(var->vartype);
}

static bool
pgduckdb_func_returns_duckdb_row(RangeTblFunction *rtfunc) {
	if (!rtfunc) {
		return false;
	}

	if (!IsA(rtfunc->funcexpr, FuncExpr)) {
		return false;
	}

	FuncExpr *func_expr = castNode(FuncExpr, rtfunc->funcexpr);

	return pgduckdb_is_duckdb_row(func_expr->funcresulttype);
}

/*
 * Returns NULL if the expression is a subscript on a duckdb specific type.
 * Returns the Var of the duckdb row if it is.
 */
static Var *
pgduckdb_duckdb_subscript_var(Expr *expr) {
	if (!expr) {
		return NULL;
	}

	if (!IsA(expr, SubscriptingRef)) {
		return NULL;
	}

	SubscriptingRef *subscript = (SubscriptingRef *)expr;

	if (!IsA(subscript->refexpr, Var)) {
		return NULL;
	}

	Var *refexpr = (Var *)subscript->refexpr;

	if (!pgduckdb_is_duckdb_subscript_type(refexpr->vartype)) {
		return NULL;
	}

	return refexpr;
}

/*
 * pgduckdb_check_for_star_start tries to figure out if this is tle_cell
 * contains a Var that is the start of a run of Vars that should be
 * reconstructed as a star. If that's the case it sets the varno_star and
 * varattno_star of the ctx.
 */
static void
pgduckdb_check_for_star_start(StarReconstructionContext *ctx, ListCell *tle_cell) {
	TargetEntry *first_tle = (TargetEntry *)lfirst(tle_cell);

	if (!IsA(first_tle->expr, Var)) {
		/* Not a Var so we're not at the start of a run of Vars. */
		return;
	}

	Var *first_var = (Var *)first_tle->expr;

	if (first_var->varattno != 1) {
		/* If we don't have varattno 1, then we are not at a run of Vars */
		return;
	}

	/*
	 * We found a Var that could potentially be the first of a run of Vars for
	 * which we have to reconstruct the star. To check if this is indeed the
	 * case we see if we can find a duckdb.row in this list of Vars.
	 */
	int varno = first_var->varno;
	int varattno = first_var->varattno;

	do {
		TargetEntry *tle = (TargetEntry *)lfirst(tle_cell);

		if (!IsA(tle->expr, Var)) {
			/*
			 * We found the end of this run of Vars, by finding something else
			 * than a Var.
			 */
			return;
		}

		Var *var = (Var *)tle->expr;

		if (var->varno != varno) {
			/* A Var from a different RTE */
			return;
		}

		if (var->varattno != varattno) {
			/* Not a consecutive Var */
			return;
		}
		if (pgduckdb_var_is_duckdb_row(var)) {
			/*
			 * If we have a duckdb.row, then we found a run of Vars that we
			 * have to reconstruct the star for.
			 */

			ctx->varno_star = varno;
			ctx->varattno_star = first_var->varattno;
			ctx->added_current_star = false;
			return;
		}

		/* Look for the next Var in the run */
		varattno++;
	} while ((tle_cell = lnext(ctx->target_list, tle_cell)));
}

/*
 * In our DuckDB queries we sometimes want to use "SELECT *", when selecting
 * from a function like read_parquet. That way DuckDB can figure out the actual
 * columns that it should return. Sadly Postgres expands the * character from
 * the original query to a list of columns. So we need to put a star, any time
 * we want to replace duckdb.row columns with a "*" in the duckdb query.
 *
 * Since the original "*" might expand to many columns we need to remove all of
 * those, when putting a "*" back. To do so we try to find a runs of Vars from
 * the same FROM entry, aka RangeTableEntry (RTE) that we expect were created
 * with a *.
 *
 * This function returns true if we should skip writing this tle_cell to the
 * DuckDB query because it is part of a run of Vars that will be reconstructed
 * as a star.
 */
static bool
pgduckdb_reconstruct_star_step(StarReconstructionContext *ctx, ListCell *tle_cell) {
	/* Detect start of a Var run that should be reconstructed to a star */
	pgduckdb_check_for_star_start(ctx, tle_cell);

	/*
	 * If we're not currently reconstructing a star we don't need to do
	 * anything.
	 */
	if (!ctx->varno_star) {
		return false;
	}

	TargetEntry *tle = (TargetEntry *)lfirst(tle_cell);

	/*
	 * Find out if this target entry is the next element in the run of Vars for
	 * the star we're currently reconstructing.
	 */
	if (tle->expr && IsA(tle->expr, Var)) {
		Var *var = castNode(Var, tle->expr);

		if (var->varno == ctx->varno_star && var->varattno == ctx->varattno_star) {
			/*
			 * We're still in the run of Vars, increment the varattno to look
			 * for the next Var on the next call.
			 */
			ctx->varattno_star++;

			/* If we already added star we skip writing this target entry */
			if (ctx->added_current_star) {
				return true;
			}

			/*
			 * If it's not a duckdb row we skip this target entry too. The way
			 * we add a single star is by expanding the first duckdb.row torget
			 * entry, which we've defined to expand to a star. So we need to
			 * skip any non duckdb.row Vars that precede the first duckdb.row.
			 */
			if (!pgduckdb_var_is_duckdb_row(var)) {
				return true;
			}

			ctx->added_current_star = true;
			return false;
		}
	}

	/*
	 * If it was not, that means we've successfully expanded this star and we
	 * should start looking for the next star start. So reset all the state
	 * used for this star reconstruction.
	 */
	ctx->varno_star = 0;
	ctx->varattno_star = 0;
	ctx->added_current_star = false;

	return false;
}

static bool
pgduckdb_replace_subquery_with_view(Query *query, StringInfo buf) {
	FuncExpr *func_expr = pgduckdb::GetDuckdbViewExprFromQuery(query);
	if (!func_expr) {
		/* Not a duckdb.view query, so we don't need to do anything */
		return false;
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
 * A wrapper around pgduckdb_is_fake_type that returns -1 if the type of the
 * Const is fake, because that's the type of value that get_const_expr requires
 * in its showtype variable to never show the type.
 */
static int
pgduckdb_show_type(Const *constval, int original_showtype) {
	if (pgduckdb_is_fake_type(constval->consttype)) {
		return -1;
	}
	return original_showtype;
}

static bool
pgduckdb_subscript_has_custom_alias(Plan *plan, List *rtable, Var *subscript_var, char *colname) {
	/* The first bit of this logic is taken from get_variable() */
	int varno;
	int varattno;

	/*
	 * If we have a syntactic referent for the Var, and we're working from a
	 * parse tree, prefer to use the syntactic referent.  Otherwise, fall back
	 * on the semantic referent.  (See comments in get_variable().)
	 */
	if (subscript_var->varnosyn > 0 && plan == NULL) {
		varno = subscript_var->varnosyn;
		varattno = subscript_var->varattnosyn;
	} else {
		varno = subscript_var->varno;
		varattno = subscript_var->varattno;
	}

	RangeTblEntry *rte = rt_fetch(varno, rtable);

	/* Custom code starts here */
	char *original_column = strVal(list_nth(rte->eref->colnames, varattno - 1));

	return strcmp(original_column, colname) != 0;
}

/*
 * Subscript expressions that index into the duckdb.row type need to be changed
 * to regular column references in the DuckDB query. The main reason we do this
 * is so that DuckDB generates nicer column names, i.e. without the square
 * brackets: "mycolumn" instead of "r['mycolumn']"
 */
static SubscriptingRef *
pgduckdb_strip_first_subscript(SubscriptingRef *sbsref, StringInfo buf) {
	if (!IsA(sbsref->refexpr, Var)) {
		return sbsref;
	}

	if (!pgduckdb_var_is_duckdb_row((Var *)sbsref->refexpr)) {
		return sbsref;
	}

	Assert(sbsref->refupperindexpr);
	Oid typoutput;
	bool typIsVarlena;
	Const *constval = castNode(Const, linitial(sbsref->refupperindexpr));
	getTypeOutputInfo(constval->consttype, &typoutput, &typIsVarlena);

	char *extval = OidOutputFunctionCall(typoutput, constval->constvalue);

	appendStringInfo(buf, ".%s", quote_identifier(extval));

	/*
	 * If there are any additional subscript expressions we should output them.
	 * Subscripts can be used in duckdb to index into arrays or json objects.
	 * It's fine if this results in an empty List, because printSubscripts
	 * handles that case correctly.
	 */
	SubscriptingRef *shorter_sbsref = (SubscriptingRef *)copyObjectImpl(sbsref);
	/* strip the first subscript from the list */
	shorter_sbsref->refupperindexpr = list_delete_first(shorter_sbsref->refupperindexpr);
	if (shorter_sbsref->reflowerindexpr) {
		shorter_sbsref->reflowerindexpr = list_delete_first(shorter_sbsref->reflowerindexpr);
	}
	return shorter_sbsref;
}

/*
 * db.schema resolution. Object-scoped: claims only relations on the "duckdb"
 * table AM and returns NULL otherwise, so the kernel falls back to its
 * "pgduckdb" storage catalog for heap/view/etc. (Row-reference `.*` expansion is
 * now handled generically by the kernel, so pg_duckdb no longer needs a
 * write_row_refname hook.)
 *
 * Returns a list of two elements: the DuckDB database name and the duckdb schema
 * name. These are not escaped yet.
 */
List *
pgduckdb_db_and_schema(const char *postgres_schema_name, const char *duckdb_table_am_name) {
	if (duckdb_table_am_name == nullptr || strcmp("duckdb", duckdb_table_am_name) != 0) {
		return nullptr; // not a duckdb-AM table; kernel falls back to the "pgduckdb" catalog
	}

	if (strcmp("pg_temp", postgres_schema_name) == 0) {
		return list_make2((void *)"pg_temp", (void *)"main");
	}

	if (strcmp("public", postgres_schema_name) == 0) {
		/* Use the "main" schema in DuckDB for tables in the public schema in Postgres */
		auto dbname = pgduckdb::DuckDBManager::Get().GetDefaultDBName().c_str();
		return list_make2((void *)dbname, (void *)"main");
	}

	/* These are MotherDuck tables, so we need credentials to access them */
	if (!pgduckdb::IsMotherDuckEnabled()) {
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
		         errmsg("MotherDuck tables cannot be accessed without MotherDuck credentials"),
		         errhint("Configure MotherDuck credentials for this user using: CREATE USER MAPPING FOR CURRENT_USER "
		                 "SERVER motherduck OPTIONS (token '<your token>');")));
	}

	if (!pgddb::IsDuckdbSchemaName(postgres_schema_name)) {
		auto dbname = pgduckdb::DuckDBManager::Get().GetDefaultDBName().c_str();
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

char *
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

}

namespace pgduckdb {

/*
 * Ruleutils::validate_create_table override: replicates pg_duckdb's
 * persistence / ownership policy from the upstream pgduckdb_get_tabledef body.
 * TEMP tables are allowed; PERMANENT tables must be owned by the MotherDuck
 * postgres role; UNLOGGED tables are rejected. Invoked by the kernel's
 * DuckdbRuleutils::get_tabledef just before the CREATE TABLE is generated.
 */
void
Ruleutils::validate_create_table(Relation relation) {
	if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
		return;
	}
	if (relation->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT) {
		elog(ERROR, "Only TEMP and non-UNLOGGED tables are supported in DuckDB");
	}
	if (relation->rd_rel->relowner != MotherDuckPostgresUserOid()) {
		elog(ERROR, "MotherDuck tables must be owned by the duckb.postgres_role");
	}
}

void
InitRuleutilsHooks() {
	Register_pgddb_function_name(pgduckdb_function_name);
	Register_pgddb_is_fake_type(pgduckdb_is_fake_type);
	Register_pgddb_var_is_duckdb_row(pgduckdb_var_is_duckdb_row);
	Register_pgddb_duckdb_subscript_var(pgduckdb_duckdb_subscript_var);
	Register_pgddb_func_returns_duckdb_row(pgduckdb_func_returns_duckdb_row);
	Register_pgddb_replace_subquery_with_view(pgduckdb_replace_subquery_with_view);
	Register_pgddb_show_type(pgduckdb_show_type);
	Register_pgddb_reconstruct_star_step(pgduckdb_reconstruct_star_step);
	Register_pgddb_strip_first_subscript(pgduckdb_strip_first_subscript);
	Register_pgddb_subscript_has_custom_alias(pgduckdb_subscript_has_custom_alias);
	// Row-refname `.*` expansion is handled generically by the kernel (no hook).
	Register_pgddb_db_and_schema(pgduckdb_db_and_schema); // object-scoped: the "duckdb" table AM
	// Heap/view/etc fall back to the kernel's "pgduckdb" storage catalog.
	// CREATE TABLE validation is a DuckdbRuleutils virtual override (Ruleutils),
	// not a registration hook -- the consumer invokes get_tabledef directly.
}

} // namespace pgduckdb
