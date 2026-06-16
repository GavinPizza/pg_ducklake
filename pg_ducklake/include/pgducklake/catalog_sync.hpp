#pragma once

/* Sync framework: DuckDB metadata -> PG catalog. */

namespace pgducklake {

/* Prevents circular triggers during metadata->PG sync; also tells the utility hook to
 * skip DuckDB execution when creating ducklake_sorted indexes during sync. */
extern bool syncing_from_metadata;

/* Snapshot trigger skips all sync handlers. Set only via SkipSnapshotSyncGuard. */
extern bool skip_snapshot_sync;

/* Use around inserts into ducklake_snapshot that have no DDL changes to reverse-sync. */
struct SkipSnapshotSyncGuard {
	SkipSnapshotSyncGuard() {
		skip_snapshot_sync = true;
	}
	~SkipSnapshotSyncGuard() {
		skip_snapshot_sync = false;
	}
	SkipSnapshotSyncGuard(const SkipSnapshotSyncGuard &) = delete;
	SkipSnapshotSyncGuard &operator=(const SkipSnapshotSyncGuard &) = delete;
};

/* Per-object-type sync handler.  Caller guarantees: active SPI connection,
 * syncing_from_metadata = true. */
using SyncHandler = void (*)(const char *snapshot_id);

} // namespace pgducklake
