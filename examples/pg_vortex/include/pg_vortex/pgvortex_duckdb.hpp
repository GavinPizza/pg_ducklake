#pragma once

#include "pgddb/pgddb_duckdb.hpp"

namespace pg_vortex {

// pg_vortex uses the base pgddb::DuckDBManager as-is and only hosts the
// per-backend singleton. Override OnInit/OnPostInit here for custom DuckDB
// setup (e.g. LOAD vortex on startup).
class DuckDBManager : public pgddb::DuckDBManager {
public:
	static bool IsInitialized();
	static DuckDBManager &Get();
	static void Reset();

private:
	static duckdb::unique_ptr<DuckDBManager> instance_;
};

// Installs pgddb_get_connection_hook; called from _PG_init.
void InitDuckDBManager();

} // namespace pg_vortex
