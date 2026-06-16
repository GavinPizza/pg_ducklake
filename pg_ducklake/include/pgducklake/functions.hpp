#pragma once

#include "pgddb/pg/declarations.hpp"

namespace duckdb {
class DatabaseInstance;
}

namespace pgducklake {

void RegisterDucklakeFunctions(duckdb::DatabaseInstance &db);

/* True for ducklake-schema functions (prosrc='ducklake_only_function') that
 * must run in DuckDB; the C stub errors when called directly. */
bool IsDucklakeOnlyFunction(Oid funcid);

} // namespace pgducklake
