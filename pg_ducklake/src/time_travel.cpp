/*
 * time_travel() DuckDB table function. Bind resolves the table at the requested
 * snapshot and swaps in its scan function, so execute/init are never called.
 */

#include "pgducklake/constants.hpp"
#include "pgducklake/time_travel.hpp"

#include <duckdb/catalog/catalog.hpp>
#include <duckdb/catalog/catalog_entry/table_catalog_entry.hpp>
#include <duckdb/catalog/catalog_transaction.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/parser/qualified_name.hpp>
#include <duckdb/planner/tableref/bound_at_clause.hpp>
#include <storage/ducklake_scan.hpp>

namespace duckdb {
// Defined in ducklake_table_insertions.cpp (no public header available)
BoundAtClause AtClauseFromValue(const Value &input);
} // namespace duckdb

namespace pgducklake {

using namespace duckdb;

static unique_ptr<FunctionData>
TimeTravelBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
               vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("Table name cannot be NULL");
	}
	auto qname = QualifiedName::Parse(input.inputs[0].GetValue<string>());
	auto schema_name = qname.schema.empty() ? "public" : qname.schema;
	auto at_clause = AtClauseFromValue(input.inputs[1]);

	auto &catalog = Catalog::GetCatalog(context, PGDUCKLAKE_DUCKDB_CATALOG);
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, qname.name, at_clause, QueryErrorContext());
	auto entry = catalog.GetEntry(context, schema_name, lookup, OnEntryNotFound::THROW_EXCEPTION);
	auto &table = entry->Cast<TableCatalogEntry>();

	unique_ptr<FunctionData> bind_data;
	input.table_function = table.GetScanFunction(context, bind_data, lookup);

	auto &function_info = input.table_function.function_info->Cast<DuckLakeFunctionInfo>();
	names = function_info.column_names;
	return_types = function_info.column_types;
	return bind_data;
}

// Rewrites the (schema, table, version/ts) inputs to single-string form, then delegates.
static unique_ptr<FunctionData>
TimeTravelSchemaTableBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                          vector<string> &names) {
	auto schema_name = input.inputs[0].GetValue<string>();
	auto table_name = input.inputs[1].GetValue<string>();
	auto version_or_ts = std::move(input.inputs[2]);

	input.inputs.clear();
	input.inputs.push_back(duckdb::Value(schema_name + "." + table_name));
	input.inputs.push_back(std::move(version_or_ts));
	return TimeTravelBind(context, input, return_types, names);
}

static unique_ptr<GlobalTableFunctionState>
TimeTravelInit(ClientContext &context, TableFunctionInitInput &input) {
	throw InternalException("TimeTravelInit should never be called");
}

static void
TimeTravelExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	throw InternalException("TimeTravelExecute should never be called");
}

TableFunctionSet
GetTimeTravelFunctions() {
	TableFunctionSet set("time_travel");
	set.AddFunction(
	    TableFunction({LogicalType::VARCHAR, LogicalType::BIGINT}, TimeTravelExecute, TimeTravelBind, TimeTravelInit));
	set.AddFunction(TableFunction({LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ}, TimeTravelExecute, TimeTravelBind,
	                              TimeTravelInit));
	set.AddFunction(TableFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT}, TimeTravelExecute,
	                              TimeTravelSchemaTableBind, TimeTravelInit));
	set.AddFunction(TableFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ},
	                              TimeTravelExecute, TimeTravelSchemaTableBind, TimeTravelInit));
	return set;
}

} // namespace pgducklake
