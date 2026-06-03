#include "duckdb.hpp"

#include <vector>

#include "pgddb/pgddb_ddl.hpp"
#include "pgddb/pg/locale.hpp"
#include "pgddb/pg/relations.hpp"
#include "pgddb/pg/string_utils.hpp"
#include "pgddb/pgddb_table_am.hpp"
#include "pgddb/pgddb_types.hpp"

extern "C" {
#include "postgres.h"
#include "pgddb/pgddb_ruleutils.h"

#include "access/relation.h"
#include "access/htup_details.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "pgddb/vendor/pg_ruleutils.h"
#include "pgddb/vendor/pg_list.hpp"
}

extern "C" {

bool outermost_query = true;

/*
 * Registered deparser hooks. Each extension point keeps a list; consumers append
 * to it in _PG_init via the Register_pgddb_* functions. The wrappers below apply
 * any generic kernel rule, then iterate the list until one hook handles the node.
 * The Register_* / wrapper symbols get C linkage + default visibility from their
 * declarations in pgddb_ruleutils.h (so a shared kernel can export them).
 */
static std::vector<pgddb_function_name_hook_t> g_function_name_hooks;
static std::vector<pgddb_relation_name_hook_t> g_relation_name_hooks;
static std::vector<pgddb_is_fake_type_hook_t> g_is_fake_type_hooks;
static std::vector<pgddb_var_is_duckdb_row_hook_t> g_var_is_duckdb_row_hooks;
static std::vector<pgddb_duckdb_subscript_var_hook_t> g_duckdb_subscript_var_hooks;
static std::vector<pgddb_func_returns_duckdb_row_hook_t> g_func_returns_duckdb_row_hooks;
static std::vector<pgddb_replace_subquery_with_view_hook_t> g_replace_subquery_with_view_hooks;
static std::vector<pgddb_show_type_hook_t> g_show_type_hooks;
static std::vector<pgddb_reconstruct_star_step_hook_t> g_reconstruct_star_step_hooks;
static std::vector<pgddb_strip_first_subscript_hook_t> g_strip_first_subscript_hooks;
static std::vector<pgddb_subscript_has_custom_alias_hook_t> g_subscript_has_custom_alias_hooks;
// db_and_schema: object-scoped resolvers. Relations none claims fall back to the
// kernel's "pgduckdb" storage catalog (see pgddb_db_and_schema_string).
static std::vector<pgddb_db_and_schema_hook_t> g_db_and_schema_hooks;

void
Register_pgddb_function_name(pgddb_function_name_hook_t fn) {
	g_function_name_hooks.push_back(fn);
}
void
Register_pgddb_relation_name(pgddb_relation_name_hook_t fn) {
	g_relation_name_hooks.push_back(fn);
}
void
Register_pgddb_is_fake_type(pgddb_is_fake_type_hook_t fn) {
	g_is_fake_type_hooks.push_back(fn);
}
void
Register_pgddb_var_is_duckdb_row(pgddb_var_is_duckdb_row_hook_t fn) {
	g_var_is_duckdb_row_hooks.push_back(fn);
}
void
Register_pgddb_duckdb_subscript_var(pgddb_duckdb_subscript_var_hook_t fn) {
	g_duckdb_subscript_var_hooks.push_back(fn);
}
void
Register_pgddb_func_returns_duckdb_row(pgddb_func_returns_duckdb_row_hook_t fn) {
	g_func_returns_duckdb_row_hooks.push_back(fn);
}
void
Register_pgddb_replace_subquery_with_view(pgddb_replace_subquery_with_view_hook_t fn) {
	g_replace_subquery_with_view_hooks.push_back(fn);
}
void
Register_pgddb_show_type(pgddb_show_type_hook_t fn) {
	g_show_type_hooks.push_back(fn);
}
void
Register_pgddb_reconstruct_star_step(pgddb_reconstruct_star_step_hook_t fn) {
	g_reconstruct_star_step_hooks.push_back(fn);
}
void
Register_pgddb_strip_first_subscript(pgddb_strip_first_subscript_hook_t fn) {
	g_strip_first_subscript_hooks.push_back(fn);
}
void
Register_pgddb_subscript_has_custom_alias(pgddb_subscript_has_custom_alias_hook_t fn) {
	g_subscript_has_custom_alias_hooks.push_back(fn);
}
void
Register_pgddb_db_and_schema(pgddb_db_and_schema_hook_t fn) {
	g_db_and_schema_hooks.push_back(fn);
}

/* --- dispatch wrappers called by the vendored deparser --- */

char *
pgddb_function_name(Oid funcid, bool *use_variadic_p) {
	for (auto fn : g_function_name_hooks) {
		char *result = fn(funcid, use_variadic_p);
		if (result) {
			return result;
		}
	}
	return NULL;
}

bool
pgddb_is_fake_type(Oid type_oid) {
	for (auto fn : g_is_fake_type_hooks) {
		if (fn(type_oid)) {
			return true;
		}
	}
	return false;
}

bool
pgddb_var_is_duckdb_row(Var *var) {
	for (auto fn : g_var_is_duckdb_row_hooks) {
		if (fn(var)) {
			return true;
		}
	}
	return false;
}

Var *
pgddb_duckdb_subscript_var(Expr *expr) {
	for (auto fn : g_duckdb_subscript_var_hooks) {
		Var *result = fn(expr);
		if (result) {
			return result;
		}
	}
	return NULL;
}

bool
pgddb_func_returns_duckdb_row(RangeTblFunction *rtfunc) {
	for (auto fn : g_func_returns_duckdb_row_hooks) {
		if (fn(rtfunc)) {
			return true;
		}
	}
	return false;
}

bool
pgddb_replace_subquery_with_view(Query *query, StringInfo buf) {
	for (auto fn : g_replace_subquery_with_view_hooks) {
		if (fn(query, buf)) {
			return true;
		}
	}
	return false;
}

int
pgddb_show_type(Const *constval, int original_showtype) {
	// Generic kernel rule: suppress a bare ::numeric (no typmod) cast. DuckDB
	// defaults plain ::numeric to DECIMAL(18,3), which overflows wide literals;
	// without the cast DuckDB parses the literal as VARCHAR and coerces it to the
	// target column type. Applies to every consumer, so it lives here, not a hook.
	if (constval && constval->consttype == NUMERICOID && constval->consttypmod == -1) {
		return -1;
	}
	for (auto fn : g_show_type_hooks) {
		int result = fn(constval, original_showtype);
		if (result != original_showtype) {
			return result;
		}
	}
	return original_showtype;
}

bool
pgddb_reconstruct_star_step(StarReconstructionContext *ctx, ListCell *tle_cell) {
	for (auto fn : g_reconstruct_star_step_hooks) {
		if (fn(ctx, tle_cell)) {
			return true;
		}
	}
	return false;
}

SubscriptingRef *
pgddb_strip_first_subscript(SubscriptingRef *sbsref, StringInfo buf) {
	for (auto fn : g_strip_first_subscript_hooks) {
		SubscriptingRef *result = fn(sbsref, buf);
		if (result != sbsref) {
			// A hook acted (stripped and wrote into buf).
			return result;
		}
	}
	return sbsref;
}

bool
pgddb_subscript_has_custom_alias(Plan *plan, List *rtable, Var *subscript_var, char *colname) {
	for (auto fn : g_subscript_has_custom_alias_hooks) {
		if (fn(plan, rtable, subscript_var, colname)) {
			return true;
		}
	}
	return false;
}

// Row reference expansion is generic (was identical in every consumer): emit
// `<ref>.*` at the top level so DuckDB returns the underlying columns, else the
// bare alias. Called by the deparser once a var_is_row / func_returns_row hook
// has matched.
char *
pgddb_write_row_refname(StringInfo buf, char *refname, bool is_top_level) {
	appendStringInfoString(buf, quote_identifier(refname));
	if (is_top_level) {
		appendStringInfoString(buf, ".*");
		return NULL;
	}
	return refname;
}

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
 * pgddb_db_and_schema_string returns "db.schema" for the given PG schema and the
 * relation's table-AM name. Tries each object-scoped resolver in registration
 * order (each claims only its own table AM); relations that none claim fall
 * through to the optional catch-all resolver (typically a storage extension that
 * reads plain PG heap relations).
 */
const char *
pgddb_db_and_schema_string(const char *postgres_schema_name, const char *table_am_name) {
	// Try each object-scoped resolver (each claims only its own table AM). Any
	// relation none claims is read through the kernel's "pgduckdb" storage
	// catalog, which DuckDBManager::Initialize always registers + attaches. The
	// "pgduckdb" name is internal to DuckDB and never user-visible.
	const char *db_name = "pgduckdb";
	const char *schema_name = postgres_schema_name;
	for (auto fn : g_db_and_schema_hooks) {
		List *db_and_schema = fn(postgres_schema_name, table_am_name);
		if (db_and_schema) {
			db_name = (const char *)linitial(db_and_schema);
			schema_name = (const char *)lsecond(db_and_schema);
			break;
		}
	}
	return psprintf("%s.%s", quote_identifier(db_name), quote_identifier(schema_name));
}

/*
 * Fully qualified DuckDB-side name "db.schema.table" for the PG relation.
 */
char *
pgddb_relation_name(Oid relation_oid) {
	for (auto fn : g_relation_name_hooks) {
		char *overridden = fn(relation_oid);
		if (overridden)
			return overridden;
	}

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
 * Wraps pgddb_pg_get_querydef_internal to force ISO date format (the
 * only one DuckDB understands) and to flag outermost_query for the
 * deparser's target-list logic.
 */
char *
pgddb_get_querydef(Query *query) {
	outermost_query = true;
	auto save_nestlevel = NewGUCNestLevel();
	SetConfigOption("DateStyle", "ISO, YMD", PGC_USERSET, PGC_S_SESSION);
	char *result = pgddb_pg_get_querydef_internal(query, false);
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
		ctx->escape_pattern = pgddb_deparse_expression((Node *)lsecond(arg2_func->args), nullptr, false, false);
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

/*
 * cookConstraint is vendored in from src/backend/catalog/heap.c -- PG does
 * not expose it via headers. Takes a raw CHECK constraint expression and
 * converts it to cooked form ready for storage.
 */
static Node *
cookConstraint(ParseState *pstate, Node *raw_constraint, char *relname) {
	Node *expr = transformExpr(pstate, raw_constraint, EXPR_KIND_CHECK_CONSTRAINT);
	expr = coerce_to_boolean(pstate, expr, "CHECK");
	assign_expr_collations(pstate, expr);
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		                errmsg("only table \"%s\" can be referenced in check constraint", relname)));
	return expr;
}

} // extern "C"

namespace pgddb {

/*
 * DuckdbRuleutils::get_tabledef returns the DuckDB CREATE TABLE statement for the
 * given relation. The schema, default values, NOT NULL and CHECK
 * constraints are included; UNIQUE/PRIMARY KEY constraints (which would
 * trigger PG index creation) are not.
 *
 * Consumer-specific persistence/ownership policy is delegated to the virtual
 * validate_create_table() override. The catalog name in the resulting CREATE
 * TABLE is whatever the relation's table AM was registered as in pgddb's
 * table-AM registry (pg_duckdb registers "duckdb", pg_ducklake "ducklake", ...).
 *
 * Originally pgduckdb_get_tabledef in pg_duckdb; inspired by
 * pg_get_tableschemadef_string from the patch Jelte submitted to
 * Postgres in 2023:
 * https://www.postgresql.org/message-id/CAGECzQSqdDHO_s8=CPTb2+4eCLGUscdh=KjYGTunhvrwcC7ZSQ@mail.gmail.com
 */
std::string
DuckdbRuleutils::get_tabledef(Oid relation_oid) {
	Relation relation = relation_open(relation_oid, AccessShareLock);
	const char *relation_name = pgddb_relation_name(relation_oid);
	const char *postgres_schema_name = get_namespace_name_or_temp(relation->rd_rel->relnamespace);
	const char *duckdb_table_am_name = pgddb::TableAmGetName(relation->rd_tableam);
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

	validate_create_table(relation);

	appendStringInfo(&buffer, "CREATE SCHEMA IF NOT EXISTS %s; ", db_and_schema);

	appendStringInfoString(&buffer, "CREATE ");
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

		const char *column_type_name = this->column_type_name(column->atttypid, column->atttypmod);
		if (column_type_name == NULL)
			column_type_name = format_type_with_typemod(column->atttypid, column->atttypmod);

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

	if (pgddb::TableAmGetName(relation->rd_tableam) == nullptr) {
		/* Defensive: only registered duckdb-y AM tables should reach here. */
		elog(ERROR, "Relation %u uses a table AM not registered with pgddb", relation_oid);
	}

	if (relation->rd_options) {
		elog(ERROR, "Storage options are not supported in DuckDB");
	}

	relation_close(relation, AccessShareLock);

	return std::string(buffer.data);
}

/*
 * DuckdbRuleutils::get_rename_relationdef returns the DuckDB ALTER TABLE ...
 * RENAME (or RENAME COLUMN) statement for the given relation. Catalog prefix
 * comes from the relation's registered table-AM name.
 */
std::string
DuckdbRuleutils::get_rename_relationdef(Oid relation_oid, RenameStmt *rename_stmt) {
	if (rename_stmt->renameType != OBJECT_TABLE && rename_stmt->renameType != OBJECT_VIEW &&
	    rename_stmt->renameType != OBJECT_COLUMN) {
		elog(ERROR, "Only renaming tables and columns is supported in DuckDB");
	}

	Relation relation = relation_open(relation_oid, AccessShareLock);

	const char *postgres_schema_name = get_namespace_name_or_temp(relation->rd_rel->relnamespace);
	const char *duckdb_table_am_name = pgddb::TableAmGetName(relation->rd_tableam);
	const char *db_and_schema = pgddb_db_and_schema_string(postgres_schema_name, duckdb_table_am_name);
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

	return std::string(buffer.data);
}

/*
 * DuckdbRuleutils::get_alter_tabledef returns the DuckDB ALTER TABLE command(s)
 * for the given table. DuckDB does not support multiple ALTER subcommands in a
 * single statement, so each subcommand is emitted as its own ALTER TABLE.
 */
std::string
DuckdbRuleutils::get_alter_tabledef(Oid relation_oid, AlterTableStmt *alter_stmt) {
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
				elog(ERROR, "Column \"%s\" not found in relation \"%s\"", column_name, RelationGetRelationName(relation));
			}

			if (cmd->def) {
				auto expr = cookDefault(pstate, cmd->def, attribute->atttypid, attribute->atttypmod, column_name,
				                        attribute->attgenerated);
				char *default_string = pgddb_deparse_expression(expr, relation_context, false, false);
				appendStringInfo(&buffer, "ALTER COLUMN %s SET DEFAULT %s; ", quote_identifier(column_name),
				                 default_string);
			} else {
				appendStringInfo(&buffer, "ALTER COLUMN %s DROP DEFAULT; ", quote_identifier(column_name));
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

	return std::string(buffer.data);
}
}
