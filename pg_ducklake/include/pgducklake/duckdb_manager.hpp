#pragma once

#include "pgddb/pgddb_duckdb.hpp"

#include <exception>
#include <string>

namespace pgducklake {

class DuckDBManager : public ::pgddb::DuckDBManager {
public:
	static bool IsInitialized();
	static DuckDBManager &Get();
	static void Reset();

protected:
	void OnPostInit(duckdb::ClientContext &context) override;
	// Syncs ducklake.default_table_path to DuckDB per GetConnection so a runtime SET
	// reaches the next CREATE TABLE; OnPostInit runs once per instance and would miss it.
	void RefreshConnectionState(duckdb::ClientContext &context) override;

private:
	static duckdb::unique_ptr<DuckDBManager> instance_;
};

/* Throws a duckdb exception on error (the DECLARE_PG_FUNCTION guard turns it into
 * a PG error). The (query) overload runs on DuckDBManager::Get()'s cached connection. */
duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(duckdb::ClientContext &context, const std::string &query);
duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(duckdb::Connection &connection, const std::string &query);
duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(const std::string &query);

/* Unwraps the JSON-serialized duckdb ErrorData in e.what() to the plain message. */
std::string DuckDBErrorMessage(const std::exception &e);

} // namespace pgducklake

/* Detach the "pgducklake" DuckLake catalog (utility hook, after DROP EXTENSION). */
void ducklake_detach_catalog();

/* Attach the "pgducklake" DuckLake catalog (OnPostInit and ducklake_initialize). */
void ducklake_attach_catalog();

namespace pgducklake {

/* Allow opening a PG subtransaction while DuckDB has an active transaction
 * (SUBXACT_EVENT_START_SUB guard; e.g. DuckLake FlushChanges retry loop). */
void SetAllowSubtransaction(bool allow);

} // namespace pgducklake
