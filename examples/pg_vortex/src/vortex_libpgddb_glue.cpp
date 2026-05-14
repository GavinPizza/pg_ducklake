// Stub implementations for the surface that libpgddb currently still expects
// the consumer to provide. Each name here is a place where libpgddb leaked
// pg_duckdb-policy across the library boundary; pg_vortex's stubs are the
// minimal "do nothing / use DuckDB defaults" variant appropriate for a
// read-only extension with no MotherDuck, no duckdb.extensions table, no
// background worker. The companion Phase-D pass will use this file as the
// inventory of what needs to be pushed back into libpgddb (or properly
// parameterized).

#include <string>
#include <vector>

#include "duckdb/optimizer/optimizer_extension.hpp"

extern "C" {
#include "postgres.h"
#include "nodes/pg_list.h"
}

namespace pgduckdb {

// --- GUC globals consumed by libpgddb's pgddb_duckdb.cpp / scan/*.cpp ---

bool duckdb_log_pg_explain = false;
bool duckdb_enable_external_access = false;
bool duckdb_allow_community_extensions = false;
bool duckdb_allow_unsigned_extensions = false;
bool duckdb_autoinstall_known_extensions = false;
bool duckdb_autoload_known_extensions = false;

int duckdb_maximum_threads = -1;            // -1 = use DuckDB default
int duckdb_maximum_memory = 0;              // 0 = use DuckDB default
int duckdb_threads_for_postgres_scan = 0;   // vortex doesn't scan PG tables
int duckdb_max_workers_per_postgres_scan = 0;

// pg_duckdb declares these as char*, then passes them straight into DuckDB
// options / std::filesystem::create_directories at Initialize time. nullptr
// crashes; the empty string makes create_directories error too on macOS.
// We supply real paths under /tmp so libpgddb's Initialize can succeed.
static char g_empty[] = "";
static char g_temp_dir[] = "/tmp/pg_vortex_duckdb_tmp";
static char g_ext_dir[] = "/tmp/pg_vortex_duckdb_ext";
char *duckdb_disabled_filesystems = g_empty;
char *duckdb_motherduck_session_hint = g_empty;
char *duckdb_temporary_directory = g_temp_dir;
char *duckdb_extension_directory = g_ext_dir;
char *duckdb_max_temp_directory_size = g_empty;
char *duckdb_default_collation = g_empty;
char *duckdb_azure_transport_option_type = g_empty;
char *duckdb_custom_user_agent = g_empty;

// --- libpgddb's call-outs into still-pg_duckdb-namespaced helpers ---

// Matches pg_duckdb's DuckdbExtension shape. Re-declared locally so this file
// stays independent of pg_duckdb's headers.
struct DuckdbExtension {
	std::string name;
	std::string repository;
	bool autoload = false;
};

std::vector<DuckdbExtension>
ReadDuckdbExtensions() {
	return {};
}

bool
IsMotherDuckEnabled() {
	return false;
}

void
RequireDuckdbExecution() {
	// no-op: only called from pg_duckdb's error-path helpers
}

void
UnclaimBgwSessionHint(int /*code*/, Datum /*arg*/) {
	// no-op: pg_vortex has no background worker
}

const char *
PossiblyReuseBgwSessionHint() {
	return nullptr;
}

class UnsupportedTypeOptimizer {
public:
	static duckdb::OptimizerExtension GetOptimizerExtension();
};

duckdb::OptimizerExtension
UnsupportedTypeOptimizer::GetOptimizerExtension() {
	return {};
}

namespace ddb {

std::string
LoadExtensionQuery(const std::string & /*extension_name*/) {
	// Never called: ReadDuckdbExtensions returns an empty vector.
	return {};
}

std::string
InstallExtensionQuery(const std::string & /*extension_name*/, const std::string & /*repository*/) {
	return {};
}

} // namespace ddb

namespace pg {

bool
IsInTransactionBlock() {
	return false;
}

List *
ListDuckDBCreateSecretQueries() {
	return NIL;
}

} // namespace pg

} // namespace pgduckdb

// --- C-linkage MotherDuck FDW option helpers libpgddb calls unconditionally ---

extern "C" {

const char *
FindMotherDuckDefaultDatabase() {
	return nullptr;
}

const char *
FindMotherDuckToken() {
	return nullptr;
}

const char *
FindMotherDuckBackgroundCatalogRefreshInactivityTimeout() {
	return nullptr;
}

} // extern "C"
