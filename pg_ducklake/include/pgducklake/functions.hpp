#pragma once

#include "pgddb/pg/declarations.hpp"

namespace duckdb {
class DatabaseInstance;
}

namespace pgducklake {

void RegisterDucklakeFunctions(duckdb::DatabaseInstance &db);

/* True for ducklake-schema functions declared with
 * prosrc='duckdb_only_function': they must run in DuckDB -- the C stub errors
 * when called directly. */
bool IsDucklakeOnlyFunction(Oid funcid);

} // namespace pgducklake
