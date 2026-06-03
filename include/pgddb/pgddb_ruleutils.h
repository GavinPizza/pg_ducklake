#ifndef PGDDB_RULEUTILS_H
#define PGDDB_RULEUTILS_H

#include "postgres.h"
#include "pgddb/vendor/pg_list.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Forward declarations of PG node types used in the hook signatures.
 * The full definitions are pulled in by the vendored ruleutils (C) and
 * by consumer .cpp files via their own `#include "postgres.h"` plus
 * `nodes/parsenodes.h`, etc. — we don't include those here so this
 * header stays cheap to consume.
 */
struct Var;
struct Expr;
struct Const;
struct Plan;
struct Query;
struct RangeTblFunction;
struct SubscriptingRef;

typedef struct StarReconstructionContext {
	List *target_list;
	int varno_star;
	int varattno_star;
	bool added_current_star;
} StarReconstructionContext;

/*
 * Deparser hook surface. Each extension point keeps a LIST of registered hooks
 * (the std::vector storage lives in the kernel .cpp; this header is also included
 * by the vendored ruleutils as C, so it stays C-compatible). The vendored
 * deparser calls the pgddb_<name> wrappers below; each wrapper applies any generic
 * kernel rule, then iterates the registered hooks until one handles the node.
 *
 * Consumers register their hooks in _PG_init via the Register_pgddb_<name>
 * functions, exported with default visibility -- the same shape as
 * RegisterDuckdbTableAm -- so a future shared libpgddb can collect hooks from
 * independently-linked extensions instead of each bundling its own copy.
 *
 * Hooks are OBJECT-SCOPED: each returns "handled" (true / non-NULL / changed
 * value) only for the types/relations/functions it owns and declines otherwise,
 * so the kernel falls through to the next hook (then PG's built-in default).
 */
#define PGDDB_EXPORT __attribute__((visibility("default")))

// Function name override: e.g. a duckdb-only function -> "system.main.f".
// Return a palloc'd name, or NULL to decline.
typedef char *(*pgddb_function_name_hook_t)(Oid funcid, bool *use_variadic_p);
PGDDB_EXPORT void Register_pgddb_function_name(pgddb_function_name_hook_t fn);
char *pgddb_function_name(Oid funcid, bool *use_variadic_p);

// Override the entire qualified relation name (e.g. FDW relations whose DuckDB
// name lives in a separately attached catalog). Return a palloc'd name or NULL.
typedef char *(*pgddb_relation_name_hook_t)(Oid relid);
PGDDB_EXPORT void Register_pgddb_relation_name(pgddb_relation_name_hook_t fn);

// (The CREATE TABLE column-type-name override is now a DdlUtils virtual; see
// include/pgddb/pgddb_ddl.hpp.)

// Is this a "fake" PG type that exists only to satisfy the PG parser and must
// not be cast-emitted to DuckDB? Return true if it is one of yours.
typedef bool (*pgddb_is_fake_type_hook_t)(Oid type_oid);
PGDDB_EXPORT void Register_pgddb_is_fake_type(pgddb_is_fake_type_hook_t fn);
bool pgddb_is_fake_type(Oid type_oid);

// Is this Var one of your "row" passthrough types (expanded to <ref>.*)?
typedef bool (*pgddb_var_is_duckdb_row_hook_t)(Var *var);
PGDDB_EXPORT void Register_pgddb_var_is_duckdb_row(pgddb_var_is_duckdb_row_hook_t fn);
bool pgddb_var_is_duckdb_row(Var *var);

// Extract the subscript base Var from one of your subscripting exprs, or NULL.
typedef Var *(*pgddb_duckdb_subscript_var_hook_t)(Expr *expr);
PGDDB_EXPORT void Register_pgddb_duckdb_subscript_var(pgddb_duckdb_subscript_var_hook_t fn);
Var *pgddb_duckdb_subscript_var(Expr *expr);

// Does this set-returning function return one of your "row" types?
typedef bool (*pgddb_func_returns_duckdb_row_hook_t)(RangeTblFunction *rtfunc);
PGDDB_EXPORT void Register_pgddb_func_returns_duckdb_row(pgddb_func_returns_duckdb_row_hook_t fn);
bool pgddb_func_returns_duckdb_row(RangeTblFunction *rtfunc);

// Replace a subquery with a view reference; write into buf and return true if handled.
typedef bool (*pgddb_replace_subquery_with_view_hook_t)(Query *query, StringInfo buf);
PGDDB_EXPORT void Register_pgddb_replace_subquery_with_view(pgddb_replace_subquery_with_view_hook_t fn);
bool pgddb_replace_subquery_with_view(Query *query, StringInfo buf);

// Decide the showtype for a Const cast. Return -1 to suppress the cast, or the
// passed-in showtype to keep it. The kernel applies its generic rules (e.g.
// suppress a bare ::numeric) before consulting the hooks.
typedef int (*pgddb_show_type_hook_t)(Const *constval, int original_showtype);
PGDDB_EXPORT void Register_pgddb_show_type(pgddb_show_type_hook_t fn);
int pgddb_show_type(Const *constval, int original_showtype);

// Star reconstruction step. Mutate ctx and return true if handled.
typedef bool (*pgddb_reconstruct_star_step_hook_t)(StarReconstructionContext *ctx, ListCell *tle_cell);
PGDDB_EXPORT void Register_pgddb_reconstruct_star_step(pgddb_reconstruct_star_step_hook_t fn);
bool pgddb_reconstruct_star_step(StarReconstructionContext *ctx, ListCell *tle_cell);

// Strip the first subscript of a `r['c']` ref into buf (-> `r.c`). Return the
// shortened ref if handled, or the input sbsref unchanged to decline.
typedef SubscriptingRef *(*pgddb_strip_first_subscript_hook_t)(SubscriptingRef *sbsref, StringInfo buf);
PGDDB_EXPORT void Register_pgddb_strip_first_subscript(pgddb_strip_first_subscript_hook_t fn);
SubscriptingRef *pgddb_strip_first_subscript(SubscriptingRef *sbsref, StringInfo buf);

// Does this subscript Var carry a custom column alias?
typedef bool (*pgddb_subscript_has_custom_alias_hook_t)(Plan *plan, List *rtable, Var *subscript_var, char *colname);
PGDDB_EXPORT void Register_pgddb_subscript_has_custom_alias(pgddb_subscript_has_custom_alias_hook_t fn);
bool pgddb_subscript_has_custom_alias(Plan *plan, List *rtable, Var *subscript_var, char *colname);

// db.schema resolution. OBJECT-SCOPED: return the (db_name, schema_name) 2-list
// for relations of YOUR table-AM, or NULL to decline. Relations that no
// registered resolver claims fall back to the kernel's "pgduckdb" storage
// catalog (registered + attached by DuckDBManager::Initialize), which reads
// plain PG heap relations.
typedef List *(*pgddb_db_and_schema_hook_t)(const char *postgres_schema_name, const char *table_am_name);
PGDDB_EXPORT void Register_pgddb_db_and_schema(pgddb_db_and_schema_hook_t fn);

// Row reference expansion (`<ref>.*` at top level, `<ref>` otherwise) is generic
// and handled entirely in the kernel -- no hook. The wrapper is still called by
// the deparser once pgddb_var_is_duckdb_row/pgddb_func_returns_duckdb_row has matched.
char *pgddb_write_row_refname(StringInfo buf, char *refname, bool is_top_level);

/*
 * Generic deparser helpers (not hooks; same logic for every consumer).
 */
bool pgddb_is_not_default_expr(Node *node, void *context);
bool is_system_sampling(const char *tsm_name, int num_args);
bool is_bernoulli_sampling(const char *tsm_name, int num_args);
void pgddb_add_tablesample_percent(const char *tsm_name, StringInfo buf, int num_args);

/*
 * Generic deparser entry points and helpers.
 */
char *pgddb_relation_name(Oid relid);
char *pgddb_get_querydef(Query *);
const char *pgddb_db_and_schema_string(const char *postgres_schema_name, const char *table_am_name);

/*
 * The CREATE TABLE / ALTER TABLE / RENAME deparsers, and their consumer-specific
 * customization points (column-type-name mapping, create-table validation), now
 * live on the pgddb::DdlUtils class in include/pgddb/pgddb_ddl.hpp -- they are
 * invoked directly by consumers, not by this (C) vendored-ruleutils surface.
 */

extern bool outermost_query;

#ifdef __cplusplus
}
#endif

#endif /* PGDDB_RULEUTILS_H */
