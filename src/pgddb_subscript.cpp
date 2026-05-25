#include "pgddb/pgddb_subscript.h"

extern "C" {
#include "postgres.h"
#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "parser/parse_coerce.h"
#include "parser/parse_node.h"
#include "parser/parse_expr.h"
#include "nodes/subscripting.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "pgddb/vendor/pg_list.hpp"
}

namespace pgddb {

namespace pg {

subscript_refrestype_hook_t subscript_refrestype_hook = nullptr;

/* "<schema>.<typname>" without format_type_be's reserved-keyword quoting. */
static char *
TypeNameOf(Oid type_oid) {
	HeapTuple tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tp))
		return pstrdup("?");
	Form_pg_type typ = (Form_pg_type)GETSTRUCT(tp);
	char *schema = get_namespace_name(typ->typnamespace);
	char *result = psprintf("%s.%s", schema ? schema : "?", NameStr(typ->typname));
	ReleaseSysCache(tp);
	return result;
}

static Oid
ResolveRefrestype(Oid container_oid) {
	if (subscript_refrestype_hook) {
		Oid oid = subscript_refrestype_hook(container_oid);
		if (OidIsValid(oid)) {
			return oid;
		}
	}
	return container_oid;
}

static Node *
CoerceSubscriptToText(struct ParseState *pstate, A_Indices *subscript, const char *type_name) {
	if (!subscript->uidx) {
		elog(ERROR, "Creating a slice out of %s is not supported", type_name);
	}

	Node *subscript_expr = transformExpr(pstate, subscript->uidx, pstate->p_expr_kind);
	int expr_location = exprLocation(subscript->uidx);
	Oid subscript_expr_type = exprType(subscript_expr);

	if (subscript->lidx) {
		elog(ERROR, "Creating a slice out of %s is not supported", type_name);
	}

	Node *coerced_expr = coerce_to_target_type(pstate, subscript_expr, subscript_expr_type, TEXTOID, -1,
	                                           COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, expr_location);
	if (!coerced_expr) {
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("%s subscript must have text type", type_name),
		                parser_errposition(pstate, expr_location)));
	}

	if (!IsA(subscript_expr, Const)) {
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("%s subscript must be a constant", type_name),
		                parser_errposition(pstate, expr_location)));
	}

	Const *subscript_const = castNode(Const, subscript_expr);
	if (subscript_const->constisnull) {
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("%s subscript cannot be NULL", type_name),
		                parser_errposition(pstate, expr_location)));
	}

	return coerced_expr;
}

/*
 * In Postgres all index operations in a row are all slices or all plain
 * index operations. If you mix them, all are converted to slices.
 * There's no difference in representation possible between
 * "col[1:2][1]" and "col[1:2][1:]". If you want this separation you
 * need to use parenthesis to separate: "(col[1:2])[1]"
 * This might seem like fairly strange behaviour, but Postgres uses
 * this to be able to slice in multi-dimensional arrays and this
 * behaviour is documented here:
 * https://www.postgresql.org/docs/current/arrays.html#ARRAYS-ACCESSING
 *
 * This is different from DuckDB, but there's not much we can do about
 * that. So we'll have this same behaviour by, which means we need to always
 * add the lower subscript to the slice. The lower subscript will be NULL in
 * that case.
 *
 * See also comments on SubscriptingRef in nodes/subscripting.h
 */
static void
AddSubscriptExpressions(SubscriptingRef *sbsref, struct ParseState *pstate, A_Indices *subscript, bool is_slice) {
	Assert(is_slice || subscript->uidx);

	Node *upper_subscript_expr = NULL;
	if (subscript->uidx) {
		upper_subscript_expr = transformExpr(pstate, subscript->uidx, pstate->p_expr_kind);
	}

	sbsref->refupperindexpr = lappend(sbsref->refupperindexpr, upper_subscript_expr);

	if (is_slice) {
		Node *lower_subscript_expr = NULL;
		if (subscript->uidx) {
			lower_subscript_expr = transformExpr(pstate, subscript->lidx, pstate->p_expr_kind);
		}
		sbsref->reflowerindexpr = lappend(sbsref->reflowerindexpr, lower_subscript_expr);
	}
}

/*
 * DuckdbSubscriptTransform is called by the parser when a subscripting
 * operation is performed on a duckdb type that can be indexed by arbitrary
 * expressions. All this does is parse those expressions and make sure the
 * subscript returns an an duckdb.unresolved_type again.
 */
static void
DuckdbSubscriptTransform(SubscriptingRef *sbsref, List *indirection, struct ParseState *pstate, bool is_slice,
                         bool is_assignment, const char *type_name) {
	if (is_assignment) {
		elog(ERROR, "Assignment to %s is not supported", type_name);
	}

	if (indirection == NIL) {
		elog(ERROR, "Subscripting %s with an empty subscript is not supported", type_name);
	}

	// Transform each subscript expression
	foreach_node(A_Indices, subscript, indirection) {
		AddSubscriptExpressions(sbsref, pstate, subscript, is_slice);
	}

	// Set the result type of the subscripting operation
	sbsref->refrestype = ResolveRefrestype(sbsref->refcontainertype);
	sbsref->reftypmod = -1;
}

/*
 * DuckdbTextSubscriptTransform is called by the parser when a subscripting
 * operation is performed on type that can only be indexed by string literals.
 * It has two main puprposes:
 * 1. Ensure that the row is being indexed using a string literal
 * 2. Ensure that the return type of this index operation is
 *    duckdb.unresolved_type
 *
 * Currently this is used for duckdb.row and duckdb.struct types.
 */
static void
DuckdbTextSubscriptTransform(SubscriptingRef *sbsref, List *indirection, struct ParseState *pstate, bool is_slice,
                             bool is_assignment, const char *type_name) {
	if (is_assignment) {
		elog(ERROR, "Assignment to %s is not supported", type_name);
	}

	if (indirection == NIL) {
		elog(ERROR, "Subscripting %s with an empty subscript is not supported", type_name);
	}

	bool first = true;

	// Transform each subscript expression
	foreach_node(A_Indices, subscript, indirection) {
		/* The first subscript needs to be a TEXT constant, since it should be
		 * a column reference. But the subscripts after that can be anything,
		 * DuckDB should interpret those. */
		if (first) {
			sbsref->refupperindexpr =
			    lappend(sbsref->refupperindexpr, CoerceSubscriptToText(pstate, subscript, type_name));
			if (is_slice) {
				sbsref->reflowerindexpr = lappend(sbsref->reflowerindexpr, NULL);
			}
			first = false;
			continue;
		}

		AddSubscriptExpressions(sbsref, pstate, subscript, is_slice);
	}

	// Set the result type of the subscripting operation
	sbsref->refrestype = ResolveRefrestype(sbsref->refcontainertype);
	sbsref->reftypmod = -1;
}

static bool
DuckdbSubscriptCheckSubscripts(ExprState * /*state*/, ExprEvalStep *op, ExprContext * /*econtext*/) {
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	char *type_name = strVal(sbsrefstate->workspace);
	elog(ERROR, "Subscripting %s is not supported in the Postgres Executor", type_name);
}

static void
DuckdbSubscriptFetch(ExprState * /*state*/, ExprEvalStep *op, ExprContext * /*econtext*/) {
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	char *type_name = strVal(sbsrefstate->workspace);
	elog(ERROR, "Subscripting %s is not supported in the Postgres Executor", type_name);
}

static void
DuckdbSubscriptAssign(ExprState * /*state*/, ExprEvalStep *op, ExprContext * /*econtext*/) {
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	char *type_name = strVal(sbsrefstate->workspace);
	elog(ERROR, "Subscripting %s is not supported in the Postgres Executor", type_name);
}

static void
DuckdbSubscriptFetchOld(ExprState * /*state*/, ExprEvalStep *op, ExprContext * /*econtext*/) {
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	char *type_name = strVal(sbsrefstate->workspace);
	elog(ERROR, "Subscripting %s is not supported in the Postgres Executor", type_name);
}

/*
 * DuckdbSubscriptExecSetup stores a bunch of functions in the methods
 * structure. These functions are called by the Postgres executor when a
 * subscripting is executed. We need to implement this function, because it is
 * called for materialized CTEs. Even in that case the actual functions that
 * are stored in methods are never supposed to be called, because pg_duckdb
 * shouldn't force usage of DuckDB execution when duckdb types are present in
 * the query. So these methods are just stubs that throw an error when called.
 */
static void
DuckdbSubscriptExecSetup(const SubscriptingRef *sbsref, SubscriptingRefState *sbsrefstate,
                         SubscriptExecSteps *methods) {

	sbsrefstate->workspace = makeString(TypeNameOf(sbsref->refcontainertype));
	methods->sbs_check_subscripts = DuckdbSubscriptCheckSubscripts;
	methods->sbs_fetch = DuckdbSubscriptFetch;
	methods->sbs_assign = DuckdbSubscriptAssign;
	methods->sbs_fetch_old = DuckdbSubscriptFetchOld;
}

static void
DuckdbRowSubscriptTransform(SubscriptingRef *sbsref, List *indirection, struct ParseState *pstate, bool is_slice,
                            bool is_assignment) {
	DuckdbTextSubscriptTransform(sbsref, indirection, pstate, is_slice, is_assignment,
	                             TypeNameOf(sbsref->refcontainertype));
}

const SubscriptRoutines duckdb_row_subscript_routines = {
    .transform = DuckdbRowSubscriptTransform,
    .exec_setup = DuckdbSubscriptExecSetup,
    .fetch_strict = false,
    .fetch_leakproof = true,
    .store_leakproof = true,
};

static void
DuckdbUnresolvedTypeSubscriptTransform(SubscriptingRef *sbsref, List *indirection, struct ParseState *pstate,
                                       bool is_slice, bool is_assignment) {
	DuckdbSubscriptTransform(sbsref, indirection, pstate, is_slice, is_assignment,
	                         TypeNameOf(sbsref->refcontainertype));
}

const SubscriptRoutines duckdb_unresolved_type_subscript_routines = {
    .transform = DuckdbUnresolvedTypeSubscriptTransform,
    .exec_setup = DuckdbSubscriptExecSetup,
    .fetch_strict = false,
    .fetch_leakproof = true,
    .store_leakproof = true,
};

static void
DuckdbStructSubscriptTransform(SubscriptingRef *sbsref, List *indirection, struct ParseState *pstate, bool is_slice,
                               bool is_assignment) {
	DuckdbTextSubscriptTransform(sbsref, indirection, pstate, is_slice, is_assignment,
	                             TypeNameOf(sbsref->refcontainertype));
}

const SubscriptRoutines duckdb_struct_subscript_routines = {
    .transform = DuckdbStructSubscriptTransform,
    .exec_setup = DuckdbSubscriptExecSetup,
    .fetch_strict = false,
    .fetch_leakproof = true,
    .store_leakproof = true,
};

static void
DuckdbMapSubscriptTransform(SubscriptingRef *sbsref, List *indirection, struct ParseState *pstate, bool is_slice,
                            bool is_assignment) {
	DuckdbSubscriptTransform(sbsref, indirection, pstate, is_slice, is_assignment,
	                         TypeNameOf(sbsref->refcontainertype));
}

const SubscriptRoutines duckdb_map_subscript_routines = {
    .transform = DuckdbMapSubscriptTransform,
    .exec_setup = DuckdbSubscriptExecSetup,
    .fetch_strict = false,
    .fetch_leakproof = true,
    .store_leakproof = true,
};

} // namespace pg

} // namespace pgddb
