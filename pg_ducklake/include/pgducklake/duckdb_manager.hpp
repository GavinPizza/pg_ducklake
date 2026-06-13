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
	// Syncs the ducklake.default_table_path GUC to DuckDB before each statement
	// (runs per GetConnection), so a runtime SET is picked up by the next
	// CREATE TABLE. OnPostInit runs only once per instance and would miss it.
	void RefreshConnectionState(duckdb::ClientContext &context) override;

private:
	static duckdb::unique_ptr<DuckDBManager> instance_;
};

/* Throws a duckdb exception on error; the DECLARE_PG_FUNCTION/InvokeCPPFunc
 * guard turns it into a PG error at the entry point.  The (query) overload
 * runs on DuckDBManager::Get()'s cached connection. */
duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(duckdb::ClientContext &context, const std::string &query);
duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(duckdb::Connection &connection, const std::string &query);
duckdb::unique_ptr<duckdb::QueryResult> DuckDBQueryOrThrow(const std::string &query);

/* Unwraps the JSON-serialized duckdb ErrorData blob in e.what() to the plain
 * message, for catch sites that report via elog/ereport instead of re-throwing. */
std::string DuckDBErrorMessage(const std::exception &e);

} // namespace pgducklake

/* Detach the "pgducklake" DuckLake catalog.  Called by the utility hook
 * after DROP EXTENSION so that a subsequent CREATE EXTENSION can
 * attach a fresh catalog. */
void ducklake_detach_catalog();

/* Attach the "pgducklake" DuckLake catalog.  Called during initial
 * extension load (DuckDBManager::OnPostInit) and on re-create
 * (ducklake_initialize). */
void ducklake_attach_catalog();

namespace pgducklake {

/* Toggle the SUBXACT_EVENT_START_SUB guard: allow opening a PG subtransaction
 * while DuckDB has an active transaction (e.g. DuckLake FlushChanges retry loop). */
void SetAllowSubtransaction(bool allow);

} // namespace pgducklake
