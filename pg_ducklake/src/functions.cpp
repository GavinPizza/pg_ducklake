/*
 * functions.cpp -- DuckLake function exposing.
 *
 * Exposes DuckLake functions as PostgreSQL functions in the
 * `ducklake` schema.
 *
 * DuckLake extension registers its functions globally as ducklake_<name>
 * with a catalog arg. Two bridging mechanisms are used:
 *
 * 1. Wrapper table macros -- for simple mappings:
 *   PG function: ducklake.snapshots()
 *     -> DuckDB-only routing: system.main.snapshots()
 *     -> Wrapper macro: FROM ducklake_snapshots('pgducklake')
 *
 * 2. Table function sets -- for overloaded signatures:
 *   PG function: ducklake.cleanup_old_files() / cleanup_old_files(interval)
 *     -> DuckDB-only routing: system.main.cleanup_old_files(...)
 *     -> TableFunctionSet bind replaces with ducklake_cleanup_old_files()
 */

#include "pgducklake/constants.hpp"
#include "pgducklake/functions.hpp"
#include "pgducklake/time_travel.hpp"

#include <cstring>

#include <duckdb/catalog/catalog.hpp>
#include <duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp>
#include <duckdb/catalog/catalog_transaction.hpp>
#include <duckdb/catalog/default/default_functions.hpp>
#include <duckdb/catalog/default/default_table_functions.hpp>
#include <duckdb/common/types/interval.hpp>
#include <duckdb/common/types/timestamp.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/parser/parsed_data/create_table_function_info.hpp>

extern "C" {
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
}

namespace pgducklake {

using namespace duckdb;

bool
IsDucklakeOnlyFunction(Oid funcid) {
	// Match by prosrc only: pg_ducklake declares all of its DuckDB-routed
	// SQL stubs with prosrc='duckdb_only_function', whether they live in
	// the ducklake schema (snapshots, table_info, ...) or in @extschema@
	// (read_csv, read_parquet -- unqualified for parity with pg_duckdb).
	HeapTuple tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		return false;
	bool isnull;
	Datum prosrc_datum = SysCacheGetAttr(PROCOID, tp, Anum_pg_proc_prosrc, &isnull);
	if (isnull) {
		ReleaseSysCache(tp);
		return false;
	}
	char *prosrc_str = TextDatumGetCString(prosrc_datum);
	ReleaseSysCache(tp);
	return std::strcmp(prosrc_str, "duckdb_only_function") == 0;
}

} // namespace pgducklake

// duckdb_only_function: marker stub for ducklake.* functions declared with
// `AS 'MODULE_PATHNAME', 'duckdb_only_function'`. The planner hook routes
// these calls to DuckDB before fmgr would call the body; if the body
// fires it errors loudly.
#include "pgddb/utility/cpp_wrapper.hpp"

extern "C" {

DECLARE_PG_FUNCTION(duckdb_only_function) {
	char *function_name = DatumGetCString(DirectFunctionCall1(regprocout, fcinfo->flinfo->fn_oid));
	elog(ERROR, "Function '%s' only works with DuckDB execution", function_name);
}

// ducklake_only_procedure: the procedure twin of duckdb_only_function. The CALL
// is caught by the utility hook (IsDucklakeOnlyProcedure) and routed to DuckDB;
// if this body runs the routing was missed, so error loudly.
DECLARE_PG_FUNCTION(ducklake_only_procedure) {
	char *proc_name = DatumGetCString(DirectFunctionCall1(regprocout, ObjectIdGetDatum(fcinfo->flinfo->fn_oid)));
	elog(ERROR, "Procedure '%s' only works with DuckDB execution", proc_name);
}

// ducklake_function_mapping: marker stub for the regclass overloads of
// table-scoped functions (e.g. ducklake.flush_inlined_data(scope regclass)),
// declared with `AS 'MODULE_PATHNAME', 'ducklake_function_mapping'`. The
// planner hook (RewriteRegclassFunctions) rewrites those calls into their
// (schema_name text, table_name text) counterparts before pg_duckdb routes
// them to DuckDB; if this body fires the rewrite was missed, so error with a
// corrective hint.
DECLARE_PG_FUNCTION(ducklake_function_mapping) {
	ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("regclass function was not rewritten by planner hook"),
	                errhint("Use the (schema_name text, table_name text) form "
	                        "for dynamic table references.")));
	PG_RETURN_NULL();
}
}

namespace pgducklake {

/*
 * Register wrapper table macros in DuckDB's system.main catalog.
 *
 * pg_duckdb's DuckDB-only routing rewrites PG function calls to
 * system.main.<func_name>(args...). DuckLake registers its functions
 * globally as ducklake_<name>(catalog, ...). These macros bridge the
 * gap: a PG function with a clean name (e.g., "snapshots") routes to
 * system.main.snapshots(), which this macro expands to
 * ducklake_snapshots('pgducklake').
 */
// clang-format off
static const DefaultTableMacro pg_ducklake_wrapper_macros[] = {
  // catalog-level functions (no table arg)
  {DEFAULT_SCHEMA, "snapshots", {nullptr}, {{nullptr, nullptr}},
   "FROM ducklake_snapshots('" PGDUCKLAKE_DUCKDB_CATALOG "')"},
  {DEFAULT_SCHEMA, "current_snapshot", {nullptr}, {{nullptr, nullptr}},
   "FROM ducklake_current_snapshot('" PGDUCKLAKE_DUCKDB_CATALOG "')"},
  {DEFAULT_SCHEMA, "last_committed_snapshot", {nullptr}, {{nullptr, nullptr}},
   "FROM ducklake_last_committed_snapshot('" PGDUCKLAKE_DUCKDB_CATALOG "')"},
  {DEFAULT_SCHEMA, "table_info", {nullptr}, {{nullptr, nullptr}},
   "FROM ducklake_table_info('" PGDUCKLAKE_DUCKDB_CATALOG "')"},
  {DEFAULT_SCHEMA, "options", {nullptr}, {{nullptr, nullptr}},
   "FROM ducklake_options('" PGDUCKLAKE_DUCKDB_CATALOG "')"},
  // maintenance functions (no args)
  {DEFAULT_SCHEMA, "expire_snapshots", {nullptr}, {{nullptr, nullptr}},
   "FROM ducklake_expire_snapshots('" PGDUCKLAKE_DUCKDB_CATALOG "')"},
  // table-scoped functions
  {DEFAULT_SCHEMA, "ensure_inlined_data_table", {"schema_name", "table_name", nullptr}, {{nullptr, nullptr}},
   "FROM ducklake_ensure_inlined_table('" PGDUCKLAKE_DUCKDB_CATALOG "', schema_name, table_name)"},
  {DEFAULT_SCHEMA, "list_files", {"schema_name", "table_name", nullptr}, {{nullptr, nullptr}},
   "FROM ducklake_list_files('" PGDUCKLAKE_DUCKDB_CATALOG "', table_name, schema => schema_name)"},
  // data change feed functions (schema + table + start + end)
  {DEFAULT_SCHEMA, "table_insertions",
   {"schema_name", "table_name", "start_snapshot", "end_snapshot", nullptr},
   {{nullptr, nullptr}},
   "FROM ducklake_table_insertions('" PGDUCKLAKE_DUCKDB_CATALOG "', schema_name, table_name, start_snapshot, end_snapshot)"},
  {DEFAULT_SCHEMA, "table_deletions",
   {"schema_name", "table_name", "start_snapshot", "end_snapshot", nullptr},
   {{nullptr, nullptr}},
   "FROM ducklake_table_deletions('" PGDUCKLAKE_DUCKDB_CATALOG "', schema_name, table_name, start_snapshot, end_snapshot)"},
  {DEFAULT_SCHEMA, "table_changes",
   {"schema_name", "table_name", "start_snapshot", "end_snapshot", nullptr},
   {{nullptr, nullptr}},
   "FROM ducklake_table_changes('" PGDUCKLAKE_DUCKDB_CATALOG "', schema_name, table_name, start_snapshot, end_snapshot)"},
};
// clang-format on

/*
 * Register scalar macros for variant field extraction.
 *
 * PG inserts store variant data as VARCHAR (JSON strings). DuckDB's
 * variant_extract only works on OBJECT variants (struct inserts).
 * These macros bridge the gap by extracting from the VARCHAR/JSON
 * representation, which handles both PG-inserted and DuckDB-inserted data.
 *
 * pg_duckdb's DuckDB-only routing rewrites PG function calls to
 * system.main.<func_name>(args...). These macros expand that to the
 * underlying json_extract_string / json_extract calls.
 */
// clang-format off
static const DefaultMacro pg_ducklake_scalar_macros[] = {
  // Virtual column accessors -- expand to bare column references
  {DEFAULT_SCHEMA, "rowid", {nullptr}, {{nullptr, nullptr}}, "rowid"},
  {DEFAULT_SCHEMA, "snapshot_id", {nullptr}, {{nullptr, nullptr}}, "snapshot_id"},
  {DEFAULT_SCHEMA, "filename", {nullptr}, {{nullptr, nullptr}}, "filename"},
  {DEFAULT_SCHEMA, "file_row_number", {nullptr}, {{nullptr, nullptr}}, "file_row_number"},
  {DEFAULT_SCHEMA, "file_index", {nullptr}, {{nullptr, nullptr}}, "file_index"},
  // Variant field extraction
  {DEFAULT_SCHEMA, "pg_variant_extract", {"v", "k", nullptr}, {{nullptr, nullptr}},
   "json_extract_string(v::VARCHAR, k)"},
  /* ::VARCHAR needed so DuckDB returns VARCHAR, which PG maps to variant */
  {DEFAULT_SCHEMA, "pg_variant_extract_json", {"v", "k", nullptr}, {{nullptr, nullptr}},
   "json_extract(v::VARCHAR, k)::VARCHAR"},
  {DEFAULT_SCHEMA, "pg_variant_extract_idx", {"v", "i", nullptr}, {{nullptr, nullptr}},
   "json_extract_string(v::VARCHAR, concat('$[', i, ']'))"},
  {DEFAULT_SCHEMA, "pg_variant_extract_json_idx", {"v", "i", nullptr}, {{nullptr, nullptr}},
   "json_extract(v::VARCHAR, concat('$[', i, ']'))::VARCHAR"},
};
// clang-format on

/*
 * Shared helpers for table function wrappers.
 *
 * DuckDB macros don't support overloading (same name, different params),
 * so functions like cleanup_old_files and merge_adjacent_files use
 * TableFunctionSets instead. Each overload's bind function looks up the
 * underlying ducklake_<name>, injects the catalog constant, sets the
 * right named parameters, and replaces input.table_function.
 */

/* Look up an upstream ducklake_<name> table function by name and arity. */
static TableFunction
LookupUpstreamFunction(ClientContext &context, const string &ducklake_name,
                       vector<LogicalType> arg_types = {LogicalType::VARCHAR}) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry(context, CatalogType::TABLE_FUNCTION_ENTRY, DEFAULT_SCHEMA, ducklake_name)
	                  .Cast<TableFunctionCatalogEntry>();
	return entry.functions.GetFunctionByArguments(context, arg_types);
}

/* Stub init/execute for bind-pattern table functions -- bind replaces
 * the function pointer before these are reached. */
static unique_ptr<GlobalTableFunctionState>
UnreachableInit(ClientContext &, TableFunctionInitInput &) {
	throw InternalException("UnreachableInit should never be called");
}

static void
UnreachableExecute(ClientContext &, TableFunctionInput &, DataChunk &) {
	throw InternalException("UnreachableExecute should never be called");
}

/* Register a TableFunctionSet in the DuckDB system catalog. */
static void
RegisterTableFunctionSet(DatabaseInstance &db, TableFunctionSet &set) {
	CreateTableFunctionInfo info(set);
	auto &catalog = Catalog::GetSystemCatalog(db);
	auto transaction = CatalogTransaction::GetSystemTransaction(db);
	catalog.CreateTableFunction(transaction, info);
}

/* Reset bind input to just the catalog constant (the first positional arg of
 * every ducklake_<name>) with no named parameters. Overload-specific inputs
 * are added by the caller afterwards. */
static void
ResetToCatalogOnly(TableFunctionBindInput &input) {
	input.inputs.clear();
	input.inputs.push_back(duckdb::Value(PGDUCKLAKE_DUCKDB_CATALOG));
	input.named_parameters.clear();
}

/*
 * cleanup_old_files() / cleanup_orphaned_files(): bind-pattern (.bind)
 * functions. The no-args overload of both passes cleanup_all=true to its
 * upstream; cleanup_old_files additionally has an interval overload.
 */
static unique_ptr<FunctionData>
CleanupAllBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
               vector<string> &names, const string &ducklake_name) {
	ResetToCatalogOnly(input);
	input.named_parameters["cleanup_all"] = duckdb::Value::BOOLEAN(true);

	auto func = LookupUpstreamFunction(context, ducklake_name);
	input.table_function = func;
	return func.bind(context, input, return_types, names);
}

static unique_ptr<FunctionData>
CleanupOldFilesNoArgsBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                          vector<string> &names) {
	return CleanupAllBind(context, input, return_types, names, "ducklake_cleanup_old_files");
}

static unique_ptr<FunctionData>
CleanupIntervalBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                    vector<string> &names) {
	auto interval_val = input.inputs[0].GetValue<interval_t>();
	auto now = duckdb::Timestamp::GetCurrentTimestamp();
	auto older_than = duckdb::Interval::Add(now, duckdb::Interval::Invert(interval_val));

	ResetToCatalogOnly(input);
	input.named_parameters["older_than"] = duckdb::Value::TIMESTAMPTZ(timestamp_tz_t(older_than.value));

	auto func = LookupUpstreamFunction(context, "ducklake_cleanup_old_files");
	input.table_function = func;
	return func.bind(context, input, return_types, names);
}

/* cleanup_orphaned_files(): no-args only; same cleanup_all=true prologue, but
 * delegates to the differently-named upstream ducklake_delete_orphaned_files. */
static unique_ptr<FunctionData>
OrphanedNoArgsBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                   vector<string> &names) {
	return CleanupAllBind(context, input, return_types, names, "ducklake_delete_orphaned_files");
}

/*
 * bind_operator-pattern functions: flush_inlined_data, merge_adjacent_files,
 * rewrite_data_files. Upstream uses bind_operator (replaces the whole logical
 * plan, rather than just producing FunctionData like .bind) and accepts
 * (catalog) or (catalog, ...table args...) overloads. PG exposes a no-args
 * form (whole catalog) and a (text, text) form (specific table). The regclass
 * overload is handled by ducklake_function_mapping in SQL, which the planner
 * rewrites to (schema_name text, table_name text).
 *
 * The no-args overload is identical across all three. The table-args overload
 * differs in how (schema, table) reach upstream, which also changes the
 * positional arity used to resolve the upstream overload:
 *   - merge/rewrite: (catalog, table_name) positional + schema => named
 *                    -> looked up by {VARCHAR, VARCHAR}
 *   - flush:         (catalog) positional + schema_name => / table_name => named
 *                    -> looked up by {VARCHAR} (the default)
 */
static unique_ptr<LogicalOperator>
BindOpNoArgs(ClientContext &context, TableFunctionBindInput &input, idx_t bind_index, vector<string> &return_names,
             const string &ducklake_name) {
	ResetToCatalogOnly(input);

	auto func = LookupUpstreamFunction(context, ducklake_name);
	input.table_function = func;
	return func.bind_operator(context, input, bind_index, return_names);
}

/* merge/rewrite: upstream signature is (catalog, table_name, schema => ...). */
static unique_ptr<LogicalOperator>
BindOpPositionalTable(ClientContext &context, TableFunctionBindInput &input, idx_t bind_index,
                      vector<string> &return_names, const string &ducklake_name) {
	auto schema_name = input.inputs[0].GetValue<string>();
	auto table_name = input.inputs[1].GetValue<string>();

	ResetToCatalogOnly(input);
	input.inputs.push_back(duckdb::Value(table_name));
	input.named_parameters["schema"] = duckdb::Value(schema_name);

	auto func = LookupUpstreamFunction(context, ducklake_name, {LogicalType::VARCHAR, LogicalType::VARCHAR});
	input.table_function = func;
	return func.bind_operator(context, input, bind_index, return_names);
}

/* flush: upstream signature is (catalog, schema_name => ..., table_name => ...). */
static unique_ptr<LogicalOperator>
BindOpNamedTable(ClientContext &context, TableFunctionBindInput &input, idx_t bind_index, vector<string> &return_names,
                 const string &ducklake_name) {
	auto schema_name = input.inputs[0].GetValue<string>();
	auto table_name = input.inputs[1].GetValue<string>();

	ResetToCatalogOnly(input);
	input.named_parameters["schema_name"] = duckdb::Value(schema_name);
	input.named_parameters["table_name"] = duckdb::Value(table_name);

	// schema_name/table_name are named params, so the upstream overload is
	// resolved by its single positional VARCHAR (catalog) -- the default arity.
	auto func = LookupUpstreamFunction(context, ducklake_name);
	input.table_function = func;
	return func.bind_operator(context, input, bind_index, return_names);
}

/* bind_operator requires plain function pointers; stamp a no-args/table-args
 * pair per pg function, closing over the upstream name and table-arg style. */
#define DEFINE_BIND_OP_SET(prefix, ducklake_name, table_args_helper)                                                   \
	static unique_ptr<LogicalOperator> prefix##NoArgsBind(ClientContext &ctx, TableFunctionBindInput &input,           \
	                                                      idx_t bind_index, vector<string> &return_names) {            \
		return BindOpNoArgs(ctx, input, bind_index, return_names, ducklake_name);                                      \
	}                                                                                                                  \
	static unique_ptr<LogicalOperator> prefix##TableArgsBind(ClientContext &ctx, TableFunctionBindInput &input,        \
	                                                         idx_t bind_index, vector<string> &return_names) {         \
		return table_args_helper(ctx, input, bind_index, return_names, ducklake_name);                                 \
	}

DEFINE_BIND_OP_SET(Merge, "ducklake_merge_adjacent_files", BindOpPositionalTable)
DEFINE_BIND_OP_SET(Rewrite, "ducklake_rewrite_data_files", BindOpPositionalTable)
DEFINE_BIND_OP_SET(Flush, "ducklake_flush_inlined_data", BindOpNamedTable)

#undef DEFINE_BIND_OP_SET

/* Register a two-overload bind_operator set: no-args + (text, text). */
static void
RegisterBindOperatorSet(DatabaseInstance &db, const string &pg_name, table_function_bind_operator_t no_args_bind,
                        table_function_bind_operator_t table_args_bind) {
	TableFunctionSet set(pg_name);

	TableFunction no_args({}, nullptr, nullptr, nullptr);
	no_args.bind_operator = no_args_bind;
	set.AddFunction(no_args);

	TableFunction table_args({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, nullptr, nullptr);
	table_args.bind_operator = table_args_bind;
	set.AddFunction(table_args);

	RegisterTableFunctionSet(db, set);
}

/*
 * Register all of pg_ducklake's DuckLake-function bridges on a fresh DuckDB
 * instance (called from DuckDBManager::OnPostInit): the wrapper table macros
 * and scalar macros (system.main.<name> -> ducklake_<name>(catalog, ...)),
 * plus the overloaded TableFunctionSets that DuckDB macros can't express.
 */
void
RegisterDucklakeFunctions(DatabaseInstance &db) {
	auto &catalog = Catalog::GetSystemCatalog(db);
	auto transaction = CatalogTransaction::GetSystemTransaction(db);

	// Wrapper table macros: system.main.<name>(...) -> ducklake_<name>(catalog, ...).
	for (const auto &macro : pg_ducklake_wrapper_macros) {
		auto info = DefaultTableFunctionGenerator::CreateTableMacroInfo(macro);
		catalog.CreateFunction(transaction, *info);
	}

	// Scalar macros: virtual-column accessors + variant field extraction.
	for (const auto &macro : pg_ducklake_scalar_macros) {
		auto info = DefaultFunctionGenerator::CreateInternalMacroInfo(macro);
		catalog.CreateFunction(transaction, *info);
	}

	// time_travel(...): query a DuckLake table at a historical snapshot
	// (built in time_travel.cpp).
	auto time_travel = GetTimeTravelFunctions();
	RegisterTableFunctionSet(db, time_travel);

	// cleanup_old_files(): no-args + interval overloads (.bind pattern).
	{
		TableFunctionSet set("cleanup_old_files");
		set.AddFunction(TableFunction({}, UnreachableExecute, CleanupOldFilesNoArgsBind, UnreachableInit));
		set.AddFunction(
		    TableFunction({LogicalType::INTERVAL}, UnreachableExecute, CleanupIntervalBind, UnreachableInit));
		RegisterTableFunctionSet(db, set);
	}

	// cleanup_orphaned_files(): no-args only (.bind pattern).
	{
		TableFunctionSet set("cleanup_orphaned_files");
		set.AddFunction(TableFunction({}, UnreachableExecute, OrphanedNoArgsBind, UnreachableInit));
		RegisterTableFunctionSet(db, set);
	}

	// Compaction + flush: bind_operator sets (no-args + (text, text)).
	RegisterBindOperatorSet(db, "merge_adjacent_files", MergeNoArgsBind, MergeTableArgsBind);
	RegisterBindOperatorSet(db, "rewrite_data_files", RewriteNoArgsBind, RewriteTableArgsBind);
	RegisterBindOperatorSet(db, "flush_inlined_data", FlushNoArgsBind, FlushTableArgsBind);
}

} // namespace pgducklake
