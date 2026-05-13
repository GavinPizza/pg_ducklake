#include "duckdb.hpp"

#include "pgddb/pg/string_utils.hpp"

extern "C" {
#include "postgres.h"
#include "pgddb/pgddb_ruleutils.h"

#include "access/relation.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "pgddb/vendor/pg_ruleutils.h"
#include "pgddb/vendor/pg_list.hpp"
}

extern "C" {

bool outermost_query = true;

/*
 * Hook globals. Default NULL; consumers install impls via their _PG_init
 * (e.g. pgduckdb::InitRuleutilsHooks()). The thin null-check wrappers in
 * include/pgddb/pgddb_ruleutils.h fall through to a sensible default
 * (false/NULL/unchanged input) when no consumer is registered.
 */
pgddb_function_name_hook_t pgddb_function_name_hook = nullptr;
pgddb_is_fake_type_hook_t pgddb_is_fake_type_hook = nullptr;
pgddb_var_is_row_hook_t pgddb_var_is_row_hook = nullptr;
pgddb_subscript_var_hook_t pgddb_subscript_var_hook = nullptr;
pgddb_func_returns_row_hook_t pgddb_func_returns_row_hook = nullptr;
pgddb_replace_subquery_with_view_hook_t pgddb_replace_subquery_with_view_hook = nullptr;
pgddb_show_type_hook_t pgddb_show_type_hook = nullptr;
pgddb_reconstruct_star_step_hook_t pgddb_reconstruct_star_step_hook = nullptr;
pgddb_strip_first_subscript_hook_t pgddb_strip_first_subscript_hook = nullptr;
pgddb_subscript_has_custom_alias_hook_t pgddb_subscript_has_custom_alias_hook = nullptr;
pgddb_write_row_refname_hook_t pgddb_write_row_refname_hook = nullptr;
pgddb_db_and_schema_hook_t pgddb_db_and_schema_hook = nullptr;

/*
 * Generic table-AM name lookup: relam Oid -> pg_am.amname. Used by
 * pgddb_relation_name to feed pgddb_db_and_schema_hook so the consumer
 * can route by table-AM name. Returns NULL for relam == InvalidOid.
 */
static const char *
get_relation_table_am_name(Oid relam) {
	if (relam == InvalidOid) {
		return nullptr;
	}
	HeapTuple tp = SearchSysCache1(AMOID, ObjectIdGetDatum(relam));
	if (!HeapTupleIsValid(tp)) {
		return nullptr;
	}
	Form_pg_am amform = (Form_pg_am)GETSTRUCT(tp);
	char *result = pstrdup(NameStr(amform->amname));
	ReleaseSysCache(tp);
	return result;
}

/*
 * pgddb_db_and_schema_string returns "db.schema" for the given PG schema
 * and the relation's table-AM name. Dispatches through
 * pgddb_db_and_schema_hook for the (db_name, schema_name) policy.
 */
const char *
pgddb_db_and_schema_string(const char *postgres_schema_name, const char *table_am_name) {
	if (!pgddb_db_and_schema_hook) {
		elog(ERROR, "pgddb_db_and_schema_hook is not installed; consumer must call InitRuleutilsHooks()");
	}
	List *db_and_schema = pgddb_db_and_schema_hook(postgres_schema_name, table_am_name);
	const char *db_name = (const char *)linitial(db_and_schema);
	const char *schema_name = (const char *)lsecond(db_and_schema);
	return psprintf("%s.%s", quote_identifier(db_name), quote_identifier(schema_name));
}

/*
 * Fully qualified DuckDB-side name "db.schema.table" for the PG relation.
 */
char *
pgddb_relation_name(Oid relation_oid) {
	HeapTuple tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relation_oid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for relation %u", relation_oid);
	Form_pg_class relation = (Form_pg_class)GETSTRUCT(tp);
	const char *relname = NameStr(relation->relname);
	const char *postgres_schema_name = get_namespace_name_or_temp(relation->relnamespace);
	const char *table_am_name = get_relation_table_am_name(relation->relam);

	const char *db_and_schema = pgddb_db_and_schema_string(postgres_schema_name, table_am_name);

	char *result = psprintf("%s.%s", db_and_schema, quote_identifier(relname));

	ReleaseSysCache(tp);

	return result;
}

/*
 * Wraps pgduckdb_pg_get_querydef_internal to force ISO date format (the
 * only one DuckDB understands) and to flag outermost_query for the
 * deparser's target-list logic.
 */
char *
pgddb_get_querydef(Query *query) {
	outermost_query = true;
	auto save_nestlevel = NewGUCNestLevel();
	SetConfigOption("DateStyle", "ISO, YMD", PGC_USERSET, PGC_S_SESSION);
	char *result = pgduckdb_pg_get_querydef_internal(query, false);
	AtEOXact_GUC(false, save_nestlevel);
	return result;
}

/*
 * Filter for DEFAULT clause expressions: returns true for things that
 * should be printed (Var, non-default Const, or non-trivial walks),
 * false for the synthetic "default" markers Postgres uses internally.
 */
bool
pgddb_is_not_default_expr(Node *node, void *context) {
	if (node == NULL) {
		return false;
	}

	if (IsA(node, Var)) {
		return true;
	} else if (IsA(node, Const)) {
		/* If location is -1, it comes from the DEFAULT clause */
		Const *con = (Const *)node;
		if (con->location != -1) {
			return true;
		}
	}

#if PG_VERSION_NUM >= 160000
	return expression_tree_walker(node, pgddb_is_not_default_expr, context);
#else
	return expression_tree_walker(node, (bool (*)())((void *)pgddb_is_not_default_expr), context);
#endif
}

bool
is_system_sampling(const char *tsm_name, int num_args) {
	return (pg_strcasecmp(tsm_name, "system") == 0) && (num_args == 1);
}

bool
is_bernoulli_sampling(const char *tsm_name, int num_args) {
	return (pg_strcasecmp(tsm_name, "bernoulli") == 0) && (num_args == 1);
}

void
pgddb_add_tablesample_percent(const char *tsm_name, StringInfo buf, int num_args) {
	if (!(is_system_sampling(tsm_name, num_args) || is_bernoulli_sampling(tsm_name, num_args))) {
		return;
	}
	appendStringInfoChar(buf, '%');
}

/*
 * DuckDB doesn't use an escape character in LIKE expressions by default.
 * https://github.com/duckdb/duckdb/blob/12183c444dd729daad5cb463e59f3112a806a88b/src/function/scalar/string/like.cpp#L152
 *
 * When converting a PG Query to DuckDB SQL we force the escape character
 * (`\`) via the xxx_escape function args. The vendored deparser uses these
 * three helpers for prefix/middle/suffix of every operator-expr so it can
 * wrap LIKE-ish expressions in an ESCAPE clause.
 */

struct PGDuckDBGetOperExprContext {
	const char *pg_op_name;
	const char *duckdb_op_name;
	const char *escape_pattern;
	bool is_likeish_op;
	bool is_negated;
};

void *
pg_duckdb_get_oper_expr_make_ctx(const char *op_name, Node **, Node **arg2) {
	auto ctx = (PGDuckDBGetOperExprContext *)palloc0(sizeof(PGDuckDBGetOperExprContext));
	ctx->pg_op_name = op_name;
	ctx->is_likeish_op = false;
	ctx->is_negated = false;
	ctx->escape_pattern = "'\\'";

	if (AreStringEqual(op_name, "~~")) {
		ctx->duckdb_op_name = "LIKE";
		ctx->is_likeish_op = true;
		ctx->is_negated = false;
	} else if (AreStringEqual(op_name, "~~*")) {
		ctx->duckdb_op_name = "ILIKE";
		ctx->is_likeish_op = true;
		ctx->is_negated = false;
	} else if (AreStringEqual(op_name, "!~~")) {
		ctx->duckdb_op_name = "LIKE";
		ctx->is_likeish_op = true;
		ctx->is_negated = true;
	} else if (AreStringEqual(op_name, "!~~*")) {
		ctx->duckdb_op_name = "ILIKE";
		ctx->is_likeish_op = true;
		ctx->is_negated = true;
	}

	if (ctx->is_likeish_op && IsA(*arg2, FuncExpr)) {
		auto arg2_func = (FuncExpr *)*arg2;
		auto func_name = get_func_name(arg2_func->funcid);
		if (!AreStringEqual(func_name, "like_escape") && !AreStringEqual(func_name, "ilike_escape")) {
			elog(ERROR, "Unexpected function in LIKE expression: '%s'", func_name);
		}

		*arg2 = (Node *)linitial(arg2_func->args);
		ctx->escape_pattern = pgduckdb_deparse_expression((Node *)lsecond(arg2_func->args), nullptr, false, false);
	}

	return ctx;
}

void
pg_duckdb_get_oper_expr_prefix(StringInfo buf, void *vctx) {
	auto ctx = static_cast<PGDuckDBGetOperExprContext *>(vctx);
	if (ctx->is_likeish_op && ctx->is_negated) {
		appendStringInfo(buf, "NOT (");
	}
}

void
pg_duckdb_get_oper_expr_middle(StringInfo buf, void *vctx) {
	auto ctx = static_cast<PGDuckDBGetOperExprContext *>(vctx);
	auto op = ctx->duckdb_op_name ? ctx->duckdb_op_name : ctx->pg_op_name;
	appendStringInfo(buf, " %s ", op);
}

void
pg_duckdb_get_oper_expr_suffix(StringInfo buf, void *vctx) {
	auto ctx = static_cast<PGDuckDBGetOperExprContext *>(vctx);
	if (ctx->is_likeish_op) {
		appendStringInfo(buf, " ESCAPE %s", ctx->escape_pattern);

		if (ctx->is_negated) {
			appendStringInfo(buf, ")");
		}
	}
}
}
