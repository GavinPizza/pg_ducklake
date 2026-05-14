// pg_vortex's binding for libpgddb's manager hook. The base
// pgddb::DuckDBManager is fine as-is: no GUCs, no MotherDuck, no extension
// table, no secrets, no postgres_role gate. The default hook bodies do
// nothing, which is what a read-only consumer wants. If pg_vortex grows
// custom DuckDB setup later (e.g., LOAD vortex on startup), subclass here
// and return that instance instead.

#include "pgddb/pgddb_duckdb.hpp"

namespace pgddb {

duckdb::unique_ptr<DuckDBManager>
GetManagerInstance() {
	return duckdb::make_uniq<DuckDBManager>();
}

} // namespace pgddb
