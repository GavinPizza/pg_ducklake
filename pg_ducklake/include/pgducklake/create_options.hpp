#pragma once

/*
 * WITH (ducklake.*) support for CREATE TABLE ... USING ducklake.
 *
 * Stock PG rejects the ducklake.* reloption namespace (HEAP_RELOPT_NAMESPACES
 * only allows "toast"), so the utility hook strips the ducklake.* DefElems
 * before standard_ProcessUtility validates options, stashes them in a
 * per-process scratchpad, and the CREATE TABLE event trigger drains it.
 *
 * v1 options:
 *   ducklake.table_path -- per-table data path, pushed via the
 *                          ducklake_default_table_path session option.
 *
 * Options routed through ducklake.set_option (e.g. data_inlining_row_limit) are
 * NOT supported here: set_option refuses transaction-local table_ids, and the
 * new table is still transaction-local inside the CREATE TABLE event trigger.
 * Set those afterwards via CALL ducklake.set_option(opt, val, ...::regclass).
 */

#include <string>

struct List;

namespace pgducklake {

struct PendingCreateOptions {
	bool present = false;
	bool has_table_path = false;
	std::string table_path;
};

/*
 * Stash ducklake.* DefElems into the scratchpad and rewrite *options_ref to the
 * remainder. Returns true if any was stripped; ereport(ERROR) on unknown
 * ducklake.* names or invalid values.
 */
bool StripDucklakeCreateOptions(List **options_ref);

/* Snapshot + clear the scratchpad. present=false if nothing was stashed. */
PendingCreateOptions TakePendingCreateOptions();

/* Discard any pending scratchpad entry without applying it (hook error path). */
void ClearPendingCreateOptions();

/* Push opts.table_path into the DuckDB session as ducklake_default_table_path
 * before the generated CREATE TABLE DDL. No-op when !opts.has_table_path. */
void ApplyTablePathBeforeCreate(const PendingCreateOptions &opts);

/* RESET ducklake_default_table_path after the DDL so the override does not leak
 * to later CREATE TABLEs (RefreshConnectionState re-pushes the GUC next
 * statement). No-op when !opts.has_table_path. */
void RestoreTablePathAfterCreate(const PendingCreateOptions &opts);

} // namespace pgducklake
