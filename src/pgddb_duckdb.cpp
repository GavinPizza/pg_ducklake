#include "pgddb/pgddb_duckdb.hpp"

#include <filesystem>

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

#include "pgddb/catalog/pgddb_storage.hpp"
#include "pgddb/pg/guc.hpp"
#include "pgddb/pg/transactions.hpp"
#include "pgddb/utility/signal_guard.hpp"

extern "C" {
#include "postgres.h"
}

namespace pgddb {

PgddbGetConnectionHook pgddb_get_connection_hook = nullptr;

duckdb::Connection *
GetConnection(bool force_transaction) {
	if (!pgddb_get_connection_hook) {
		elog(ERROR, "pgddb_get_connection_hook is not installed; consumer must install it in _PG_init");
	}
	return pgddb_get_connection_hook(force_transaction);
}

namespace ddb {
bool
DidWrites(duckdb::ClientContext &context) {
	if (!context.transaction.HasActiveTransaction()) {
		return false;
	}
	return context.ActiveTransaction().ModifiedDatabase() != nullptr;
}
} // namespace ddb

bool
DuckDBManager::ShouldBeginTransaction() {
	return pgddb::pg::IsInTransactionBlock(true);
}

namespace {

template <typename T>
std::string
ToString(T value) {
	return std::to_string(value);
}

template <>
std::string
ToString(char *value) {
	return std::string(value);
}

} // anonymous namespace

// DuckDB v1.5 removed direct field access on DBConfigOptions for most settings;
// route everything through SetOptionByName. See duckdb/pg_duckdb#1025.
#define SET_DUCKDB_OPTION(ddb_option_name)                                                                             \
	config.SetOptionByName(#ddb_option_name, duckdb::Value(duckdb_##ddb_option_name));                                 \
	elog(DEBUG2, "[pgddb] Set DuckDB option: '" #ddb_option_name "'=%s", ToString(duckdb_##ddb_option_name).c_str());

void
DuckDBManager::Initialize() {
	elog(DEBUG2, "(pgddb/DuckDBManager) Creating DuckDB instance");

	duckdb::DBConfig config;
	config.SetOptionByName("default_null_order", "postgres");

	SET_DUCKDB_OPTION(temp_directory);
	SET_DUCKDB_OPTION(extension_directory);
	if (duckdb_temp_directory && strlen(duckdb_temp_directory) > 0) {
		std::filesystem::create_directories(duckdb_temp_directory);
	}
	if (duckdb_extension_directory && strlen(duckdb_extension_directory) > 0) {
		std::filesystem::create_directories(duckdb_extension_directory);
	}

	if (duckdb_maximum_memory > 0) {
		// Convert the memory limit from MB (as set by Postgres GUC_UNIT_MB, which is actually MiB; see
		// memory_unit_conversion_table in guc.c) to a string with the "MiB" suffix, as required by DuckDB's memory
		// parser. This ensures the value is interpreted correctly by DuckDB.
		std::string memory_limit = std::to_string(duckdb_maximum_memory) + "MiB";
		config.options.maximum_memory = duckdb::DBConfig::ParseMemoryLimit(memory_limit);
		elog(DEBUG2, "[pgddb] Set DuckDB option: 'maximum_memory'=%dMB", duckdb_maximum_memory);
	}
	if (duckdb_max_temp_directory_size != NULL && strlen(duckdb_max_temp_directory_size) != 0) {
		config.SetOptionByName("max_temp_directory_size", duckdb_max_temp_directory_size);
		elog(DEBUG2, "[pgddb] Set DuckDB option: 'max_temp_directory_size'=%s", duckdb_max_temp_directory_size);
	}

	if (duckdb_threads > -1) {
		SET_DUCKDB_OPTION(threads);
	}

	std::string connection_string;
	connection_string = ConnectionString();
	std::string pg_time_zone(pgddb::pg::GetConfigOption("TimeZone"));

	OnInit(config);

	{
    	// Block signals before initializing DuckDB to ensure signal is handled by the Postgres main thread only
    	pgddb::ThreadSignalBlockGuard guard;
    	database = new duckdb::DuckDB(connection_string, &config);
	}

	connection = duckdb::make_uniq<duckdb::Connection>(*database);

	auto &context = *connection->context;

	auto &db_manager = duckdb::DatabaseManager::Get(context);
	default_dbname = db_manager.GetDefaultDatabase(context);
	QueryOrThrow(context, "SET TimeZone =" + duckdb::KeywordHelper::WriteQuoted(pg_time_zone));

	// Register the Postgres storage extension and attach it as the "pgduckdb"
	// catalog so DuckDB can read PG heap relations. The name is internal to
	// DuckDB (never user-visible), so it is fixed here rather than per-consumer.
	// The deparser's db_and_schema fallback (pgddb_ruleutils.cpp) routes every
	// relation no consumer resolver claims to this catalog.
	{
		auto &dbconfig = duckdb::DBConfig::GetConfig(*database->instance);
		duckdb::StorageExtension::Register(dbconfig, "pgduckdb",
		                                   duckdb::make_shared_ptr<PostgresStorageExtension>());
	}
	QueryOrThrow(context, "ATTACH DATABASE 'pgduckdb' (TYPE pgduckdb)");

	OnPostInit(context);
}

duckdb::unique_ptr<duckdb::Connection>
DuckDBManager::CreateConnection() {
	RequireExecution();

	auto new_connection = duckdb::make_uniq<duckdb::Connection>(*database);
	auto &context = *new_connection->context;

	RefreshConnectionState(context);

	return new_connection;
}

duckdb::Connection *
DuckDBManager::GetConnection(bool force_transaction) {
	RequireExecution();

	auto &context = *connection->context;

	if (!context.transaction.HasActiveTransaction()) {
		if (IsSubTransaction()) {
			throw duckdb::NotImplementedException("SAVEPOINT and subtransactions are not supported in DuckDB");
		}

		if (force_transaction || ShouldBeginTransaction()) {
			/*
			 * We only want to open a new DuckDB transaction if we're already
			 * in a Postgres transaction block. Always opening a transaction
			 * incurs a significant performance penalty for single statement
			 * queries on MotherDuck. This is because a second round-trip is
			 * needed to send the COMMIT to MotherDuck when Postgres its
			 * transaction finishes. So we only want to do this when actually
			 * necessary.
			 */
			connection->BeginTransaction();
		}
	}

	RefreshConnectionState(context);

	return connection.get();
}
} // namespace pgddb
