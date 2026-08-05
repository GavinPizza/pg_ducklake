/* PostgreSQL-backed DuckLake metadata manager: DuckDB metadata requests -> SQL on ducklake_* tables. */

#include "pgducklake/catalog_sync.hpp"
#include "pgducklake/constants.hpp"
#include "pgducklake/duckdb_manager.hpp"
#include "pgducklake/guc.hpp"
#include "pgducklake/pgducklake_metadata_manager.hpp"

#include <cstring>
#include <map>

#include "pgddb/pgddb_types.hpp"
#include "pgddb/pgddb_utils.hpp"

#include <common/ducklake_types.hpp>
#include <common/ducklake_util.hpp>
#include <duckdb/common/allocator.hpp>
#include <duckdb/common/enums/statement_type.hpp>
#include <duckdb/common/exception.hpp>
#include <duckdb/common/string_util.hpp>
#include <duckdb/common/types.hpp>
#include <duckdb/common/types/column/column_data_collection.hpp>
#include <duckdb/common/types/data_chunk.hpp>
#include <duckdb/common/types/value.hpp>
#include <duckdb/main/client_context.hpp>
#include <duckdb/main/materialized_query_result.hpp>
#include <duckdb/parser/keyword_helper.hpp>
#include <storage/ducklake_metadata_info.hpp>
#include <storage/ducklake_partition_data.hpp>
#include <storage/ducklake_stats.hpp>
#include <storage/ducklake_table_entry.hpp>

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
}

// pgddb_process_lock.hpp transitively pulls postgres.h, so it follows the PG header block.
#include "pgddb/pgddb_process_lock.hpp"

namespace pgducklake {
static duckdb::StatementType
ConvertSPIResultToDuckStatementType(int result) {
	switch (result) {
	case SPI_OK_UTILITY:
		return duckdb::StatementType::EXECUTE_STATEMENT;
	case SPI_OK_SELECT:
	case SPI_OK_SELINTO:
		return duckdb::StatementType::SELECT_STATEMENT;
	case SPI_OK_INSERT:
	case SPI_OK_INSERT_RETURNING:
		return duckdb::StatementType::INSERT_STATEMENT;
	case SPI_OK_DELETE:
	case SPI_OK_DELETE_RETURNING:
		return duckdb::StatementType::DELETE_STATEMENT;
	case SPI_OK_UPDATE:
	case SPI_OK_UPDATE_RETURNING:
		return duckdb::StatementType::UPDATE_STATEMENT;
	default:
		return duckdb::StatementType::INVALID_STATEMENT;
	}
}

static duckdb::unique_ptr<duckdb::MaterializedQueryResult>
CreateEmptyResult(duckdb::StatementType type) {
	duckdb::vector<duckdb::string> names;
	duckdb::StatementProperties properties;
	duckdb::ClientProperties client_properties;
	auto &allocator = duckdb::Allocator::DefaultAllocator();
	auto empty_collection = duckdb::make_uniq<duckdb::ColumnDataCollection>(allocator);
	return duckdb::make_uniq<duckdb::MaterializedQueryResult>(type, properties, names, std::move(empty_collection),
	                                                          client_properties);
}

/*
 * SPI_finish() and PopActiveSnapshot() cannot ereport (spi.c, snapmgr.c), so these destructors
 * are safe to run while a C++ exception unwinds. Acquisition goes through PostgresFunctionGuard
 * because SPI_connect() and GetTransactionSnapshot() can.
 * Two objects rather than one: a destructor runs only for a fully constructed object, so a
 * failed snapshot push still releases the SPI connection.
 */
struct SpiConnectionScope {
	SpiConnectionScope() {
		PostgresFunctionGuard(SPI_connect);
	}
	~SpiConnectionScope() {
		SPI_finish();
	}
	SpiConnectionScope(const SpiConnectionScope &) = delete;
	SpiConnectionScope &operator=(const SpiConnectionScope &) = delete;
};

struct ActiveSnapshotScope {
	ActiveSnapshotScope() {
		Snapshot snapshot = PostgresFunctionGuard(GetTransactionSnapshot);
		PostgresFunctionGuard(PushActiveSnapshot, snapshot);
	}
	~ActiveSnapshotScope() {
		PopActiveSnapshot();
	}
	ActiveSnapshotScope(const ActiveSnapshotScope &) = delete;
	ActiveSnapshotScope &operator=(const ActiveSnapshotScope &) = delete;
};

/*
 * Catch PG ERRORs in a subtransaction: a bare longjmp catch leaks
 * ActiveSnapshot/executor resources. CurrentResourceOwner must be restored by
 * hand after release/rollback; the GUC nest level stays outside the subxact.
 */
static int
SPIExecuteInSubtransaction(const duckdb::string &query, bool &had_error, duckdb::string &error_message) {
	MemoryContext old_context = CurrentMemoryContext;
	ResourceOwner old_owner = CurrentResourceOwner;
	int ret = -1;
	had_error = false;

	/* GUC_ACTION_SAVE, not SetConfigOption()'s GUC_ACTION_SET: a metadata query can be issued while a
	 * libpgduckdb PostgresTableReader has put the backend in PG parallel mode, and guc.c rejects
	 * GUC_ACTION_SET there. SAVE is exempt because a parallel worker pops it too; PG itself relies on
	 * that in execute_extension_script(). */
	/* Suppress NOTICEs: DuckLake re-runs CREATE TABLE IF NOT EXISTS, whose NOTICE would leak to the client. */
	int save_nestlevel = NewGUCNestLevel();
	::set_config_option("client_min_messages", "warning", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	/* DuckLake-generated SQL calls DuckDB-dialect functions (month(), murmur3_32(), ...) unqualified;
	 * resolve them to the ducklake-schema UDFs deterministically, independent of the caller's search_path. */
	::set_config_option("search_path", "ducklake", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);

	SetAllowSubtransaction(true);
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(old_context);

	PG_TRY();
	{
		ret = SPI_execute(query.c_str(), false, 0);
		ReleaseCurrentSubTransaction();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(old_context);
		ErrorData *edata = CopyErrorData();
		error_message = edata->message;
		FreeErrorData(edata);
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		had_error = true;
	}
	PG_END_TRY();

	SetAllowSubtransaction(false);
	MemoryContextSwitchTo(old_context);
	CurrentResourceOwner = old_owner;

	AtEOXact_GUC(false, save_nestlevel);
	return ret;
}

/*
 * Every PG call that can ereport(ERROR) belongs here, not in the caller: the guard's sigsetjmp is
 * in its own frame, so a longjmp out of this function skips this frame entirely. Nothing here may
 * rely on a local destructor, and hoisting a PG call into the caller reopens the leak this split
 * exists to close.
 * Query passed by pointer: __PostgresFunctionGuard__ takes its arguments by value.
 */
static duckdb::unique_ptr<duckdb::QueryResult>
CreateSPIResultBody(const duckdb::string *query_ptr) {
	duckdb::string error_message;
	bool had_error = false;
	int ret = SPIExecuteInSubtransaction(*query_ptr, had_error, error_message);

	if (had_error) {
		duckdb::ErrorData error(duckdb::ExceptionType::IO, "SPI execution failed: " + error_message);
		return duckdb::make_uniq<duckdb::MaterializedQueryResult>(std::move(error));
	}

	if (ret < 0) {
		duckdb::ErrorData error(duckdb::ExceptionType::IO,
		                        "SPI execution failed: " + duckdb::string(SPI_result_code_string(ret)));
		return duckdb::make_uniq<duckdb::MaterializedQueryResult>(std::move(error));
	}

	SPITupleTable *tuptable = SPI_tuptable;
	if (!tuptable) {
		return CreateEmptyResult(ConvertSPIResultToDuckStatementType(ret));
	}

	TupleDesc tupdesc = tuptable->tupdesc;
	int num_columns = tupdesc->natts;
	uint64 num_rows = tuptable->numvals;

	duckdb::vector<duckdb::LogicalType> types;
	duckdb::vector<duckdb::string> names;

	for (int i = 0; i < num_columns; i++) {
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		D_ASSERT(!attr->attisdropped);

		names.push_back(NameStr(attr->attname));

		types.push_back(pgddb::ConvertPostgresToDuckColumnType(attr));
	}

	duckdb::ClientProperties client_properties;
	auto &allocator = duckdb::Allocator::DefaultAllocator();
	auto collection_p = duckdb::make_uniq<duckdb::ColumnDataCollection>(allocator, types);

	// Reusable chunk, append state, and per-column append fn table so the loop below allocates nothing.
	duckdb::DataChunk chunk;
	chunk.Initialize(allocator, types, STANDARD_VECTOR_SIZE);
	duckdb::ColumnDataAppendState append_state;
	collection_p->InitializeAppend(append_state);

	auto values = (Datum *)palloc(num_columns * sizeof(Datum));
	auto deform_nulls = (bool *)palloc(num_columns * sizeof(bool));
	auto column_append = (pgddb::PostgresToDuckValueFn *)palloc(num_columns * sizeof(pgddb::PostgresToDuckValueFn));
	for (int i = 0; i < num_columns; i++) {
		column_append[i] = pgddb::GetPostgresToDuckValueFn(TupleDescAttr(tupdesc, i)->atttypid, chunk.data[i]);
	}

	for (idx_t row_idx = 0; row_idx < num_rows; row_idx += STANDARD_VECTOR_SIZE) {
		idx_t chunk_size = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, num_rows - row_idx);
		chunk.Reset();
		for (idx_t row = 0; row < chunk_size; row++) {
			HeapTuple tuple = tuptable->vals[row_idx + row];
			heap_deform_tuple(tuple, tupdesc, values, deform_nulls);
			for (int col = 0; col < num_columns; col++) {
				auto &result = chunk.data[col];
				if (deform_nulls[col]) {
					duckdb::FlatVector::Validity(result).SetInvalid(row);
				} else {
					column_append[col](result, values[col], row);
				}
			}
		}
		chunk.SetCardinality(chunk_size);
		collection_p->Append(append_state, chunk);
	}

	duckdb::StatementProperties properties;
	return duckdb::make_uniq<duckdb::MaterializedQueryResult>(duckdb::StatementType::SELECT_STATEMENT, properties,
	                                                          names, std::move(collection_p), client_properties);
}

static duckdb::unique_ptr<duckdb::QueryResult>
CreateSPIResult(const duckdb::string &query) {
	elog(DEBUG1, "Creating SPI result for query: %s", query.c_str());

	std::lock_guard<std::recursive_mutex> lock(pgddb::GlobalProcessLock::GetLock());
	pgddb::PostgresScopedStackReset scoped_stack_reset;

	try {
		SpiConnectionScope spi;
		ActiveSnapshotScope snapshot;
		return PostgresFunctionGuard(CreateSPIResultBody, &query);
	} catch (const std::exception &ex) {
		/* Repackage rather than propagate: DuckLake's metadata manager treats an error-carrying
		 * QueryResult as a value and branches on it. */
		return duckdb::make_uniq<duckdb::MaterializedQueryResult>(duckdb::ErrorData(ex));
	}
}

/* Avoids transaction.GetCatalog(): during init the AttachedDatabase is not yet reachable via db_manager. */
static void
SubstitutePgCatalogPlaceholders(duckdb::string &query) {
	query = duckdb::StringUtil::Replace(query, "{METADATA_CATALOG}", "\"" PGDUCKLAKE_PG_SCHEMA "\"");
	query =
	    duckdb::StringUtil::Replace(query, "{METADATA_CATALOG_NAME_IDENTIFIER}", "\"" PGDUCKLAKE_DUCKDB_CATALOG "\"");
	query = duckdb::StringUtil::Replace(query, "{METADATA_CATALOG_NAME_LITERAL}", "'" PGDUCKLAKE_DUCKDB_CATALOG "'");
	query = duckdb::StringUtil::Replace(query, "{METADATA_SCHEMA_NAME_LITERAL}", "'" PGDUCKLAKE_PG_SCHEMA "'");
	query = duckdb::StringUtil::Replace(query, "{METADATA_SCHEMA_ESCAPED}", "\"" PGDUCKLAKE_PG_SCHEMA "\"");
}

/*
 * Below the guard frame -- see CreateSPIResultBody.
 * Failures become duckdb::TransactionException so DuckLake's FlushChanges() retry loop can
 * intercept unique-violations from concurrent commits; a C++ throw from here propagates through
 * the guard unchanged.
 */
static duckdb::unique_ptr<duckdb::QueryResult>
CreateSPIExecuteInSubtransactionBody(const duckdb::string *query_ptr) {
	duckdb::string error_message;
	bool had_error = false;
	int ret = SPIExecuteInSubtransaction(*query_ptr, had_error, error_message);

	if (!had_error && ret < 0) {
		error_message = duckdb::string("SPI execute failed: ") + SPI_result_code_string(ret);
		had_error = true;
	}

	if (had_error) {
		throw duckdb::TransactionException("%s", error_message.c_str());
	}

	return CreateEmptyResult(duckdb::StatementType::EXECUTE_STATEMENT);
}

static duckdb::unique_ptr<duckdb::QueryResult>
CreateSPIExecuteInSubtransaction(const duckdb::string &query) {
	elog(DEBUG1, "CreateSPIExecuteInSubtransaction: %s", query.c_str());

	std::lock_guard<std::recursive_mutex> lock(pgddb::GlobalProcessLock::GetLock());
	pgddb::PostgresScopedStackReset scoped_stack_reset;

	SpiConnectionScope spi;
	// PRE_COMMIT of a pipelined implicit txn (extended protocol) has no active snapshot; SPI needs one pushed.
	ActiveSnapshotScope snapshot;

	return PostgresFunctionGuard(CreateSPIExecuteInSubtransactionBody, &query);
}

PgDuckLakeMetadataManager::PgDuckLakeMetadataManager(duckdb::DuckLakeTransaction &transaction_)
    : duckdb::PostgresMetadataManager(transaction_) {
}

PgDuckLakeMetadataManager::~PgDuckLakeMetadataManager() {
}

/* find()-guarded: GetCatalog() is unsafe during init, and these placeholders never appear in init queries. */
static void
SubstitutePathPlaceholders(duckdb::string &query, duckdb::DuckLakeTransaction &transaction) {
	if (query.find("{DATA_PATH}") == duckdb::string::npos && query.find("{METADATA_PATH}") == duckdb::string::npos) {
		return;
	}
	auto &catalog = transaction.GetCatalog();
	query =
	    duckdb::StringUtil::Replace(query, "{DATA_PATH}", duckdb::DuckLakeUtil::SQLLiteralToString(catalog.DataPath()));
	query = duckdb::StringUtil::Replace(query, "{METADATA_PATH}",
	                                    duckdb::DuckLakeUtil::SQLLiteralToString(catalog.MetadataPath()));
}

duckdb::unique_ptr<duckdb::QueryResult>
PgDuckLakeMetadataManager::Query(duckdb::string query) {
	SubstitutePathPlaceholders(query, transaction);
	SubstitutePgCatalogPlaceholders(query);
	return CreateSPIResult(query);
}

/* Mirrors the static GetProjection() in ducklake_metadata_manager.cpp. */
static duckdb::string
BuildProjection(const duckdb::vector<duckdb::string> &columns_to_read) {
	duckdb::string result;
	duckdb::idx_t i = 1;
	for (auto &entry : columns_to_read) {
		if (!result.empty()) {
			result += ", ";
		}
		result += "inlined_data." + entry + " AS c" + std::to_string(i++);
	}
	return result;
}

/* Route through DuckDB, not SPI: PostgresTableReader holds GlobalProcessLock per 32-tuple batch, not whole op. */
duckdb::unique_ptr<duckdb::QueryResult>
PgDuckLakeMetadataManager::ReadInlinedData(duckdb::DuckLakeSnapshot snapshot, const duckdb::string &inlined_table_name,
                                           const duckdb::vector<duckdb::string> &columns_to_read) {
	auto projection = BuildProjection(columns_to_read);
	auto query =
	    duckdb::StringUtil::Format(R"(
SELECT %s
FROM pgduckdb."%s".%s inlined_data
WHERE %llu >= begin_snapshot AND (%llu < end_snapshot OR end_snapshot IS NULL)
ORDER BY row_id;)",
	                               projection, PGDUCKLAKE_PG_SCHEMA, duckdb::SQLIdentifier(inlined_table_name),
	                               (unsigned long long)snapshot.snapshot_id, (unsigned long long)snapshot.snapshot_id);
	elog(DEBUG1, "ReadInlinedData via DuckDB: %s", query.c_str());
	return transaction.ExecuteRaw(query);
}

/* Same DuckDB routing as ReadInlinedData, but keeps deleted rows (no end_snapshot filter) for deletion vectors.
 * Unlike ReadInlinedData (which filters to the single live version per row_id), this read keeps ALL versions,
 * so a row_id can appear multiple times. The flush's delete-position query derives ordinals from
 * ROW_NUMBER() OVER (ORDER BY row_id, begin_snapshot) - the physical read order MUST match that, or the
 * positional delete file tombstones the wrong version and the latest value silently reverts. Over a Postgres
 * heap scan, ORDER BY row_id alone leaves version ties to physical/TID order, not begin_snapshot - so the
 * begin_snapshot tiebreaker is required here (mirrors upstream ducklake 8dd38ce0). */
duckdb::unique_ptr<duckdb::QueryResult>
PgDuckLakeMetadataManager::ReadAllInlinedDataForFlush(duckdb::DuckLakeSnapshot snapshot,
                                                      const duckdb::string &inlined_table_name,
                                                      const duckdb::vector<duckdb::string> &columns_to_read) {
	auto projection = BuildProjection(columns_to_read);
	auto query = duckdb::StringUtil::Format(R"(
SELECT %s
FROM pgduckdb."%s".%s inlined_data
WHERE %llu >= begin_snapshot
ORDER BY row_id, begin_snapshot;)",
	                                        projection, PGDUCKLAKE_PG_SCHEMA, duckdb::SQLIdentifier(inlined_table_name),
	                                        (unsigned long long)snapshot.snapshot_id);
	elog(DEBUG1, "ReadAllInlinedDataForFlush via DuckDB: %s", query.c_str());
	return transaction.ExecuteRaw(query);
}

/*
 * The flush's deleted-rows filter runs over SPI against the inlined heap table, so each partition
 * column must be rendered as PG SQL that recovers the DuckLake-typed value from its storage type:
 *  - VARCHAR is stored as BYTEA; the upstream CAST(col AS VARCHAR) would yield PG's hex form
 *    ('\x6170706c65'), silently mis-hashing every bucket/identity comparison. convert_from()
 *    restores the original string.
 *  - BLOB is stored as BYTEA; pass it through raw (murmur3_32(bytea) hashes the bytes directly,
 *    matching DuckDB, which hashes a BLOB's raw bytes).
 *  - Types stored as VARCHAR (date/timestamp family) keep the upstream CAST to their DuckDB type
 *    name, which PG parses from the stored text form.
 *  - Native types cast via GetColumnTypeInternal so the type name is PG-parseable (e.g. DuckDB
 *    DOUBLE -> DOUBLE PRECISION).
 * The transform wrapper (month(...), (murmur3_32(...) & ...) % N) then resolves to the ducklake
 * schema UDFs via the search_path forced in SPIExecuteInSubtransaction.
 */
duckdb::vector<duckdb::string>
PgDuckLakeMetadataManager::GetFlushPartitionSQLExpressions(const duckdb::DuckLakeTableEntry &table) {
	duckdb::vector<duckdb::string> result;
	auto partition_data = table.GetPartitionData();
	if (!partition_data) {
		return result;
	}
	for (auto &field : partition_data->fields) {
		auto &col = table.GetColumnByFieldId(field.field_id);
		auto col_name = duckdb::KeywordHelper::WriteOptionallyQuoted(col.GetName());
		auto type = col.GetType();
		duckdb::string rendered;
		if (type.id() == duckdb::LogicalTypeId::VARCHAR) {
			rendered = "convert_from(" + col_name + ", 'UTF8')";
		} else if (type.id() == duckdb::LogicalTypeId::BLOB) {
			rendered = col_name;
		} else if (GetColumnTypeInternal(type) == "VARCHAR") {
			rendered = "CAST(" + col_name + " AS " + type.ToString() + ")";
		} else {
			rendered = "CAST(" + col_name + " AS " + GetColumnTypeInternal(type) + ")";
		}
		/* DuckDB hashes DECIMALs wider than 18 digits (INT128 storage) by their fixed-scale
		 * string form; ducklake.murmur3_32(numeric) implements the narrow unscaled-long rule. */
		if (field.transform.type == duckdb::DuckLakeTransformType::BUCKET &&
		    type.id() == duckdb::LogicalTypeId::DECIMAL && duckdb::DecimalType::GetWidth(type) > 18) {
			rendered = "CAST(" + rendered + " AS TEXT)";
		}
		result.push_back(duckdb::DuckLakePartitionUtils::GetPartitionSQLExpression(field.transform, rendered));
	}
	return result;
}

duckdb::unique_ptr<duckdb::QueryResult>
PgDuckLakeMetadataManager::Query(duckdb::DuckLakeSnapshot snapshot, duckdb::string query) {
	DuckLakeMetadataManager::FillSnapshotArgs(query, snapshot);
	return Query(query);
}

duckdb::unique_ptr<duckdb::QueryResult>
PgDuckLakeMetadataManager::Execute(duckdb::string query) {
	SubstitutePathPlaceholders(query, transaction);
	SubstitutePgCatalogPlaceholders(query);
	return CreateSPIResult(query);
}

duckdb::unique_ptr<duckdb::QueryResult>
PgDuckLakeMetadataManager::Execute(duckdb::DuckLakeSnapshot snapshot, duckdb::string query) {
	DuckLakeMetadataManager::FillSnapshotArgs(query, snapshot);
	return Execute(query);
}

duckdb::unique_ptr<duckdb::QueryResult>
PgDuckLakeMetadataManager::ExecuteCommit(duckdb::DuckLakeSnapshot snapshot, duckdb::string query) {
	DuckLakeMetadataManager::FillSnapshotArgs(query, snapshot);
	SubstitutePgCatalogPlaceholders(query);
	/* Skip the snapshot sync trigger: nothing to reverse-sync, and it crashes on a DuckDB worker thread
	 * (PG's InterruptHoldoffCount is not thread-safe). */
	SkipSnapshotSyncGuard sync_guard;
	return CreateSPIExecuteInSubtransaction(query);
}

bool
PgDuckLakeMetadataManager::IsInitialized() {

	auto tup = SearchSysCache1(NAMESPACENAME, CStringGetDatum(PGDUCKLAKE_PG_SCHEMA));

	if (!HeapTupleIsValid(tup))
		return false;

	auto nspoid = ((Form_pg_namespace)GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);

	auto rel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyData scankey;

	ScanKeyInit(&scankey, Anum_pg_class_relnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nspoid));

	auto scan = systable_beginscan(rel, ClassNameNspIndexId, /* pg_class_relname_nsp_index */
	                               true, NULL, 1, &scankey);

	bool found = false;

	while (HeapTupleIsValid(tup = systable_getnext(scan))) {
		Form_pg_class classForm = (Form_pg_class)GETSTRUCT(tup);
		const char *relname = NameStr(classForm->relname);

		if (strncmp(relname, "ducklake_", 9) == 0 && classForm->relkind == RELKIND_RELATION) {
			found = true;
			break;
		}
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return found;
}

/* Below the guard frame -- see CreateSPIResultBody. */
static void
EnsureSnapshotTriggerBody() {
	auto save_nestlevel = NewGUCNestLevel();
	::SetConfigOption("duckdb.force_execution", "false", PGC_USERSET, PGC_S_SESSION);

	duckdb::string error_message;
	bool had_error = false;
	int ret = SPIExecuteInSubtransaction(R"(
		SELECT 1 FROM pg_trigger t
		JOIN pg_class c ON t.tgrelid = c.oid
		JOIN pg_namespace n ON c.relnamespace = n.oid
		WHERE n.nspname = 'ducklake'
		  AND c.relname = 'ducklake_snapshot'
		  AND t.tgname = 'ducklake_snapshot_sync_trigger'
		)",
	                                     had_error, error_message);

	if (!had_error && ret == SPI_OK_SELECT && SPI_processed == 0) {
		// OR REPLACE: two backends can race the probe; the loser must not error on the duplicate trigger.
		ret = SPIExecuteInSubtransaction(R"(
		CREATE OR REPLACE TRIGGER ducklake_snapshot_sync_trigger
		AFTER INSERT ON ducklake.ducklake_snapshot
		FOR EACH ROW
		EXECUTE FUNCTION ducklake._snapshot_trigger()
		)",
		                                 had_error, error_message);
	}

	AtEOXact_GUC(false, save_nestlevel);

	if (had_error || ret < 0) {
		if (!had_error) {
			error_message = SPI_result_code_string(ret);
		}
		throw duckdb::IOException("EnsureSnapshotTrigger failed: %s", error_message.c_str());
	}
}

/* Raw SPI: runs inside DuckDB's ATTACH path, where re-entering DuckDB would recurse infinitely. */
void
PgDuckLakeMetadataManager::EnsureSnapshotTrigger() {
	std::lock_guard<std::recursive_mutex> lock(pgddb::GlobalProcessLock::GetLock());
	pgddb::PostgresScopedStackReset scoped_stack_reset;

	SpiConnectionScope spi;
	ActiveSnapshotScope snapshot;

	/* No repackaging: this function already reports failure by throwing, so the guard's exception
	 * can propagate as-is. */
	PostgresFunctionGuard(EnsureSnapshotTriggerBody);
}

bool
PgDuckLakeMetadataManager::MetadataExists() {
	// Base MetadataExists probes ducklake_metadata, aborting the PG txn when absent; scan pg_class instead.
	bool initialized = IsInitialized();
	if (initialized)
		EnsureSnapshotTrigger();
	return initialized;
}

duckdb::unique_ptr<duckdb::QueryResult>
PgDuckLakeMetadataManager::AttachMetadata(const duckdb::string & /*attach_query*/) {
	// Metadata lives in PG via SPI, nothing to ATTACH; return empty success so Initialize() reaches MetadataExists().
	return CreateEmptyResult(duckdb::StatementType::SELECT_STATEMENT);
}

void
PgDuckLakeMetadataManager::InitializeDuckLake(bool has_explicit_schema, duckdb::DuckLakeEncryption encryption) {
	DuckLakeMetadataManager::InitializeDuckLake(has_explicit_schema, encryption);
	EnsureSnapshotTrigger();
}

duckdb::string
PgDuckLakeMetadataManager::GetInlinedTableQueries(duckdb::DuckLakeSnapshot commit_snapshot,
                                                  const duckdb::DuckLakeTableInfo &table,
                                                  duckdb::string &inlined_tables,
                                                  duckdb::string &inlined_table_queries) {
	auto table_name =
	    DuckLakeMetadataManager::GetInlinedTableQueries(commit_snapshot, table, inlined_tables, inlined_table_queries);

	// Grant predefined roles so SPI metadata queries succeed regardless of who created the inlined data table.
	duckdb::string roles;
	for (const char *role : {superuser_role, writer_role, reader_role}) {
		if (role && role[0] != '\0') {
			if (!roles.empty())
				roles += ", ";
			roles += duckdb::StringUtil::Format("%s", duckdb::SQLIdentifier(role));
		}
	}
	if (!roles.empty()) {
		inlined_table_queries += duckdb::StringUtil::Format("\nGRANT ALL ON {METADATA_CATALOG}.%s TO %s;",
		                                                    duckdb::SQLIdentifier(table_name), roles);
	}

	return table_name;
}

duckdb::string
PgDuckLakeMetadataManager::GenerateFileColumnStatsCTEBody(const duckdb::CTERequirement &req,
                                                          duckdb::TableIndex table_id) {
	// Plain-SQL form runs directly under SPI; the base wraps it in postgres_query(), not a real PG function.
	return DuckLakeMetadataManager::GenerateFileColumnStatsCTEBody(req, table_id);
}

TableInliningState
GetTableInliningState(Oid table_oid, uint64_t *table_id_out, uint64_t *schema_version_out, int64_t *row_limit_out) {
	int ret;
	TableInliningState state = TI_NO_TABLE;

	if ((ret = SPI_connect()) < 0) {
		elog(ERROR, "SPI_connect failed: %d", ret);
		return TI_NO_TABLE;
	}

	HeapTuple tp = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(tp)) {
		SPI_finish();
		return TI_NO_TABLE;
	}

	Form_pg_class reltup = (Form_pg_class)GETSTRUCT(tp);
	char *table_name = pstrdup(NameStr(reltup->relname));
	Oid schema_oid = reltup->relnamespace;
	ReleaseSysCache(tp);

	HeapTuple ntp = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(schema_oid));
	if (!HeapTupleIsValid(ntp)) {
		SPI_finish();
		return TI_NO_TABLE;
	}

	Form_pg_namespace nstup = (Form_pg_namespace)GETSTRUCT(ntp);
	char *schema_name = pstrdup(NameStr(nstup->nspname));
	ReleaseSysCache(ntp);

	/* Schema-bumping DDL keeps the old inlined-data row, so read the MAX(schema_version) one. */
	// Names go as query parameters, not interpolated: they are data values here and may contain quotes.
	const char *query = "SELECT dt.table_id, "
	                    "       (SELECT MAX(idt.schema_version) "
	                    "        FROM ducklake.ducklake_inlined_data_tables idt "
	                    "        WHERE idt.table_id = dt.table_id), "
	                    "       (SELECT m.value::bigint "
	                    "        FROM ducklake.ducklake_metadata m "
	                    "        WHERE m.key = 'data_inlining_row_limit' "
	                    "        AND m.scope IS NULL) "
	                    "FROM ducklake.ducklake_table dt "
	                    "JOIN ducklake.ducklake_schema ds ON dt.schema_id = ds.schema_id "
	                    "WHERE dt.table_name = $1 "
	                    "AND ds.schema_name = $2 "
	                    "AND dt.end_snapshot IS NULL "
	                    "AND ds.end_snapshot IS NULL "
	                    "LIMIT 1";
	Oid arg_types[2] = {TEXTOID, TEXTOID};
	Datum arg_values[2] = {CStringGetTextDatum(table_name), CStringGetTextDatum(schema_name)};

	ret = SPI_execute_with_args(query, 2, arg_types, arg_values, NULL, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0) {
		HeapTuple tuple = SPI_tuptable->vals[0];
		bool isnull;

		/* col 0: table_id (must be present; NULL here means no ducklake row) */
		Datum table_id_datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
		if (isnull) {
			state = TI_NO_TABLE;
			goto done;
		}
		uint64_t table_id = DatumGetInt64(table_id_datum);

		/* col 1: MAX inlined schema_version (NULL if no inlined_data_tables row) */
		Datum sv_datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 2, &isnull);
		if (isnull) {
			state = TI_NO_INLINED_TABLE;
			goto done;
		}
		uint64_t schema_version = DatumGetInt64(sv_datum);

		/* col 2: data_inlining_row_limit must be explicitly set > 0 */
		Datum limit_datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 3, &isnull);
		if (isnull || DatumGetInt64(limit_datum) <= 0) {
			state = TI_NO_INLINED_TABLE;
			goto done;
		}
		int64_t row_limit = DatumGetInt64(limit_datum);

		*table_id_out = table_id;
		*schema_version_out = schema_version;
		if (row_limit_out)
			*row_limit_out = row_limit;
		state = TI_OK;
	}

done:

	SPI_finish();
	return state;
}

bool
GetTableInliningInfo(Oid table_oid, uint64_t *table_id_out, uint64_t *schema_version_out) {
	return GetTableInliningState(table_oid, table_id_out, schema_version_out, NULL) == TI_OK;
}

uint64_t
GetNextRowIdForTable(uint64_t table_id, uint64_t schema_version) {
	int ret;
	uint64_t next_row_id = 0;

	if ((ret = SPI_connect()) < 0) {
		elog(ERROR, "SPI_connect failed: %d", ret);
		return 0;
	}

	/* Fall back to MAX(row_id) + 1 when no stats row exists yet (first insert). */
	StringInfoData query;
	initStringInfo(&query);
	appendStringInfo(&query,
	                 "SELECT next_row_id "
	                 "FROM ducklake.ducklake_table_stats "
	                 "WHERE table_id = %llu",
	                 (unsigned long long)table_id);

	ret = SPI_execute(query.data, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0) {
		HeapTuple tuple = SPI_tuptable->vals[0];
		bool isnull;
		Datum row_id_datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull) {
			next_row_id = DatumGetInt64(row_id_datum);
		}
	} else if (ret == SPI_OK_SELECT) {
		StringInfoData fallback;
		initStringInfo(&fallback);
		appendStringInfo(&fallback,
		                 "SELECT COALESCE(MAX(row_id) + 1, 0) "
		                 "FROM ducklake.ducklake_inlined_data_%llu_%llu",
		                 (unsigned long long)table_id, (unsigned long long)schema_version);

		ret = SPI_execute(fallback.data, true, 1);
		if (ret == SPI_OK_SELECT && SPI_processed > 0) {
			HeapTuple tuple = SPI_tuptable->vals[0];
			bool isnull;
			Datum row_id_datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
			if (!isnull) {
				next_row_id = DatumGetInt64(row_id_datum);
			}
		}
	}

	SPI_finish();
	return next_row_id;
}

uint64_t
GetNextSnapshotId() {
	int ret;
	uint64_t next_snapshot_id = 1;

	if ((ret = SPI_connect()) < 0) {
		elog(ERROR, "SPI_connect failed: %d", ret);
		return next_snapshot_id;
	}

	const char *query = "SELECT snapshot_id + 1 FROM ducklake.ducklake_snapshot "
	                    "ORDER BY snapshot_id DESC LIMIT 1";

	ret = SPI_execute(query, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0) {
		HeapTuple tuple = SPI_tuptable->vals[0];
		bool isnull;
		Datum snapshot_id_datum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull) {
			next_snapshot_id = DatumGetInt64(snapshot_id_datum);
		}
	}

	SPI_finish();
	return next_snapshot_id;
}

/* Widens through the engine's MergeStats so the value-vs-lexicographic comparison semantics cannot
 * drift from the read side's.
 *
 * A stats row can legitimately hold one bound and not the other, because MergeStats invalidates min
 * and max independently on a retype. The absent side stays absent; MergeStats fills a missing bound
 * only through its !AnyValid() branch, when the persisted row carries nothing at all.
 *
 * Seeding it from this batch would be silently wrong. A NULL bound costs only pruning, while a
 * too-narrow one prunes live rows, and DuckDB's compressed materialization treats a table-level min
 * as an exact constant, so a too-high min also corrupts the values an unfiltered SELECT returns. A
 * truthful bound needs every live row; a batch knows only its own.
 *
 * The one-sided row that results disables pruning for the column entirely, not just on the missing
 * side: CreateNumericStats builds on NumericStats::CreateEmpty, whose pre-seeded min = MaximumValue
 * and max = MinimumValue make it an inverted range, and HasMinMax
 * (duckdb/src/storage/statistics/numeric_stats.cpp:378-381) requires Min <= Max. VARCHAR escapes it;
 * CreateStringStats' 0x00..0xFF sentinels stay a valid range. Recovering the bound needs a rescan or
 * a fix at ALTER time. */
static duckdb::DuckLakeGlobalColumnStatsInfo
WidenColumnStats(const duckdb::LogicalType &type, const duckdb::DuckLakeGlobalColumnStatsInfo &persisted,
                 const DirectInsertColumnStat &cs) {
	auto current = duckdb::DuckLakeColumnStats::FromGlobalStats(type, persisted);

	duckdb::DuckLakeColumnStats incoming(type);
	incoming.min = cs.min_value;
	incoming.has_min = cs.has_min;
	incoming.max = cs.max_value;
	incoming.has_max = cs.has_max;
	/* Leaving this false would degrade the persisted contains_null to unknown on every direct
	 * insert. Native's inline writer sets it unconditionally too. */
	incoming.has_null_count = true;
	incoming.null_count = cs.null_count;
	/* Never seeded, like the bounds: MergeStats forgets contains_nan when the incoming stat lacks it
	 * and otherwise only ORs it in, so a persisted NULL stays NULL and false only moves to true. */
	incoming.has_contains_nan = cs.has_contains_nan;
	incoming.contains_nan = cs.contains_nan;
	/* Derived rather than hardcoded true: claiming validity while carrying no bound drives MergeStats
	 * into its !has_min / !has_max branches, which clear the persisted bounds. */
	incoming.any_valid = cs.has_min || cs.has_max;

	/* Both sides come from cs.column_type, so types_differ is false and ReconcileStatToType never
	 * runs here. */
	current.MergeStats(incoming);

	/* The mapping DuckLakeTransaction::ConvertNewGlobalStats applies, minus extra_stats -- see the
	 * degrade branch in CreateSnapshotForDirectInsert for why that one is never written back. */
	duckdb::DuckLakeGlobalColumnStatsInfo out = persisted;
	out.has_min = current.has_min;
	out.min_val = current.min;
	out.has_max = current.has_max;
	out.max_val = current.max;
	/* MergeStats folds null_count before its !AnyValid() early return, so an all-NULL batch still
	 * updates null-ness. */
	out.has_contains_null = current.has_null_count;
	out.contains_null = current.null_count > 0;
	out.has_contains_nan = current.has_contains_nan;
	out.contains_nan = current.contains_nan;
	return out;
}

/* ToStats() is the read path -- GetColumnStats hands its result to the optimizer, and a nullptr
 * cannot exclude a row -- so asking it is the only way to know whether a persisted row can still
 * mis-prune. A type switch here would drift the moment upstream teaches ToStats() a new type, and
 * liveness is not a function of the type anyway: a FLOAT row is inert while contains_nan is NULL or
 * true, live the moment it is false.
 *
 * Errs live: ToStats() throws on a bound that no longer casts to the column's current type, and a
 * row that makes the read path throw is not inert. */
static bool
PersistedStatsAreLive(const duckdb::LogicalType &type, const duckdb::DuckLakeGlobalColumnStatsInfo &persisted) {
	try {
		return duckdb::DuckLakeColumnStats::FromGlobalStats(type, persisted).ToStats() != nullptr;
	} catch (...) {
		return true;
	}
}

void
CreateSnapshotForDirectInsert(uint64_t snapshot_id, uint64_t table_id, int64_t rows_inserted,
                              const std::vector<DirectInsertColumnStat> &column_stats) {
	int ret;

	elog(DEBUG1, "CreateSnapshotForDirectInsert: creating snapshot %llu", (unsigned long long)snapshot_id);

	if ((ret = SPI_connect()) < 0) {
		elog(ERROR, "CreateSnapshotForDirectInsert: SPI_connect failed: %d", ret);
		return;
	}

	/* Carry the latest schema_version forward: direct insert is data-only, and bumping it would roll
	 * back the global catalog view and hide tables created after this one. */
	const char *query_state = "SELECT COALESCE(next_catalog_id, 1), COALESCE(next_file_id, 0), "
	                          "       COALESCE(schema_version, 0) "
	                          "FROM ducklake.ducklake_snapshot "
	                          "ORDER BY snapshot_id DESC LIMIT 1";

	uint64_t next_catalog_id = 1;
	uint64_t next_file_id = 0;
	uint64_t schema_version = 0;

	ret = SPI_execute(query_state, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0) {
		HeapTuple tuple = SPI_tuptable->vals[0];
		TupleDesc tupdesc = SPI_tuptable->tupdesc;
		bool isnull;

		Datum catalog_id_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
		if (!isnull) {
			next_catalog_id = DatumGetInt64(catalog_id_datum);
		}

		Datum file_id_datum = SPI_getbinval(tuple, tupdesc, 2, &isnull);
		if (!isnull) {
			next_file_id = DatumGetInt64(file_id_datum);
		}

		Datum schema_version_datum = SPI_getbinval(tuple, tupdesc, 3, &isnull);
		if (!isnull) {
			schema_version = DatumGetInt64(schema_version_datum);
		}
	}

	/* next_file_id + 1 mirrors upstream's inlined-only commits: DuckLake keys
	 * its table-stats cache on (next_file_id, schema_version, table_id), so a
	 * data change that does not advance it leaves stale cached stats (and a
	 * later normal-path commit would reuse row ids from the stale next_row_id). */
	StringInfoData snapshot_insert;
	initStringInfo(&snapshot_insert);
	appendStringInfo(&snapshot_insert,
	                 "INSERT INTO ducklake.ducklake_snapshot "
	                 "(snapshot_id, snapshot_time, schema_version, next_catalog_id, "
	                 "next_file_id) "
	                 "VALUES (%llu, NOW(), %llu, %llu, %llu)",
	                 (unsigned long long)snapshot_id, (unsigned long long)schema_version,
	                 (unsigned long long)next_catalog_id, (unsigned long long)(next_file_id + 1));

	elog(DEBUG1, "CreateSnapshotForDirectInsert: executing %s", snapshot_insert.data);
	ret = SPI_execute(snapshot_insert.data, false, 0);
	if (ret != SPI_OK_INSERT) {
		elog(ERROR, "CreateSnapshotForDirectInsert: failed to insert snapshot: %d", ret);
	}

	/* Must be spelled 'inlined_insert:<table_id>': DuckLake's ParseChangesMade
	 * rejects unknown change types, aborting any concurrent commit retry. */
	StringInfoData changes_insert;
	initStringInfo(&changes_insert);
	appendStringInfo(&changes_insert,
	                 "INSERT INTO ducklake.ducklake_snapshot_changes "
	                 "(snapshot_id, changes_made, author, commit_message, commit_extra_info) "
	                 "VALUES (%llu, 'inlined_insert:%llu', NULL, NULL, NULL)",
	                 (unsigned long long)snapshot_id, (unsigned long long)table_id);

	elog(DEBUG1, "CreateSnapshotForDirectInsert: executing %s", changes_insert.data);
	ret = SPI_execute(changes_insert.data, false, 0);
	if (ret != SPI_OK_INSERT) {
		elog(ERROR, "CreateSnapshotForDirectInsert: failed to insert snapshot changes: %d", ret);
	}

	/* A new stats row must also populate ducklake_table_column_stats:
	 * TransformGlobalStatsRow reads the LEFT JOINed column_id with no null check. */
	StringInfoData stats_update;
	initStringInfo(&stats_update);
	appendStringInfo(&stats_update,
	                 "UPDATE ducklake.ducklake_table_stats "
	                 "SET next_row_id = next_row_id + %lld, "
	                 "    record_count = record_count + %lld "
	                 "WHERE table_id = %llu",
	                 (long long)rows_inserted, (long long)rows_inserted, (unsigned long long)table_id);

	ret = SPI_execute(stats_update.data, false, 0);
	if (ret != SPI_OK_UPDATE) {
		elog(ERROR, "CreateSnapshotForDirectInsert: failed to update table stats: %d", ret);
	}

	if (SPI_processed == 0) {
		StringInfoData stats_insert;
		initStringInfo(&stats_insert);
		appendStringInfo(&stats_insert,
		                 "INSERT INTO ducklake.ducklake_table_stats "
		                 "(table_id, record_count, next_row_id, file_size_bytes) "
		                 "VALUES (%llu, %lld, %lld, 0)",
		                 (unsigned long long)table_id, (long long)rows_inserted, (long long)rows_inserted);

		ret = SPI_execute(stats_insert.data, false, 0);
		if (ret != SPI_OK_INSERT) {
			elog(ERROR, "CreateSnapshotForDirectInsert: failed to insert table stats: %d", ret);
		}

		StringInfoData col_stats_insert;
		initStringInfo(&col_stats_insert);
		appendStringInfo(&col_stats_insert,
		                 "INSERT INTO ducklake.ducklake_table_column_stats "
		                 "(table_id, column_id, contains_null, contains_nan, "
		                 "min_value, max_value, extra_stats) "
		                 "SELECT %llu, column_id, NULL, NULL, NULL, NULL, NULL "
		                 "FROM ducklake.ducklake_column "
		                 "WHERE table_id = %llu AND end_snapshot IS NULL",
		                 (unsigned long long)table_id, (unsigned long long)table_id);

		ret = SPI_execute(col_stats_insert.data, false, 0);
		if (ret != SPI_OK_INSERT) {
			elog(ERROR, "CreateSnapshotForDirectInsert: failed to insert column stats: %d", ret);
		}

		elog(DEBUG1, "CreateSnapshotForDirectInsert: created new stats row for table %llu",
		     (unsigned long long)table_id);
	}

	/* Widen the rows this batch described; degrade every other row of the table to unknown. Those
	 * others describe data the batch has just added to and no longer bound, and leaving them stale is
	 * the bug this block exists to prevent.
	 *
	 * Degrading keys on "not maintained" rather than on a list of known-bad types so that a type
	 * nobody anticipated -- one upstream teaches ToStats() tomorrow, a nested child column the
	 * accumulator structurally cannot see -- fails safe without anyone editing this function. The
	 * price is lost pruning, never a wrong answer. A row is widened or degraded, never both.
	 *
	 * Rows are only ever UPDATEd, never created. A missing row means the column has no table-level
	 * stats yet, which after ALTER TABLE ADD COLUMN means its pre-existing rows were back-filled from
	 * initial_default -- values this batch never saw, so a row seeded from the batch would exclude
	 * them and lie. The first-insert branch above already creates NULL rows for every column.
	 *
	 * Rows whose values do not move emit nothing, so the steady state costs one SELECT, writes no dead
	 * tuples and takes no row locks concurrent inserters would wait on. Reachable only because
	 * degrading is sticky -- DuckLake has no stats-rebuild entry point. */
	{
		std::map<uint64_t, const DirectInsertColumnStat *> observed;
		for (const auto &cs : column_stats) {
			observed[cs.column_id] = &cs;
		}

		StringInfoData sel;
		initStringInfo(&sel);
		/* Every stats row, including the ones the accumulator never looked at -- those are what the
		 * degrade exists for. No parent_column filter, because DuckLake keys child (STRUCT field, LIST
		 * element) stats by their own column_id in this same table and nothing else here maintains
		 * them; column_type comes from the catalog for the same reason. LEFT JOIN so a stats row whose
		 * column metadata is gone still reaches the loop, where an unknown type counts as
		 * unmaintainable. */
		appendStringInfo(&sel,
		                 "SELECT s.column_id, s.min_value, s.max_value, s.contains_null, s.contains_nan, "
		                 "       s.extra_stats, c.column_type "
		                 "FROM ducklake.ducklake_table_column_stats s "
		                 "LEFT JOIN ducklake.ducklake_column c "
		                 "  ON c.table_id = s.table_id AND c.column_id = s.column_id "
		                 " AND c.end_snapshot IS NULL "
		                 "WHERE s.table_id = %llu",
		                 (unsigned long long)table_id);
		/* read_only = false: take a fresh snapshot so the NULL column_stats rows inserted earlier in
		 * this same SPI connection (first-insert branch) are visible to the widen. */
		ret = SPI_execute(sel.data, false, 0);
		if (ret != SPI_OK_SELECT) {
			elog(ERROR, "CreateSnapshotForDirectInsert: failed to read column stats: %d", ret);
		}

		/* No further SPI runs before the UPDATE, so SPI_tuptable stays valid across this read loop. */
		uint64_t nrows = SPI_processed;
		StringInfoData vals;
		initStringInfo(&vals);
		int rows_to_update = 0;
		for (uint64_t r = 0; r < nrows; r++) {
			bool isnull;
			Datum cid_d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &isnull);
			if (isnull) {
				continue;
			}
			uint64_t cid = (uint64_t)DatumGetInt64(cid_d);

			auto obs_it = observed.find(cid);
			const DirectInsertColumnStat *cs = obs_it == observed.end() ? nullptr : obs_it->second;

			/* Mirrors native's TransformGlobalStatsRow: a SQL NULL means "no such stat", never a
			 * value. */
			duckdb::DuckLakeGlobalColumnStatsInfo persisted;
			persisted.column_id = duckdb::FieldIndex(cid);
			char *m = SPI_getvalue(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2);
			if (m) {
				persisted.has_min = true;
				persisted.min_val = m;
				pfree(m);
			}
			char *x = SPI_getvalue(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 3);
			if (x) {
				persisted.has_max = true;
				persisted.max_val = x;
				pfree(x);
			}
			Datum cnull_d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 4, &isnull);
			if (!isnull) {
				persisted.has_contains_null = true;
				persisted.contains_null = DatumGetBool(cnull_d);
			}
			Datum cnan_d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 5, &isnull);
			if (!isnull) {
				persisted.has_contains_nan = true;
				persisted.contains_nan = DatumGetBool(cnan_d);
			}
			char *es = SPI_getvalue(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 6);
			if (es) {
				persisted.has_extra_stats = true;
				persisted.extra_stats = es;
				pfree(es);
			}

			duckdb::LogicalType type;
			bool type_known = false;
			char *type_str = SPI_getvalue(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 7);
			if (type_str) {
				try {
					type = duckdb::DuckLakeTypes::FromString(type_str);
					type_known = true;
				} catch (...) {
					/* Nothing here can reason about an unparseable type, so the row falls to layer 2. */
				}
				pfree(type_str);
			}

			/* Starts from the catalog row so the no-op detection below reads "left alone" as
			 * "unchanged". */
			duckdb::DuckLakeGlobalColumnStatsInfo merged = persisted;
			bool maintained = false;
			if (cs && type_known) {
				try {
					merged = WidenColumnStats(type, persisted, *cs);
					maintained = cs->bounds_maintained;
				} catch (const std::exception &ex) {
					/* Reachable, not defensive: MergeStats compares through the non-Try DefaultCastAs,
					 * which throws once a persisted bound stops parsing as the column's current type --
					 * an ordinary ALTER COLUMN TYPE does that. Layer 2 then owns the row. */
					merged = persisted;
					elog(DEBUG1, "CreateSnapshotForDirectInsert: stats widen failed for table %llu column %llu: %s",
					     (unsigned long long)table_id, (unsigned long long)cid, ex.what());
				} catch (...) {
					merged = persisted;
					elog(DEBUG1, "CreateSnapshotForDirectInsert: stats widen failed for table %llu column %llu",
					     (unsigned long long)table_id, (unsigned long long)cid);
				}
			}

			if (!maintained && (!type_known || PersistedStatsAreLive(type, persisted))) {
				/* Blanking min/max and contains_nan yields CreateNumericStats/CreateStringStats' unknown:
				 * universal bounds, no pruning.
				 *
				 * extra_stats is left alone. It backs only GEOMETRY and VARIANT, whose
				 * DuckLakeColumnStats constructor always builds an extra_stats object, so a NULL column
				 * yields a default-constructed one -- and DuckLakeColumnGeoStats' default extent is
				 * inverted (xmin = +max, xmax = -max), an empty bounding box that prunes everything.
				 * Blanking would trade a stale claim for a stronger false one. SupportsInlining rejects
				 * both types, so the row cannot exist today; if that changes it needs its own degrade
				 * form.
				 *
				 * contains_null survives for a column the accumulator saw. ObserveNull needs no
				 * comparison proc, so null-ness is accurate even where the bounds are not, and it feeds
				 * HasNull on the read side. */
				merged.has_min = false;
				merged.has_max = false;
				merged.has_contains_nan = false;
				if (!cs) {
					merged.has_contains_null = false;
				}
			}

			/* Emitting nothing when nothing moved is what keeps the steady state free of dead tuples. */
			bool changed =
			    (merged.has_min != persisted.has_min) || (merged.has_min && merged.min_val != persisted.min_val) ||
			    (merged.has_max != persisted.has_max) || (merged.has_max && merged.max_val != persisted.max_val) ||
			    (merged.has_contains_null != persisted.has_contains_null) ||
			    (merged.has_contains_null && merged.contains_null != persisted.contains_null) ||
			    (merged.has_contains_nan != persisted.has_contains_nan) ||
			    (merged.has_contains_nan && merged.contains_nan != persisted.contains_nan);
			if (!changed) {
				continue;
			}

			char *min_lit = merged.has_min ? quote_literal_cstr(merged.min_val.c_str()) : pstrdup("NULL");
			char *max_lit = merged.has_max ? quote_literal_cstr(merged.max_val.c_str()) : pstrdup("NULL");
			const char *cnull_lit = merged.has_contains_null ? (merged.contains_null ? "true" : "false") : "NULL";
			const char *cnan_lit = merged.has_contains_nan ? (merged.contains_nan ? "true" : "false") : "NULL";
			/* Cast the first VALUES row so PG resolves the v(...) column types even when later rows
			 * carry SQL NULL bounds. */
			if (rows_to_update == 0) {
				appendStringInfo(&vals, "(%llu::bigint, %s::text, %s::text, %s::boolean, %s::boolean)",
				                 (unsigned long long)cid, min_lit, max_lit, cnull_lit, cnan_lit);
			} else {
				appendStringInfo(&vals, ", (%llu, %s, %s, %s, %s)", (unsigned long long)cid, min_lit, max_lit,
				                 cnull_lit, cnan_lit);
			}
			pfree(min_lit);
			pfree(max_lit);
			rows_to_update++;
		}

		if (rows_to_update > 0) {
			StringInfoData wr;
			initStringInfo(&wr);
			appendStringInfo(&wr, R"(
				UPDATE ducklake.ducklake_table_column_stats s
				SET min_value = v.new_min, max_value = v.new_max,
				    contains_null = v.new_contains_null, contains_nan = v.new_contains_nan
				FROM (VALUES %s) AS v(column_id, new_min, new_max, new_contains_null, new_contains_nan)
				WHERE s.table_id = %llu AND s.column_id = v.column_id)",
			                 vals.data, (unsigned long long)table_id);
			ret = SPI_execute(wr.data, false, 0);
			if (ret != SPI_OK_UPDATE) {
				elog(ERROR, "CreateSnapshotForDirectInsert: failed to update column stats: %d", ret);
			}
		}
	}

	SPI_finish();
	elog(DEBUG1, "CreateSnapshotForDirectInsert: successfully created snapshot %llu", (unsigned long long)snapshot_id);
}

} // namespace pgducklake
