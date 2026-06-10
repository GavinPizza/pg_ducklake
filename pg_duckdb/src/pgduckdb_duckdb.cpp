#include "pgduckdb/pgduckdb_duckdb.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/storage/storage_extension.hpp"

#include "pgddb/catalog/pgddb_storage.hpp"
#include "pgddb/pg/guc.hpp"
#include "pgddb/pg/permissions.hpp"
#include "pgddb/pg/string_utils.hpp"
#include "pgddb/pg/transactions.hpp"
#include "pgddb/pgddb_utils.hpp"
#include "pgddb/utility/cpp_wrapper.hpp"
#include "pgddb/vendor/pg_list.hpp"

#include "pgduckdb/pgduckdb_background_worker.hpp"
#include "pgduckdb/pgduckdb_extensions.hpp"
#include "pgduckdb/pgduckdb_fdw.hpp"
#include "pgduckdb/pgduckdb_guc.hpp"
#include "pgduckdb/pgduckdb_metadata_cache.hpp"
#include "pgduckdb/pgduckdb_secrets_helper.hpp"
#include "pgduckdb/pgduckdb_unsupported_type_optimizer.hpp"
#include "pgduckdb/pgduckdb_userdata_cache.hpp"
#include "pgduckdb/pgduckdb_xact.hpp"

extern "C" {
#include "postgres.h"

#include "catalog/namespace.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
}

namespace pgduckdb {

namespace {

// Used by SET_DUCKDB_OPTION below. After the dir/memory/threads globals
// moved to libpgddb, the only remaining SET_DUCKDB_OPTION call sites are
// bool flags -- so we only need the integral overload.
template <typename T>
std::string
ToString(T value) {
	return std::to_string(value);
}

const char *
GetSessionHint() {
	if (!IsEmptyString(duckdb_motherduck_session_hint)) {
		return duckdb_motherduck_session_hint;
	}
	return PossiblyReuseBgwSessionHint();
}

int64
GetSeqLastValue(const char *seq_name) {
	Oid duckdb_namespace = get_namespace_oid("duckdb", false);
	Oid table_seq_oid = get_relname_relid(seq_name, duckdb_namespace);
	return PostgresFunctionGuard(DirectFunctionCall1Coll, pg_sequence_last_value, InvalidOid, table_seq_oid);
}

std::string
DisabledFileSystems() {
	if (::pgddb::pg::AllowRawFileAccess()) {
		return duckdb_disabled_filesystems;
	}
	if (IsEmptyString(duckdb_disabled_filesystems)) {
		return "LocalFileSystem";
	}
	std::vector<std::string> fs_list = duckdb::StringUtil::Split(duckdb_disabled_filesystems, ',');
	for (auto &fs : fs_list) {
		std::string trimmed_fs = fs;
		duckdb::StringUtil::Trim(trimmed_fs);
		if (duckdb::StringUtil::CIEquals(trimmed_fs, "LocalFileSystem")) {
			return duckdb_disabled_filesystems;
		}
	}
	return "LocalFileSystem," + std::string(duckdb_disabled_filesystems);
}

} // anonymous namespace

// DuckDB v1.5 removed direct field access on DBConfigOptions for most settings;
// route everything through SetOptionByName. See duckdb/pg_duckdb#1025.
#define SET_DUCKDB_OPTION(ddb_option_name)                                                                             \
	config.SetOptionByName(#ddb_option_name, duckdb::Value(duckdb_##ddb_option_name));                                 \
	elog(DEBUG2, "[PGDuckDB] Set DuckDB option: '" #ddb_option_name "'=%s", ToString(duckdb_##ddb_option_name).c_str());

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
		instance_->ApplyGucConfig();
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

duckdb::unique_ptr<duckdb::QueryResult>
DuckDBManager::QueryOrThrow(const std::string &query) {
	return ::pgddb::DuckDBManager::QueryOrThrow(*Get().GetConnection(), query);
}

void
DuckDBManager::ApplyGucConfig() {
	// LHS = inherited base instance members; RHS = pg_duckdb GUC globals.
	duckdb_temp_directory = pgduckdb::duckdb_temp_directory;
	duckdb_extension_directory = pgduckdb::duckdb_extension_directory;
	duckdb_max_temp_directory_size = pgduckdb::duckdb_max_temp_directory_size;
	duckdb_maximum_memory = pgduckdb::duckdb_maximum_memory;
	duckdb_threads = pgduckdb::duckdb_threads;
}

bool
DuckDBManager::DidWrites() {
	if (!IsInitialized()) {
		return false;
	}
	auto *conn = Get().GetConnectionUnsafe();
	return ::pgddb::ddb::DidWrites(*conn->context);
}

void
DuckDBManager::InvalidateSecretsIfInitialized() {
	if (!IsInitialized()) {
		return;
	}
	Get().secrets_valid_ = false;
}

void
DuckDBManager::OnInit(duckdb::DBConfig &config) {
	// Build pg_duckdb-flavored user agent.
	std::string user_agent = "pg_duckdb";
	if (!IsEmptyString(duckdb_custom_user_agent)) {
		user_agent += ", ";
		user_agent += duckdb_custom_user_agent;
	}
	const char *application_name = ::pgddb::pg::GetConfigOption("application_name", true);
	if (!IsEmptyString(application_name)) {
		user_agent += ", ";
		user_agent += application_name;
	}
	config.SetOptionByName("custom_user_agent", user_agent);

	SET_DUCKDB_OPTION(allow_unsigned_extensions);
	SET_DUCKDB_OPTION(enable_external_access);
	SET_DUCKDB_OPTION(allow_community_extensions);
	SET_DUCKDB_OPTION(autoinstall_known_extensions);
	SET_DUCKDB_OPTION(autoload_known_extensions);
}

std::string
DuckDBManager::ConnectionString() {
	if (!IsMotherDuckEnabled()) {
		return {};
	}

	// Disable web login for MotherDuck. Token comes via FDW user mapping.
	setenv("motherduck_disable_web_login", "1", 1);

	std::ostringstream oss;
	oss << "md:";
	auto default_database = FindMotherDuckDefaultDatabase();
	::pgddb::AppendEscapedUri(oss, default_database);
	oss << "?";

	auto session_hint = GetSessionHint();
	if (!IsEmptyString(session_hint)) {
		oss << "session_hint=";
		::pgddb::AppendEscapedUri(oss, session_hint);
		oss << "&";
	}

	auto token = FindMotherDuckToken();
	if (token != nullptr && !AreStringEqual(token, "::FROM_ENV::")) {
		setenv("motherduck_token", token, 1);
	}

	return oss.str();
}

void
DuckDBManager::OnPostInit(duckdb::ClientContext &context) {
	auto &dbconfig = duckdb::DBConfig::GetConfig(*database->instance);
	// The "pgduckdb" PostgresStorageExtension is registered + attached by the
	// kernel's DuckDBManager::Initialize. DuckDB v1.5 removed direct push_back /
	// index-assign access on these vectors; use the new Register helpers (see
	// duckdb/pg_duckdb#1025).
	duckdb::OptimizerExtension::Register(dbconfig, UnsupportedTypeOptimizer::GetOptimizerExtension());

	auto &extension_manager = database->instance->GetExtensionManager();
	auto extension_active_load = extension_manager.BeginLoad("pgduckdb");
	D_ASSERT(extension_active_load);
	duckdb::ExtensionInstallInfo extension_install_info;
	extension_active_load->FinishLoad(extension_install_info);

	QueryOrThrow(context, "SET default_collation =" +
	                                duckdb::KeywordHelper::WriteQuoted(duckdb_default_collation));
	QueryOrThrow(context, "ATTACH DATABASE ':memory:' AS pg_temp;");

	if (IsMotherDuckEnabled()) {
		auto timeout = FindMotherDuckBackgroundCatalogRefreshInactivityTimeout();
		if (timeout != nullptr) {
			auto quoted_timeout = duckdb::KeywordHelper::WriteQuoted(timeout);
			QueryOrThrow(context,
			                   "SET motherduck_background_catalog_refresh_inactivity_timeout=" + quoted_timeout);
		}
	}

	if (duckdb_autoinstall_known_extensions) {
		InstallExtensions(context);
	}
	LoadExtensions(context);
}

void
DuckDBManager::RefreshConnectionState(duckdb::ClientContext &context) {
	std::string disabled_filesystems = DisabledFileSystems();
	if (disabled_filesystems != "") {
		QueryOrThrow(context,
		                   "SET disabled_filesystems=" + duckdb::KeywordHelper::WriteQuoted(disabled_filesystems));
	}

	if (strlen(duckdb_azure_transport_option_type) > 0) {
		QueryOrThrow(context, "SET azure_transport_option_type=" +
		                                duckdb::KeywordHelper::WriteQuoted(duckdb_azure_transport_option_type));
	}

	const auto extensions_table_last_seq = GetSeqLastValue("extensions_table_seq");
	if (extensions_table_current_seq_ < extensions_table_last_seq) {
		LoadExtensions(context);
		extensions_table_current_seq_ = extensions_table_last_seq;
	}

	if (!secrets_valid_) {
		DropSecrets(context);
		LoadSecrets(context);
		secrets_valid_ = true;
	}
}

void
DuckDBManager::RequireExecution() {
	::pgduckdb::RequireDuckdbExecution();
}

bool
DuckDBManager::ShouldBeginTransaction() {
	return ::pgduckdb::pg::IsInTransactionBlock();
}

void
DuckDBManager::LoadSecrets(duckdb::ClientContext &context) {
	auto queries = InvokeCPPFunc(::pgduckdb::pg::ListDuckDBCreateSecretQueries);
	foreach_ptr(char, query, queries) {
		QueryOrThrow(context, query);
	}
}

void
DuckDBManager::DropSecrets(duckdb::ClientContext &context) {
	auto secrets =
	    QueryOrThrow(context, "SELECT name FROM duckdb_secrets() WHERE name LIKE 'pgduckdb_secret_%';");
	while (auto chunk = secrets->Fetch()) {
		for (size_t i = 0, s = chunk->size(); i < s; ++i) {
			auto drop_secret_cmd = duckdb::StringUtil::Format("DROP SECRET %s;", chunk->GetValue(0, i).ToString());
			QueryOrThrow(context, drop_secret_cmd);
		}
	}
}

void
DuckDBManager::LoadExtensions(duckdb::ClientContext &context) {
	auto duckdb_extensions = ReadDuckdbExtensions();
	for (auto &extension : duckdb_extensions) {
		if (extension.autoload) {
			QueryOrThrow(context, ::pgduckdb::ddb::LoadExtensionQuery(extension.name));
		}
	}
}

void
DuckDBManager::InstallExtensions(duckdb::ClientContext &context) {
	auto duckdb_extensions = ReadDuckdbExtensions();
	for (auto &extension : duckdb_extensions) {
		QueryOrThrow(context, ::pgduckdb::ddb::InstallExtensionQuery(extension.name, extension.repository));
	}
}

static duckdb::Connection *
GetConnectionForScan(bool force_transaction) {
	return DuckDBManager::Get().GetConnection(force_transaction);
}

void
InitDuckDBManager() {
	::pgddb::pgddb_get_connection_hook = GetConnectionForScan;
}

} // namespace pgduckdb
