/*
 * pgducklake.cpp -- PostgreSQL extension bootstrap entry points.
 *
 * Defines module metadata and _PG_init(), wiring GUC registration, pg_duckdb
 * callback registration, and pg_ducklake hook initialization.
 */

#include "pgducklake/constants.hpp"
#include "pgducklake/duckdb_manager.hpp"
#include "pgducklake/pgducklake_metadata_manager.hpp"
#include "storage/ducklake_metadata_manager.hpp"

#include "pgddb/pgddb_node.hpp"
#include "pgddb/utility/cpp_wrapper.hpp"

extern "C" {
#include "postgres.h"

#include "commands/extension.h"
#include "fmgr.h"
}

namespace pgducklake {

// Bootstrap entry points wired up by _PG_init below. _PG_init is their only
// caller, so they are declared here rather than in public headers. Each is
// defined in the correspondingly-named translation unit.
void InitGUCs();
void InitMaintenanceWorker();
void InitDirectInsertStatsShmem();
void InitDuckDBManager();
void RegisterDirectInsertNode();
void InitTableAmHook();
void InitHooks();
void InitRuleutilsHooks();
void InitTypeHooks();
void RegisterXactCallback();
void InitFDW();

} // namespace pgducklake

extern "C" {

#ifdef PG_MODULE_MAGIC_EXT
#ifndef PG_DUCKLAKE_VERSION
// Should always be defined via build system, but keep a fallback here for
// static analysis tools etc.
#define PG_DUCKLAKE_VERSION "unknown"
#endif
PG_MODULE_MAGIC_EXT(.name = "pg_ducklake", .version = PG_DUCKLAKE_VERSION);
#else
PG_MODULE_MAGIC;
#endif

void _PG_init(void) {
  // Register metadata manager factory in DuckLake's process-global registry.
  duckdb::DuckLakeMetadataManager::Register(PGDUCKLAKE_DUCKDB_CATALOG, pgducklake::PgDuckLakeMetadataManager::Create);
  // Register DuckLake GUCs
  pgducklake::InitGUCs();
  // Register shared memory + background maintenance launcher
  pgducklake::InitMaintenanceWorker();
  // Register shared memory for direct-insert planner/exec counters
  pgducklake::InitDirectInsertStatsShmem();
  // Install the connection hook for libpgddb's scan layer.
  pgducklake::InitDuckDBManager();
  // Register libpgddb's CustomScan node (for DuckDB-routed plans) and
  pgddb::InitNode("DuckLakeScan");
  pgducklake::RegisterDirectInsertNode();
  // Install the table-AM name hook so the lib deparser/planner can
  // recognize ducklake_methods relations.
  pgducklake::InitTableAmHook();
  // Install pg_ducklake planner/utility hooks.
  pgducklake::InitHooks();
  // Install libpgddb ruleutils hooks (db_and_schema policy).
  pgducklake::InitRuleutilsHooks();
  // Install libpgddb type hooks (DuckDB STRUCT -> ducklake.duckdb_struct).
  pgducklake::InitTypeHooks();
  // Mirror PG transaction events to DuckDB's DuckLake transaction.
  pgducklake::RegisterXactCallback();
  // Register FDW callbacks and hooks.
  pgducklake::InitFDW();
}

/*
 * ducklake_initialize() -- SQL bootstrap run once during CREATE EXTENSION. It
 * forces DuckDB init (whose OnPostInit attaches the pgducklake DuckLake
 * catalog) and, on DROP+CREATE within one backend, re-attaches it.
 */
DECLARE_PG_FUNCTION(ducklake_initialize) {
  elog(LOG, "ducklake_initialize() called");

  if (!creating_extension) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("ducklake_initialize() can only be called during "
                                                                     "CREATE EXTENSION")));
  }

  if (pgducklake::PgDuckLakeMetadataManager::IsInitialized()) {
    ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_SCHEMA), errmsg("DuckLake reserved schema \"ducklake\" is already in use")));
  }

  // Force DuckDB initialization (no-op if already alive).
  //   First CREATE: the SELECT 1 triggers DuckDBManager::Initialize(), whose
  //     OnPostInit() calls ducklake_attach_catalog().
  //   DROP+CREATE in the same backend: DuckDB is already alive, so SELECT 1
  //     does not re-run OnPostInit; the catalog was detached by the utility
  //     hook during DROP, so re-attach it here.
  bool duckdb_already_initialized = pgducklake::DuckDBManager::IsInitialized();

  pgducklake::DuckDBQueryOrThrow("SELECT 1");

  if (duckdb_already_initialized) {
    ducklake_attach_catalog();
  }

  PG_RETURN_VOID();
}

} // extern "C"
