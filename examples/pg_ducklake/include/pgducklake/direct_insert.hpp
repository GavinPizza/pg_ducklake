/*
 * direct_insert.hpp
 *
 * Direct insert optimization for INSERT patterns into inlined DuckLake tables.
 *
 * Supported patterns:
 *   1. INSERT INTO <table> SELECT UNNEST($1), UNNEST($2), ...
 *      -- parameterized array bulk insert via SPI
 *   2. INSERT INTO <table> VALUES (const, ...), ...
 *      -- constant-value insert via table_multi_insert (heap AM)
 *
 * Both patterns bypass DuckDB execution and write directly to the inlined
 * data table when ducklake.enable_direct_insert = true.
 */

#pragma once

#include <stdint.h>

extern "C" {
#include "postgres.h"

#include "fmgr.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"

#include "optimizer/planner.h"
}

namespace pgducklake {

struct ParamInfo {
  int param_id;
  Oid param_type;
  Oid element_type;
};

struct DirectInsertContext {
  Oid target_table_oid;
  uint64_t table_id;
  uint64_t schema_version;
  List *param_infos; // List of ParamInfo*
  int expected_row_count;
  List *target_col_names; // List of char*
  List *target_col_types; // List of Oid
};

// DirectInsertScanState is defined in the implementation file to avoid
// requiring full CustomScanState definition

PlannedStmt *TryCreateDirectInsertPlan(Query *parse, ParamListInfo bound_params);

/* Clear session-level caches.  Must be called on DuckDB instance recycle
 * (recycle_ddb) since table_id/schema_version may change. */
void ResetDirectInsertCaches();

/*
 * Shared-memory outcome counters for the direct-insert planner/exec path.
 *
 * Two axes:
 *   pattern = { matched_unnest, matched_values, unmatched }
 *   reason  = { ok, invalid_rte, no_inlined_table, schema_version_mismatch,
 *               col_types_unsupported, greater_than_limit,
 *               unsupported_insert_shape, retry }
 *
 * Matched rows always pair with reason=ok. Unmatched rows carry a specific
 * reason. Gating (non-INSERT / in tx block / GUC off / non-ducklake target)
 * does NOT bump counters -- those queries are filtered before direct-insert
 * even considers them.
 */
enum DirectInsertPattern {
  DI_PAT_MATCHED_UNNEST = 0,
  DI_PAT_MATCHED_VALUES,
  DI_PAT_UNMATCHED,
  DI_PAT_NUM,
};

enum DirectInsertReason {
  DI_R_OK = 0,
  DI_R_INVALID_RTE,
  DI_R_NO_INLINED_TABLE,
  DI_R_SCHEMA_VERSION_MISMATCH,
  DI_R_COL_TYPES_UNSUPPORTED,
  DI_R_GREATER_THAN_LIMIT,
  DI_R_UNSUPPORTED_INSERT_SHAPE,
  DI_R_RETRY,
  DI_R_NUM,
};

/* Bump counter; safe to call from any backend after ShmemStartup ran. */
void DirectInsertStatsBump(DirectInsertPattern pattern, DirectInsertReason reason);

/* Zero all counters. */
void DirectInsertStatsReset();

/* Read a single cell; used by tests and the SRF. */
uint64_t DirectInsertStatsRead(DirectInsertPattern pattern, DirectInsertReason reason);

/* Snapshot the whole matrix under one spinlock acquisition.  The
 * destination must be at least DI_PAT_NUM * DI_R_NUM uint64_t's. */
void DirectInsertStatsReadAll(uint64_t out[DI_PAT_NUM][DI_R_NUM]);

/* Human-readable enum labels (lowercase, snake_case). */
const char *DirectInsertPatternName(DirectInsertPattern pattern);
const char *DirectInsertReasonName(DirectInsertReason reason);

} // namespace pgducklake

extern "C" {

/* SRF: (pattern text, reason text, count bigint). */
Datum ducklake_direct_insert_stats(PG_FUNCTION_ARGS);

/* Zero all counters. */
Datum ducklake_reset_direct_insert_stats(PG_FUNCTION_ARGS);

} // extern "C"
