#pragma once

#include "pgddb/pgddb_duckdb.hpp"

namespace pg_vortex {

// Override OnInit/OnPostInit for custom DuckDB setup (e.g. LOAD vortex).
class DuckDBManager : public pgddb::DuckDBManager {
public:
	static bool IsInitialized();
	static DuckDBManager &Get();
	static void Reset();

private:
	static duckdb::unique_ptr<DuckDBManager> instance_;
};

// Installs pgddb_get_connection_hook.
void InitDuckDBManager();

} // namespace pg_vortex
