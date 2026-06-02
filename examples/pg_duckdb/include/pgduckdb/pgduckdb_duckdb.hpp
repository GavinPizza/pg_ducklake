#pragma once

#include "pgddb/pgddb_duckdb.hpp"

namespace pgduckdb {

// pgduckdb::DuckDBManager extends pgddb::DuckDBManager with pg_duckdb's
// init/refresh policy (GUC -> DBConfig, MotherDuck, storage-extension
// registration, secrets + extensions refresh, the duckdb.postgres_role check)
// and owns the per-backend singleton the library no longer provides.
class DuckDBManager : public pgddb::DuckDBManager {
public:
	DuckDBManager() = default;

	static bool IsInitialized();
	static DuckDBManager &Get();
	static void Reset();

	// One-off query with transaction policy applied (equivalent to the
	// pre-refactor DuckDBQueryOrThrow(query)).
	static duckdb::unique_ptr<duckdb::QueryResult> QueryOrThrow(const std::string &query);
	// Unhide the inherited (context, query) / (connection, query) overloads.
	using ::pgddb::DuckDBManager::QueryOrThrow;

	static bool DidWrites();

	// Called from pgduckdb_userdata_cache to invalidate the in-DuckDB secrets cache.
	static void InvalidateSecretsIfInitialized();

	void OnInit(duckdb::DBConfig &config) override;
	void OnPostInit(duckdb::ClientContext &context) override;
	void RefreshConnectionState(duckdb::ClientContext &context) override;

	std::string ConnectionString() override;
	void RequireExecution() override;
	bool ShouldBeginTransaction() override;

private:
	// Copies the pg_duckdb GUC config globals into the base instance members;
	// called before each Initialize() so recycle_ddb() picks up GUC changes.
	void ApplyGucConfig();

	void LoadSecrets(duckdb::ClientContext &context);
	void DropSecrets(duckdb::ClientContext &context);
	void LoadExtensions(duckdb::ClientContext &context);
	void InstallExtensions(duckdb::ClientContext &context);

	static duckdb::unique_ptr<DuckDBManager> instance_;
	int64_t extensions_table_current_seq_ = 0;
	bool secrets_valid_ = false;
};

// Installs pgddb_get_connection_hook; called from _PG_init.
void InitDuckDBManager();

} // namespace pgduckdb
