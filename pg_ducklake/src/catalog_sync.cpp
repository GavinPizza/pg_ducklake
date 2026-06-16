/*
 * catalog_sync.cpp -- Reverse sync framework: DuckDB metadata -> PG catalog.
 */

#include "pgducklake/catalog_sync.hpp"
#include "pgducklake/ducklake_table.hpp"
#include "pgducklake/guc.hpp"
#include "pgducklake/sorted_by.hpp"

#include <string>

#include "pgddb/utility/cpp_wrapper.hpp"

extern "C" {
#include "postgres.h"

#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
}

namespace pgducklake {

bool syncing_from_metadata = false;
bool skip_snapshot_sync = false;

} // namespace pgducklake

namespace {

/* Sync handlers called by ducklake_snapshot_trigger, in order. */
pgducklake::SyncHandler sync_handlers[] = {
    pgducklake::SyncNewTables,
    pgducklake::SyncDroppedTables,
    pgducklake::SyncSortKeys,
};

} // anonymous namespace

extern "C" {

/* AFTER INSERT on ducklake.ducklake_snapshot: reverse-syncs DDL made by external
 * DuckDB clients into the PG catalog via sync_handlers. */
DECLARE_PG_FUNCTION(ducklake_snapshot_trigger) {
	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "not fired by trigger manager");

	TriggerData *trigdata = (TriggerData *)fcinfo->context;

	/* skip_snapshot_sync paths have no DDL to reverse-sync and may run on a DuckDB
	 * worker thread (PG's InterruptHoldoffCount is not thread-safe); enable_metadata_sync
	 * off opts out of the per-commit sync overhead. */
	if (pgducklake::skip_snapshot_sync || !pgducklake::enable_metadata_sync) {
		return PointerGetDatum(trigdata->tg_trigtuple);
	}

	bool isnull;
	int64 snapshot_id = DatumGetInt64(SPI_getbinval(trigdata->tg_trigtuple, trigdata->tg_relation->rd_att, 1, &isnull));
	if (isnull)
		elog(ERROR, "snapshot_id is NULL");

	SPI_connect();

	auto save_nestlevel = NewGUCNestLevel();
	SetConfigOption("duckdb.force_execution", "false", PGC_USERSET, PGC_S_SESSION);

	pgducklake::syncing_from_metadata = true;

	PG_TRY();
	{
		std::string sid = std::to_string(snapshot_id);
		for (auto handler : sync_handlers)
			handler(sid.c_str());
	}
	PG_FINALLY();
	{
		pgducklake::syncing_from_metadata = false;
	}
	PG_END_TRY();

	AtEOXact_GUC(false, save_nestlevel);
	SPI_finish();
	return PointerGetDatum(trigdata->tg_trigtuple);
}
}
