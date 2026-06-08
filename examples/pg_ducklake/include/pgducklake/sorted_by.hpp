#pragma once

/*
 * sorted_by.hpp
 *
 * Sorted index AM handler, CREATE/DROP INDEX interception, and pg_class
 * sync helpers for ducklake_sorted.
 */

#include "pgducklake/catalog_sync.hpp"

#include <string>
#include <vector>

#include "pgddb/pg/declarations.hpp"

namespace pgducklake {

/* When true, the snapshot trigger skips sort-key sync because set_sort/
 * reset_sort will handle the pg_class index directly after the DuckDB call. */
extern bool sort_synced_from_pg;

struct SortedIndexDrop {
	Oid index_oid;
	Oid table_oid;
};

void ApplyCreateSortedIndex(const std::string &query);

std::vector<SortedIndexDrop> FindSortedIndexDrops(DropStmt *drop);
void HandleDropSortedIndex(const std::vector<SortedIndexDrop> &drops);

struct SortedIndexCreate {
	Oid relid;
	std::string sort_spec;
};

/* Batch-sync ducklake_sorted pg_class indexes: create new ones and drop
 * reset ones.  Caller must have an active SPI connection with
 * syncing_from_metadata = true. */
void SyncSortedIndexes(const std::vector<SortedIndexCreate> &creates, const std::vector<Oid> &resets);

/* Single-table helpers (thin wrappers around SyncSortedIndexes). */
void CreateSortedIndexForTable(Oid relid, const char *sort_spec);
void DropSortedIndexForTable(Oid relid);

/* Sync sort keys from DuckLake metadata for the given snapshot.
 * Registered as a SyncHandler with the snapshot trigger framework. */
void SyncSortKeys(const char *snapshot_id);

} // namespace pgducklake
