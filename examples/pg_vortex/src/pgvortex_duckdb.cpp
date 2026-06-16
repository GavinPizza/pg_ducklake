#include "pg_vortex/pgvortex_duckdb.hpp"

namespace pg_vortex {

duckdb::unique_ptr<DuckDBManager> DuckDBManager::instance_;

bool
DuckDBManager::IsInitialized() {
	return instance_ != nullptr && instance_->database != nullptr;
}

DuckDBManager &
DuckDBManager::Get() {
	if (!instance_) {
		instance_ = duckdb::make_uniq<DuckDBManager>();
	}
	if (!instance_->database) {
		instance_->Initialize();
	}
	return *instance_;
}

void
DuckDBManager::Reset() {
	if (!instance_) {
		return;
	}
	instance_->connection = nullptr;
	delete instance_->database;
	instance_->database = nullptr;
}

static duckdb::Connection *
GetConnectionForScan(bool force_transaction) {
	return DuckDBManager::Get().GetConnection(force_transaction);
}

void
InitDuckDBManager() {
	pgddb::pgddb_get_connection_hook = GetConnectionForScan;
}

} // namespace pg_vortex
